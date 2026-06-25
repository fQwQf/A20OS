#include "proc/signal.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/string.h"

static void signal_make_page_exec(uint64_t addr)
{
    task_t *t = proc_current();
    if (!t || !t->pgdir)
        return;
    vaddr_t page = addr & ~(vaddr_t)(PAGE_SIZE - 1);
    paddr_t pa = pt_translate(t->pgdir, page);
    if (!pa)
        return;
    pt_unmap(t->pgdir, page);
    pt_map(t->pgdir, page, pa,
           mm_pte_flags_make_writable_dirty(arch_signal_tramp_pte_flags()));
    arch_tlb_flush_page(page);
}

static void build_siginfo_code(siginfo_t *si, int sig, task_t *sender, int code)
{
    memset(si, 0, sizeof(*si));
    si->si_signo = sig;
    si->si_code = code;
    if (sender) {
        si->_sifields[0] = 0;
        si->_sifields[1] = sender->pid;
        si->_sifields[2] = (int)sender->cred.uid;
    }
}

static void build_ucontext(ucontext_t *uc, trap_context_t *ctx,
                           uint64_t old_blocked, sigaltstack_t *altstack)
{
    memset(uc, 0, sizeof(*uc));
    uc->uc_sigmask[0] = signal_mask_to_user(old_blocked);
    uc->uc_sigmask[1] = 0;
    uc->uc_stack.ss_sp = altstack->ss_sp;
    uc->uc_stack.ss_flags = altstack->ss_flags;
    uc->uc_stack.ss_size = altstack->ss_size;
    arch_signal_build_mcontext(&uc->uc_mcontext, ctx);
}

signal_state_t *a20_signal_task_signals(task_t *task)
{
    return task ? (signal_state_t *)task->signals : NULL;
}

int a20_signal_task_has_pgdir(task_t *task)
{
    return task && task->pgdir != NULL;
}

int a20_signal_task_pid(task_t *task)
{
    return task ? task->pid : -1;
}

int a20_signal_task_uid(task_t *task)
{
    return task ? task->cred.uid : 0;
}

int a20_signal_task_state(task_t *task)
{
    return task ? (int)task->state : -1;
}

void a20_signal_task_set_state(task_t *task, int state)
{
    if (task)
        task->state = (proc_state_t)state;
}

void a20_signal_task_set_exit_code(task_t *task, int code)
{
    if (task)
        task->exit_code = code;
}

int a20_signal_task_exit_pending(task_t *task)
{
    return task ? __atomic_load_n(&task->exit_pending, __ATOMIC_ACQUIRE) : 0;
}

uint64_t a20_signal_task_sig_blocked(task_t *task)
{
    return task ? task->sig_blocked : 0;
}

void a20_signal_task_set_sig_blocked(task_t *task, uint64_t mask)
{
    if (task)
        task->sig_blocked = mask;
}

uint64_t a20_signal_task_thread_pending(task_t *task)
{
    return task ? task->thread_pending : 0;
}

void a20_signal_task_set_thread_pending(task_t *task, uint64_t mask)
{
    if (task)
        task->thread_pending = mask;
}

int a20_signal_task_sigsuspend_active(task_t *task)
{
    return task ? task->sigsuspend_active : 0;
}

uint64_t a20_signal_task_sigsuspend_old_blocked(task_t *task)
{
    return task ? task->sigsuspend_old_blocked : 0;
}

void a20_signal_task_set_sigsuspend_active(task_t *task, int active)
{
    if (task)
        task->sigsuspend_active = active;
}

void a20_signal_task_set_sig_old_blocked(task_t *task, uint64_t mask)
{
    if (task)
        task->sig_old_blocked = mask;
}

void a20_signal_task_set_sig_handling(task_t *task, int sig)
{
    if (task)
        task->sig_handling = sig;
}

void a20_signal_task_save_sig_ctx(task_t *task, const trap_context_t *ctx)
{
    if (task && ctx)
        task->sig_saved_ctx = *ctx;
}

void a20_signal_task_restore_sigsuspend_mask(task_t *task)
{
    if (task)
        task->sig_blocked = task->sigsuspend_old_blocked;
}

int a20_signal_task_setup_user_frame(task_t *task, trap_context_t *ctx, int sig,
                                     const sigaction_t *sa, uint64_t old_blocked,
                                     const siginfo_t *info)
{
    if (!task || !ctx || !sa)
        return -EINVAL;

    uint64_t sp = TRAP_CTX_SP(ctx);
    if ((sa->sa_flags & SA_ONSTACK) &&
        task->sigaltstack.ss_flags == 0 &&
        task->sigaltstack.ss_sp != NULL &&
        task->sigaltstack.ss_size >= MINSIGSTKSZ) {
        sp = (uintptr_t)task->sigaltstack.ss_sp + task->sigaltstack.ss_size;
    }

    sig_rt_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.flag = 0x77777777ULL;
    if (info)
        frame.info = *info;
    else
        build_siginfo_code(&frame.info, sig, NULL, SI_KERNEL);
    build_ucontext(&frame.uc, ctx, old_blocked, &task->sigaltstack);
    arch_signal_build_frame_extra(&frame.arch_extra, ctx);

    sp -= sizeof(sig_rt_frame_t);
    sp &= ~15ULL;
    if (copy_to_user((void *)sp, &frame, sizeof(frame)) < 0)
        return -EFAULT;

    uint32_t tramp[2];
    arch_signal_prepare_trampoline(tramp);
    uint64_t tramp_addr = sp + offsetof(sig_rt_frame_t, tramp);
    if (copy_to_user((void *)tramp_addr, tramp, sizeof(tramp)) < 0)
        return -EFAULT;

    signal_make_page_exec(tramp_addr);

    TRAP_CTX_SP(ctx) = sp;
    TRAP_CTX_EPC(ctx) = sa->sa_handler;
    TRAP_CTX_ARG0(ctx) = sig;
    if (sa->sa_flags & SA_SIGINFO) {
        TRAP_CTX_ARG1(ctx) = sp + offsetof(sig_rt_frame_t, info);
        TRAP_CTX_ARG2(ctx) = sp + offsetof(sig_rt_frame_t, uc);
    }
    TRAP_CTX_RA(ctx) = tramp_addr;
    return 0;
}

int64_t a20_signal_rt_sigreturn(task_t *task, trap_context_t *ctx)
{
    if (!task || !ctx)
        return -EFAULT;

    uint64_t sp = TRAP_CTX_SP(ctx);
    sig_rt_frame_t frame;
    if (copy_from_user(&frame, (void *)sp, sizeof(frame)) < 0)
        return -EFAULT;

    task->sig_blocked = signal_mask_from_user(frame.uc.uc_sigmask[0]);
    arch_signal_restore_mcontext(ctx, &frame.uc.uc_mcontext);
    arch_signal_restore_frame_extra(ctx, &frame.arch_extra);
    task->sig_handling = 0;
    return 0;
}
