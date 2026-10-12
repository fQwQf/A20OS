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
#include "native-memory-contract.h"
#include "native-domain-contract.h"
#include "native-thread-contract.h"

static volatile uint32_t g_futex_word;
static volatile uint32_t g_worker_woke;
static volatile uint32_t g_worker_err;

/* These helpers deliberately use the public syscall on server targets too:
 * the same application algorithm also works without user CPU atomics. */
static uint32_t load_word(volatile uint32_t *word) {
    uint32_t value = 0;
    if (a20_atomic32_load((uint32_t *)word, &value) != A20_OK)
        a20_thread_exit(91);
    return value;
}
static void store_word(volatile uint32_t *word, uint32_t value) {
    if (a20_atomic32_store((uint32_t *)word, value) != A20_OK)
        a20_thread_exit(92);
}

static void worker_entry(uint64_t arg)
{
    (void)arg;
    /* Standard futex loop: re-check the word after every wakeup. */
    while (load_word(&g_futex_word) == 0) {
        a20_status_t st = a20_futex_wait((uint32_t *)&g_futex_word, 0,
                                         A20_TIMEOUT_INFINITE);
        if (st != A20_OK && st != -A20_ERR_WOULD_BLOCK) {
            store_word(&g_worker_err, (uint32_t)(-st));
            a20_thread_exit(1);
        }
    }
    store_word(&g_worker_woke, 1);
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

    a20_execution_domain_info_t domain;
    if (native_domain_contract(&domain))
        return fail(out, 94, "execution domain",
                    sizeof("execution domain") - 1);
    char domain_line[] = "NATIVE_DOMAIN_CPUS: 00\n";
    domain_line[sizeof(domain_line) - 4] = '0' + domain.cpu_online / 10;
    domain_line[sizeof(domain_line) - 3] = '0' + domain.cpu_online % 10;
    a20_hdl_write_buf(out, domain_line, sizeof(domain_line) - 1, (void *)0);
    a20_hdl_write_buf(out, "NATIVE_DOMAIN: PASS\n",
                      sizeof("NATIVE_DOMAIN: PASS\n") - 1, (void *)0);
    if (native_runtime_contract())
        return fail(out, 96, "runtime identity",
                    sizeof("runtime identity") - 1);
    if (native_thread_contract())
        return fail(out, 95, "thread contract", sizeof("thread contract") - 1);
    a20_hdl_write_buf(out, "NATIVE_THREAD: PASS\n",
                      sizeof("NATIVE_THREAD: PASS\n") - 1, (void *)0);
    a20_hdl_write_buf(out, "NATIVE_RUNTIME: PASS\n",
                      sizeof("NATIVE_RUNTIME: PASS\n") - 1, (void *)0);
    char thread_line[] = "NATIVE_THREAD_CPUS: 00->00\n";
    unsigned offset = sizeof("NATIVE_THREAD_CPUS: ") - 1;
    thread_line[offset] = '0' + native_thread_shared.parent_cpu / 10;
    thread_line[offset + 1] = '0' + native_thread_shared.parent_cpu % 10;
    thread_line[offset + 4] = '0' + native_thread_shared.worker_cpu / 10;
    thread_line[offset + 5] = '0' + native_thread_shared.worker_cpu % 10;
    a20_hdl_write_buf(out, thread_line, sizeof(thread_line) - 1, (void *)0);
    if (native_memory_contract())
        return fail(out, 93, "allocator contract",
                    sizeof("allocator contract") - 1);
    a20_hdl_write_buf(out, "NATIVE_ALLOCATOR: PASS\n",
                      sizeof("NATIVE_ALLOCATOR: PASS\n") - 1, (void *)0);
    _Static_assert(sizeof(a20_atomic32_args_t) == 40, "atomic32 ABI layout");
    a20_execution_info_t execution;
    if (a20_execution_info(&execution) != A20_OK ||
        !(execution.features & A20_EXEC_USER_HEAP) ||
        !(execution.features & A20_EXEC_ATOMIC32) ||
        !(execution.features & A20_EXEC_FUTEX))
        return fail(out, 10, "sync capabilities missing",
                    sizeof("sync capabilities missing") - 1);
    uint32_t observed = 0, exchanged = 0;
#define CHECK_ATOMIC(test)                                                     \
    do {                                                                       \
        if (!(test))                                                           \
            return fail(out, 11, "atomic32 contract",                          \
                        sizeof("atomic32 contract") - 1);                      \
    } while (0)
    CHECK_ATOMIC(a20_atomic32_store((uint32_t *)&g_futex_word, 0xaabbccdd) ==
                 A20_OK);
    CHECK_ATOMIC(a20_atomic32((uint32_t *)&g_futex_word,
                              A20_ATOMIC32_COMPARE_EXCHANGE, 7, 9, &observed,
                              &exchanged) == A20_OK &&
                 observed == 0xaabbccdd && !exchanged);
    CHECK_ATOMIC(a20_atomic32((uint32_t *)&g_futex_word,
                              A20_ATOMIC32_COMPARE_EXCHANGE, 0xaabbccdd, 9,
                              &observed, &exchanged) == A20_OK &&
                 observed == 0xaabbccdd && exchanged);
    CHECK_ATOMIC(a20_atomic32((uint32_t *)&g_futex_word, A20_ATOMIC32_EXCHANGE,
                              0, 0, &observed, &exchanged) == A20_OK &&
                 observed == 9 && !exchanged);
    CHECK_ATOMIC(a20_atomic32_load((void *)1, &observed) ==
                 -A20_ERR_INVALID_ARGUMENT);
    CHECK_ATOMIC(a20_atomic32_load((void *)0, &observed) == -A20_ERR_FAULT);
    CHECK_ATOMIC(a20_atomic32_load((void *)4096, &observed) == -A20_ERR_FAULT);
    CHECK_ATOMIC(a20_atomic32((uint32_t *)&g_futex_word, 99, 0, 0, &observed,
                              0) == -A20_ERR_INVALID_ARGUMENT);
    a20_atomic32_args_t bad = {.size = sizeof(bad),
                               .version = 1,
                               .addr = (uintptr_t)&g_futex_word,
                               .flags = 1};
    CHECK_ATOMIC(a20_syscall6(A20_SYS_atomic32, (uintptr_t)&bad, 0, 0, 0, 0,
                              0) == -A20_ERR_INVALID_ARGUMENT);
    bad.flags = 0;
    bad.size--;
    CHECK_ATOMIC(a20_syscall6(A20_SYS_atomic32, (uintptr_t)&bad, 0, 0, 0, 0,
                              0) == -A20_ERR_INVALID_ARGUMENT);
    uint64_t page;
    CHECK_ATOMIC(a20_vm_alloc_pages(UINT64_MAX / 4096 + 1, 3, &page) ==
                 -A20_ERR_INVALID_ARGUMENT);
    CHECK_ATOMIC(a20_vm_alloc_pages(1, 3, &page) == A20_OK);
    CHECK_ATOMIC(a20_atomic32_store((void *)(uintptr_t)page, 37) == A20_OK);
    CHECK_ATOMIC(a20_vm_protect(page, 4096, 1) == A20_OK);
    CHECK_ATOMIC(a20_atomic32_store((void *)(uintptr_t)page, 38) ==
                 -A20_ERR_FAULT);
    CHECK_ATOMIC(a20_atomic32_load((void *)(uintptr_t)page, &observed) ==
                     A20_OK &&
                 observed == 37);
    CHECK_ATOMIC(a20_vm_unmap(page, 4096) == A20_OK);
    CHECK_ATOMIC(a20_atomic32_load((void *)(uintptr_t)page, &observed) ==
                 -A20_ERR_FAULT);
    a20_hdl_write_buf(out, "NATIVE_ATOMIC32: PASS\n",
                      sizeof("NATIVE_ATOMIC32: PASS\n") - 1, (void *)0);
#undef CHECK_ATOMIC

    /* 1. Mismatch: *word != expected must not block. */
    store_word(&g_futex_word, 1);
    a20_status_t st = a20_futex_wait((uint32_t *)&g_futex_word, 0, A20_TIMEOUT_INFINITE);
    if (st != -A20_ERR_WOULD_BLOCK)
        return fail(out, 1, "mismatch did not return WOULDBLOCK", 35);

    /* 2. Timeout: matching value with finite timeout must time out. */
    store_word(&g_futex_word, 0);
    st = a20_futex_wait((uint32_t *)&g_futex_word, 0, 5ULL * 1000 * 1000);
    if (st != -A20_ERR_TIMED_OUT)
        return fail(out, 2, "finite timeout did not return TIMED_OUT", 39);

    /* 3. Cross-thread wake. */
    uint64_t stack;
    st = a20_vm_alloc_pages(16, 3 /* RW */, &stack);
    if (st != A20_OK)
        return fail(out, 3, "vm_alloc_pages failed", 21);

    a20_thread_create_args_t tc = {0};
    tc.entry = (uint64_t)(uintptr_t)worker_entry;
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
    store_word(&g_futex_word, 1);

    uint32_t woken = 0;
    st = a20_futex_wake((uint32_t *)&g_futex_word, 1, &woken);
    if (st != A20_OK)
        return fail(out, 5, "futex_wake failed", 18);

    /* Wait for the worker to observe the wakeup (bounded spin). */
    int ok = 0;
    for (int i = 0; i < 100000; i++) {
        if (load_word(&g_worker_woke)) {
            ok = 1;
            break;
        }
        if (load_word(&g_worker_err))
            break;
        a20_thread_yield();
    }
    if (!ok)
        return fail(out, 6, "worker did not wake", 20);

    a20_hdl_write_buf(out, "NATIVE_FUTEX: PASS\n", 19, (void *)0);
    return 0;
}
