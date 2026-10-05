/*
 * A20OS aarch64 — signal return trampoline page
 *
 * On aarch64 a signal handler returns with RET, so the return address comes out
 * of x30.  The generic delivery in kernel/proc/signal.c fills x30 with
 * arch_signal_tramp_addr(); this file supplies the answer.
 *
 * The obvious answer is the trampoline slot inside the signal frame, which is
 * what most architectures use and what the weak default still returns.  That
 * does not work here: the signal frame is built on the user stack, so its page
 * is writable by definition, and SCTLR_EL1.WXN (set in arch/aarch64/mm/kwx.c
 * once the kernel image has been split into RO-X/RO-NX/RW-NX segments) forces
 * every EL0-writable stage-1 descriptor to be execute-never at EL0.  The leaf
 * therefore had to be AP=01 (EL0 read/write) to stay a usable stack page, and
 * the CPU then refused to fetch the trampoline from it -- ESR EC=0x20
 * (instruction abort from a lower EL) with FSC=0x0f, i.e. a permission fault
 * rather than a translation fault, even though the software PTE carried PTE_X.
 * The only difference from a text leaf that executes correctly was AP=01 versus
 * AP=11, so no flag combination can make one page both a live stack and an
 * executable trampoline.
 *
 * So the trampoline gets its own page, outside the stack, mapped read-only and
 * executable: RO+X is exactly what WXN permits.  This mirrors what x86_64 does
 * with X86_64_SIGRET_TRAMP_ADDR in arch/x86_64/signal/signal_frame.c, but
 * without copying its fixed address.  x86_64's USER_VA_LIMIT is 2^47 and aarch64's
 * is only 2^46 (kernel/arch/aarch64/include/platform.h), so the same constant
 * would sit above the aarch64 user limit and be rejected by mm_mmap(); instead
 * the address comes from the first free gap, which keeps working whatever a
 * board sets USER_VA_LIMIT to.
 *
 * The page is exposed as an ordinary anonymous executable VMA, so it is torn
 * down with the rest of the address space and needs no special-casing on exit.
 */

#include "proc/signal.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/string.h"
#include "page_table.h"

/*
 * Map the dedicated R-X trampoline page into a new address space.  Called once
 * per mm (exec and first process creation), long before any signal can be
 * delivered, so the address stored in mm->sig_tramp is stable by the time the
 * first delivery reads it.
 */
void arch_setup_signal_trampoline(struct mm_struct *mm)
{
    mm_struct_t *m = (mm_struct_t *)mm;
    if (!m || !m->pgdir || m->sig_tramp)
        return;

    /*
     * No MAP_FIXED: let mm_mmap() pick the first free gap.  The VMA it leaves
     * behind is what keeps the address reserved for the life of the mm --
     * mm_find_gap() skips occupied ranges, so an ordinary mmap() later in the
     * process cannot land here.
     */
    vaddr_t addr = mm_mmap(m, 0, PAGE_SIZE,
                           PROT_READ | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS);
    if (mm_addr_is_error(addr) || !addr)
        return;

    /*
     * The trampoline is position-independent: it only has to put
     * __NR_rt_sigreturn (139) into x8 and trap.  It reads nothing, so it does
     * not care that sp still points at the signal frame rather than here.
     */
#ifdef CONFIG_NOMMU
    /*
     * Under NOMMU there is no page table to publish a separate frame through:
     * pt_map() is a no-op, so the VMA address already *is* the mapping.
     * Staging the code in a frame_alloc() page and pointing m->sig_tramp at
     * the VMA would leave the first signal a process ever takes returning
     * through x30 into the zeroed region mm_mmap() handed back -- a udf #0,
     * reported as SIGILL.  Write through the VMA address itself.  This is the
     * same reasoning that retires the bogus vDSO advertisement in
     * vdso_auxv_ehdr() (kernel/mm/vdso.c), and it uses
     * arch_signal_write_trampoline(), which existed for this and had no
     * caller.  It also drops a per-exec leaked buddy page.
     */
    arch_signal_write_trampoline((void *)(uintptr_t)addr);
#else
    void *page = frame_alloc();
    if (!page)
        return;
    memset(page, 0, PAGE_SIZE);

    arch_signal_prepare_trampoline((uint32_t *)page);

    pt_map(m->pgdir, addr, va_to_pa(page), arch_signal_tramp_pte_flags());
#endif
    m->sig_tramp = addr;
}

/*
 * The handler returns to the dedicated page.  If setup could not map one (a
 * frame allocation failure on a memory-tight target), fall back to the
 * in-frame slot: on aarch64 that page is writable and therefore execute-never,
 * so the delivery takes a reported SIGSEGV instead of running wild, which is
 * still the safer outcome.
 */
uint64_t arch_signal_tramp_addr(struct mm_struct *mm, uint64_t stack_tramp_addr)
{
    mm_struct_t *m = (mm_struct_t *)mm;
    if (!m || !m->sig_tramp)
        return stack_tramp_addr;
    return m->sig_tramp;
}
