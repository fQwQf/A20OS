/*
 * ARM 32 architecture extension hooks: Linux ABI ptrace register-file
 * layout and drvmod MMIO direct-map window validation.  Architecture
 * specifics stay in kernel/arch, never in common code.
 */

#include "abi/linux/ptrace_layout.h"
#include "core/errno.h"
#include "proc/debug_regs.h"

/* ARM32 has no drvmod packages; reject framework MMIO mapping. */
int arch_drv_mmio_window_ok(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    return 0;
}

/* No Linux ptrace register-file translation yet. */
int linux_arch_regs_count(void) { return 0; }
int linux_arch_fpregs_count(void) { return 0; }

void linux_arch_regs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    (void)in;
    (void)out;
}

int linux_arch_regs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    (void)in;
    (void)r;
    return -EIO;
}

void linux_arch_fpregs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    (void)in;
    (void)out;
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
