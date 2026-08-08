/*
 * LoongArch 64 architecture extension hooks: Linux ABI ptrace register-file
 * layout and drvmod MMIO direct-map window validation.  Architecture
 * specifics stay in kernel/arch, never in common code.
 */

#include "abi/linux/ptrace_layout.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "core/trap.h"
#include "proc/debug_regs.h"

/* Identity-mapped (PAGE_OFFSET == 0): VA == PA, low-4 GiB window. */
int arch_drv_mmio_window_ok(uintptr_t phys, size_t size)
{
    if (size == 0)
        return 1;
    return phys + size > phys && phys + size <= 0x100000000ULL;
}

void arch_dump_trap_extra_context(const trap_context_t *ctx)
{
    if (!ctx)
        return;
    for (int i = 10; i < 30; i += 4)
        kerr("  regs: r%d=0x%lx r%d=0x%lx r%d=0x%lx r%d=0x%lx\n",
             i, (unsigned long)TRAP_CTX_REG(ctx, i),
             i + 1, (unsigned long)TRAP_CTX_REG(ctx, i + 1),
             i + 2, (unsigned long)TRAP_CTX_REG(ctx, i + 2),
             i + 3, (unsigned long)TRAP_CTX_REG(ctx, i + 3));
    kerr("  regs: r30=0x%lx r31=0x%lx\n",
         (unsigned long)TRAP_CTX_REG(ctx, 30),
         (unsigned long)TRAP_CTX_REG(ctx, 31));
}

/* struct user_pt_regs: regs[32]; orig_a0; csr_era; csr_badv; reserved[10]; */
int linux_arch_regs_count(void) { return 44; }
int linux_arch_fpregs_count(void) { return 64; }

void linux_arch_regs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    for (int i = 0; i < 32; i++)
        out[i] = in->regs[i];
    out[32] = in->orig_syscall;
    out[33] = in->pc;
    out[34] = 0;
    for (int i = 35; i < 44; i++)
        out[i] = 0;
}

int linux_arch_regs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    for (int i = 0; i < 32; i++)
        in->regs[i] = r[i];
    in->orig_syscall = r[32];
    in->pc = r[33];
    return 0;
}

void linux_arch_fpregs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    (void)in;
    for (int i = 0; i < 64; i++)
        out[i] = 0;
}

void linux_arch_fpregs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    (void)in;
    (void)r;
}

int linux_arch_has_peekuser(void) { return 0; }

unsigned long linux_arch_user_offset(int field)
{
    (void)field;
    return 0;
}
