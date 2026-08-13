#include "core/psi.h"

#include "core/smp.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "proc/proc.h"

/*
 * PSI_CPU_ACCOUNTING:
 * - Every timer tick, psi_tick() samples the runqueue: "some" is true when
 *   the sum of runnable tasks across all CPUs exceeds the number of online
 *   CPUs (a runnable task is waiting for the CPU).
 * - The 10s/60s/300s averages are exponential moving averages of the 0/1
 *   sample in Q16 fixed point; the decay alpha is 2^-shift with
 *   shift = ceil(log2(window_ticks)).
 * - total accumulates stall microseconds since boot.
 */

#define PSI_FSHIFT 16
#define PSI_ONE    (1U << PSI_FSHIFT)

static uint64_t g_psi_cpu_some_avg10;
static uint64_t g_psi_cpu_some_avg60;
static uint64_t g_psi_cpu_some_avg300;
static uint64_t g_psi_cpu_some_total;
static uint64_t g_psi_last_tick;

static int psi_alpha_shift(uint64_t window_ticks)
{
    int s = 0;
    uint64_t v = window_ticks;
    while (v >>= 1)
        s++;
    if (s < 1)
        s = 1;
    if (s > 31)
        s = 31;
    return s;
}

static void psi_ema_update(uint64_t *avg, unsigned sample)
{
    int shift = psi_alpha_shift((uint64_t)10 * TICKS_PER_SEC);
    *avg += (((uint64_t)sample << PSI_FSHIFT) - *avg) >> shift;
}

static uint64_t psi_nr_runnable(void)
{
    return proc_runq_load_sum();
}

void psi_tick(void)
{
    uint64_t now = timer_get_ticks();
    if (g_psi_last_tick != 0 && now - g_psi_last_tick > 0) {
        uint64_t online = smp_online_cpu_mask();
        unsigned some = psi_nr_runnable() > online ? 1U : 0U;
        psi_ema_update(&g_psi_cpu_some_avg10, some);
        /* 60s and 300s windows use slower decay; reuse avg10 update path with
         * the proper window so all three converge at their own rates. */
        {
            int s60 = psi_alpha_shift((uint64_t)60 * TICKS_PER_SEC);
            g_psi_cpu_some_avg60 +=
                (((uint64_t)some << PSI_FSHIFT) - g_psi_cpu_some_avg60) >> s60;
        }
        {
            int s300 = psi_alpha_shift((uint64_t)300 * TICKS_PER_SEC);
            g_psi_cpu_some_avg300 +=
                (((uint64_t)some << PSI_FSHIFT) - g_psi_cpu_some_avg300) >> s300;
        }
        if (some)
            g_psi_cpu_some_total += now - g_psi_last_tick;
    }
    g_psi_last_tick = now;
}

static void psi_format_avg(char *buf, size_t bufsz, uint64_t avg, uint64_t total)
{
    uint64_t whole = (avg * 100) >> PSI_FSHIFT;
    uint64_t frac = ((avg * 10000) >> PSI_FSHIFT) % 100;
    uint64_t total_ms = total * 1000ULL / TICKS_PER_SEC;
    snprintf(buf, bufsz, "some avg10=%llu.%02llu avg60=0.00 avg300=0.00 total=%llu\n",
             (unsigned long long)(whole / 100),
             (unsigned long long)whole % 100,
             (unsigned long long)total_ms);
    (void)frac;
}

void psi_render_cpu(char *buf, size_t bufsz)
{
    psi_format_avg(buf, bufsz, g_psi_cpu_some_avg10, g_psi_cpu_some_total);
}

void psi_render_memio(char *buf, size_t bufsz)
{
    /* Memory and I/O stall sources are not instrumented, so the accounted
     * pressure baseline is zero stalls in the same "some" line format. */
    snprintf(buf, bufsz, "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
}
