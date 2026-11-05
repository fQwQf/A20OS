/*
 * Independent RT latency probe.
 *
 * Kept out of the CMSIS MCU demo image because that image sits on a heap
 * region boundary: adding a second RT thread there pushes experiments 4.6 and
 * 4.9 over the edge.  This program runs on a server target instead and
 * reports two separate numbers:
 *
 *   1. wake-to-run: cycles from the coordinator's futex_wake to the first
 *      user instruction of a higher-priority RT thread on the same CPU,
 *      including the kernel wake path and the preemption switch.
 *   2. timer IRQ entry: cycles between the compare value that fired and the
 *      first instruction of the kernel timer handler, collected by the
 *      architecture timer path.  This does not include device IRQs.
 *
 * Neither number is a WCET proof; they are observed distributions on the
 * running machine.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

#ifndef LAT_SAMPLES
#define LAT_SAMPLES 200U
#endif

#define LAT_HANDSHAKE_NS (2ULL * 1000 * 1000 * 1000)

static volatile uint32_t g_go;
static volatile uint32_t g_ack;
static volatile uint32_t g_ready;
static volatile uint32_t g_stop;
static volatile uint32_t g_error;
static volatile uint64_t g_wake_cycles;

static uint32_t lat_load(volatile uint32_t *word)
{
    uint32_t value = 0;
    if (a20_atomic32_load((uint32_t *)word, &value) != A20_OK)
        a20_thread_exit(91);
    return value;
}

static void lat_store(volatile uint32_t *word, uint32_t value)
{
    if (a20_atomic32_store((uint32_t *)word, value) != A20_OK)
        a20_thread_exit(92);
}

static void lat_set(volatile uint32_t *word, uint32_t value)
{
    lat_store(word, value);
    (void)a20_futex_wake((uint32_t *)word, 0x7fffffff, NULL);
}

static int lat_sched_apply(a20_handle_t h, uint32_t flags, int32_t policy,
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

static void latency_hi_entry(uint64_t arg)
{
    (void)arg;
    if (lat_sched_apply(A20_HANDLE_NULL, A20_SCHED_AFFINITY, 0, 0, 1)) {
        lat_store(&g_error, 1);
        a20_thread_exit(0);
    }
    uint32_t seq = 0;
    lat_set(&g_ready, 1);
    while (!lat_load(&g_stop)) {
        uint32_t value = lat_load(&g_go);
        while (value == seq && !lat_load(&g_stop)) {
            a20_status_t st =
                a20_futex_wait((uint32_t *)&g_go, seq, LAT_HANDSHAKE_NS);
            if (st == -A20_ERR_TIMED_OUT) {
                value = lat_load(&g_go);
                if (value == seq)
                    continue;
                break;
            }
            if (st != A20_OK && st != -A20_ERR_WOULD_BLOCK) {
                lat_store(&g_error, 2);
                a20_thread_exit(0);
            }
            value = lat_load(&g_go);
        }
        if (lat_load(&g_stop))
            break;
        seq = value;
        uint64_t now = 0;
        (void)a20_clock_get_raw(&now);
        g_wake_cycles = now;
        lat_set(&g_ack, seq);
    }
    a20_thread_exit(0);
}

static size_t append_text(char *out, size_t cap, size_t at, const char *text)
{
    while (*text) {
        if (at >= cap)
            return cap + 1;
        out[at++] = *text++;
    }
    return at;
}

static size_t append_u64(char *out, size_t cap, size_t at, uint64_t value)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) {
        if (at >= cap)
            return cap + 1;
        out[at++] = digits[--n];
    }
    return at;
}

static void print_stats(a20_handle_t out, const char *tag, const char *unit,
                        uint64_t min, uint64_t avg, uint64_t max,
                        uint64_t count)
{
    char line[192];
    size_t at = 0;
    at = append_text(line, sizeof(line), at, "NATIVE_LATENCY: ");
    at = append_text(line, sizeof(line), at, tag);
    at = append_text(line, sizeof(line), at, " min=");
    at = append_u64(line, sizeof(line), at, min);
    at = append_text(line, sizeof(line), at, " avg=");
    at = append_u64(line, sizeof(line), at, avg);
    at = append_text(line, sizeof(line), at, " max=");
    at = append_u64(line, sizeof(line), at, max);
    at = append_text(line, sizeof(line), at, " count=");
    at = append_u64(line, sizeof(line), at, count);
    at = append_text(line, sizeof(line), at, " ");
    at = append_text(line, sizeof(line), at, unit);
    at = append_text(line, sizeof(line), at, "\n");
    if (at <= sizeof(line))
        a20_hdl_write_buf(out, line, (uint32_t)at, 0);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    a20_handle_t out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (out == A20_HANDLE_NULL)
        return 90;

    uint64_t hz = 0;
    (void)a20_clock_raw_freq(&hz);
    if (!hz)
        hz = 1;

    uint64_t stack = 0;
    if (a20_vm_alloc_pages(4, 3, &stack) != A20_OK)
        return 1;

    g_go = 0;
    g_ack = 0;
    g_ready = 0;
    g_stop = 0;
    g_error = 0;
    g_wake_cycles = 0;

    a20_thread_create_args_t tc = {0};
    tc.entry = (uint64_t)(uintptr_t)latency_hi_entry;
    tc.stack_base = (stack + 4 * 4096) & ~15ULL;
    tc.stack_size = 4 * 4096;
    tc.sched_flags = A20_SCHED_POLICY | A20_SCHED_PRIORITY;
    tc.sched_policy = A20_SCHED_FIFO;
    tc.sched_priority = 80;
    if (a20_thread_create(&tc) < 0)
        return 2;

    /* Coordinator is RT but strictly below the probe thread, pinned to the
     * same CPU so every wake is a real priority preemption. */
    if (lat_sched_apply(A20_HANDLE_NULL,
                        A20_SCHED_POLICY | A20_SCHED_PRIORITY |
                            A20_SCHED_AFFINITY,
                        A20_SCHED_FIFO, 50, 1))
        return 3;

    for (unsigned i = 0; i < 2000 && !lat_load(&g_ready); i++) {
        a20_time_t pause = {.secs = 0, .nsecs = 1000 * 1000};
        a20_thread_sleep(pause);
    }
    if (!lat_load(&g_ready) || lat_load(&g_error))
        return 4;

    uint64_t min = ~0ULL, max = 0, sum = 0;
    unsigned valid = 0;
    for (unsigned i = 0; i < LAT_SAMPLES; i++) {
        uint64_t before = 0;
        (void)a20_clock_get_raw(&before);
        uint32_t seq = i + 1;
        lat_store(&g_go, seq);
        (void)a20_futex_wake((uint32_t *)&g_go, 0x7fffffff, NULL);

        uint32_t acked = lat_load(&g_ack);
        while (acked != seq) {
            a20_status_t st =
                a20_futex_wait((uint32_t *)&g_ack, acked, LAT_HANDSHAKE_NS);
            if (st == -A20_ERR_TIMED_OUT)
                return 5;
            if (st != A20_OK && st != -A20_ERR_WOULD_BLOCK)
                return 6;
            acked = lat_load(&g_ack);
        }
        uint64_t after = g_wake_cycles;
        if (after <= before)
            continue;   /* counter wrap or a missed sample: not a measurement */
        uint64_t delta = after - before;
        if (delta > hz)     /* > 1 s: an unrelated stall, not a wake path */
            continue;
        if (delta < min)
            min = delta;
        if (delta > max)
            max = delta;
        sum += delta;
        valid++;
    }
    if (!valid)
        return 7;

    uint64_t wake_avg = sum / valid;

    uint64_t irq_min = 0, irq_avg = 0, irq_max = 0, irq_count = 0;
    (void)a20_clock_timer_irq(A20_CLOCK_TIMER_IRQ_MIN, &irq_min);
    (void)a20_clock_timer_irq(A20_CLOCK_TIMER_IRQ_AVG, &irq_avg);
    (void)a20_clock_timer_irq(A20_CLOCK_TIMER_IRQ_MAX, &irq_max);
    a20_status_t irq_status =
        a20_clock_timer_irq(A20_CLOCK_TIMER_IRQ_COUNT, &irq_count);

    print_stats(out, "wake", "cycles", min, wake_avg, max, valid);
    print_stats(out, "timer_irq", "cycles", irq_min, irq_avg, irq_max,
                irq_count);

    int irq_supported = irq_status != -A20_ERR_NOT_SUPPORTED;
    char line[160];
    size_t at = 0;
    at = append_text(line, sizeof(line), at, "NATIVE_LATENCY: ");
    at = append_text(line, sizeof(line), at,
                     !irq_supported ? "PARTIAL" :
                     irq_count ? "PASS" : "FAIL");
    at = append_text(line, sizeof(line), at, " hz=");
    at = append_u64(line, sizeof(line), at, hz);
    at = append_text(line, sizeof(line), at, " wake_avg_ns=");
    at = append_u64(line, sizeof(line), at,
                    wake_avg > UINT64_MAX / 1000000000ULL
                        ? 0
                        : wake_avg * 1000000000ULL / hz);
    at = append_text(line, sizeof(line), at, " timer_irq_samples=");
    at = append_u64(line, sizeof(line), at, irq_count);
    at = append_text(line, sizeof(line), at, "\n");
    if (at <= sizeof(line))
        a20_hdl_write_buf(out, line, (uint32_t)at, 0);

    lat_set(&g_stop, 1);
    lat_store(&g_go, lat_load(&g_go) + 1);
    (void)a20_futex_wake((uint32_t *)&g_go, 0x7fffffff, NULL);
    a20_time_t settle = {.secs = 0, .nsecs = 20 * 1000 * 1000};
    a20_thread_sleep(settle);

    (void)lat_sched_apply(A20_HANDLE_NULL,
                          A20_SCHED_POLICY | A20_SCHED_PRIORITY |
                              A20_SCHED_AFFINITY,
                          A20_SCHED_NORMAL, 0, 0xffffffffULL);
    if (!irq_supported)
        return 0;       /* the raw wake measurement is still valid */
    return irq_count ? 0 : 8;
}
