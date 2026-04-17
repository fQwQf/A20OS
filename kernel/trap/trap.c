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

    paddr_t old_pa = SV39_PTE_ADDR(*pte);
    pfn_t old_pfn = phys_to_pfn(old_pa);
    if (!pfn_valid(old_pfn)) return -1;

    uint16_t rc = pfa.meta[old_pfn].refcount;

    if (rc > 1) {
        pfn_t new_pfn = pfa_alloc_page();
        if (new_pfn == PFN_NONE) return -1;
        memcpy(pfn_to_virt(new_pfn), pfn_to_virt(old_pfn), PAGE_SIZE);
        frame_put(old_pfn);
        *pte = PTE_FROM_PA(pfn_to_phys(new_pfn)) | (*pte & (PTE_R | PTE_X | PTE_U | PTE_A | PTE_D)) | PTE_W | PTE_V;
    } else {
        *pte = (*pte & ~PTE_COW) | PTE_W;
    }

    __asm__ volatile("sfence.vma %0, zero" :: "r"(stval));
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
        __asm__ volatile("sfence.vma %0, zero" :: "r"(stval));
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
        __asm__ volatile("sfence.vma %0, zero" :: "r"(stval));
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
        __asm__ volatile("sfence.vma %0, zero" :: "r"(stval));
        return 0;
    }

    return -1;
}

void trap_init(void) {
    w_stvec((uint64_t)__trap_from_kernel);
    w_sscratch(0);
}

static void handle_irq(uint64_t irq, uint64_t sepc, int from_user) {
    if (irq == IRQ_S_TIMER) {
        timer_set_interval(TICKS_PER_SEC / 100);
        if (from_user) {
            task_t *cur = proc_current();
            if (cur) cur->total_time++;
            proc_yield();
        }
    } else if (irq == IRQ_S_EXT) {
        uint32_t irq_id = plic_claim();
        if (irq_id == UART0_IRQ)
            uart_handle_irq();
        if (irq_id != 0)
            plic_complete(irq_id);
    } else if (irq == IRQ_S_SOFT) {
        w_sip(r_sip() & ~SIE_SSIE);
        timer_set_interval(TICKS_PER_SEC / 100);
        if (from_user) proc_yield();
    } else {
        kdebug("TRAP IRQ: irq=%d sepc=0x%lx\n", (int)irq, sepc);
    }
}

void trap_handler(trap_context_t *ctx) {
    uint64_t scause = r_scause();
    uint64_t stval = r_stval();
    uint64_t sepc = r_sepc();

    ctx->x[0] = r_satp();

    if (scause & CAUSE_INTR_MASK) {
        handle_irq(scause & CAUSE_CODE_MASK, sepc, 1);
    } else {
        uint64_t code = scause & CAUSE_CODE_MASK;
        if (code == CAUSE_ECALL_U) {
            ctx->sepc += 4;
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
                uint64_t *pte_sepc = pt_walk(cur->mm->pgdir, sepc, 0);
                pfn_t dbg_pfn = 0;
                if (pte_sepc && (*pte_sepc & PTE_V)) {
                    dbg_pfn = phys_to_pfn(SV39_PTE_ADDR(*pte_sepc));
                    uint8_t *kva = (uint8_t *)(SV39_PTE_ADDR(*pte_sepc) + PAGE_OFFSET + (sepc & 0xFFF));
                    insn = *(uint32_t *)kva;
                }
                uint64_t *pte_stval = pt_walk(cur->mm->pgdir, stval, 0);
                uint64_t stval_pte = pte_stval ? *pte_stval : 0;
                uint64_t *pgdir = cur->mm->pgdir;
                uint64_t pte2 = pgdir ? pgdir[0] : 0;
                uint64_t *l1 = (pte2 & PTE_V) ? PTE_TO_PTR(pte2) : NULL;
                uint64_t pte1 = l1 ? l1[0] : 0;
                uint64_t *l0 = (pte1 & PTE_V) ? PTE_TO_PTR(pte1) : NULL;
                uint64_t pte0 = l0 ? l0[0x2f] : 0;
                kerr("User Page Fault: pid=%d scause=0x%lx sepc=0x%lx stval=0x%lx insn=0x%08x ctx_sepc=0x%lx pfn=%u flags=%u rc=%u\n",
                        cur ? cur->pid : -1, scause, sepc, stval, insn, ctx->sepc,
                        (unsigned)dbg_pfn, dbg_pfn < pfa.total_frames ? pfa.meta[dbg_pfn].flags : 99,
                        dbg_pfn < pfa.total_frames ? pfa.meta[dbg_pfn].refcount : 99);
                kerr("[PTEWALK] pgdir=0x%lx pgdir[0]=0x%lx l1=0x%lx l1[0]=0x%lx l0=0x%lx l0[0x2f]=0x%lx stval_pte=0x%lx\n",
                        (unsigned long)pgdir, pte2, (unsigned long)l1, pte1, (unsigned long)l0, pte0, stval_pte);
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
    uint64_t scause = r_scause();
    uint64_t sepc = r_sepc();
    uint64_t stval = r_stval();

    if (scause & CAUSE_INTR_MASK) {
        handle_irq(scause & CAUSE_CODE_MASK, sepc, 0);
    } else {
        uint64_t code = scause & CAUSE_CODE_MASK;
        if (code == CAUSE_ECALL_U) {
            ctx->sepc += 4;
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
            kdebug("KERNEL TRAP: scause=0x%lx sepc=0x%lx stval=0x%lx\n",
                   scause, sepc, stval);
            kdebug("[KTRAP] pid=%d name=%s ra=0x%lx a0=0x%lx a1=0x%lx a2=0x%lx\n",
                   cur2 ? cur2->pid : -1, cur2 ? cur2->name : "?",
                   ctx->x[1], ctx->x[10], ctx->x[11], ctx->x[12]);
            if (cur && cur->mm && stval < 0x4000000000UL) {
                proc_exit(-SIGSEGV);
            }
            panic("Unhandled kernel trap");
        } else {
            task_t *cur = proc_current();
            kdebug("KERNEL TRAP: scause=0x%lx sepc=0x%lx stval=0x%lx\n",
                   scause, sepc, stval);
            kdebug("[KTRAP] pid=%d name=%s ra=0x%lx a0=0x%lx a1=0x%lx a2=0x%lx\n",
                   cur ? cur->pid : -1, cur ? cur->name : "?",
                   ctx->x[1], ctx->x[10], ctx->x[11], ctx->x[12]);
            panic("Unhandled kernel trap");
        }
    }
}
