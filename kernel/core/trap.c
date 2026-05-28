#include "core/trap.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "sys/syscall.h"
#include "core/timer.h"
#include "drv/uart.h"
#include "mm/mm.h"
#include "mm/fault.h"
#include "mm/vm.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/defs.h"
#include "core/consts.h"
#include "core/klog.h"

static int fetch_user_insn(task_t *task, uint64_t va, uint32_t *insn_out) {
    if (!task || !task->mm || !task->mm->pgdir || !insn_out)
        return 0;

    uint64_t leaf_base = 0;
    uint64_t *pte = pt_lookup_leaf(task->mm->pgdir, va, NULL, &leaf_base, NULL);
    if (!pte || !(*pte & PTE_V))
        return 0;

    uint8_t *insn_kva = (uint8_t *)(arch_pte_addr(*pte) + PAGE_OFFSET +
                                    (va - leaf_base));
    *insn_out = *(uint32_t *)insn_kva;
    return 1;
}

static int ktrap_diag_count = 0;

static void dump_trap_context(trap_context_t *ctx) {
    kerr("  regs: ra=0x%lx sp=0x%lx tp=0x%lx\n",
         (unsigned long)TRAP_CTX_RA(ctx),
         (unsigned long)TRAP_CTX_SP(ctx),
         (unsigned long)TRAP_CTX_TP(ctx));
    kerr("  regs: a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx\n",
         (unsigned long)TRAP_CTX_ARG0(ctx),
         (unsigned long)TRAP_CTX_ARG1(ctx),
         (unsigned long)TRAP_CTX_ARG2(ctx),
         (unsigned long)TRAP_CTX_ARG3(ctx));
    kerr("  regs: a4=0x%lx a5=0x%lx a7=0x%lx\n",
         (unsigned long)TRAP_CTX_ARG4(ctx),
         (unsigned long)TRAP_CTX_ARG5(ctx),
         (unsigned long)TRAP_CTX_SYSCALL_NUM(ctx));
}

static void dump_kernel_backtrace(trap_context_t *ctx, uint64_t pc, int max_frames) {
    kerr("  backtrace:\n");
    kerr("    [%d] pc=0x%lx\n", 0, (unsigned long)pc);
    struct backtrace_frame frames[16];
    int n = arch_unwind_frames(TRAP_CTX_FP(ctx), frames, max_frames > 16 ? 16 : max_frames);
    for (int i = 0; i < n; i++) {
        kerr("    [%d] pc=0x%lx\n", i + 1, (unsigned long)frames[i].pc);
    }
}

static void dump_fault_pte(task_t *task, uint64_t va) {
    if (!task || !task->mm || !task->mm->pgdir)
        return;

    uint64_t *pte = pt_lookup_leaf(task->mm->pgdir, va, NULL, NULL, NULL);
    kerr("  pte=%p value=0x%lx\n", (void *)pte, pte ? *pte : 0UL);
    kerr("  mm: brk=0x%lx start_brk=0x%lx stack=[0x%lx,0x%lx)\n",
         (unsigned long)task->mm->brk, (unsigned long)task->mm->start_brk,
         (unsigned long)task->mm->stack_bottom, (unsigned long)task->mm->stack_top);
    vm_area_t *vma = mm_find_vma(task->mm, va & ~(PAGE_SIZE - 1));
    if (vma) {
        kerr("  vma=[0x%lx,0x%lx) flags=0x%lx pte_flags=0x%lx file_fd=%d off=0x%lx\n",
             vma->start, vma->end, vma->vm_flags, vma->pte_flags,
             vma->file_fd, vma->file_offset);
    } else {
        kerr("  vma=<none> (mmap=%p, total_vm=%lu)\n",
             task->mm->mmap, (unsigned long)task->mm->total_vm);
        int nvma = 0;
        for (vm_area_t *v = task->mm->mmap; v && nvma < 10; v = v->next, nvma++)
            kerr("    [%d] [0x%lx,0x%lx)\n", nvma,
                 (unsigned long)v->start, (unsigned long)v->end);
    }
}

static int deliver_user_sync_signal(trap_context_t *ctx, int sig, int fatal_code) {
    task_t *cur = proc_current();
    if (!cur || !cur->signals || !cur->pgdir) {
        printf("FATAL: pid=%d signal=%d (no signal state) pc=0x%lx\n",
               cur ? cur->pid : -1, sig,
               (unsigned long)TRAP_CTX_EPC(ctx));
        proc_exit_group(fatal_code);
    }

    signal_state_t *ss = (signal_state_t *)cur->signals;
    sigaction_t *sa = &ss->actions[sig];
    if (sa->sa_handler == SIG_DFL || sa->sa_handler == SIG_IGN ||
        (cur->sig_blocked & signal_mask_bit(sig))) {
        printf("FATAL: pid=%d signal=%d abi=%d pc=0x%lx sp=0x%lx\n",
               cur->pid, sig, cur->abi_mode,
               (unsigned long)TRAP_CTX_EPC(ctx),
               (unsigned long)TRAP_CTX_SP(ctx));
        proc_exit_group(fatal_code);
    }

    signal_send(cur->pid, sig);
    signal_deliver_user(ctx);
    return 1;
}

void trap_handler(trap_context_t *ctx) {
    uint64_t scause = arch_read_cause();
    uint64_t stval = arch_read_tval();
    uint64_t sepc = arch_read_epc();

    TRAP_CTX_KScratch0(ctx) = arch_read_satp();
    task_t *current = proc_current();
    if (current && current->pgdir)
        current->trap_ctx = ctx;

    if (scause & CAUSE_INTR_MASK) {
        arch_handle_irq(scause & CAUSE_CODE_MASK, 1);
        if (current && current->pid >= 4)
            ktrace_trap("[TRAP] irq done: pid=%d sig_deliver...\n", current->pid);
        if (proc_current() != current)
            return;
        proc_check_exit_pending();
    } else {
        uint64_t code = scause & CAUSE_CODE_MASK;
        uint32_t user_insn = 0;
        task_t *cur = proc_current();
        int have_user_insn = fetch_user_insn(cur, sepc, &user_insn);
        if (code == CAUSE_ECALL_U) {
            arch_advance_syscall_epc(ctx);
            /*
             * RISC-V enters the trap handler with SIE cleared.  Keep the
             * normal syscall body interruptible so UART input, block/network
             * completions, and timer bookkeeping are not starved by long
             * kernel-side loops such as fork-heavy benchmarks.
            */
            arch_local_irq_enable();
            syscall_dispatch(ctx);
            arch_local_irq_disable();
            proc_check_exit_pending();
            if (cur && cur->pid >= 5)
                ktrace_trap("[TRAP] ret-to-user: pid=%d pgdl=0x%lx era=0x%lx prmd_csr=0x%lx prmd_ctx=0x%lx sp_ctx=0x%lx\n",
                            cur->pid,
                            (unsigned long)TRAP_CTX_KScratch0(ctx),
                            (unsigned long)TRAP_CTX_EPC(ctx),
                            (unsigned long)arch_read_sstatus(),
                            (unsigned long)TRAP_CTX_STATUS(ctx),
                            (unsigned long)TRAP_CTX_SP(ctx));
            return;
        } else if (code == CAUSE_LOAD_PAGE_FAULT || code == CAUSE_STORE_PAGE_FAULT || code == CAUSE_INSN_PAGE_FAULT) {
            uint64_t mm_flags = 0;
            int mm_locked = 0;
            if (cur && cur->mm) {
                mm_flags = spin_lock_irqsave(&cur->mm->lock);
                mm_locked = 1;
            }
            if (code == CAUSE_STORE_PAGE_FAULT) {
                if (handle_cow_fault(cur, stval) == 0) {
                    if (mm_locked)
                        spin_unlock_irqrestore(&cur->mm->lock, mm_flags);
                    signal_deliver_user(ctx);
                    return;
                }
            }
            if (mm_locked) {
                spin_unlock_irqrestore(&cur->mm->lock, mm_flags);
                mm_locked = 0;
            }
            if (handle_demand_fault(cur, stval) == 0) {
                signal_deliver_user(ctx);
                return;
            }
            printf("SIGSEGV: pid=%d code=%lu sepc=0x%lx stval=0x%lx abi=%d\n",
                  cur ? cur->pid : -1, (unsigned long)code,
                  (unsigned long)sepc, (unsigned long)stval,
                  cur ? cur->abi_mode : -1);
            if (have_user_insn)
                kerr("  insn@sepc=0x%08x\n", user_insn);
            dump_trap_context(ctx);
            dump_fault_pte(cur, stval);
            if (deliver_user_sync_signal(ctx, SIGSEGV, -SIGSEGV))
                return;
            proc_exit_group(-SIGSEGV);
        } else if (code == CAUSE_PAGE_MODIFICATION) {
            /*
             * LoongArch PME: hardware write to a V=1, D=0 page.
             * Two cases:
             *   1. COW page: PTE_COW set, W/D cleared intentionally → allocate copy.
             *   2. Clean tracking: page is writable but D was 0 (e.g. after
             *      fork/exec page-table copy). Set D=1 and retry.
             */
            task_t *cur = proc_current();
            /* Case 1: try COW resolution first */
            if (handle_cow_fault(cur, stval) == 0) {
                return;
            }
            /* Case 2: just set D=1 in the existing PTE if it's writable */
            if (cur && cur->mm && cur->mm->pgdir) {
                uint64_t *pte = pt_lookup_leaf(cur->mm->pgdir, stval, NULL, NULL, NULL);
                if (pte && (*pte & PTE_V) && (*pte & PTE_W)) {
                    uint64_t flags = (*pte & (PTE_R | PTE_W | PTE_X | PTE_U |
                                              PTE_G | PTE_A | PTE_MAT1 |
                                              PTE_LEAF | PTE_COW)) | PTE_D;
                    *pte = arch_pte_leaf(arch_pte_addr(*pte), flags);
                    arch_tlb_flush_page(stval);
                    return;
                }
            }
            kerr("User page modification fault: pid=%d sepc=0x%lx stval=0x%lx\n",
                 cur ? cur->pid : -1, sepc, stval);
            if (have_user_insn)
                kerr("  insn@sepc=0x%08x\n", user_insn);
            dump_trap_context(ctx);
            dump_fault_pte(cur, stval);
            if (deliver_user_sync_signal(ctx, SIGSEGV, -SIGSEGV))
                return;
            proc_exit_group(-SIGSEGV);
        } else if (code == CAUSE_INSN_FAULT || code == CAUSE_LOAD_FAULT || code == CAUSE_STORE_FAULT) {
            printf("ADE/ALE: pid=%d sepc=0x%lx stval=0x%lx code=%lu\n",
                  cur ? cur->pid : -1, (unsigned long)sepc, (unsigned long)stval, (unsigned long)code);
            if (deliver_user_sync_signal(ctx, SIGSEGV, -SIGSEGV))
                return;
            proc_exit_group(-SIGSEGV); 
        } else if (code == CAUSE_ILLEGAL_INSN) {
            printf("SIGILL: pid=%d sepc=0x%lx stval=0x%lx\n",
                  cur ? cur->pid : -1, (unsigned long)sepc, (unsigned long)stval);
            if (deliver_user_sync_signal(ctx, SIGILL, -SIGILL))
                return;
            kerr("User Illegal Instruction: pid=%d sepc=0x%lx stval=0x%lx\n",
                 cur ? cur->pid : -1, sepc, stval);
            if (have_user_insn)
                kerr("  insn@sepc=0x%08x\n", user_insn);
            dump_trap_context(ctx);
            proc_exit_group(-SIGILL);
        } else if (code == CAUSE_BREAKPOINT) {
            printf("SIGTRAP: pid=%d sepc=0x%lx stval=0x%lx\n",
                  cur ? cur->pid : -1, (unsigned long)sepc, (unsigned long)stval);
            if (deliver_user_sync_signal(ctx, SIGTRAP, -SIGTRAP))
                return;
            proc_exit_group(-SIGTRAP);
        } else {
            kerr("TRAP EXCEPTION: scause=0x%lx code=%lu sepc=0x%lx stval=0x%lx\n",
                   scause, code, sepc, stval);
            proc_exit_group(-1);
        }
    }
    proc_check_exit_pending();
    signal_deliver_user(ctx);
    proc_check_exit_pending();
}

void kernel_trap_handler(trap_context_t *ctx) {
    TRAP_CTX_KScratch0(ctx) = arch_read_satp();
    uint64_t scause = arch_read_cause();
    uint64_t sepc = arch_read_epc();
    uint64_t stval = arch_read_tval();

    if (scause & CAUSE_INTR_MASK) {
        arch_handle_irq(scause & CAUSE_CODE_MASK, 0);
    } else {
        uint64_t code = scause & CAUSE_CODE_MASK;
        task_t *cur = proc_current();

        if (code == CAUSE_ECALL_U) {
            arch_advance_syscall_epc(ctx);
        } else if (code == CAUSE_LOAD_PAGE_FAULT ||
                   code == CAUSE_STORE_PAGE_FAULT ||
                   code == CAUSE_INSN_PAGE_FAULT ||
                   code == CAUSE_PAGE_MODIFICATION) {
            if (cur && cur->mm && stval < USER_VA_LIMIT) {
                if (code == CAUSE_STORE_PAGE_FAULT || code == CAUSE_PAGE_MODIFICATION) {
                    if (handle_cow_fault(cur, stval) == 0)
                        return;
                }
                if (handle_demand_fault(cur, stval) == 0)
                    return;
            }
            
            kerr("\n========== KERNEL PAGE FAULT ==========\n");
            kerr("Kernel failed to access user address. code=%lu\n", code);
            kerr("pid=%d sepc(ERA)=0x%lx stval(BADV)=0x%lx\n", cur ? cur->pid : -1, sepc, stval);
            dump_trap_context(ctx);
            dump_kernel_backtrace(ctx, sepc, 16);
            
            if (cur && cur->mm && stval < USER_VA_LIMIT) {
                proc_exit_group(-SIGSEGV);
            }
            panic("Unhandled kernel page fault");

        } else if (code == CAUSE_INSN_FAULT || code == CAUSE_LOAD_FAULT || code == CAUSE_STORE_FAULT) {
            kerr("\n========== KERNEL OOPS ==========\n");
            kerr("Kernel Address Error: code=%lu\n", code);
            kerr("Faulting PC (ERA): 0x%lx\n", sepc);
            kerr("Fault Address (BADV): 0x%lx\n", stval);
            kerr("Current Task: pid=%d name=%s\n", cur ? cur->pid : -1, cur ? cur->name : "?");
            dump_trap_context(ctx);
            dump_kernel_backtrace(ctx, sepc, 16);
            kerr("=================================\n");
            panic("Kernel Address Error");

        } else if (code == CAUSE_ILLEGAL_INSN) {
            kerr("\n========== KERNEL OOPS ==========\n");
            kerr("Kernel Illegal Instruction at sepc=0x%lx\n", sepc);
            dump_trap_context(ctx);
            panic("Kernel Illegal Instruction");
        } else {
            if (ktrap_diag_count < 5) {
                ktrap_diag_count++;
                kdebug("KERNEL TRAP: scause=0x%lx sepc=0x%lx stval=0x%lx code=%lu\n",
                       scause, sepc, stval, code);
                kdebug("[KTRAP] pid=%d name=%s ra=0x%lx a0=0x%lx\n",
                        cur ? cur->pid : -1, cur ? cur->name : "?",
                        TRAP_CTX_RA(ctx), TRAP_CTX_ARG0(ctx));
            }
            panic("Unhandled kernel trap");
        }
    }
}
