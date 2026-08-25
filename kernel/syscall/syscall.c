/*
 * A20OS syscall dispatcher.
 *
 * Keep ABI dispatch here; concrete syscall implementations live in sys_*.c.
 */
#include "syscall_internal.h"
#include "abi/syscall_entry.h"
#include "abi/current.h"
#include "arch/syscall_hook.h"
#include "core/klog.h"
#include "core/perf.h"
#include "core/timer.h"
#include "proc/signal.h"
#include "proc/debug.h"
#include "ext/kep.h"
#include "sys/syscall.h"
#include "sys/usercopy.h"

syscall_prof_t sys_prof[SYSCALL_PROFILE_MAX];

static inline uint64_t syscall_profile_now(void)
{
    return __atomic_load_n(&g_a20_perf_enabled, __ATOMIC_RELAXED) ?
        timer_get_ticks() : 0;
}

static inline void syscall_profile_record(uint64_t num, uint64_t start, uint64_t end)
{
    if (!__atomic_load_n(&g_a20_perf_enabled, __ATOMIC_RELAXED))
        return;
    if (num >= SYSCALL_PROFILE_MAX)
        return;

    uint64_t elapsed = (end >= start) ? (end - start) : 0;
    syscall_prof_t *prof = &sys_prof[num];
    __atomic_fetch_add(&prof->count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&prof->cycles, elapsed, __ATOMIC_RELAXED);
}

void syscall_profile_reset(void)
{
    memset(sys_prof, 0, sizeof(sys_prof));
}

void syscall_init(void)
{
    syscall_profile_reset();
    kdebug("[SYSCALL] Initialized\n");
}

int64_t syscall_dispatch(trap_context_t *ctx)
{
    uint64_t num = TRAP_CTX_SYSCALL_NUM(ctx);
    uint64_t start_time = syscall_profile_now();
    task_t *cur_task = proc_current();

    /*
     * KEP syscall filter: attached programs may deny or kill the caller
     * before any ABI handling runs.  A denied syscall returns -EACCES
     * (Linux) / -A20_ERR_ACCESS (Native).
     */
    if (__builtin_expect(kep_syscall_filter_active(), 0)) {
        uint64_t args[KEP_SCF_ARGS] = {
            TRAP_CTX_ARG0(ctx), TRAP_CTX_ARG1(ctx), TRAP_CTX_ARG2(ctx),
            TRAP_CTX_ARG3(ctx), TRAP_CTX_ARG4(ctx), TRAP_CTX_ARG5(ctx),
        };
        int abi = (cur_task && cur_task->abi_mode) ? 1 : 0;
        int verdict = kep_syscall_filter_check(num, args, abi);
        if (verdict == KEP_SCF_KILL) {
            proc_exit_group(-SIGKILL);
        } else if (verdict != KEP_SCF_ALLOW) {
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
            int64_t denied = abi ? -A20_ERR_ACCESS : -EACCES;
#else
            int64_t denied = -EACCES;
#endif
            TRAP_CTX_SET_RET(ctx, denied);
            return denied;
        }
    }

#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    int is_native = cur_task && cur_task->abi_mode;

    if (is_native) {
        a20_syscall_args_t a20_args = {
            .nr = num,
            .arg = {
                TRAP_CTX_ARG0(ctx),
                TRAP_CTX_ARG1(ctx),
                TRAP_CTX_ARG2(ctx),
                TRAP_CTX_ARG3(ctx),
                TRAP_CTX_ARG4(ctx),
                TRAP_CTX_ARG5(ctx),
            },
            .ctx = ctx,
        };

        int64_t ret = -A20_ERR_NOT_SUPPORTED;
        const a20_syscall_entry_t *entry = a20_syscall_lookup(num);
        if (entry) {
            ret = entry->handler(&a20_args);
        } else {
            printf("[A20] UNHANDLED syscall: 0x%lx\n", (unsigned long)num);
        }

        /*
         * execve replaces the whole user context (including a0, which the
         * native startup protocol uses to pass a20_start_info_t) inside
         * proc_exec; writing the return value back to a0 would clobber it.
         * Mirror the Linux path's restores_context handling.
         */
        if (!(num == A20_SYS_execve && ret >= 0))
            TRAP_CTX_SET_RET(ctx, ret);
        syscall_profile_record(num, start_time, syscall_profile_now());
        /*
         * Native tasks use checkpoint-style signal delivery for their own
         * signal API, but core kernel signals (SIGSTOP from a debugger
         * attach, SIGKILL, ptrace delivery) queue through the shared signal
         * state and must reach the delivery boundary here, exactly like the
         * Linux ABI path below.
         */
        signal_deliver_user(ctx);
        return ret;
    }
#endif

#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    linux_syscall_args_t args = {
        .nr = num,
        .arg = {
            TRAP_CTX_ARG0(ctx),
            TRAP_CTX_ARG1(ctx),
            TRAP_CTX_ARG2(ctx),
            TRAP_CTX_ARG3(ctx),
            TRAP_CTX_ARG4(ctx),
            TRAP_CTX_ARG5(ctx),
        },
        .ctx = ctx,
    };

    arch_syscall_adjust_args(&args);
    num = args.nr;

    /*
     * PT_DEBUG_SYSCALL_STOPS: a tracee resumed in syscall-stop mode stops
     * before the syscall executes (entry stop; on resume the arch layer
     * rewinds the saved EPC so the syscall runs) and again after it
     * completes (exit stop, result visible in the registers).  The ptrace
     * syscall itself is never stopped to avoid observer recursion.
     */
    if (num != SYS_ptrace && proc_debug_is_traced(cur_task))
        proc_debug_syscall_entry(ctx);

    int64_t ret = -ENOSYS;
    int context_restored = 0;
    const linux_syscall_entry_t *entry = linux_syscall_lookup(args.nr);

    /*
     * An intervening dispatch proves execution resumed past the interrupted
     * syscall, so its pending SYS_restart_syscall replay is stale.  The
     * replay itself must not clear the block before re-running the handler.
     */
    if (num != SYS_restart_syscall && cur_task && cur_task->restart_active)
        cur_task->restart_active = 0;

    if (entry) {
        ret = entry->handler(&args);
        context_restored = entry->restores_context;
        if (context_restored)
            ARCH_TRAP_FAST_RETURN_DISARM(ctx);

    } else {
        if (args.nr < 300)
            kdebug("[SYSCALL] Unimplemented: %lu\n", (unsigned long)args.nr);
    }

    int restart_syscall = 0;
    if (ret == -ERESTARTSYS) {
        task_t *cur = proc_current();
        int restart = signal_task_should_restart(cur);
        if (restart) {
            cur->restart_active = 1;
            cur->restart_nr = args.nr;
            for (int i = 0; i < 6; i++)
                cur->restart_args[i] = args.arg[i];
            TRAP_CTX_EPC(ctx) -= 4;
            restart_syscall = 1;
        } else {
            ret = -EINTR;
        }
    }

    if (!context_restored || ret < 0) {
        if (!restart_syscall)
            TRAP_CTX_SET_RET(ctx, ret);
        context_restored = 0;
    }
    syscall_profile_record(num, start_time, syscall_profile_now());
    if (num != SYS_ptrace && proc_debug_is_traced(cur_task))
        proc_debug_syscall_exit(ctx);
    signal_deliver_user(ctx);
    /* Timer/IPI-driven reschedule requests are consumed by trap_handler's
     * common safe point.  Forcing a new request every 32 syscalls created
     * thousands of same-task scheduler round trips in syscall-heavy builds. */
    if (args.nr == SYS_sigsuspend)
        signal_task_restore_sigsuspend(proc_current());
    return ret;
#endif
    return -ENOSYS;
}
