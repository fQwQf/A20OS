/* Shared Native clock entry.
 *
 * Kept separate from the full sys_core.c so an MCU profile can link exactly
 * one time syscall without dragging exec, VFS and signal code in with it.
 */
#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "core/types.h"
#include "core/timekeeping.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "sys/usercopy.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"

int64_t sys_a20_clock_get(const a20_syscall_args_t *args)
{
    uint32_t clock_id = (uint32_t)args->arg[0];
    a20_time_ns_t *out = (a20_time_ns_t *)(uintptr_t)args->arg[1];
    if (!out)
        return -A20_ERR_FAULT;

    /* Raw free-running cycle counter for the latency experiments.  Clock id 7
     * returns cycles and id 8 returns the counter frequency; both reuse the
     * existing clock_get syscall so no profile grows.  On AArch64 the
     * architectural physical counter is the scheduler timebase. */
    if (clock_id == 7) {
#if defined(CONFIG_AARCH64)
        uint64_t cycles = timer_get_ticks();
#elif defined(CONFIG_MCU)
        uint64_t cycles = arch_read_cycle();
#else
        uint64_t cycles = 0;
#endif
        if (copy_to_user(out, &cycles, sizeof(cycles)) < 0)
            return -A20_ERR_FAULT;
        return A20_OK;
    }
    if (clock_id == 8) {
#if defined(CONFIG_AARCH64)
        uint64_t hz = (uint64_t)TICKS_PER_SEC;
#elif defined(A20_CPU_HZ)
        uint64_t hz = A20_CPU_HZ;
#else
        uint64_t hz = 1;
#endif
        if (copy_to_user(out, &hz, sizeof(hz)) < 0)
            return -A20_ERR_FAULT;
        return A20_OK;
    }

    /* Timer IRQ entry-latency statistics (cycles).  Ids 9..12 are min, avg,
     * max and sample count; unsupported arches report NOT_SUPPORTED rather
     * than fabricating a value. */
    if (clock_id >= 9 && clock_id <= 12) {
#ifdef CONFIG_AARCH64
        uint64_t mn = 0, avg = 0, mx = 0, cnt = 0;
        timer_irq_latency_read(&mn, &avg, &mx, &cnt);
        uint64_t value = clock_id == 9 ? mn :
                         clock_id == 10 ? avg :
                         clock_id == 11 ? mx : cnt;
        if (copy_to_user(out, &value, sizeof(value)) < 0)
            return -A20_ERR_FAULT;
        return A20_OK;
#else
        return -A20_ERR_NOT_SUPPORTED;
#endif
    }

    uint64_t ts[2]; /* [seconds, nanoseconds] */
    switch (clock_id) {
    case 0: /* CLOCK_REALTIME */
    case 5: /* CLOCK_REALTIME_COARSE */
        timekeeping_get_realtime(ts);
        break;
    case 1: /* CLOCK_MONOTONIC */
    case 4: /* CLOCK_MONOTONIC_RAW */
    case 6: /* CLOCK_MONOTONIC_COARSE */
        timekeeping_get_monotonic(ts);
        break;
    default:
        return -A20_ERR_INVALID_ARGUMENT;
    }

    a20_time_ns_t ns = ts[0] * 1000000000ULL + ts[1];
    if (copy_to_user(out, &ns, sizeof(ns)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

/*
 * Low-power idle used by CMSIS osKernelSuspend().  On a single-core MCU the
 * per-task scheduler lock is a real global scheduler stop: timer and UART
 * IRQs still run, but the IRQ-return safe point refuses to switch tasks while
 * the depth is non-zero.  The timer is rearmed one-shot for the next kernel
 * deadline and the core enters WFI.  SMP returns NOT_SUPPORTED because this
 * would stop only one CPU while peers kept running user threads.
 */
int64_t sys_a20_kernel_idle(const a20_syscall_args_t *args)
{
    (void)args;
#if defined(CONFIG_MCU) && CONFIG_NR_CPUS == 1
    task_t *cur = proc_current();
    if (!cur)
        return -A20_ERR_INVALID_ARGUMENT;
    uint64_t start = timer_get_ticks();
    (void)proc_sched_lock_set(PROC_SCHED_LOCK_ACQUIRE, 0);
    uint64_t interval = proc_next_timer_interval(start);
    if (!interval)
        interval = 1;
    timer_set_interval(interval);
    arch_local_irq_enable();
    while (timer_get_ticks() < start + interval) {
        arch_wfi();
    }
    arch_local_irq_disable();
    (void)proc_sched_lock_set(PROC_SCHED_LOCK_RELEASE, 0);
    return (int64_t)(timer_get_ticks() - start);
#else
    return -A20_ERR_NOT_SUPPORTED;
#endif
}
#endif
