/*
 * Native ABI Sync (futex) smoke test.
 *
 * Exercises sys_a20_futex_wait / sys_a20_futex_wake:
 *   1. value mismatch returns A20_ERR_WOULD_BLOCK immediately
 *   2. finite timeout expires with A20_ERR_TIMED_OUT
 *   3. cross-thread wake: worker sleeps on the futex word, main stores and
 *      wakes; worker observes the wakeup and exits
 * Also covers proc_create_thread (native thread_create) end to end.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

static volatile uint32_t g_futex_word;
static volatile uint32_t g_worker_woke;
static volatile uint32_t g_worker_err;

static void worker_entry(uint64_t arg)
{
    (void)arg;
    /* Standard futex loop: re-check the word after every wakeup. */
    while (__atomic_load_n(&g_futex_word, __ATOMIC_ACQUIRE) == 0) {
        a20_status_t st = a20_futex_wait((uint32_t *)&g_futex_word, 0,
                                         A20_TIMEOUT_INFINITE);
        if (st != A20_OK && st != -A20_ERR_WOULD_BLOCK) {
            g_worker_err = (uint32_t)(-st);
            a20_thread_exit(1);
        }
    }
    __atomic_store_n(&g_worker_woke, 1, __ATOMIC_RELEASE);
    a20_thread_exit(0);
}

static int fail(a20_handle_t out, int code, const char *msg, uint32_t len)
{
    a20_hdl_write_buf(out, "NATIVE_FUTEX: FAIL ", 19, (void *)0);
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

    /* 1. Mismatch: *word != expected must not block. */
    g_futex_word = 1;
    a20_status_t st = a20_futex_wait((uint32_t *)&g_futex_word, 0, A20_TIMEOUT_INFINITE);
    if (st != -A20_ERR_WOULD_BLOCK)
        return fail(out, 1, "mismatch did not return WOULDBLOCK", 35);

    /* 2. Timeout: matching value with finite timeout must time out. */
    g_futex_word = 0;
    st = a20_futex_wait((uint32_t *)&g_futex_word, 0, 5ULL * 1000 * 1000);
    if (st != -A20_ERR_TIMED_OUT)
        return fail(out, 2, "finite timeout did not return TIMED_OUT", 39);

    /* 3. Cross-thread wake. */
    uint64_t stack;
    st = a20_vm_alloc_pages(16, 3 /* RW */, &stack);
    if (st != A20_OK)
        return fail(out, 3, "vm_alloc_pages failed", 21);

    a20_thread_create_args_t tc = {0};
    tc.entry = (uint64_t)worker_entry;
    tc.arg = 0;
    tc.stack_base = (stack + 16 * 4096) & ~15ULL; /* stack grows down */
    tc.stack_size = 16 * 4096;
    tc.tls_base = 0;
    st = a20_thread_create(&tc);
    if (st < 0)
        return fail(out, 4, "thread_create failed", 21);

    /* Let the worker reach the wait, then set the word and wake it. */
    a20_time_t delay = { .secs = 0, .nsecs = 20 * 1000 * 1000 };
    a20_thread_sleep(delay);
    __atomic_store_n(&g_futex_word, 1, __ATOMIC_RELEASE);

    uint32_t woken = 0;
    st = a20_futex_wake((uint32_t *)&g_futex_word, 1, &woken);
    if (st != A20_OK)
        return fail(out, 5, "futex_wake failed", 18);

    /* Wait for the worker to observe the wakeup (bounded spin). */
    int ok = 0;
    for (int i = 0; i < 100000; i++) {
        if (__atomic_load_n(&g_worker_woke, __ATOMIC_ACQUIRE)) { ok = 1; break; }
        if (g_worker_err) break;
        a20_thread_yield();
    }
    if (!ok)
        return fail(out, 6, "worker did not wake", 20);

    a20_hdl_write_buf(out, "NATIVE_FUTEX: PASS\n", 19, (void *)0);
    return 0;
}
