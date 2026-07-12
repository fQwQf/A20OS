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
#include "core/timer.h"
#include "proc/signal.h"
#include "sys/syscall.h"
#include "sys/usercopy.h"

syscall_prof_t sys_prof[SYSCALL_PROFILE_MAX];
static uint64_t syscall_resched_counter;

static inline uint64_t syscall_profile_now(void)
{
#if CONFIG_SYSCALL_PROFILE
    return timer_get_ticks();
#else
    return 0;
#endif
}

static inline void syscall_profile_record(uint64_t num, uint64_t start, uint64_t end)
{
#if CONFIG_SYSCALL_PROFILE
    if (num >= SYSCALL_PROFILE_MAX)
        return;

    uint64_t elapsed = (end >= start) ? (end - start) : 0;
    syscall_prof_t *prof = &sys_prof[num];
    __atomic_fetch_add(&prof->count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&prof->cycles, elapsed, __ATOMIC_RELAXED);
#else
    (void)num;
    (void)start;
    (void)end;
#endif
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

#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    task_t *cur_task = proc_current();
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

        TRAP_CTX_SET_RET(ctx, ret);
        syscall_profile_record(num, start_time, syscall_profile_now());
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

    int64_t ret = -ENOSYS;
    int context_restored = 0;
    const linux_syscall_entry_t *entry = linux_syscall_lookup(args.nr);

    if (entry) {
        ret = entry->handler(&args);
        context_restored = entry->restores_context;

    } else {
        if (args.nr < 300)
            kdebug("[SYSCALL] Unimplemented: %lu\n", (unsigned long)args.nr);
    }

    int restart_syscall = 0;
    if (ret == -ERESTARTSYS) {
        task_t *cur = proc_current();
        int restart = 0;
        if (cur && cur->signals) {
            signal_state_t *ss = (signal_state_t *)cur->signals;
            uint64_t deliverable = (ss->pending | cur->thread_pending) &
                                   ~cur->sig_blocked;
            if (deliverable) {
                restart = 1;
                for (int sig = 1; sig < NSIG && restart; sig++) {
                    if (!(deliverable & signal_mask_bit(sig)))
                        continue;
                    sigaction_t *sa = &ss->actions[sig];
                    if (sa->sa_handler == SIG_IGN ||
                        sa->sa_handler == SIG_DFL)
                        continue;
                    if (!(sa->sa_flags & SA_RESTART))
                        restart = 0;
                }
            }
        }
        if (restart) {
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
    proc_check_exit_pending();
    signal_deliver_user(ctx);
    proc_check_exit_pending();
    if (!context_restored && proc_current() &&
        arch_syscall_resched_allowed() &&
        (++syscall_resched_counter & 0x1f) == 0)
        proc_yield();
    proc_check_exit_pending();
    if (args.nr == SYS_sigsuspend)
    {
        task_t *cur = proc_current();
        if (cur && cur->signals && cur->sigsuspend_active && !cur->sig_handling)
        {
            cur->sig_blocked = cur->sigsuspend_old_blocked;
            cur->sigsuspend_active = 0;
        }
    }
    return ret;
#endif
    return -ENOSYS;
}
