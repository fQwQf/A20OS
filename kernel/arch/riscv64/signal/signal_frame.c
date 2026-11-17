/* A signal restorer is code, not writable stack data. The ordinary VMO/VMA
 * lifetime owns this RX page across fork, exec failure and task teardown. */
#include "proc/signal.h"
#include "mm/vm.h"
#include "mm/frame.h"
#include "mm/vmo.h"
#include "core/arch.h"

#ifdef CONFIG_NOMMU
void arch_setup_signal_trampoline(struct mm_struct *mm)
{
    if (!mm || mm->sig_tramp) return;
    /* NOMMU stacks are flat allocations, not page-table VMAs that can be
     * upgraded to executable during signal delivery. Keep the restorer in
     * its own ordinary mapping, just as on the MMU path. */
    vaddr_t addr = mm_mmap(mm, 0, PAGE_SIZE, PROT_READ | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS);
    if (mm_addr_is_error(addr) || !addr) return;
    arch_signal_write_trampoline((void *)(uintptr_t)addr);
    arch_flush_icache_range((void *)(uintptr_t)addr, 8);
    mm->sig_tramp = addr;
}
#else
void arch_setup_signal_trampoline(struct mm_struct *mm)
{
    if (mm->sig_tramp) return;
    struct vmo *code = vmo_create(VMO_ANONYMOUS, PAGE_SIZE, 0);
    if (!code) return;
    pfn_t pfn = vmo_get_page(code, 0);
    if (pfn == PFN_NONE) {
        vmo_release(code);
        return;
    }
    void *page = pfn_to_virt(pfn);
    arch_signal_write_trampoline(page);
    arch_flush_icache_range(page, 8);
    mm_tlb_invalidate_begin(mm);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    vaddr_t addr = mm_mmap_vmo_cap_locked(mm, 0, PAGE_SIZE, PROT_READ | PROT_EXEC,
                                         MAP_PRIVATE, code, 0, PROT_READ | PROT_EXEC, NULL);
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_tlb_invalidate_finish(mm);
    vmo_release(code); /* a successful VMA owns its pin; failure frees the page */
    if (!mm_addr_is_error(addr) && addr)
        mm->sig_tramp = addr;
}
#endif

uint64_t arch_signal_tramp_addr(struct mm_struct *mm, uint64_t stack_tramp_addr)
{
    /* Allocation failure cannot relax W^X: the generic delivery rejects an
     * in-frame executable stack under deny, terminating the affected task. */
    return mm && mm->sig_tramp ? mm->sig_tramp : stack_tramp_addr;
}
