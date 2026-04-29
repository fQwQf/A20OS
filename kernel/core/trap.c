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

static void dump_fault_pte(task_t *task, uint64_t va) {
    if (!task || !task->mm || !task->mm->pgdir)
        return;

    uint64_t *pte = pt_lookup_leaf(task->mm->pgdir, va, NULL, NULL, NULL);
    kerr("  pte=%p value=0x%lx\n", (void *)pte, pte ? *pte : 0UL);
}

void trap_handler(trap_context_t *ctx) {
    uint64_t scause = arch_read_cause();
    uint64_t stval = arch_read_tval();
    uint64_t sepc = arch_read_epc();

    TRAP_CTX_KScratch0(ctx) = arch_read_satp();

    if (scause & CAUSE_INTR_MASK) {
        arch_handle_irq(scause & CAUSE_CODE_MASK, 1);
    } else {
        uint64_t code = scause & CAUSE_CODE_MASK;
        uint32_t user_insn = 0;
        task_t *cur = proc_current();
        int have_user_insn = fetch_user_insn(cur, sepc, &user_insn);
        if (code == CAUSE_ECALL_U) {
            arch_advance_syscall_epc(ctx);
            syscall_dispatch(ctx);
        } else if (code == CAUSE_LOAD_PAGE_FAULT || code == CAUSE_STORE_PAGE_FAULT || code == CAUSE_INSN_PAGE_FAULT) {
            if (code == CAUSE_STORE_PAGE_FAULT) {
                if (handle_cow_fault(cur, stval) == 0) {
                    signal_deliver_user(ctx);
                    return;
                }
            }
            if (handle_demand_fault(cur, stval) == 0) {
                signal_deliver_user(ctx);
                return;
            }
            kerr("User PF unresolved: pid=%d code=%lu sepc=0x%lx stval=0x%lx\n",
                 cur ? cur->pid : -1, code, sepc, stval);
            if (have_user_insn)
                kerr("  insn@sepc=0x%08x\n", user_insn);
            dump_trap_context(ctx);
            dump_fault_pte(cur, stval);
            proc_exit(-SIGSEGV);
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
            proc_exit(-SIGSEGV);
        } else if (code == CAUSE_INSN_FAULT || code == CAUSE_LOAD_FAULT || code == CAUSE_STORE_FAULT) {
            kerr("User Address Error (ADE/ALE): pid=%d sepc=0x%lx stval=0x%lx code=%lu\n", 
                 cur ? cur->pid : -1, sepc, stval, code);
            dump_trap_context(ctx);
            proc_exit(-SIGSEGV); 
        } else if (code == CAUSE_ILLEGAL_INSN) {
            kerr("User Illegal Instruction: pid=%d sepc=0x%lx stval=0x%lx\n",
                 cur ? cur->pid : -1, sepc, stval);
            if (have_user_insn)
                kerr("  insn@sepc=0x%08x\n", user_insn);
            dump_trap_context(ctx);
            proc_exit(-SIGILL);
        } else {
            kerr("TRAP EXCEPTION: scause=0x%lx code=%lu sepc=0x%lx stval=0x%lx\n",
                   scause, code, sepc, stval);
            proc_exit(-1);
        }
    }
}

void kernel_trap_handler(trap_context_t *ctx) {
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
            
            if (cur && cur->mm && stval < USER_VA_LIMIT) {
                proc_exit(-SIGSEGV);
            }
            panic("Unhandled kernel page fault");

        } else if (code == CAUSE_INSN_FAULT || code == CAUSE_LOAD_FAULT || code == CAUSE_STORE_FAULT) {
            kerr("\n========== KERNEL OOPS ==========\n");
            kerr("Kernel Address Error: code=%lu\n", code);
            kerr("Faulting PC (ERA): 0x%lx\n", sepc);
            kerr("Fault Address (BADV): 0x%lx\n", stval);
            kerr("Current Task: pid=%d name=%s\n", cur ? cur->pid : -1, cur ? cur->name : "?");
            dump_trap_context(ctx);
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
