/*
 * RISC-V 64 architecture extension hooks: Linux ABI ptrace register-file
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

/* struct user_regs_struct (riscv64): unsigned long pc; unsigned long regs[31]; */
int linux_arch_regs_count(void) { return 32; }
int linux_arch_fpregs_count(void) { return 33; }

void linux_arch_regs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    out[0] = in->pc;
    for (int i = 1; i < 32; i++)
        out[i] = in->regs[i];
}

int linux_arch_regs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    in->pc = r[0];
    for (int i = 1; i < 32; i++)
        in->regs[i] = r[i];
    return 0;
}

/* struct user_fpregs_struct: 33 u64 (32 FP regs + fcsr). */
void linux_arch_fpregs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    for (int i = 0; i < 32; i++)
        out[i] = in->fp[i];
    out[32] = in->fcsr;
}

void linux_arch_fpregs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    for (int i = 0; i < 32; i++)
        in->fp[i] = r[i];
    in->fcsr = r[32];
}

int linux_arch_has_peekuser(void) { return 0; }

unsigned long linux_arch_user_offset(int field)
{
    (void)field;
    return 0;
}
