#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_futex
#define SYS_futex 98
#endif

#ifndef SYS_set_robust_list
#define SYS_set_robust_list 99
#endif

#ifndef SYS_get_robust_list
#define SYS_get_robust_list 100
#endif

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_OWNER_DIED 0x40000000U
#define FUTEX_WAITERS 0x80000000U
#define FUTEX_BITSET_MATCH_ANY 0xffffffffU

struct robust_list {
    uintptr_t next;
};

struct robust_list_head {
    struct robust_list list;
    uint64_t futex_offset;
    struct robust_list *list_op_pending;
};

struct robust_node {
    struct robust_list list;
    int futex_word;
};

static int fail(const char *what)
{
    printf("FUTEX_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int wait_exit(pid_t pid, int expected, const char *what)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail(what);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected)
        return fail(what);
    return 0;
}

static int *shared_words(size_t count)
{
    int *words = mmap(NULL, count * sizeof(int), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    return words == MAP_FAILED ? NULL : words;
}

static int wait_wake_shared(void)
{
    int *word = shared_words(1);
    if (!word)
        return fail("shared-mmap");
    *word = 0;
    pid_t pid = fork();
    if (pid < 0)
        return fail("shared-fork");
    if (pid == 0) {
        long r = syscall(SYS_futex, word, FUTEX_WAIT, 0, NULL, NULL, 0);
        _exit(r == 0 && *word == 1 ? 0 : 2);
    }
    usleep(20000);
    *word = 1;
    if (syscall(SYS_futex, word, FUTEX_WAKE, 1, NULL, NULL, 0) != 1)
        return fail("shared-wake");
    int r = wait_exit(pid, 0, "shared-wait-child");
    munmap(word, sizeof(int));
    return r;
}

static int wait_bitset_timeout(void)
{
    int word = 0;
    struct timespec past = {0, 1};
    errno = 0;
    if (syscall(SYS_futex, &word, FUTEX_WAIT_BITSET, 0, &past, NULL, 0x2) != -1 || errno != ETIMEDOUT)
        return fail("bitset-timeout");

    int *shared = shared_words(1);
    if (!shared)
        return fail("bitset-mmap");
    *shared = 0;
    pid_t pid = fork();
    if (pid < 0)
        return fail("bitset-fork");
    if (pid == 0) {
        long r = syscall(SYS_futex, shared, FUTEX_WAIT_BITSET, 0, NULL, NULL, 0x4);
        _exit(r == 0 ? 0 : 3);
    }
    usleep(20000);
    if (syscall(SYS_futex, shared, FUTEX_WAKE_BITSET, 1, NULL, NULL, 0x2) != 0)
        return fail("wrong-bitset-wake");
    if (syscall(SYS_futex, shared, FUTEX_WAKE_BITSET, 1, NULL, NULL, 0x4) != 1)
        return fail("right-bitset-wake");
    int r = wait_exit(pid, 0, "bitset-child");
    munmap(shared, sizeof(int));
    return r;
}

static int requeue_cases(void)
{
    int *words = shared_words(2);
    if (!words)
        return fail("requeue-mmap");
    words[0] = 7;
    words[1] = 0;
    pid_t pid = fork();
    if (pid < 0)
        return fail("requeue-fork");
    if (pid == 0) {
        long r = syscall(SYS_futex, &words[0], FUTEX_WAIT, 7, NULL, NULL, 0);
        _exit(r == 0 ? 0 : 4);
    }
    usleep(20000);
    if (syscall(SYS_futex, &words[0], FUTEX_CMP_REQUEUE, 0, (void *)1, &words[1], 7) != 1)
        return fail("cmp-requeue");
    if (syscall(SYS_futex, &words[1], FUTEX_WAKE, 1, NULL, NULL, 0) != 1)
        return fail("requeue-wake-target");
    int r = wait_exit(pid, 0, "requeue-child");
    munmap(words, 2 * sizeof(int));
    return r;
}

static int private_shared_boundary(void)
{
    int private_word = 0;
    if (syscall(SYS_futex, &private_word, FUTEX_WAKE, 1, NULL, NULL, 0) != 0)
        return fail("private-wake-empty");
    int *shared = shared_words(1);
    if (!shared)
        return fail("private-shared-mmap");
    *shared = 0;
    if (syscall(SYS_futex, shared, FUTEX_WAKE, 1, NULL, NULL, 0) != 0)
        return fail("shared-wake-empty");
    munmap(shared, sizeof(int));
    return 0;
}

static int robust_list_case(void)
{
    struct robust_list_head head;
    struct robust_node node;
    memset(&head, 0, sizeof(head));
    memset(&node, 0, sizeof(node));
    head.list.next = (uintptr_t)&node.list;
    head.futex_offset = (uintptr_t)&node.futex_word - (uintptr_t)&node.list;
    node.list.next = (uintptr_t)&head.list;
    node.futex_word = (int)getpid() | FUTEX_WAITERS;

    if (syscall(SYS_set_robust_list, &head, sizeof(head)) < 0)
        return fail("set-robust-list");
    void *got = NULL;
    size_t len = 0;
    if (syscall(SYS_get_robust_list, 0, &got, &len) < 0)
        return fail("get-robust-list");
    if (got != &head || len != sizeof(head))
        return fail("robust-roundtrip");

    int *shared = shared_words(1);
    if (!shared)
        return fail("robust-shared-mmap");
    *shared = 0;
    pid_t pid = fork();
    if (pid < 0)
        return fail("robust-fork");
    if (pid == 0) {
        struct robust_list_head child_head;
        struct robust_node child_node;
        memset(&child_head, 0, sizeof(child_head));
        memset(&child_node, 0, sizeof(child_node));
        child_head.list.next = (uintptr_t)&child_node.list;
        child_head.futex_offset = (uintptr_t)&child_node.futex_word - (uintptr_t)&child_node.list;
        child_node.list.next = (uintptr_t)&child_head.list;
        child_node.futex_word = (int)getpid() | FUTEX_WAITERS;
        syscall(SYS_set_robust_list, &child_head, sizeof(child_head));
        *shared = 1;
        _exit(0);
    }
    int r = wait_exit(pid, 0, "robust-child-exit");
    if (r != 0) {
        munmap(shared, sizeof(int));
        return r;
    }
    if (*shared != 1)
        return fail("robust-child-ran");
    munmap(shared, sizeof(int));
    return (node.futex_word & FUTEX_OWNER_DIED) ? fail("parent-robust-mutated") : 0;
}

int main(void)
{
    printf("FUTEX_STRESS: start\n");
    if (wait_wake_shared() != 0)
        return 1;
    if (wait_bitset_timeout() != 0)
        return 1;
    if (requeue_cases() != 0)
        return 1;
    if (private_shared_boundary() != 0)
        return 1;
    if (robust_list_case() != 0)
        return 1;
    printf("FUTEX_STRESS: PASS\n");
    return 0;
}
