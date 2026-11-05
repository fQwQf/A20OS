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
#define A20_NATIVE_FILE_TEST 1
#define A20_NATIVE_TASK_IDENTITY_BENCH 1
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "native-memory-contract.h"
#include "native-domain-contract.h"
#include "native-thread-contract.h"
#include "native-file-contract.h"
#include "native-spawn-contract.h"
#include "native-task-control-contract.h"
#include "native-compute-cancel-contract.h"

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

/* ---------------------------------------------------------------------- */
/* SMP: cross-core RT donation, multi-donor merge, chained PI, migration  */
/* ---------------------------------------------------------------------- */

static volatile uint32_t g_owner_ready;
static volatile uint32_t g_owner_stop;
static volatile uint32_t g_owner_park;
static volatile uint32_t g_owner_parked;
static volatile uint32_t g_owner_count;
static volatile uint32_t g_spin_count;
static volatile uint32_t g_spin_stop;
static volatile uint32_t g_low_go, g_low_ack;
static volatile uint32_t g_high_go, g_high_ack;
static volatile uint32_t g_chain_mid_go, g_chain_mid_ack;
static volatile uint32_t g_chain_undo_go, g_chain_undo_ack;
static volatile uint32_t g_chain_low_stop;
static volatile uint32_t g_pi_error;
static a20_handle_t g_pi_owner = A20_HANDLE_NULL;
static a20_handle_t g_chain_low = A20_HANDLE_NULL;

static int sched_read(a20_handle_t h, a20_sched_args_t *out);
static int sched_apply(a20_handle_t h, uint32_t flags, int32_t policy,
                       int32_t priority, uint64_t affinity);

/* A stuck coordinator must fail the contract, not hang the suite. */
#define PI_HANDSHAKE_NS (2ULL * 1000 * 1000 * 1000)

static int pi_flag_wait(volatile uint32_t *flag, uint32_t want)
{
    uint32_t value = load_word(flag);
    while (value != want) {
        a20_status_t st = a20_futex_wait((uint32_t *)flag, value,
                                         PI_HANDSHAKE_NS);
        if (st == -A20_ERR_TIMED_OUT)
            return -1;
        if (st != A20_OK && st != -A20_ERR_WOULD_BLOCK)
            return -1;
        value = load_word(flag);
    }
    return 0;
}

static void pi_flag_set(volatile uint32_t *flag, uint32_t value)
{
    store_word(flag, value);
    (void)a20_futex_wake((uint32_t *)flag, 0x7fffffff, NULL);
}

/* The owner is both the donation target and the scheduling-effect probe: it
 * counts loop iterations while READY, and can park so the coordinator may
 * migrate it across CPUs without fighting it for a CPU. */
static void pi_owner_entry(uint64_t arg)
{
    (void)arg;
    /* Pin itself: a running task may only migrate itself, not be migrated by
     * a coordinator that does not own its CPU. */
    if (sched_apply(A20_HANDLE_NULL, A20_SCHED_AFFINITY, 0, 0, 1)) {
        store_word(&g_pi_error, 41);
        a20_thread_exit(0);
    }
    /* Wake the coordinator: it may already be asleep waiting for readiness. */
    pi_flag_set(&g_owner_ready, 1);
    while (!load_word(&g_owner_stop)) {
        if (load_word(&g_owner_park)) {
            /* Wake the coordinator instead of making it poll out its timeout. */
            pi_flag_set(&g_owner_parked, 1);
            while (load_word(&g_owner_park)) {
                /* Sleep long: the coordinator migrates us while we are parked,
                 * so it must not catch a periodic 5 ms re-check window. */
                a20_status_t st =
                    a20_futex_wait((uint32_t *)&g_owner_park, 1,
                                   PI_HANDSHAKE_NS);
                if (st != A20_OK && st != -A20_ERR_WOULD_BLOCK) {
                    store_word(&g_pi_error, 35);
                    a20_thread_exit(0);
                }
            }
            store_word(&g_owner_parked, 0);
            continue;
        }
        g_owner_count++;
        (void)a20_thread_yield();
    }
    a20_thread_exit(0);
}

static void pi_spin_entry(uint64_t arg)
{
    (void)arg;
    if (sched_apply(A20_HANDLE_NULL, A20_SCHED_AFFINITY, 0, 0, 1)) {
        store_word(&g_pi_error, 42);
        a20_thread_exit(0);
    }
    while (!load_word(&g_spin_stop)) {
        g_spin_count++;     /* plain word: progress, not a synchronization */
        (void)a20_thread_yield();
    }
    a20_thread_exit(0);
}

/* Two donors with different RT priorities race the same owner.  Each uses
 * priority 0, so the kernel reads the donor's own effective RT priority. */
static void pi_donor_low_entry(uint64_t arg)
{
    (void)arg;
    if (sched_apply(A20_HANDLE_NULL, A20_SCHED_AFFINITY, 0, 0, 2)) {
        store_word(&g_pi_error, 43);
        a20_thread_exit(0);
    }
    if (pi_flag_wait(&g_low_go, 1)) {
        store_word(&g_pi_error, 31);
        a20_thread_exit(0);
    }
    for (unsigned i = 0; i < 64; i++) {
        if (sched_apply(g_pi_owner, A20_SCHED_DONATE, 0, 0, 0)) {
            store_word(&g_pi_error, 32);
            break;
        }
    }
    pi_flag_set(&g_low_ack, 1);
    a20_thread_exit(0);
}

static void pi_donor_high_entry(uint64_t arg)
{
    (void)arg;
    if (sched_apply(A20_HANDLE_NULL, A20_SCHED_AFFINITY, 0, 0, 2)) {
        store_word(&g_pi_error, 44);
        a20_thread_exit(0);
    }
    if (pi_flag_wait(&g_high_go, 1)) {
        store_word(&g_pi_error, 33);
        a20_thread_exit(0);
    }
    for (unsigned i = 0; i < 64; i++) {
        if (sched_apply(g_pi_owner, A20_SCHED_DONATE, 0, 0, 0)) {
            store_word(&g_pi_error, 34);
            break;
        }
    }
    pi_flag_set(&g_high_ack, 1);
    a20_thread_exit(0);
}

/* Chained donation: the coordinator boosts mid, then mid propagates its own
 * boosted effective priority to low with priority 0. */
static void pi_chain_mid_entry(uint64_t arg)
{
    (void)arg;
    if (sched_apply(A20_HANDLE_NULL, A20_SCHED_AFFINITY, 0, 0, 1)) {
        store_word(&g_pi_error, 45);
        a20_thread_exit(0);
    }
    if (pi_flag_wait(&g_chain_mid_go, 1)) {
        store_word(&g_pi_error, 36);
        a20_thread_exit(0);
    }
    if (sched_apply(g_chain_low, A20_SCHED_DONATE, 0, 0, 0))
        store_word(&g_pi_error, 37);
    pi_flag_set(&g_chain_mid_ack, 1);
    if (pi_flag_wait(&g_chain_undo_go, 1)) {
        store_word(&g_pi_error, 38);
        a20_thread_exit(0);
    }
    if (sched_apply(g_chain_low, A20_SCHED_UNDONATE, 0, 0, 0))
        store_word(&g_pi_error, 39);
    pi_flag_set(&g_chain_undo_ack, 1);
    a20_thread_exit(0);
}

static void pi_chain_low_entry(uint64_t arg)
{
    (void)arg;
    if (sched_apply(A20_HANDLE_NULL, A20_SCHED_AFFINITY, 0, 0, 1)) {
        store_word(&g_pi_error, 46);
        a20_thread_exit(0);
    }
    while (!load_word(&g_chain_low_stop)) {
        /* A periodic timeout is the idle poll; only real errors end the
         * thread.  Exiting here would invalidate the donation target. */
        a20_status_t st = a20_futex_wait((uint32_t *)&g_chain_low_stop, 0,
                                         5ULL * 1000 * 1000);
        if (st != A20_OK && st != -A20_ERR_WOULD_BLOCK &&
            st != -A20_ERR_TIMED_OUT)
            break;
    }
    a20_thread_exit(0);
}

static int sched_read(a20_handle_t h, a20_sched_args_t *out)
{
    for (unsigned i = 0; i < sizeof(*out); i++)
        ((volatile uint8_t *)out)[i] = 0;
    return a20_sched_get(h, out) == A20_OK ? 0 : -1;
}

static int sched_apply(a20_handle_t h, uint32_t flags, int32_t policy,
                       int32_t priority, uint64_t affinity)
{
    a20_sched_args_t s;
    for (unsigned i = 0; i < sizeof(s); i++)
        ((volatile uint8_t *)&s)[i] = 0;
    s.flags = flags;
    s.policy = policy;
    s.priority = priority;
    s.affinity = affinity;
    s.affinity_size = sizeof(uint64_t);
    return a20_sched_set(h, &s) == A20_OK ? 0 : -1;
}

static int native_smp_contract(a20_handle_t out, uint32_t cpus)
{
    if (cpus < 2) {
        a20_hdl_write_buf(out, "NATIVE_SMP_PI: SKIP cpus<2\n",
                          sizeof("NATIVE_SMP_PI: SKIP cpus<2\n") - 1, 0);
        return 0;
    }

    enum { PI_PAGES = 4, PI_BYTES = PI_PAGES * 4096 };
    uint64_t stacks[6] = {0};
    a20_handle_t h[6] = {0};
    for (unsigned i = 0; i < 6; i++)
        if (a20_vm_alloc_pages(PI_PAGES, 3, &stacks[i]) != A20_OK)
            return 1;

    /* Every coordination word starts clear before the first thread exists. */
    g_owner_ready = 0;
    g_owner_stop = 0;
    g_owner_park = 0;
    g_owner_parked = 0;
    g_owner_count = 0;
    g_spin_count = 0;
    g_spin_stop = 0;
    g_low_go = 0;
    g_low_ack = 0;
    g_high_go = 0;
    g_high_ack = 0;
    g_chain_mid_go = 0;
    g_chain_mid_ack = 0;
    g_chain_undo_go = 0;
    g_chain_undo_ack = 0;
    g_chain_low_stop = 0;
    g_pi_error = 0;

#define PI_CREATE(slot, fn, prio)                                              \
    do {                                                                       \
        a20_thread_create_args_t tc = {0};                                     \
        tc.entry = (uint64_t)(uintptr_t)(fn);                                  \
        tc.stack_base = (stacks[(slot)] + PI_BYTES) & ~15ULL;                 \
        tc.stack_size = PI_BYTES;                                              \
        tc.sched_flags = A20_SCHED_POLICY | A20_SCHED_PRIORITY;                \
        tc.sched_policy = A20_SCHED_FIFO;                                      \
        tc.sched_priority = (prio);                                            \
        if (a20_thread_create(&tc) < 0)                                        \
            return 2;                                                          \
        h[(slot)] = tc.out_thread;                                             \
    } while (0)

    /* The owner runs first and publishes readiness before the prio-30
     * spinner exists; otherwise it would be starved before its first store. */
    PI_CREATE(0, pi_owner_entry, 10);
    g_pi_owner = h[0];
    /* Wait for the owner's own pin so the spinner cannot race it onto CPU1. */
    if (pi_flag_wait(&g_owner_ready, 1))
        return 3;
    if (load_word(&g_pi_error))
        return (int)load_word(&g_pi_error);

    PI_CREATE(1, pi_spin_entry, 30);
    PI_CREATE(2, pi_donor_low_entry, 30);
    PI_CREATE(3, pi_donor_high_entry, 60);
    PI_CREATE(4, pi_chain_mid_entry, 20);
    PI_CREATE(5, pi_chain_low_entry, 10);
    g_chain_low = h[5];

    /* Every helper pins itself on entry.  The coordinator pins itself to
     * CPU1 so all donations and boosts cross cores. */
    if (sched_apply(A20_HANDLE_NULL,
                    A20_SCHED_POLICY | A20_SCHED_PRIORITY | A20_SCHED_AFFINITY,
                    A20_SCHED_FIFO, 50, 2))
        return 4;

    a20_time_t settle = {.secs = 0, .nsecs = 20 * 1000 * 1000};
    a20_thread_sleep(settle);

    a20_sched_args_t info;
    if (sched_read(h[0], &info) || info.affinity != 1 || info.priority != 10)
        return 6;

    /* Baseline: the prio-30 spinner owns CPU0, so the prio-10 owner makes no
     * progress at all despite being READY the whole window. */
    uint32_t starve_before = load_word(&g_owner_count);
    a20_thread_sleep(settle);
    uint32_t starve_delta = load_word(&g_owner_count) - starve_before;

    /* Two donors race 64 donations each with priority 0 ("my effective RT
     * priority").  The kernel must merge them to the maximum, not the last. */
    pi_flag_set(&g_low_go, 1);
    pi_flag_set(&g_high_go, 1);
    if (pi_flag_wait(&g_low_ack, 1) || pi_flag_wait(&g_high_ack, 1))
        return 7;
    if (load_word(&g_pi_error))
        return (int)load_word(&g_pi_error);
    if (sched_read(h[0], &info) || info.priority != 60 ||
        info.policy != A20_SCHED_FIFO || info.affinity != 1)
        return 8;

    /* The boost must be real scheduling state, not just a readable field:
     * the owner now outruns the spinner that previously starved it. */
    uint32_t boost_before = load_word(&g_owner_count);
    a20_thread_sleep(settle);
    uint32_t boost_delta = load_word(&g_owner_count) - boost_before;
    /* Every loop iteration is a yield syscall, so the absolute count is small;
     * the contract is that a starved task starts running, and keeps running
     * well above the baseline. */
    if (boost_delta < 50 || boost_delta <= starve_delta * 8U)
        return 9;

    /* Park, then migrate the boosted owner across cores.  A blocked task can
     * be re-keyed without the coordinator racing it for a CPU. */
    store_word(&g_owner_park, 1);
    if (pi_flag_wait(&g_owner_parked, 1))
        return 10;
    /* Let the owner actually enter the futex sleep: parked is published just
     * before the syscall, and a running task cannot be migrated by a peer. */
    a20_thread_sleep(settle);
    if (sched_read(h[0], &info) || info.priority != 60 || info.affinity != 1)
        return 11;
    if (sched_apply(h[0], A20_SCHED_AFFINITY, 0, 0, 2))
        return 12;
    if (sched_read(h[0], &info) || info.priority != 60 || info.affinity != 2)
        return 13;
    if (sched_apply(h[0], A20_SCHED_AFFINITY, 0, 0, 1))
        return 14;
    if (sched_read(h[0], &info) || info.priority != 60 || info.affinity != 1)
        return 15;

    if (sched_apply(h[0], A20_SCHED_UNDONATE, 0, 0, 0))
        return 16;
    if (sched_read(h[0], &info) || info.priority != 10)
        return 17;

    /* Back at base priority the spinner starves it again. */
    store_word(&g_owner_park, 0);
    (void)a20_futex_wake((uint32_t *)&g_owner_park, 0x7fffffff, NULL);
    a20_thread_sleep(settle);
    uint32_t back_before = load_word(&g_owner_count);
    a20_thread_sleep(settle);
    uint32_t back_delta = load_word(&g_owner_count) - back_before;
    if (back_delta >= boost_delta / 4U)
        return 18;

    /* The CMSIS scheduler lock is per-task: holding it on CPU1 must not stop
     * the CPU0 spinner.  This is the documented boundary, not a global RTOS
     * stop-the-world lock. */
    if (a20_sched_lock(A20_SCHED_LOCK_ACQUIRE, 0) < 0)
        return 19;
    uint32_t before = load_word(&g_spin_count);
    a20_thread_sleep(settle);
    uint32_t during = load_word(&g_spin_count);
    (void)a20_sched_lock(A20_SCHED_LOCK_RELEASE, 0);
    if (during == before)
        return 20;

    /* Release CPU0 before the chain phase: mid is prio 20, below the prio-30
     * spinner, so it could not run its donation while the spinner owns the
     * CPU and the coordinator would instead observe a timeout. */
    store_word(&g_spin_stop, 1);
    store_word(&g_owner_stop, 1);
    store_word(&g_owner_park, 0);
    (void)a20_futex_wake((uint32_t *)&g_owner_park, 0x7fffffff, NULL);
    a20_thread_sleep(settle);

    /* Chained donation: the coordinator boosts mid to 50, mid propagates its
     * own boosted value to low with priority 0. */
    if (sched_apply(h[4], A20_SCHED_DONATE, 0, 0, 0))
        return 21;
    if (sched_read(h[4], &info) || info.priority != 50)
        return 22;
    pi_flag_set(&g_chain_mid_go, 1);
    if (pi_flag_wait(&g_chain_mid_ack, 1))
        return 23;
    if (load_word(&g_pi_error))
        return (int)load_word(&g_pi_error);
    a20_sched_args_t low_info;
    if (sched_read(h[4], &info) || info.priority != 50 ||
        sched_read(h[5], &low_info) || low_info.priority != 50)
        return 24;
    if (sched_apply(h[4], A20_SCHED_UNDONATE, 0, 0, 0))
        return 25;
    if (sched_read(h[4], &info) || info.priority != 20)
        return 26;
    pi_flag_set(&g_chain_undo_go, 1);
    if (pi_flag_wait(&g_chain_undo_ack, 1))
        return 27;
    if (load_word(&g_pi_error))
        return (int)load_word(&g_pi_error);
    if (sched_read(h[5], &low_info) || low_info.priority != 10)
        return 28;

    store_word(&g_chain_low_stop, 1);
    (void)a20_futex_wake((uint32_t *)&g_chain_low_stop, 0x7fffffff, NULL);
    /* The helper threads are detached and exit on the stop flags.  Their
     * stacks stay mapped until this process exits; unmapping them here would
     * race a thread that has not yet been scheduled to observe the flag. */
    (void)sched_apply(A20_HANDLE_NULL,
                      A20_SCHED_POLICY | A20_SCHED_PRIORITY | A20_SCHED_AFFINITY,
                      A20_SCHED_NORMAL, 0, (1ULL << cpus) - 1);

    a20_hdl_write_buf(
        out, "NATIVE_SMP_PI: PASS donors=2 prio=60/10 chain=50 migrate=1 "
             "preempt=starved->runs lock=progress\n",
        sizeof("NATIVE_SMP_PI: PASS donors=2 prio=60/10 chain=50 migrate=1 "
               "preempt=starved->runs lock=progress\n") - 1,
        0);
    return 0;
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
    int smp_result = native_smp_contract(out, (uint32_t)domain.cpu_online);
    if (smp_result) {
        char error[] = "NATIVE_SMP_PI_ERROR: 00\n";
        error[sizeof(error) - 4] = '0' + smp_result / 10;
        error[sizeof(error) - 3] = '0' + smp_result % 10;
        a20_hdl_write_buf(out, error, sizeof(error) - 1, 0);
        return fail(out, 102, "smp pi", sizeof("smp pi") - 1);
    }

    if (native_runtime_contract())
        return fail(out, 96, "runtime identity",
                    sizeof("runtime identity") - 1);
    int compute_result = native_compute_cancel_contract();
    if (compute_result) {
        char error[] = "NATIVE_COMPUTE_CANCEL_ERROR: 00\n";
        error[sizeof(error) - 4] = '0' + compute_result / 10;
        error[sizeof(error) - 3] = '0' + compute_result % 10;
        a20_hdl_write_buf(out, error, sizeof(error) - 1, 0);
        return fail(out, 101, "compute cancel", sizeof("compute cancel") - 1);
    }
    a20_hdl_write_buf(
        out, "NATIVE_COMPUTE_CANCEL: PASS workers=2 exit=137 pure-user=1\n",
        sizeof("NATIVE_COMPUTE_CANCEL: PASS workers=2 exit=137 pure-user=1\n") -
            1,
        0);
    int file_result = native_file_contract(1);
    if (file_result) {
        char error[] = "NATIVE_FILE_ERROR: 00\n";
        error[sizeof(error) - 4] = '0' + file_result / 10;
        error[sizeof(error) - 3] = '0' + file_result % 10;
        a20_hdl_write_buf(out, error, sizeof(error) - 1, 0);
        return fail(out, 97, "file contract", sizeof("file contract") - 1);
    }
    a20_hdl_write_buf(
        out, "NATIVE_FILES: PASS bytes=2048 fnv=60f4edc5\n",
        sizeof("NATIVE_FILES: PASS bytes=2048 fnv=60f4edc5\n") - 1, 0);
    if (native_file_reopen(4100))
        return fail(out, 98, "file reopen", sizeof("file reopen") - 1);
    a20_hdl_write_buf(out, "NATIVE_FILES_REOPEN: PASS count=4100\n",
                      sizeof("NATIVE_FILES_REOPEN: PASS count=4100\n") - 1, 0);
    int spawn_result = native_spawn_contract();
    if (spawn_result) {
        char error[] = "NATIVE_SPAWN_ERROR: 00\n";
        error[sizeof(error) - 4] = '0' + spawn_result / 10;
        error[sizeof(error) - 3] = '0' + spawn_result % 10;
        a20_hdl_write_buf(out, error, sizeof(error) - 1, 0);
        return fail(out, 99, "file spawn", sizeof("file spawn") - 1);
    }
    int control_result = native_task_control_contract();
    if (control_result) {
        char error[] = "NATIVE_TASK_CONTROL_ERROR: 00\n";
        error[sizeof(error) - 4] = '0' + control_result / 10;
        error[sizeof(error) - 3] = '0' + control_result % 10;
        a20_hdl_write_buf(out, error, sizeof(error) - 1, 0);
        return fail(out, 100, "task control", sizeof("task control") - 1);
    }
    a20_hdl_write_buf(
        out, "NATIVE_TASK_CONTROL: PASS signal=12 masked=1 exit=137\n",
        sizeof("NATIVE_TASK_CONTROL: PASS signal=12 masked=1 exit=137\n") - 1,
        0);
    a20_hdl_write_buf(
        out, "NATIVE_SPAWN: PASS children=2 exit=37 args=4 env=2 work=32\n",
        sizeof("NATIVE_SPAWN: PASS children=2 exit=37 args=4 env=2 work=32\n") -
            1,
        0);
    int thread_result = native_thread_contract();
    if (thread_result) {
        char code[] = "NATIVE_THREAD_ERROR: 00 mask=00000000\n";
        unsigned error_offset = sizeof("NATIVE_THREAD_ERROR: ") - 1;
        unsigned mask_offset = sizeof("NATIVE_THREAD_ERROR: 00 mask=") - 1;
        code[error_offset] = '0' + thread_result / 10;
        code[error_offset + 1] = '0' + thread_result % 10;
        for (unsigned i = 0; i < 8; i++)
            code[mask_offset + i] =
                "0123456789abcdef"[(native_balance_mask() >> (28 - 4 * i)) &
                                   15];
        a20_hdl_write_buf(out, code, sizeof(code) - 1, 0);
        return fail(out, 95, "thread contract", sizeof("thread contract") - 1);
    }
    char balance[] = "NATIVE_BALANCE: PASS jobs=32 sum=4359168 mask=00000000 "
                     "range=dynamic\n";
    unsigned balance_offset =
        sizeof("NATIVE_BALANCE: PASS jobs=32 sum=4359168 mask=") - 1;
    for (unsigned i = 0; i < 8; i++)
        balance[balance_offset + i] =
            "0123456789abcdef"[(native_balance_mask() >> (28 - 4 * i)) & 15];
    a20_hdl_write_buf(out, balance, sizeof(balance) - 1, 0);
    a20_hdl_write_buf(out, "NATIVE_OBJECT_DIRECTORY: PASS\n",
                      sizeof("NATIVE_OBJECT_DIRECTORY: PASS\n") - 1, 0);
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
    int mem_result = native_memory_contract();
    if (mem_result) {
        char error[] = "NATIVE_ALLOCATOR_ERROR: 00\n";
        error[sizeof(error) - 4] = '0' + mem_result / 10;
        error[sizeof(error) - 3] = '0' + mem_result % 10;
        a20_hdl_write_buf(out, error, sizeof(error) - 1, 0);
        return fail(out, 93, "allocator contract",
                    sizeof("allocator contract") - 1);
    }
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
