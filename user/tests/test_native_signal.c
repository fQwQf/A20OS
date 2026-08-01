/*
 * Native ABI checkpoint-based signal simulation smoke test.
 *
 * Signals are never delivered asynchronously in A20OS: a20_task_kill()
 * records the signal and wakes a thread parked at a checkpoint, and the
 * handler runs when the thread calls a20_signal_check() at the next explicit
 * checkpoint.  This test verifies:
 *   1. Cross-thread kill wakes a futex-waiting thread (INTERRUPTED).
 *   2. A blocked signal is not delivered at the checkpoint and stays
 *      pending; it becomes deliverable after unmasking.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "liba20rt/a20_sync.h"
#include "liba20rt/a20_task.h"

#define SIGUSR1 10

static volatile uint32_t g_word;
static volatile int g_woke_interrupted;
static volatile int g_blocked_ok;
static volatile int g_done;

static void worker_entry(uint64_t arg)
{
    a20_handle_t out = (a20_handle_t)arg;

    /* Block SIGUSR1: it must not be delivered at the checkpoint. */
    (void)a20_signal_mask(1ULL << SIGUSR1, NULL);

    /* Block at a checkpoint (futex wait).  The kill wakes it with
     * A20_ERR_INTERRUPTED; the handler runs at the signal_check checkpoint. */
    a20_status_t st = a20_futex_wait((uint32_t *)&g_word, 0, A20_TIMEOUT_INFINITE);
    if (st == -A20_ERR_INTERRUPTED)
        g_woke_interrupted = 1;

    int64_t sigs = a20_signal_check();
    /* SIGUSR1 is blocked, so it must NOT be delivered here. */
    if (!(sigs & (1LL << SIGUSR1)))
        g_blocked_ok = 1;
    a20_hdl_write_buf(out, "NATIVE_SIGNAL: worker st=", 26, (void *)0);
    a20_hdl_write_buf(out, (st == -A20_ERR_INTERRUPTED) ? "interrupted\n" : "other\n", 12, (void *)0);

    g_done = 1;
    a20_thread_exit(0);
}

static int fail(a20_handle_t out, int code, const char *msg, uint32_t len)
{
    a20_hdl_write_buf(out, "NATIVE_SIGNAL: FAIL ", 20, (void *)0);
    a20_hdl_write_buf(out, msg, len, (void *)0);
    a20_hdl_write_buf(out, "\n", 1, (void *)0);
    return code;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    a20_handle_t out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (out == A20_HANDLE_NULL)
        return 90;

    /* Create the worker. */
    uint64_t stack;
    a20_status_t st = a20_vm_alloc_pages(16, 3, &stack);
    if (st != A20_OK)
        return fail(out, 1, "vm_alloc failed", 15);

    a20_thread_create_args_t tc = {0};
    tc.entry = (uint64_t)worker_entry;
    tc.arg = (uint64_t)out;
    tc.stack_base = (stack + 16 * 4096) & ~15ULL;
    tc.stack_size = 16 * 4096;
    tc.tls_base = 0;
    st = a20_thread_create(&tc);
    if (st < 0)
        return fail(out, 2, "thread_create failed", 21);

    /* Let the worker reach the futex wait, then kill it with SIGUSR1. */
    a20_time_t delay = { .secs = 0, .nsecs = 20 * 1000 * 1000 };
    a20_thread_sleep(delay);
    st = a20_task_kill(tc.out_thread, SIGUSR1);
    if (st != A20_OK)
        return fail(out, 3, "task_kill failed", 16);

    int ok = 0;
    for (int i = 0; i < 50000; i++) {
        if (g_done) { ok = 1; break; }
        a20_thread_yield();
    }
    if (!ok)
        return fail(out, 4, "worker did not run", 19);
    if (!g_woke_interrupted)
        return fail(out, 5, "wait not interrupted", 21);
    if (!g_blocked_ok)
        return fail(out, 6, "blocked signal delivered", 25);

    /* The pending SIGUSR1 is still queued; unmask and it becomes
     * deliverable at the next checkpoint. */
    (void)a20_signal_mask(0, NULL);
    int64_t sigs = a20_signal_check();
    if (!(sigs & (1LL << SIGUSR1)))
        return fail(out, 7, "pending signal lost after unmask", 33);

    a20_hdl_write_buf(out, "NATIVE_SIGNAL: PASS\n", 20, (void *)0);
    return 0;
}
