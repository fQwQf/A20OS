#include "proc/signal.h"
#include "core/trap.h"
#include "core/string.h"
#include "core/consts.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "page_table.h"

/* Fixed per-process user address for the signal return trampoline page. */
#define X86_64_SIGRET_TRAMP_ADDR  0x700000000000ULL

/*
 * x86_64 uses the normal C calling convention: a signal handler returns with
 * RET, which pops the return address from the top-of-stack word.  We therefore
 * replace the magic flag word at the frame base with the address of a
 * dedicated, executable trampoline page.  The trampoline first pushes %rax to
 * restore the stack pointer to the frame base, then invokes rt_sigreturn.
 */
void arch_signal_prepare_frame(sig_rt_frame_t *frame, uint64_t tramp_addr,
                               trap_context_t *ctx)
{
    (void)tramp_addr;
    (void)ctx;
    frame->flag = X86_64_SIGRET_TRAMP_ADDR;
}

/*
 * Map a dedicated RX page for the signal return trampoline into the new
 * process address space.  It is freed automatically with the mm because we
 * expose it as a normal anonymous executable VMA.
 */
void arch_setup_signal_trampoline(struct mm_struct *mm)
{
    mm_struct_t *m = (mm_struct_t *)mm;
    if (!m || !m->pgdir)
        return;

    uint64_t addr = X86_64_SIGRET_TRAMP_ADDR;
    uint64_t r = mm_mmap(m, addr, PAGE_SIZE,
                         PROT_READ | PROT_EXEC,
                         MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS);
    if (r == (uint64_t)-ENOMEM || r == (uint64_t)-EINVAL)
        return;

    void *page = frame_alloc();
    if (!page)
        return;
    memset(page, 0, PAGE_SIZE);

    uint8_t *p = (uint8_t *)page;
    p[0] = 0x50;         /* pushq %rax */
    p[1] = 0xB8;         /* mov imm32,%eax */
    p[2] = 15;           /* rt_sigreturn */
    p[3] = 0;
    p[4] = 0;
    p[5] = 0;
    p[6] = 0x0F;         /* syscall */
    p[7] = 0x05;

    paddr_t pa = va_to_pa(page);
    pt_map(m->pgdir, addr, pa,
           PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D);
}

