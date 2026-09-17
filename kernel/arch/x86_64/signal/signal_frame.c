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
 * RET, which pops the return address from the top-of-stack word.  We place the
 * address of a dedicated, executable trampoline page there (see the delivery
 * path in signal.c, which puts it 8 bytes below the 16-aligned frame so the
 * handler is entered with rsp ≡ 8 (mod 16) per the SysV ABI).  The handler's
 * RET pops that word, leaving rsp pointing at the frame base, and the
 * trampoline invokes rt_sigreturn.
 */
void arch_signal_prepare_frame(arch_sig_rt_frame_t *frame, uint64_t tramp_addr,
                                trap_context_t *ctx)
{
    (void)ctx;
    frame->flag = X86_64_SIGRET_TRAMP_ADDR;
    /* fpregs must be the *user* address of the fpstate: the frame is copied to
     * user memory, so a kernel-side __fpregs_mem address would be wrong there.
     * The trampoline lives in that frame, so recover the frame base from it. */
    uint64_t frame_base = tramp_addr - arch_sigframe_tramp_offset();
    frame->uc.uc_mcontext.fpregs = frame_base + arch_sigframe_fpstate_offset();
}

/*
 * The SysV ABI requires a function -- and a signal handler -- to be entered
 * with rsp ≡ 8 (mod 16), as if reached through a call that pushed an 8-byte
 * return address onto a 16-aligned stack.  The signal frame base is kept
 * 16-aligned so the embedded fxsave64 area stays aligned, so the handler is
 * entered 8 bytes below it.  Without this, SSE-using handlers (musl/HotSpot
 * prologues doing `movaps [rsp+X], xmmN`) take a #GP on a misaligned stack.
 */
uint64_t arch_signal_handler_sp(uint64_t frame_sp)
{
    return frame_sp - 8;
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
    if (mm_addr_is_error((vaddr_t)r))
        return;

    void *page = frame_alloc();
    if (!page)
        return;
    memset(page, 0, PAGE_SIZE);

    /* The handler's RET already leaves rsp pointing at the signal frame base,
     * so the trampoline only needs to invoke rt_sigreturn. */
    uint8_t *p = (uint8_t *)page;
    p[0] = 0xB8;         /* mov imm32,%eax */
    p[1] = 15;           /* rt_sigreturn */
    p[2] = 0;
    p[3] = 0;
    p[4] = 0;
    p[5] = 0x0F;         /* syscall */
    p[6] = 0x05;

    paddr_t pa = va_to_pa(page);
    pt_map(m->pgdir, addr, pa,
           PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D);
}
