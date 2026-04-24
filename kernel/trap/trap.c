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

    if (*pte & PTE_COW) {
        paddr_t old_pa = arch_pte_addr(*pte);
        pfn_t old_pfn = phys_to_pfn(old_pa);
        if (!pfn_valid(old_pfn)) return -1;

        uint16_t rc = pfa.meta[old_pfn].refcount;
        if (rc > 1) {
            pfn_t new_pfn = pfa_alloc_page();
            if (new_pfn == PFN_NONE) return -1;
            memcpy(pfn_to_virt(new_pfn), pfn_to_virt(old_pfn), PAGE_SIZE);
            frame_put(old_pfn);
            *pte = arch_pte_from_pa(pfn_to_phys(new_pfn)) | (*pte & (PTE_R | PTE_X | PTE_U | PTE_A | PTE_MAT1 | PTE_LEAF)) | PTE_W | PTE_D | PTE_V;
        } else {
            *pte = (*pte & ~PTE_COW) | PTE_W | PTE_D;
        }
        arch_tlb_flush_page(stval);
        return 0;
    }

    if (*pte & PTE_W) {
        *pte |= PTE_D;
        arch_tlb_flush_page(stval);
        return 0;
    }

    return -1;
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

            uint64_t pte_flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF;
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

        uint64_t pte_flags = PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF;
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
    /*
     * ECFG (CSR 0x4) — Local Interrupt Enable:
     *   IS[11] = TI  (Timer Interrupt)
     *   IS[2]  = HWI0 (Hardware Interrupt 0, used by Virtio/PCIe on QEMU virt)
     *
     * Without enabling IS[2], Virtio block-device completion interrupts pile
     * up in ESTAT.IS but are never delivered, causing the device to stall.
     */
    uint64_t ecfg;
    __asm__ __volatile__("csrrd %0, 0x4" : "=r"(ecfg));
    ecfg |= (1UL << 11) | (1UL << 2);
    __asm__ __volatile__("csrwr %0, 0x4" :: "r"(ecfg));
#else
    arch_write_tvec((uint64_t)__trap_from_kernel);
    arch_write_sscratch(0);
#endif
}

static int ktrap_diag_count = 0;

static void handle_irq(uint64_t irq, uint64_t sepc, int from_user) {
    if (irq == IRQ_S_TIMER) {
#ifdef CONFIG_LOONGARCH64
        /*
         * LoongArch timer interrupt must be acknowledged via TICLR (CSR 0x44)
         * before re-programming the interval, otherwise the CPU will
         * immediately re-enter the timer handler in an infinite loop.
         */
        __asm__ __volatile__("csrwr %0, 0x44" :: "r"(1UL) : "memory");
#endif
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
#ifdef CONFIG_LOONGARCH64
    } else if (irq == IRQ_S_EXT) {
        uart_handle_irq();
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

    {
        static int first_user_trap = 0;
        if (first_user_trap < 20) {
            first_user_trap++;
            task_t *cur = proc_current();
            uint64_t code_check = scause & CAUSE_CODE_MASK;
            kdebug("[UTRAP#%d] pid=%d scause=0x%lx sepc=0x%lx stval=0x%lx a7=%lu\n",
                   first_user_trap, cur ? cur->pid : -1, scause, sepc, stval,
                   (unsigned long)TRAP_CTX_SYSCALL_NUM(ctx));
            if (code_check == CAUSE_STORE_PAGE_FAULT || code_check == CAUSE_LOAD_PAGE_FAULT) {
#ifdef CONFIG_LOONGARCH64
                kdebug("  >> a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx\n",
                       (unsigned long)TRAP_CTX_ARG0(ctx), (unsigned long)TRAP_CTX_ARG1(ctx),
                       (unsigned long)TRAP_CTX_ARG2(ctx), (unsigned long)TRAP_CTX_ARG3(ctx));
                kdebug("  >> t0=0x%lx t1=0x%lx t2=0x%lx t3=0x%lx t4=0x%lx t5=0x%lx\n",
                       (unsigned long)ctx->regs[12], (unsigned long)ctx->regs[13],
                       (unsigned long)ctx->regs[14], (unsigned long)ctx->regs[15],
                       (unsigned long)ctx->regs[16], (unsigned long)ctx->regs[17]);
                kdebug("  >> t6=0x%lx t7=0x%lx t8=0x%lx tp=0x%lx sp=0x%lx ra=0x%lx\n",
                       (unsigned long)ctx->regs[18], (unsigned long)ctx->regs[19],
                       (unsigned long)ctx->regs[20], (unsigned long)TRAP_CTX_TP(ctx),
                       (unsigned long)TRAP_CTX_SP(ctx), (unsigned long)TRAP_CTX_RA(ctx));
#endif
            }
        }
    }

    if (scause & CAUSE_INTR_MASK) {
        handle_irq(scause & CAUSE_CODE_MASK, sepc, 1);
    } else {
        uint64_t code = scause & CAUSE_CODE_MASK;
        if (code == CAUSE_ECALL_U) {
            TRAP_CTX_EPC(ctx) += 4;
            syscall_dispatch(ctx);
        } else if (code == CAUSE_LOAD_PAGE_FAULT || code == CAUSE_STORE_PAGE_FAULT || code == CAUSE_INSN_PAGE_FAULT) {
            task_t *cur = proc_current();
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
#ifdef CONFIG_LOONGARCH64
            /* Dump instruction at sepc and register state for crash diagnosis */
            if (cur && cur->mm && cur->mm->pgdir) {
                uint64_t *pte_sepc = pt_walk(cur->mm->pgdir, sepc, 0);
                if (pte_sepc && (*pte_sepc & PTE_V)) {
                    uint8_t *insn_kva = (uint8_t *)(arch_pte_addr(*pte_sepc) + PAGE_OFFSET + (sepc & 0xFFF));
                    uint32_t insn = *(uint32_t *)insn_kva;
                    kerr("  insn@sepc=0x%08x  ", insn);
                    /* Decode common LoongArch store patterns */
                    uint32_t opcode = insn & 0xFF;
                    if (opcode >= 0x20 && opcode <= 0x27) {
                        uint32_t rj = (insn >> 5) & 0x1F;
                        uint32_t rk = (insn >> 10) & 0x1F;
                        uint32_t rd = (insn >> 15) & 0x1F;
                        kerr("st.%s $r%d, $r%d, $r%d",
                             opcode == 0x20 ? "b" : opcode == 0x21 ? "h" :
                             opcode == 0x22 ? "w" : opcode == 0x23 ? "d" :
                             opcode == 0x24 ? "xb" : opcode == 0x25 ? "xh" :
                             opcode == 0x26 ? "xw" : "xd",
                             rd, rj, rk);
                    } else if ((insn & 0xFE000000) == 0x28000000 ||
                               (insn & 0xFE000000) == 0x2C000000) {
                        /* stptr.w / stptr.d: [28/2C] rd, rj, si12 */
                        uint32_t rd = (insn >> 5) & 0x1F;
                        uint32_t rj = (insn >> 10) & 0x1F;
                        int32_t si12 = ((int32_t)(insn >> 10)) >> 20; /* sign-extend bits[21:10] */
                        kerr("stptr.? $r%d, $r%d, %d", rd, rj, si12);
                    }
                    kerr("\n");
                }
                kerr("  regs: a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx\n",
                     (unsigned long)TRAP_CTX_ARG0(ctx), (unsigned long)TRAP_CTX_ARG1(ctx),
                     (unsigned long)TRAP_CTX_ARG2(ctx), (unsigned long)TRAP_CTX_ARG3(ctx));
                kerr("  regs: a4=0x%lx a5=0x%lx a6=0x%lx a7=0x%lx\n",
                     (unsigned long)ctx->regs[8], (unsigned long)ctx->regs[9],
                     (unsigned long)ctx->regs[10], (unsigned long)ctx->regs[11]);
                kerr("  regs: t0=0x%lx t1=0x%lx t2=0x%lx t3=0x%lx t4=0x%lx t5=0x%lx\n",
                     (unsigned long)ctx->regs[12], (unsigned long)ctx->regs[13],
                     (unsigned long)ctx->regs[14], (unsigned long)ctx->regs[15],
                     (unsigned long)ctx->regs[16], (unsigned long)ctx->regs[17]);
                kerr("  regs: t6=0x%lx t7=0x%lx t8=0x%lx tp=0x%lx sp=0x%lx fp=0x%lx ra=0x%lx\n",
                     (unsigned long)ctx->regs[18], (unsigned long)ctx->regs[19],
                     (unsigned long)ctx->regs[20], (unsigned long)TRAP_CTX_TP(ctx),
                     (unsigned long)TRAP_CTX_SP(ctx), (unsigned long)ctx->regs[22],
                     (unsigned long)TRAP_CTX_RA(ctx));
                kerr("  regs: s0=0x%lx s1=0x%lx s2=0x%lx s3=0x%lx s4=0x%lx\n",
                     (unsigned long)ctx->regs[23], (unsigned long)ctx->regs[24],
                     (unsigned long)ctx->regs[25], (unsigned long)ctx->regs[26],
                     (unsigned long)ctx->regs[27]);
                kerr("  regs: s5=0x%lx s6=0x%lx s7=0x%lx s8=0x%lx\n",
                     (unsigned long)ctx->regs[28], (unsigned long)ctx->regs[29],
                     (unsigned long)ctx->regs[30], (unsigned long)ctx->regs[31]);
                if (cur && cur->mm && cur->mm->pgdir) {
                    uint64_t *pte_sepc = pt_walk(cur->mm->pgdir, sepc, 0);
                    if (pte_sepc && (*pte_sepc & PTE_V)) {
                        uint8_t *insn_kva = (uint8_t *)(arch_pte_addr(*pte_sepc) + PAGE_OFFSET + (sepc & 0xFFF));
                        uint32_t insn = *(uint32_t *)insn_kva;
                        kerr("  insn=0x%08x", insn);
                        if (insn == 0x38103d80) {
                            uint64_t expected = (uint64_t)((int64_t)ctx->regs[12] + (int64_t)ctx->regs[15]);
                            kerr("  stx.b t0+t3=0x%lx %s stval\n",
                                 (unsigned long)expected,
                                 expected == stval ? "==" : "!=");
                        } else { kerr("\n"); }
                    }
                }
            }
            if (cur && cur->mm && cur->mm->pgdir) {
                uint64_t *pte = pt_walk(cur->mm->pgdir, stval, 0);
                kerr("  pte=%p *pte=0x%lx brk=0x%lx start_brk=0x%lx\n",
                     (void*)pte, pte ? *pte : 0UL,
                     (unsigned long)cur->mm->brk, (unsigned long)cur->mm->start_brk);
            }
#endif
            proc_exit(-SIGSEGV);
        } else if (code == CAUSE_PAGE_MODIFICATION) {
            /*
             * LoongArch PME: hardware write to a V=1, D=0 page.
             * Two cases:
             *   1. COW page: PTE_COW set, D=0 intentionally → allocate copy.
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
                uint64_t *pte = pt_walk(cur->mm->pgdir, stval, 0);
                if (pte && (*pte & PTE_V) && (*pte & PTE_W)) {
                    *pte |= PTE_D;   /* PTE_D == PTE_W == bit 1, set dirty */
                    arch_tlb_flush_page(stval);
                    return;
                }
            }
            {
                uint32_t insn = 0;
                pfn_t dbg_pfn = 0;
                uint64_t stval_pte = 0;
                if (cur && cur->mm && cur->mm->pgdir) {
                    uint64_t *pte_sepc = pt_walk(cur->mm->pgdir, sepc, 0);
                    if (pte_sepc && (*pte_sepc & PTE_V)) {
                        uint8_t *kva = (uint8_t *)(arch_pte_addr(*pte_sepc) + PAGE_OFFSET + (sepc & 0xFFF));
                        insn = *(uint32_t *)kva;
                        dbg_pfn = phys_to_pfn(arch_pte_addr(*pte_sepc));
                    }
                    uint64_t *pte_stval = pt_walk(cur->mm->pgdir, stval, 0);
                    stval_pte = pte_stval ? *pte_stval : 0;
                }
                kerr("User Page Fault: pid=%d scause=0x%lx sepc=0x%lx stval=0x%lx insn=0x%08x pte=0x%lx pfn=%u\n",
                        cur ? cur->pid : -1, scause, sepc, stval, insn, stval_pte, (unsigned)dbg_pfn);
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
        task_t *cur = proc_current();
        kdebug("KERNEL TRAP: scause=0x%lx sepc=0x%lx stval=0x%lx code=%lu\n",
               scause, sepc, stval, code);
        kdebug("[KTRAP] pid=%d name=%s ra=0x%lx a0=0x%lx era_ctx=0x%lx prmd_ctx=0x%lx\n",
                cur ? cur->pid : -1, cur ? cur->name : "?",
                TRAP_CTX_RA(ctx), TRAP_CTX_ARG0(ctx),
                TRAP_CTX_EPC(ctx), TRAP_CTX_STATUS(ctx));
        if (code == CAUSE_ECALL_U) {
            TRAP_CTX_EPC(ctx) += 4;
        } else if (code == CAUSE_LOAD_PAGE_FAULT ||
                   code == CAUSE_STORE_PAGE_FAULT ||
                   code == CAUSE_INSN_PAGE_FAULT ||
                   code == CAUSE_PAGE_MODIFICATION) {
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
                if (code == CAUSE_STORE_PAGE_FAULT || code == CAUSE_PAGE_MODIFICATION) {
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
