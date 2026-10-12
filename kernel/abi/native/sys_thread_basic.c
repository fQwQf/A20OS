/* Shared Native thread syscalls: server and shared-memory MCU use the same
 * scheduling, timeout and copy-out implementation. */
#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "core/types.h"
#include "core/timer.h"
#include "core/cpu.h"
#include "proc/proc.h"
#include "sys/usercopy.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#define A20_ARG(n) (args->arg[(n)])

int64_t sys_a20_thread_sleep(const a20_syscall_args_t *args) {
    uint64_t duration_ns = A20_ARG(0);
    if (duration_ns == 0) {
        proc_yield();
        return A20_OK;
    }

    /* Interruptible park-based sleep: a task_kill wakes the sleeper with
     * PROC_WAKE_SIGNAL, so the sleep returns -A20_ERR_INTERRUPTED and the
     * caller can dispatch signals at this checkpoint.  The wait-timer heap
     * fires the deadline on timeout. */
    uint64_t sec = duration_ns / 1000000000ULL;
    uint64_t nsec = duration_ns % 1000000000ULL;
    uint64_t ticks = sec * TICKS_PER_SEC + nsec * TICKS_PER_SEC / 1000000000ULL;
    if (ticks == 0)
        ticks = 1;
    uint64_t now = timer_get_ticks();
    uint64_t deadline = now + ticks;
    if (deadline < now)
        deadline = UINT64_MAX / 2; /* overflow: cap far future */

    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
    if (!token.task) {
        proc_yield();
        return A20_OK;
    }
    proc_wake_reason_t reason = proc_park_commit(token);
    proc_park_finish(token);
    if (proc_wake_reason_is_task_interrupt(reason))
        return -A20_ERR_INTERRUPTED;
    return A20_OK;
}

int64_t sys_a20_thread_yield(const a20_syscall_args_t *args) {
    (void)args;
    proc_yield();
    return A20_OK;
}

int64_t sys_a20_thread_get_cpu(const a20_syscall_args_t *args) {
    uint32_t *out = (uint32_t *)(uintptr_t)A20_ARG(0);
    uint32_t cpu = cpu_current_id();
    if (!out || copy_to_user(out, &cpu, sizeof(cpu)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_thread_exit(const a20_syscall_args_t *args) {
    int32_t code = (int32_t)A20_ARG(0);
    proc_exit(code);
    return 0;
}
#endif
