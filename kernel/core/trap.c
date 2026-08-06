#include "core/trap.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "sys/syscall.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "mm/mm.h"
#include "mm/fault.h"
#include "mm/vm.h"
#include "mm/frame.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/defs.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/kallsyms.h"

__attribute__((weak)) void arch_dump_trap_ring(void) {}

static int fetch_user_insn(task_t *task, vaddr_t va, uint32_t *insn_out) {
    if (!task || !task->mm || !task->mm->pgdir || !insn_out)
        return 0;
    return mm_fetch_user_insn32(task->mm->pgdir, va, insn_out);
}

static int ktrap_diag_count = 0;

static void dump_trap_context(trap_context_t *ctx) {
    kerr("  regs: ra=0x%lx sp=0x%lx tp=0x%lx status=0x%lx\n",
         (unsigned long)TRAP_CTX_RA(ctx),
         (unsigned long)TRAP_CTX_SP(ctx),
         (unsigned long)TRAP_CTX_TP(ctx),
         (unsigned long)TRAP_CTX_STATUS(ctx));
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

static void dump_kernel_backtrace(trap_context_t *ctx, vaddr_t pc, int max_frames) {
    kerr("  backtrace:\n");
    kerr("    [%d] pc=", 0);
    kallsyms_print(pc);
    kerr("\n");
    struct backtrace_frame frames[16];
    int n = arch_unwind_frames(TRAP_CTX_FP(ctx), frames, max_frames > 16 ? 16 : max_frames);
    for (int i = 0; i < n; i++) {
        kerr("    [%d] pc=", i + 1);
        kallsyms_print(frames[i].pc);
        kerr("\n");
    }
}

static void dump_fault_pte(task_t *task, vaddr_t va) {
    if (!task || !task->mm || !task->mm->pgdir)
        return;

    uintptr_t pte_slot = 0;
    pte_t pte_value = 0;
    mm_debug_pte_value(task->mm->pgdir, va, &pte_slot, &pte_value);
    kerr("  pte=%p value=0x%lx\n", (void *)pte_slot, pte_value);
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
    /* Physical-frame overlap diagnosis: dump the frame of the faulting VA
     * (and the stack frame) next to every VM_VMO mapping's frames.  If a
     * user stack frame collides with a VMO frame, ring writes are corrupting
     * the stack. */
    {
        mm_leaf_info_t leaf;
        if (mm_query_leaf(task->mm->pgdir, va & ~(PAGE_SIZE - 1), &leaf) == 0) {
            kerr("  [PFN] fault va=0x%lx -> pa=0x%lx pfn=%lu\n",
                 (unsigned long)(va & ~(PAGE_SIZE - 1)),
                 (unsigned long)leaf.pa,
                 (unsigned long)(leaf.pa >> 12));
            if (leaf.pa) {
                uint32_t *content = (uint32_t *)(leaf.pa + PAGE_OFFSET);
                kerr("  [CONTENT] va=0x%lx first=0x%08x\n",
                     (unsigned long)(va & ~(PAGE_SIZE - 1)),
                     (unsigned int)*content);
            }
        }
        for (vm_area_t *v = task->mm->mmap; v; v = v->next) {
            if (!(v->vm_flags & VM_VMO) || !v->vmo)
                continue;
            if (mm_query_leaf(task->mm->pgdir, v->start, &leaf) == 0)
                kerr("  [PFN] vmo va=0x%lx -> pa=0x%lx pfn=%lu\n",
                     (unsigned long)v->start, (unsigned long)leaf.pa,
                     (unsigned long)(leaf.pa >> 12));
        }
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

    if (!signal_task_user_handler_available(cur, sig)) {
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

static int user_sync_signal_is_handled(task_t *task, int sig) {
    if (!task || !task->signals || !task->pgdir)
        return 0;

    return signal_task_user_handler_available(task, sig);
}

static void user_trap_handler(trap_context_t *ctx) {
    reg_t scause = arch_read_cause();
    vaddr_t stval = arch_read_tval();
    vaddr_t sepc = arch_read_epc();

    TRAP_CTX_KScratch0(ctx) = arch_read_addr_space_token();
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
        reg_t code = scause & CAUSE_CODE_MASK;
        task_t *cur = proc_current();
        uint32_t user_insn = 0;
        int have_user_insn = fetch_user_insn(cur, sepc, &user_insn);
        if (code == CAUSE_ECALL_U) {
            arch_advance_syscall_epc(ctx);
            arch_syscall_dispatch_enter();
            syscall_dispatch(ctx);
            arch_syscall_dispatch_leave();
            proc_check_exit_pending();
            return;
        }
        if (code == CAUSE_LOAD_PAGE_FAULT || code == CAUSE_STORE_PAGE_FAULT || code == CAUSE_INSN_PAGE_FAULT) {
            if (code == CAUSE_STORE_PAGE_FAULT) {
                if (handle_cow_fault(cur, stval) == 0) {
                    arch_tlb_flush_page(stval); /* publish remote PTE change */
                    signal_deliver_user(ctx);
                    return;
                }
            }
            enum mm_fault_access access =
                code == CAUSE_STORE_PAGE_FAULT ? MM_FAULT_ACCESS_WRITE :
                code == CAUSE_INSN_PAGE_FAULT ? MM_FAULT_ACCESS_EXEC :
                                                MM_FAULT_ACCESS_READ;
            if (handle_present_page_fault(cur, stval, access) == 0) {
                signal_deliver_user(ctx);
                return;
            }
            if (handle_demand_fault(cur, stval) == 0) {
                arch_tlb_flush_page(stval); /* publish remote PTE change */
                signal_deliver_user(ctx);
                return;
            }
            /*
             * Demand paging may drop mm->lock for file I/O. Another thread can
             * complete the same mapping between our first present-PTE check
             * and a failed/redundant demand-fault attempt.
             */
            if (handle_present_page_fault(cur, stval, access) == 0) {
                signal_deliver_user(ctx);
                return;
            }
            if (user_sync_signal_is_handled(cur, SIGSEGV) &&
                deliver_user_sync_signal(ctx, SIGSEGV, -SIGSEGV))
                return;
            printf("SIGSEGV: pid=%d code=%lu sepc=0x%lx stval=0x%lx abi=%d\n",
                  cur ? cur->pid : -1, (unsigned long)code,
                  (unsigned long)sepc, (unsigned long)stval,
                   cur ? (int)cur->abi_mode : -1);
            if (arch_is_kernel_address((void *)stval))
                printf("SIGSEGV: !!! user fault on KERNEL address stval=0x%lx (kernel page leak)\n",
                       (unsigned long)stval);
            arch_dump_trap_ring();
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
            if (handle_cow_fault(cur, stval) == 0) {
                return;
            }
            /* Case 2: just set D=1 in the existing PTE if it's writable */
            if (cur && cur->mm && mm_mark_leaf_dirty_if_writable(cur->mm->pgdir, stval) == 0)
                return;
            /*
             * User programs may deliberately probe write protection and handle
             * SIGSEGV, e.g. lmbench's protection-fault benchmark.  Do not dump
             * every expected user fault to the serial console; default/unhandled
             * signals are still reported by deliver_user_sync_signal().
             */
            (void)have_user_insn;
            if (deliver_user_sync_signal(ctx, SIGSEGV, -SIGSEGV))
                return;
            proc_exit_group(-SIGSEGV);
        } else if (arch_is_user_page_permission_fault(code)) {
            /* LoongArch reports access-permission failures separately from
             * invalid-page faults.  They are user protection violations and
             * must be delivered as SIGSEGV rather than treated as an unknown
             * kernel exception. */
            if (deliver_user_sync_signal(ctx, SIGSEGV, -SIGSEGV))
                return;
            proc_exit_group(-SIGSEGV);
        } else if (code == CAUSE_INSN_FAULT || code == CAUSE_LOAD_FAULT || code == CAUSE_STORE_FAULT) {
            printf("ADE/ALE: pid=%d sepc=0x%lx stval=0x%lx code=%lu\n",
                  cur ? cur->pid : -1, (unsigned long)sepc, (unsigned long)stval, (unsigned long)code);
            if (have_user_insn)
                kerr("  insn@sepc=0x%08x\n", user_insn);
            dump_trap_context(ctx);
            if (deliver_user_sync_signal(ctx, SIGSEGV, -SIGSEGV))
                return;
            proc_exit_group(-SIGSEGV); 
        } else if (code == CAUSE_ILLEGAL_INSN) {
            printf("SIGILL: pid=%d sepc=0x%lx stval=0x%lx",
                  cur ? cur->pid : -1, (unsigned long)sepc, (unsigned long)stval);
#ifdef CONFIG_TRAP_ESR_DIAG
            printf(" esr=0x%lx ec=0x%lx", (unsigned long)arch_read_esr(),
                   (unsigned long)((arch_read_esr() >> 26) & 0x3fUL));
#endif
            if (have_user_insn)
                printf(" insn=0x%08x", user_insn);
            printf("\n");
            if (deliver_user_sync_signal(ctx, SIGILL, -SIGILL))
                return;
            kerr("User Illegal Instruction: pid=%d sepc=0x%lx stval=0x%lx\n",
                 cur ? cur->pid : -1, sepc, stval);
            if (cur && cur->mm) {
                mm_leaf_info_t leaf;
                extern void frame_trace_dump_pfn(pfn_t pfn);
                if (mm_query_leaf(cur->mm->pgdir, sepc & ~(PAGE_SIZE - 1),
                                  &leaf) == 0 && leaf.pa) {
                    uint32_t *content = (uint32_t *)(leaf.pa + PAGE_OFFSET);
                    kerr("  [CONTENT] sepc=0x%lx pa=0x%lx first=0x%08x\n",
                         (unsigned long)sepc, (unsigned long)leaf.pa,
                         (unsigned int)*content);
                    frame_trace_dump_pfn((pfn_t)(leaf.pa >> 12));
                }
            }
            if (have_user_insn)
                kerr("  insn@sepc=0x%08x\n", user_insn);
            dump_trap_context(ctx);
            proc_exit_group(-SIGILL);
        } else if (code == CAUSE_BREAKPOINT) {
            printf("SIGTRAP: pid=%d sepc=0x%lx stval=0x%lx\n",
                  cur ? cur->pid : -1, (unsigned long)sepc, (unsigned long)stval);
#if defined(CONFIG_X86_64)
            /* Single-step #DB (vec 1 maps here): clear TF in the saved
             * context so the step is one-shot; the traced stop reports the
             * SIGTRAP and the resume continues without re-trapping. */
            ctx->rflags &= ~(1UL << 8);
#endif
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
    {
        task_t *t = proc_current();
        if (t && t->mm)
            signal_deliver_user(ctx);
    }
    proc_check_exit_pending();
}

void trap_handler(trap_context_t *ctx)
{
    user_trap_handler(ctx);
    /*
     * All user trap exits share this safe point. A reschedule IPI or a timer
     * interrupt may have arrived while the task was in a long syscall; the
     * persistent request is consumed here, never in the IPI handler.
     */
    if (arch_syscall_resched_allowed())
        (void)proc_sched_safe_point();
}

void kernel_trap_handler(trap_context_t *ctx) {
    TRAP_CTX_KScratch0(ctx) = arch_read_addr_space_token();
    reg_t scause = arch_read_cause();
    vaddr_t sepc = arch_read_epc();
    vaddr_t stval = arch_read_tval();

    if (scause & CAUSE_INTR_MASK) {
        arch_handle_irq(scause & CAUSE_CODE_MASK, 0);
    } else {
        reg_t code = scause & CAUSE_CODE_MASK;
        task_t *cur = proc_current();

        if (code == CAUSE_ECALL_U) {
            arch_advance_syscall_epc(ctx);
        } else if (code == CAUSE_LOAD_PAGE_FAULT ||
                   code == CAUSE_STORE_PAGE_FAULT ||
                   code == CAUSE_INSN_PAGE_FAULT ||
                   code == CAUSE_PAGE_MODIFICATION) {
            if (cur && cur->mm && stval < USER_VA_LIMIT) {
                if (code == CAUSE_STORE_PAGE_FAULT || code == CAUSE_PAGE_MODIFICATION) {
                    if (handle_cow_fault(cur, stval) == 0) {
                        return;
                    }
                }
                enum mm_fault_access access =
                    (code == CAUSE_STORE_PAGE_FAULT ||
                     code == CAUSE_PAGE_MODIFICATION) ? MM_FAULT_ACCESS_WRITE :
                    code == CAUSE_INSN_PAGE_FAULT ? MM_FAULT_ACCESS_EXEC :
                                                   MM_FAULT_ACCESS_READ;
                if (handle_present_page_fault(cur, stval, access) == 0) {
                    return;
                }
                if (handle_demand_fault(cur, stval) == 0) {
                    return;
                }
                if (handle_present_page_fault(cur, stval, access) == 0) {
                    return;
                }
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
#ifdef CONFIG_TRAP_ESR_DIAG
            kerr("AArch64 ESR_EL1=0x%lx (EC=0x%lx ISS=0x%lx), FAR_EL1=0x%lx\n",
                 arch_read_esr(), (arch_read_esr() >> 26) & 0x3fUL,
                 arch_read_esr() & 0x1ffffffUL, stval);
#endif
            dump_trap_context(ctx);
            panic("Kernel Illegal Instruction");
        } else {
            if (ktrap_diag_count < 5) {
                ktrap_diag_count++;
                kerr("KERNEL TRAP: scause=0x%lx sepc=0x%lx stval=0x%lx code=%lu\n",
                       scause, sepc, stval, code);
                kerr("[KTRAP] pid=%d name=%s ra=0x%lx a0=0x%lx\n",
                        cur ? cur->pid : -1, cur ? cur->name : "?",
                        TRAP_CTX_RA(ctx), TRAP_CTX_ARG0(ctx));
            }
            panic("Unhandled kernel trap");
        }
    }
}
