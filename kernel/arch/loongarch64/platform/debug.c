#if defined(CONFIG_LOONGARCH64) && defined(CONFIG_DEBUG_BOOT_TRACE)

/*
 * LoongArch register dumps behind the two optional arch hooks.
 *
 * Both dumps are bring-up diagnostics for a board whose firmware hands control
 * over in a state nobody has characterised yet.  They used to live inline in
 * kernel/core/panic.c and kernel/proc/task.c, which put LoongArch CSR numbers
 * and an LS2K1000 board test inside architecture-neutral files.  The generic
 * callers now say only "dump what you can interpret"; this file is the only
 * place that knows what a CRMD is.
 *
 * The whole file compiles only when CONFIG_DEBUG_BOOT_TRACE is set, which the
 * build does for the LS2K1000 cooperative recovery profile and nowhere else.
 */

#include "core/arch.h"
#include "core/stdio.h"


/*
 * CSR numbers are passed as immediates, not in a register.  A helper taking the
 * number as an argument lets GCC allocate the same register for the destination
 * and the CSR number -- `csrrd $r8, $r8` -- which this assembler rejects, and
 * which would be ambiguous anyway.  The immediate form is what the inline reads
 * this replaced used.
 */
#define LA_CSR(csr) ({ uint64_t _v; \
    __asm__ __volatile__("csrrd %0, " #csr : "=r"(_v)); _v; })

void arch_panic_dump(void)
{
    uint64_t sp;
    __asm__ __volatile__("move %0, $sp" : "=r"(sp));

    /* crmd holds CRMD.IE and CRMD.PLV, which together say whether the panic
     * happened with interrupts on and at which privilege level -- the first
     * two questions anyone asks about a LoongArch panic. */
    printf("[LA-DIAG] sp=0x%lx crmd=0x%lx prmd=0x%lx era=0x%lx badv=0x%lx\n",
           (unsigned long)sp,
           (unsigned long)LA_CSR(0x0),
           (unsigned long)LA_CSR(0x1),
           (unsigned long)LA_CSR(0x6),
           (unsigned long)LA_CSR(0x7));

    /* The TLBR registers describe a TLB refill that faulted.  On a board whose
     * firmware enters the kernel through an unexpected path these are usually
     * what explains it. */
    printf("[LA-DIAG] tlbrera=0x%lx tlbrbadv=0x%lx tlbrehi=0x%lx"
           " tlbrlo0=0x%lx tlbrlo1=0x%lx\n",
           (unsigned long)LA_CSR(0x8a),
           (unsigned long)LA_CSR(0x89),
           (unsigned long)LA_CSR(0x8e),
           (unsigned long)LA_CSR(0x8c),
           (unsigned long)LA_CSR(0x8d));
}

void arch_debug_dump_user_state(void)
{
    /* Read at a context switch back to user: PGDL/PWCL/PWCH describe the page
     * table base and the windowed TLB bounds, and EENTRY is where the next user
     * exception will land.  If the first user instruction traps unexpectedly,
     * the difference between these and what the boot firmware left behind is
     * the whole story. */
    printf("[LA-DIAG] crmd=0x%lx eentry=0x%lx tlbrentry=0x%lx pgdl=0x%lx\n",
           (unsigned long)LA_CSR(0x0),
           (unsigned long)LA_CSR(0xc),
           (unsigned long)LA_CSR(0x88),
           (unsigned long)LA_CSR(0x19));
    printf("[LA-DIAG] pwcl=0x%lx pwch=0x%lx tlbrehi=0x%lx prcfg1=0x%lx\n",
           (unsigned long)LA_CSR(0x1c),
           (unsigned long)LA_CSR(0x1d),
           (unsigned long)LA_CSR(0x8e),
           (unsigned long)LA_CSR(0x21));
}

#endif /* CONFIG_LOONGARCH64 && CONFIG_DEBUG_BOOT_TRACE */