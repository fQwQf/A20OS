/*
 * AArch64 architecture extension hooks: Linux ABI ptrace register-file
 * layout and drvmod MMIO direct-map window validation.  Architecture
 * specifics stay in kernel/arch, never in common code.
 */

#include "abi/linux/ptrace_layout.h"
#include "proc/debug_regs.h"

int arch_drv_mmio_window_ok(uintptr_t phys, size_t size)
{
    if (size == 0)
        return 1;
    return phys + size > phys && phys + size <= 0x80000000ULL;
}

/* struct user_pt_regs: unsigned long regs[31]; sp; pc; pstate; */
int linux_arch_regs_count(void) { return 34; }
int linux_arch_fpregs_count(void) { return 66; }

void linux_arch_regs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    for (int i = 0; i < 31; i++)
        out[i] = in->regs[i];
    out[31] = in->sp;
    out[32] = in->pc;
    out[33] = in->status;
}

int linux_arch_regs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    for (int i = 0; i < 31; i++)
        in->regs[i] = r[i];
    in->sp = r[31];
    in->pc = r[32];
    in->status = r[33];
    return 0;
}

/* struct user_fpsimd_state: 64 vregs + fpsr + fpcr. */
void linux_arch_fpregs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    for (int i = 0; i < 64; i++)
        out[i] = in->fp[i];
    out[64] = 0; /* fpsr */
    out[65] = 0; /* fpcr */
}

void linux_arch_fpregs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    for (int i = 0; i < 64; i++)
        in->fp[i] = r[i];
}

int linux_arch_has_peekuser(void) { return 0; }

unsigned long linux_arch_user_offset(int field)
{
    (void)field;
    return 0;
}
