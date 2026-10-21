#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* The guest build is freestanding and has no linux/ headers; these are the
 * two operation numbers the test uses (same definitions as futex_stress.c). */
#ifndef SYS_futex
#define SYS_futex 98
#endif
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7

/* Must match A20_FUTEX_COW_TEST_VA in
 * kernel/include/uapi/a20/futex_cow_window.h.  The header cannot be included
 * from here: this tree's kernel/include/sys/syscall.h shadows musl's
 * <sys/syscall.h>, and pulling in kernel headers drags core/arch.h with it.
 * The smoke gate pins the agreement -- it asserts the kernel's inject line
 * names this exact address. */
#define FUTEX_COW_TEST_VA 0x22000000UL

/*
 * Guest-side acceptance for the futex word COW window
 * (kernel/ipc/futex.c, FUTEX_WRITABLE_WORD_WINDOW).
 *
 * The window: user_prepare_write() breaks COW outside mm->lock, a concurrent
 * fork can write-protect the leaf again before the read-modify-write, and the
 * code must re-validate the translation under mm->lock and retry instead of
 * storing into the now-shared COW frame.
 *
 * The page lives at A20_FUTEX_COW_TEST_VA and the child is forked before the
 * operation, so the word's frame really is COW-shared when the window opens.
 * In a FUTEX_COW_WINDOW_TEST build the kernel spends one shot skipping the COW
 * resolution for that page, which leaves exactly the post-fork leaf state; the
 * re-validation must then force a retry.
 *
 * Assertions:
 *   - the parent acquires the PI futex and its own view holds its tid;
 *   - the forked child's view still holds 0 -- the write did not land in the
 *     frame the child shares (this is the half a missing re-check breaks);
 *   - unlocking restores the word and a second acquire still works.
 *
 * The kernel's `[FUTEX-COW-WINDOW] ... inject` line, asserted by the smoke
 * case, is what proves the injection actually fired rather than the test
 * passing on an ordinary acquire.
 */

static int *map_word(void)
{
#ifdef CONFIG_FUTEX_COW_WINDOW_TEST
    void *want = (void *)FUTEX_COW_TEST_VA;
    void *got = mmap(want, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (got == MAP_FAILED) {
        printf("FUTEX_COW_WINDOW: FAIL mmap-fixed errno=%d\n", errno);
        return NULL;
    }
    if (got != want) {
        printf("FUTEX_COW_WINDOW: FAIL mmap-moved\n");
        return NULL;
    }
    return (int *)got;
#else
    void *got = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (got == MAP_FAILED) {
        printf("FUTEX_COW_WINDOW: FAIL mmap errno=%d\n", errno);
        return NULL;
    }
    return (int *)got;
#endif
}

static long futex_pi(int *word, int op)
{
    return syscall(SYS_futex, word, op, 0, NULL, NULL, 0);
}

int main(int argc, char **argv)
{
    int expect_inject = argc > 1 && strcmp(argv[1], "--inject") == 0;
    int failures = 0;

    if (!expect_inject) {
        printf("FUTEX_COW_WINDOW: PASS (no injection requested)\n");
        return 0;
    }
#ifndef CONFIG_FUTEX_COW_WINDOW_TEST
    printf("FUTEX_COW_WINDOW: FAIL --inject needs a FUTEX_COW_WINDOW_TEST build\n");
    return 1;
#else
    int *word = map_word();
    if (!word)
        return 1;
    *word = 0;

    /* Two pipes: parent -> child "look now", child -> parent "I saw N". */
    int go[2], seen[2];
    if (pipe(go) != 0 || pipe(seen) != 0) {
        printf("FUTEX_COW_WINDOW: FAIL pipe errno=%d\n", errno);
        return 1;
    }

    /* Fork before the operation: the word's frame is COW-shared from here on,
     * which is the state the window is about. */
    pid_t child = fork();
    if (child < 0) {
        printf("FUTEX_COW_WINDOW: FAIL fork errno=%d\n", errno);
        return 1;
    }
    if (child == 0) {
        close(go[1]);
        close(seen[0]);
        char byte;
        if (read(go[0], &byte, 1) != 1)
            _exit(2);
        int observed = *word;
        if (write(seen[1], &observed, sizeof(observed)) != sizeof(observed))
            _exit(3);
        _exit(0);
    }
    close(go[0]);
    close(seen[1]);

    int tid = (int)syscall(SYS_gettid);
    if (futex_pi(word, FUTEX_LOCK_PI) != 0) {
        printf("FUTEX_COW_WINDOW: FAIL lock errno=%d\n", errno);
        kill(child, SIGKILL);
        return 1;
    }
    if (*word != tid) {
        printf("FUTEX_COW_WINDOW: FAIL owned=%d tid=%d\n", *word, tid);
        failures++;
    } else {
        printf("FUTEX_COW_WINDOW: acquire-owns-word PASS\n");
    }

    /* The child must not see the parent's tid: that would mean the store
     * landed in the COW frame the fork made shared. */
    int observed = -1;
    if (write(go[1], "x", 1) != 1 ||
        read(seen[0], &observed, sizeof(observed)) != sizeof(observed)) {
        printf("FUTEX_COW_WINDOW: FAIL handshake errno=%d\n", errno);
        failures++;
    } else if (observed != 0) {
        printf("FUTEX_COW_WINDOW: FAIL child-saw=%d (store leaked into the "
               "shared COW frame)\n", observed);
        failures++;
    } else {
        printf("FUTEX_COW_WINDOW: cow-isolated PASS\n");
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        printf("FUTEX_COW_WINDOW: FAIL child status=%d\n", status);
        failures++;
    }

    if (futex_pi(word, FUTEX_UNLOCK_PI) != 0 || *word != 0) {
        printf("FUTEX_COW_WINDOW: FAIL unlock errno=%d word=%d\n", errno,
               *word);
        failures++;
    } else {
        printf("FUTEX_COW_WINDOW: release-clears-word PASS\n");
    }
    if (futex_pi(word, FUTEX_LOCK_PI) != 0 || *word != tid ||
        futex_pi(word, FUTEX_UNLOCK_PI) != 0) {
        printf("FUTEX_COW_WINDOW: FAIL reacquire errno=%d\n", errno);
        failures++;
    } else {
        printf("FUTEX_COW_WINDOW: reacquire PASS\n");
    }

    munmap(word, 4096);
    if (failures)
        return 1;
    printf("FUTEX_COW_WINDOW: PASS\n");
    return 0;
#endif
}
