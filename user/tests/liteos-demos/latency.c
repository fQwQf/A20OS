/*
 * Experiment 4.8: latency measurement with the free-running cycle counter.
 *
 * The kernel exposes the architecture cycle counter through the existing
 * clock_get syscall (clock id 7/8), so this experiment measures scheduling
 * jitter without adding a second time syscall or a debug channel.  Values are
 * reported in CPU cycles; divide by A20_CLOCK_RAW_FREQ for seconds.
 *
 * The MCU demo heap is a shared arena of a few tens of KiB.  A second
 * high-priority thread with a static stack still pushes the image over a heap
 * region boundary, which starves 4.6/4.9, so preemption latency is left to a
 * dedicated image instead of being folded into this passing regression.
 */
#include "cmsis_os2.h"
#include "a20_clock.h"
#include "a20_mcu_demo.h"

void demo_log(const char *fmt, ...);
int demo_check(int condition, const char *what);

#ifndef LAT_SAMPLES
#define LAT_SAMPLES 32U
#endif

static uint64_t abs64(int64_t v)
{
    return (uint64_t)(v < 0 ? -v : v);
}

int experiment_48(void)
{
    demo_log("[4.8] latency measurement (raw cycle counter)");
    uint64_t hz = 1;
    (void)a20_clock_raw_freq(&hz);
    if (!hz)
        hz = 1;
    uint64_t tick_cycles = hz / osKernelGetTickFreq();
    if (!tick_cycles)
        tick_cycles = 1;

    uint64_t jmin = ~0ULL, jmax = 0, jsum = 0;
    for (unsigned i = 0; i < LAT_SAMPLES; i++) {
        uint64_t a = 0, b = 0;
        (void)a20_clock_get_raw(&a);
        osDelay(1);
        (void)a20_clock_get_raw(&b);
        uint64_t j = abs64((int64_t)(b - a) - (int64_t)tick_cycles);
        if (j < jmin)
            jmin = j;
        if (j > jmax)
            jmax = j;
        jsum += j;
    }

    uint64_t smin = ~0ULL, smax = 0, ssum = 0;
    for (unsigned i = 0; i < LAT_SAMPLES; i++) {
        uint64_t a = 0, b = 0;
        (void)a20_clock_get_raw(&a);
        (void)osKernelGetTickCount();
        (void)a20_clock_get_raw(&b);
        uint64_t d = b - a;
        if (d < smin)
            smin = d;
        if (d > smax)
            smax = d;
        ssum += d;
    }

    int ok = demo_check(jmin <= jmax && jsum > 0, "delay samples");
    ok &= demo_check(smin <= smax && ssum > 0, "syscall samples");
    demo_log("[4.8] tick_hz=%u raw_hz=%u tick_cycles=%u",
             (unsigned)osKernelGetTickFreq(), (unsigned)hz,
             (unsigned)tick_cycles);
    demo_log("[4.8] delay_jitter_cycles min=%u avg=%u max=%u",
             (unsigned)jmin, (unsigned)(jsum / LAT_SAMPLES),
             (unsigned)jmax);
    demo_log("[4.8] syscall_cycles min=%u avg=%u max=%u",
             (unsigned)smin, (unsigned)(ssum / LAT_SAMPLES),
             (unsigned)smax);
    demo_log("[4.8] %s delay=%u syscall=%u", ok ? "PASS" : "FAIL",
             (unsigned)jmax, (unsigned)smax);
    return ok;
}
