#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Minimal W^X + ASLR validation:
 *   1. Under the deny policy (a20.wx=deny on the cmdline, NOT the default any
 *      more) mmap(PROT_READ|PROT_WRITE|PROT_EXEC) must be rejected with
 *      errno=EACCES; mprotect promoting RW to RWX must likewise be rejected;
 *      legitimate RW/RX operations are unaffected.
 *   2. fork+exec ourselves, read back the child's post-exec stack address
 *      through the inherited pipe, and compare it against the parent's stack
 *      address -- the stack addresses of two execs of the same program must
 *      differ. The in-page offset is determined by the call chain and is
 *      identical on both sides, so differing addresses are equivalent to
 *      differing stack pages; identical addresses occur by chance with
 *      probability 1/1024, so retry for two rounds to eliminate a spurious
 *      verdict.
 */

static int fail(const char *what)
{
    printf("WX_ASLR: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static uintptr_t stack_addr(void)
{
    volatile char marker;
    return (uintptr_t)&marker;
}

/* The landing spot of the first anonymous mmap is decided by the
 * per-process mmap_base, so it can be used to observe mmap ASLR */
static uintptr_t mmap_addr(void)
{
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return 0;
    uintptr_t a = (uintptr_t)p;
    munmap(p, 4096);
    return a;
}

static int child_stack_addr(uintptr_t *out, uintptr_t *out_map)
{
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        char fdarg[16];
        snprintf(fdarg, sizeof(fdarg), "%d", pipefd[1]);
        char *args[] = { "wx_aslr_test", "child", fdarg, NULL };
        execv("/bin/wx_aslr_test", args);
        _exit(127);
    }

    close(pipefd[1]);
    char buf[64] = { 0 };
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || n <= 0)
        return -1;

    if (sscanf(buf, "%lx %lx", (unsigned long *)out,
               (unsigned long *)out_map) != 2)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        /* Child phase: write this exec's stack address and first mmap landing
         * spot back to the inherited pipe fd */
        if (argc != 3)
            _exit(2);
        int fd = atoi(argv[2]);
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%lx %lx\n",
                         (unsigned long)stack_addr(),
                         (unsigned long)mmap_addr());
        if (write(fd, buf, (size_t)n) != n)
            _exit(2);
        _exit(0);
    }

    errno = 0;
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED)
        return fail("mmap-rwx-accepted");
    if (errno != EACCES)
        return fail("mmap-rwx-errno");

    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return fail("mmap-rw");

    errno = 0;
    if (mprotect(p, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
        return fail("mprotect-rwx-accepted");
    if (errno != EACCES)
        return fail("mprotect-rwx-errno");

    /* Legitimate permission changes are unaffected: RW -> R */
    if (mprotect(p, 4096, PROT_READ) != 0)
        return fail("mprotect-r");
    if (munmap(p, 4096) != 0)
        return fail("munmap");

    uintptr_t my_sp = stack_addr();
    uintptr_t my_map = mmap_addr();
    if (my_map == 0)
        return fail("mmap-addr");
    int differ = 0;
    for (int round = 0; round < 3 && !differ; round++) {
        uintptr_t child_sp = 0, child_map = 0;
        if (child_stack_addr(&child_sp, &child_map) < 0)
            return fail("child-exec");
        printf("WX_ASLR: round=%d parent_sp=0x%lx child_exec_sp=0x%lx "
               "parent_map=0x%lx child_exec_map=0x%lx\n",
               round, (unsigned long)my_sp, (unsigned long)child_sp,
               (unsigned long)my_map, (unsigned long)child_map);
        differ = (child_sp != my_sp) && (child_map != my_map);
    }
    if (!differ)
        return fail("aslr-identical-layout");

    printf("WX_ASLR: PASS\n");
    return 0;
}
