/*
 * A20OS syscall dispatcher.
 *
 * Keep ABI dispatch here; concrete syscall implementations live in sys_*.c.
 */
#include "syscall_internal.h"
#include "abi/syscall_entry.h"
#include "abi/current.h"
#include "core/klog.h"
#include "core/timer.h"
#include "proc/signal.h"
#include "sys/syscall.h"
#ifdef CONFIG_X86_64
#include "arch/x86_64/include/syscall_nr_x86_64.h"
#endif

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
    uint64_t orig_num = TRAP_CTX_SYSCALL_NUM(ctx);
    uint64_t num = orig_num;
    uint64_t start_time = syscall_profile_now();

#ifdef CONFIG_X86_64
    /* Translate x86_64 native syscall numbers to kernel internal numbers. */
    if (num < X86_SYSCALL_TABLE_SIZE) {
        uint32_t kernel_nr = x86_syscall_to_kernel_nr((uint32_t)num);
        if (kernel_nr == (uint32_t)-1) {
            /* Unsupported x86_64 syscall - return ENOSYS */
            if (num < 300)
                kdebug("[X86_SYSCALL] Unimplemented x86_64 nr=%lu\n", (unsigned long)num);
            TRAP_CTX_SET_RET(ctx, -ENOSYS);
            return -ENOSYS;
        }
        num = kernel_nr;
    }
#endif

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
            kdebug("[A20] UNHANDLED syscall: orig=0x%lx kernel=0x%lx\n", (unsigned long)orig_num, (unsigned long)num);
        }
#ifdef CONFIG_X86_64
        if (orig_num == X86_SYS_fork || orig_num == X86_SYS_vfork || orig_num == X86_SYS_clone) {
            klog(KLOG_ERR, "[X86_CLONE] ret=%ld\n", (long)ret);
        }
#endif

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

#ifdef CONFIG_X86_64
    x86_syscall_rewrite_args(orig_num, &args);
#endif

    int64_t ret = -ENOSYS;
    int context_restored = 0;
    const linux_syscall_entry_t *entry = linux_syscall_lookup(num);
    if (num == SYS_execve) {
        char *path = (char *)TRAP_CTX_ARG0(ctx);
        klog(KLOG_ERR, "SYS_execve path addr: %p\n", path);
    }
    task_t *dbg_cur = proc_current();
    if (dbg_cur && dbg_cur->pid >= 5 &&
        (num == SYS_futex || num == SYS_exit || num == SYS_exit_group ||
         num == SYS_clone || num == SYS_clone3 ||
         num == SYS_close || num == SYS_close_range ||
         num == SYS_brk || num == SYS_mmap || num == SYS_munmap ||
         num == SYS_mremap || num == SYS_mprotect || num == SYS_madvise ||
         num == SYS_accept4 || num == SYS_socket || num == SYS_connect ||
         num == SYS_listen)) {
        ktrace_syscall("[SYSDBG] enter: pid=%d nr=%lu a0=0x%lx a1=0x%lx a2=0x%lx\n",
                       dbg_cur->pid, (unsigned long)num,
                       (unsigned long)args.arg[0],
                       (unsigned long)args.arg[1],
                       (unsigned long)args.arg[2]);
    }

#ifdef CONFIG_X86_64
    if (orig_num == X86_SYS_fork || orig_num == X86_SYS_vfork || orig_num == X86_SYS_clone) {
        klog(KLOG_ERR, "[X86_CLONE] pid=%d orig_nr=%lu kernel_nr=%lu args=%lx %lx %lx %lx %lx\n",
               proc_current()->pid, orig_num, num,
               args.arg[0], args.arg[1], args.arg[2], args.arg[3], args.arg[4]);
    }
#endif

    if (entry) {
        ret = entry->handler(&args);
        context_restored = entry->restores_context;
    } else {
        klog(KLOG_ERR, "[A20] UNHANDLED linux syscall: orig=0x%lx kernel=0x%lx\n", (unsigned long)orig_num, (unsigned long)num);
    }

    if (num == SYS_execve) {
        klog(KLOG_ERR, "SYS_execve ret: %ld\n", ret);
    }

#ifdef CONFIG_X86_64
    if (orig_num == X86_SYS_fork || orig_num == X86_SYS_vfork || orig_num == X86_SYS_clone) {
        klog(KLOG_ERR, "[X86_CLONE] ret=%ld\n", (long)ret);
    }
#endif

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
    if (!context_restored && proc_current() && (++syscall_resched_counter & 0x1f) == 0)
        proc_yield();
    proc_check_exit_pending();
    if (num == SYS_sigsuspend)
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
