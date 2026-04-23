#include "trap.h"
#include "proc.h"
#include "syscall.h"
#include "timer.h"
#include "uart.h"
#include "plic.h"
#include "mm.h"
#include "frame.h"
#include "vm.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"
#include "defs.h"
#include "consts.h"
#include "klog.h"

int handle_cow_fault(task_t *t, uint64_t stval) {
    if (!t->mm || !t->mm->pgdir) return -1;

    uint64_t *pte = pt_walk(t->mm->pgdir, stval, 0);
    if (!pte || !(*pte & PTE_V)) return -1;
    if (*pte & PTE_W) return -1;
    if (!(*pte & PTE_COW)) return -1;

    paddr_t old_pa = arch_pte_addr(*pte);
    pfn_t old_pfn = phys_to_pfn(old_pa);
    if (!pfn_valid(old_pfn)) return -1;

    uint16_t rc = pfa.meta[old_pfn].refcount;

    if (rc > 1) {
        pfn_t new_pfn = pfa_alloc_page();
        if (new_pfn == PFN_NONE) return -1;
        memcpy(pfn_to_virt(new_pfn), pfn_to_virt(old_pfn), PAGE_SIZE);
        frame_put(old_pfn);
        *pte = arch_pte_from_pa(pfn_to_phys(new_pfn)) | (*pte & (PTE_R | PTE_X | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF)) | PTE_W | PTE_V;
    } else {
        *pte = (*pte & ~PTE_COW) | PTE_W;
    }

    arch_tlb_flush_page(stval);
    return 0;
}

/*
 * handle_demand_fault — demand paging + stack growth.
 *
 * Called for any page fault (load / insn / store) after COW has been
 * ruled out (or was not applicable).  Two cases:
 *
 *   1. Stack growth:  stval is below current stack_bottom but still
 *      within the maximum stack region.  Allocate a zero page, map it,
 *      and lower stack_bottom.
 *
 *   2. Anonymous VMA: stval falls inside a VMA that has no backing
 *      page yet (lazy mmap / brk).  Allocate a zero page and map it
 *      with the VMA's pte_flags.
 *
 * Returns 0 on success, -1 if the fault cannot be resolved (→ SIGSEGV).
 */
int handle_demand_fault(task_t *t, uint64_t stval) {
    if (!t->mm || !t->mm->pgdir) return -1;

    uint64_t page_va = stval & ~(PAGE_SIZE - 1);

    if (t->mm->stack_top != 0) {
        uint64_t stack_limit = t->mm->stack_top - DEFAULT_STACK_SIZE;
        if (page_va >= stack_limit && page_va < t->mm->stack_bottom) {
            pfn_t pfn = pfa_alloc_page();
            if (pfn == PFN_NONE) return -1;
            memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

            uint64_t pte_flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D;
            int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(pfn), pte_flags);
            if (r < 0) { frame_put(pfn); return -1; }

        t->mm->stack_bottom = page_va;
        t->mm->rss++;
        arch_tlb_flush_page(stval);
        return 0;
    }
    }

    if (page_va >= t->mm->start_brk &&
        page_va < ROUND_UP(t->mm->brk, PAGE_SIZE)) {
        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) return -1;
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

        uint64_t pte_flags = PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D;
        int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(pfn), pte_flags);
        if (r < 0) { frame_put(pfn); return -1; }

        t->mm->rss++;
        arch_tlb_flush_page(stval);
        return 0;
    }

    vm_area_t *vma = mm_find_vma(t->mm, page_va);
    if (vma) {
        uint64_t *pte = pt_walk(t->mm->pgdir, page_va, 0);
        if (pte && (*pte & PTE_V)) return -1;

        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) return -1;
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

        int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(pfn),
                        vma->pte_flags);
        if (r < 0) { frame_put(pfn); return -1; }

        t->mm->rss++;
        arch_tlb_flush_page(stval);
        return 0;
    }

    return -1;
}

extern void trap_entry_la64(void);

void trap_init(void) {
#ifdef CONFIG_LOONGARCH64
    arch_write_tvec((uint64_t)trap_entry_la64);
    /* ECFG: enable timer interrupt (IS bit 11) */
    uint64_t ecfg;
    __asm__ __volatile__("csrrd %0, 0x4" : "=r"(ecfg));
    ecfg |= (1UL << 11);
    __asm__ __volatile__("csrwr %0, 0x4" :: "r"(ecfg));
#else
    arch_write_tvec((uint64_t)__trap_from_kernel);
    arch_write_sscratch(0);
#endif
}

static int ktrap_diag_count = 0;

static void handle_irq(uint64_t irq, uint64_t sepc, int from_user) {
    if (irq == IRQ_S_TIMER) {
        timer_set_interval(TICKS_PER_SEC / 100);
        if (from_user) {
            task_t *cur = proc_current();
            if (cur) cur->total_time++;
            proc_yield();
        }
#ifdef CONFIG_RISCV64
    } else if (irq == IRQ_S_EXT) {
        uint32_t irq_id = plic_claim();
        if (irq_id == UART0_IRQ)
            uart_handle_irq();
        if (irq_id != 0)
            plic_complete(irq_id);
    } else if (irq == IRQ_S_SOFT) {
        arch_write_sip(arch_read_sip() & ~SIE_SSIE);
        timer_set_interval(TICKS_PER_SEC / 100);
        if (from_user) proc_yield();
#endif
    } else {
        (void)sepc;
    }
}

void trap_handler(trap_context_t *ctx) {
    uint64_t scause = arch_read_cause();
    uint64_t stval = arch_read_tval();
    uint64_t sepc = arch_read_epc();

    TRAP_CTX_KScratch0(ctx) = arch_read_satp();

    if (scause & CAUSE_INTR_MASK) {
        handle_irq(scause & CAUSE_CODE_MASK, sepc, 1);
    } else {
        uint64_t code = scause & CAUSE_CODE_MASK;
        if (code == CAUSE_ECALL_U) {
            TRAP_CTX_EPC(ctx) += 4;
            syscall_dispatch(ctx);
        } else if (code == CAUSE_LOAD_PAGE_FAULT || code == CAUSE_STORE_PAGE_FAULT || code == CAUSE_INSN_PAGE_FAULT) {
            task_t *cur = proc_current();
            /* Try COW first (store fault on read-only mapped page) */
            if (code == CAUSE_STORE_PAGE_FAULT) {
                if (handle_cow_fault(cur, stval) == 0) {
                    signal_deliver_user(ctx);
                    return;
                }
            }
            /* Then try demand paging / stack growth */
            if (handle_demand_fault(cur, stval) == 0) {
                signal_deliver_user(ctx);
                return;
            }
            {
                uint32_t insn = 0;
                pfn_t dbg_pfn = 0;
                uint64_t stval_pte = 0;
                uint64_t pte2 = 0, pte1 = 0, pte0 = 0;
                uint64_t *l1 = NULL, *l0 = NULL;
                if (cur && cur->mm && cur->mm->pgdir) {
                    uint64_t *pte_sepc = pt_walk(cur->mm->pgdir, sepc, 0);
                    if (pte_sepc && (*pte_sepc & PTE_V)) {
                        dbg_pfn = phys_to_pfn(arch_pte_addr(*pte_sepc));
                        uint8_t *kva = (uint8_t *)(arch_pte_addr(*pte_sepc) + PAGE_OFFSET + (sepc & 0xFFF));
                        insn = *(uint32_t *)kva;
                    }
                    uint64_t *pte_stval = pt_walk(cur->mm->pgdir, stval, 0);
                    stval_pte = pte_stval ? *pte_stval : 0;
                    uint64_t *pgdir = cur->mm->pgdir;
                    pte2 = pgdir ? pgdir[0] : 0;
                    l1 = (pte2 & PTE_V) ? arch_pte_to_ptr(pte2) : NULL;
                    pte1 = l1 ? l1[0] : 0;
                    l0 = (pte1 & PTE_V) ? arch_pte_to_ptr(pte1) : NULL;
                    pte0 = l0 ? l0[0x2f] : 0;
                }
                kerr("User Page Fault: pid=%d scause=0x%lx sepc=0x%lx stval=0x%lx insn=0x%08x ctx_sepc=0x%lx pfn=%u flags=%u rc=%u\n",
                        cur ? cur->pid : -1, scause, sepc, stval, insn, TRAP_CTX_EPC(ctx),
                        (unsigned)dbg_pfn, dbg_pfn < pfa.total_frames ? pfa.meta[dbg_pfn].flags : 99,
                        dbg_pfn < pfa.total_frames ? pfa.meta[dbg_pfn].refcount : 99);
                kerr("[PTEWALK] pgdir=0x%lx pgdir[0]=0x%lx l1=0x%lx l1[0]=0x%lx l0=0x%lx l0[0x2f]=0x%lx stval_pte=0x%lx\n",
                        (unsigned long)(cur && cur->mm ? cur->mm->pgdir : NULL), pte2, (unsigned long)l1, pte1, (unsigned long)l0, pte0, stval_pte);
            }
            proc_exit(-SIGSEGV);
        } else if (code == CAUSE_ILLEGAL_INSN) {
            kerr("User Illegal Instruction: sepc=0x%lx\n", sepc);
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
        handle_irq(scause & CAUSE_CODE_MASK, sepc, 0);
    } else {
        uint64_t code = scause & CAUSE_CODE_MASK;
        if (code == CAUSE_ECALL_U) {
            TRAP_CTX_EPC(ctx) += 4;
        } else if (code == CAUSE_LOAD_PAGE_FAULT ||
                   code == CAUSE_STORE_PAGE_FAULT ||
                   code == CAUSE_INSN_PAGE_FAULT) {
            /*
             * Kernel took a page fault on a user-space address while
             * executing a syscall (e.g. memset/memcpy on a user buffer).
             * Try demand-fault resolution: COW → demand-paging → stack-growth.
             * If the faulting address is in user space (below 0x4000000000,
             * i.e. vpn2 < 256) and we can resolve it, just return — the
             * faulting instruction will retry successfully.
             */
            task_t *cur = proc_current();
            if (cur && cur->mm && stval < 0x4000000000UL) {
                if (code == CAUSE_STORE_PAGE_FAULT) {
                    if (handle_cow_fault(cur, stval) == 0)
                        return;
                }
                if (handle_demand_fault(cur, stval) == 0)
                    return;
            }
            task_t *cur2 = proc_current();
            if (ktrap_diag_count < 5) {
                ktrap_diag_count++;
                kdebug("KERNEL TRAP: scause=0x%lx sepc=0x%lx stval=0x%lx\n",
                       scause, sepc, stval);
                kdebug("[KTRAP] pid=%d name=%s ra=0x%lx a0=0x%lx a1=0x%lx\n",
                        cur2 ? cur2->pid : -1, cur2 ? cur2->name : "?",
                        TRAP_CTX_RA(ctx), TRAP_CTX_ARG0(ctx), TRAP_CTX_ARG1(ctx));
            }
            if (cur && cur->mm && stval < 0x4000000000UL) {
                proc_exit(-SIGSEGV);
            }
            panic("Unhandled kernel trap");
        } else {
            task_t *cur = proc_current();
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
