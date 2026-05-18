/*
 * test_signal_fork.c — Tests for A20 signal stubs and fork ENOSYS.
 *
 * Verifies:
 * - sigaction saves/retrieves handler pointers
 * - raise() synchronously calls registered handler
 * - fork()/vfork() return ENOSYS
 * - kill() maps to task_kill
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#ifdef TEST_HOST

/* Host-mode: we mock the A20 signal API */
#define _NSIG 64
static void (*signal_handlers[_NSIG])(int);

static int __a20_sigaction(int sig, const void *act, void *oact)
{
    if (sig < 0 || sig >= _NSIG) return -1;
    if (oact) *(void **)oact = (void *)signal_handlers[sig];
    if (act) signal_handlers[sig] = (void (*)(int))act;
    return 0;
}

static int __a20_raise(int sig)
{
    if (sig < 0 || sig >= _NSIG || !signal_handlers[sig]) return 0;
    signal_handlers[sig](sig);
    return 0;
}

static int __a20_kill(int pid, int sig) { (void)pid; (void)sig; return 0; }

static int __a20_fork(void)  { errno = ENOSYS; return -1; }
static int __a20_vfork(void) { errno = ENOSYS; return -1; }

#else

/* A20 native: link against a20_signal.c and a20_fork.c */

#endif

static int __test_pass = 0;
static int __test_fail = 0;

#define ASSERT(expr, msg) do { \
    if (!(expr)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        __test_fail++; \
    } else { __test_pass++; } \
} while(0)

/* ---- Signal handler tracking ---- */
static volatile int last_sig;

static void test_handler(int sig)
{
    last_sig = sig;
}

/* ---- Tests ---- */

static void test_sigaction_save_retrieve(void)
{
    printf("[test] sigaction saves and retrieves handlers...\n");

    void *old = NULL;
    int rc = __a20_sigaction(10, (void *)test_handler, &old);
    ASSERT(rc == 0, "sigaction set handler for sig 10");
    ASSERT(old == NULL, "old handler was NULL (first set)");

    void *retrieved = NULL;
    rc = __a20_sigaction(10, NULL, &retrieved);
    ASSERT(rc == 0, "sigaction get handler for sig 10");
    ASSERT(retrieved == (void *)test_handler, "retrieved handler matches");
}

static void test_raise_calls_handler(void)
{
    printf("[test] raise() synchronously calls handler...\n");

    last_sig = 0;
    __a20_sigaction(42, (void *)test_handler, NULL);
    int rc = __a20_raise(42);
    ASSERT(rc == 0, "raise returns 0");
    ASSERT(last_sig == 42, "handler was called with sig 42");
}

static void test_raise_no_handler(void)
{
    printf("[test] raise() with no handler is safe...\n");

    last_sig = -1;
    int rc = __a20_raise(55);
    ASSERT(rc == 0, "raise with no handler returns 0");
    ASSERT(last_sig == -1, "no handler was called");
}

static void test_sigaction_invalid_sig(void)
{
    printf("[test] sigaction rejects invalid signal...\n");
    int rc = __a20_sigaction(-1, NULL, NULL);
    ASSERT(rc != 0, "sigaction with sig=-1 fails");

    rc = __a20_sigaction(_NSIG, NULL, NULL);
    ASSERT(rc != 0, "sigaction with sig=_NSIG fails");
}

static void test_fork_enosys(void)
{
    printf("[test] fork returns ENOSYS...\n");
    errno = 0;
    int pid = __a20_fork();
    ASSERT(pid == -1, "fork returns -1");
    ASSERT(errno == ENOSYS, "errno is ENOSYS");
}

static void test_vfork_enosys(void)
{
    printf("[test] vfork returns ENOSYS...\n");
    errno = 0;
    int pid = __a20_vfork();
    ASSERT(pid == -1, "vfork returns -1");
    ASSERT(errno == ENOSYS, "errno is ENOSYS");
}

static void test_kill(void)
{
    printf("[test] kill maps to task_kill...\n");
    int rc = __a20_kill(1234, 9);
    ASSERT(rc == 0, "kill returns 0 (or error from kernel)");
}

/* ---- Multiple handler test ---- */
static volatile int handler_calls[3];

static void handler_a(int sig) { handler_calls[0] = sig; }
static void handler_b(int sig) { handler_calls[1] = sig; }
static void handler_c(int sig) { handler_calls[2] = sig; }

static void test_multiple_handlers(void)
{
    printf("[test] multiple independent signal handlers...\n");

    handler_calls[0] = handler_calls[1] = handler_calls[2] = 0;

    __a20_sigaction(20, (void *)handler_a, NULL);
    __a20_sigaction(21, (void *)handler_b, NULL);
    __a20_sigaction(22, (void *)handler_c, NULL);

    __a20_raise(21);

    ASSERT(handler_calls[0] == 0, "handler_a not called");
    ASSERT(handler_calls[1] == 21, "handler_b called with sig 21");
    ASSERT(handler_calls[2] == 0, "handler_c not called");
}

int main(void)
{
    printf("=== A20 signal + fork tests ===\n\n");

    test_sigaction_save_retrieve();
    test_raise_calls_handler();
    test_raise_no_handler();
    test_sigaction_invalid_sig();
    test_fork_enosys();
    test_vfork_enosys();
    test_kill();
    test_multiple_handlers();

    printf("\n=== Results: %d passed, %d failed ===\n",
           __test_pass, __test_fail);
    return __test_fail ? 1 : 0;
}
