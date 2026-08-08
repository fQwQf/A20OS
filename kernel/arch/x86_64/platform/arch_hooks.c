/*
 * x86_64 architecture extension hooks.
 *
 * Provides the Linux ABI ptrace register-file layout and the drvmod MMIO
 * direct-map window validation.  This file exists because architecture
 * specifics must stay in kernel/arch, never in common code.
 */

#include "abi/linux/ptrace_layout.h"
#include "proc/debug_regs.h"

/* ---- drvmod MMIO window: low-4 GiB direct-map window ---- */

int arch_drv_mmio_window_ok(uintptr_t phys, size_t size)
{
    if (size == 0)
        return 1;
    return phys + size > phys && phys + size <= 0x100000000ULL;
}

/* ---- Linux ABI ptrace register layouts ---- */

int linux_arch_regs_count(void) { return 27; }
int linux_arch_fpregs_count(void) { return 64; }

/* struct user_regs_struct (x86_64): 27 u64:
 *   r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rax rcx rdx rsi rdi
 *   orig_rax rip cs eflags rsp ss fs_base gs_base ds es fs gs
 */
void linux_arch_regs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    unsigned long u[16];
    u[0]  = in->regs[0];   /* rax */
    u[1]  = in->regs[1];   /* rbx */
    u[2]  = in->regs[2];   /* rcx */
    u[3]  = in->regs[3];   /* rdx */
    u[4]  = in->regs[4];   /* rsi */
    u[5]  = in->regs[5];   /* rdi */
    u[6]  = in->regs[6];   /* rbp */
    u[7]  = in->regs[7];   /* rsp */
    u[8]  = in->regs[8];   /* r8 */
    u[9]  = in->regs[9];   /* r9 */
    u[10] = in->regs[10];  /* r10 */
    u[11] = in->regs[11];  /* r11 */
    u[12] = in->regs[12];  /* r12 */
    u[13] = in->regs[13];  /* r13 */
    u[14] = in->regs[14];  /* r14 */
    u[15] = in->regs[15];  /* r15 */
    out[0]  = u[15];
    out[1]  = u[14];
    out[2]  = u[13];
    out[3]  = u[12];
    out[4]  = u[6];
    out[5]  = u[1];
    out[6]  = u[11];
    out[7]  = u[10];
    out[8]  = u[9];
    out[9]  = u[8];
    out[10] = u[0];
    out[11] = u[2];
    out[12] = u[3];
    out[13] = u[4];
    out[14] = u[5];
    out[15] = in->orig_syscall;
    out[16] = in->pc;
    out[17] = 0x33;              /* cs (user) */
    out[18] = in->status;        /* eflags */
    out[19] = in->sp;
    out[20] = 0x2b;              /* ss (user) */
    out[21] = 0;                 /* fs_base */
    out[22] = 0;                 /* gs_base */
    out[23] = 0;
    out[24] = 0;
    out[25] = 0;
    out[26] = 0;
}

int linux_arch_regs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    unsigned long u[16];
    u[0]  = r[10];   /* rax */
    u[1]  = r[5];    /* rbx */
    u[2]  = r[11];   /* rcx */
    u[3]  = r[12];   /* rdx */
    u[4]  = r[13];   /* rsi */
    u[5]  = r[14];   /* rdi */
    u[6]  = r[4];    /* rbp */
    u[7]  = r[19];   /* rsp */
    u[8]  = r[9];    /* r8 */
    u[9]  = r[8];    /* r9 */
    u[10] = r[7];    /* r10 */
    u[11] = r[6];    /* r11 */
    u[12] = r[3];    /* r12 */
    u[13] = r[2];    /* r13 */
    u[14] = r[1];    /* r14 */
    u[15] = r[0];    /* r15 */
    in->regs[0]  = u[0];
    in->regs[1]  = u[1];
    in->regs[2]  = u[2];
    in->regs[3]  = u[3];
    in->regs[4]  = u[4];
    in->regs[5]  = u[5];
    in->regs[6]  = u[6];
    in->regs[7]  = u[7];
    in->regs[8]  = u[8];
    in->regs[9]  = u[9];
    in->regs[10] = u[10];
    in->regs[11] = u[11];
    in->regs[12] = u[12];
    in->regs[13] = u[13];
    in->regs[14] = u[14];
    in->regs[15] = u[15];
    in->pc = r[16];
    in->status = r[18];
    return 0;
}

/* struct user_fpregs_struct: 512-byte fxsave layout; the kernel does not
 * save x87/SSE state in the trap frame, so the image is zero-filled. */
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

int linux_arch_has_peekuser(void) { return 1; }

unsigned long linux_arch_user_offset(int field)
{
    switch (field) {
    case LINUX_USER_OFFSET_ORIG_RAX: return 15 * sizeof(unsigned long);
    case LINUX_USER_OFFSET_RIP:      return 16 * sizeof(unsigned long);
    case LINUX_USER_OFFSET_EFLAGS:   return 18 * sizeof(unsigned long);
    case LINUX_USER_OFFSET_RSP:      return 19 * sizeof(unsigned long);
    case LINUX_USER_OFFSET_FS_BASE:  return 21 * sizeof(unsigned long);
    case LINUX_USER_OFFSET_GS_BASE:  return 22 * sizeof(unsigned long);
    default:                         return 0;
    }
}
