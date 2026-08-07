/*
 * A20OS — Linux ABI ptrace(2) wrapper.
 *
 * LINUX_ABI_BOUNDARY_CONTRACT: this file translates the Linux ptrace
 * request/argument shapes and per-arch struct user_regs_struct layouts into
 * the kernel-internal debugging interface (proc_debug_*, kernel/proc/
 * debug.c).  No ptrace state lives here; the kernel-internal layer owns the
 * trace state machine and is ABI-agnostic (a Native ABI debugger maps onto
 * the same surface).
 */

#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "proc/debug.h"

/* ptrace request numbers (Linux ABI) */
#define PTRACE_TRACEME      0
#define PTRACE_PEEKTEXT     1
#define PTRACE_PEEKDATA     2
#define PTRACE_PEEKUSER     3
#define PTRACE_POKETEXT     4
#define PTRACE_POKEDATA     5
#define PTRACE_POKEUSER     6
#define PTRACE_CONT         7
#define PTRACE_KILL         8
#define PTRACE_SINGLESTEP   9
#define PTRACE_GETREGS      12
#define PTRACE_SETREGS      13
#define PTRACE_GETFPREGS    14
#define PTRACE_SETFPREGS    15
#define PTRACE_ATTACH       16
#define PTRACE_DETACH       17
#define PTRACE_SYSCALL      24
#define PTRACE_SETOPTIONS   0x4200
#define PTRACE_GETEVENTMSG  0x4201
#define PTRACE_GETSIGINFO   0x4202
#define PTRACE_SETSIGINFO   0x4203
#define PTRACE_GETREGSET    0x4204
#define PTRACE_SETREGSET    0x4205

#define PTRACE_O_TRACESYSGOOD 0x00000001
#define PTRACE_O_TRACEEXEC    0x00000010
#define PTRACE_O_TRACEEXIT    0x00000040
#define PTRACE_O_EXITKILL     0x00100000

#define NT_PRSTATUS  1
#define NT_FPREGSET  2

/* ---- per-arch Linux register file layouts ---- */

#if defined(CONFIG_X86_64)
/*
 * struct user_regs_struct (x86_64): 27 u64:
 *   r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rax rcx rdx rsi rdi
 *   orig_rax rip cs eflags rsp ss fs_base gs_base ds es fs gs
 */
#define LINUX_REGS_COUNT 27
static void linux_regs_export(const proc_debug_regs_t *in, unsigned long *out)
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

static int linux_regs_import(proc_debug_regs_t *in, const unsigned long *r)
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
#define LINUX_FPREGS_COUNT 64

#elif defined(CONFIG_RISCV64) || defined(CONFIG_RISCV32)
/*
 * struct user_regs_struct (riscv): unsigned long pc; unsigned long regs[31];
 */
#define LINUX_REGS_COUNT 32
static void linux_regs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    out[0] = in->pc;
    for (int i = 1; i < 32; i++)
        out[i] = in->regs[i];
}

static int linux_regs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    in->pc = r[0];
    for (int i = 1; i < 32; i++)
        in->regs[i] = r[i];
    return 0;
}

/* struct user_fpregs_struct: 33 u64 (32 FP regs + fcsr). */
#define LINUX_FPREGS_COUNT 33

#elif defined(CONFIG_AARCH64)
/*
 * struct user_pt_regs: unsigned long regs[31]; sp; pc; pstate;
 */
#define LINUX_REGS_COUNT 34
static void linux_regs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    for (int i = 0; i < 31; i++)
        out[i] = in->regs[i];
    out[31] = in->sp;
    out[32] = in->pc;
    out[33] = in->status;
}

static int linux_regs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    for (int i = 0; i < 31; i++)
        in->regs[i] = r[i];
    in->sp = r[31];
    in->pc = r[32];
    in->status = r[33];
    return 0;
}

/* struct user_fpsimd_state: 64 vregs + fpsr + fpcr. */
#define LINUX_FPREGS_COUNT 66

#elif defined(CONFIG_LOONGARCH64)
/*
 * struct user_pt_regs: regs[32]; orig_a0; csr_era; csr_badv; reserved[10];
 */
#define LINUX_REGS_COUNT 44
static void linux_regs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    for (int i = 0; i < 32; i++)
        out[i] = in->regs[i];
    out[32] = in->orig_syscall;
    out[33] = in->pc;
    out[34] = 0;
    for (int i = 35; i < 44; i++)
        out[i] = 0;
}

static int linux_regs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    for (int i = 0; i < 32; i++)
        in->regs[i] = r[i];
    in->orig_syscall = r[32];
    in->pc = r[33];
    return 0;
}

#define LINUX_FPREGS_COUNT 64

#else
#define LINUX_REGS_COUNT 0
static void linux_regs_export(const proc_debug_regs_t *in, unsigned long *out)
{
    (void)in;
    (void)out;
}
static int linux_regs_import(proc_debug_regs_t *in, const unsigned long *r)
{
    (void)in;
    (void)r;
    return -EIO;
}
#define LINUX_FPREGS_COUNT 0
#endif

static void linux_fpregs_export(const proc_debug_regs_t *in, unsigned long *out)
{
#if defined(CONFIG_RISCV64) || defined(CONFIG_RISCV32)
    for (int i = 0; i < 32; i++)
        out[i] = in->fp[i];
    out[32] = in->fcsr;
#elif defined(CONFIG_AARCH64)
    for (int i = 0; i < 64; i++)
        out[i] = in->fp[i];
    out[64] = 0; /* fpsr */
    out[65] = 0; /* fpcr */
#else
    for (int i = 0; i < LINUX_FPREGS_COUNT; i++)
        out[i] = 0;
#endif
}

static void linux_fpregs_import(proc_debug_regs_t *in, const unsigned long *r)
{
#if defined(CONFIG_RISCV64) || defined(CONFIG_RISCV32)
    for (int i = 0; i < 32; i++)
        in->fp[i] = r[i];
    in->fcsr = r[32];
#elif defined(CONFIG_AARCH64)
    for (int i = 0; i < 64; i++)
        in->fp[i] = r[i];
#else
    (void)in;
    (void)r;
#endif
}

/* ---- PEEKUSER offsets for struct user (x86_64) ---- */
#if defined(CONFIG_X86_64)
#define LINUX_USER_OFFSET_ORIG_RAX (15 * sizeof(unsigned long))
#define LINUX_USER_OFFSET_RIP      (16 * sizeof(unsigned long))
#define LINUX_USER_OFFSET_EFLAGS   (18 * sizeof(unsigned long))
#define LINUX_USER_OFFSET_RSP      (19 * sizeof(unsigned long))
#define LINUX_USER_OFFSET_FS_BASE  (21 * sizeof(unsigned long))
#define LINUX_USER_OFFSET_GS_BASE  (22 * sizeof(unsigned long))
#endif

int64_t sys_ptrace(int request, int pid, void *addr, void *data)
{
    switch (request) {
    case PTRACE_TRACEME:
        return proc_debug_traceme();

    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA: {
        long val = 0;
        int ret = proc_debug_peek_word(pid, (uintptr_t)addr, &val);
        if (ret)
            return ret;
        return val;
    }

    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA:
        return proc_debug_poke_word(pid, (uintptr_t)addr, (long)data);

#if defined(CONFIG_X86_64)
    case PTRACE_PEEKUSER: {
        unsigned long off = (unsigned long)addr;
        if (off % sizeof(unsigned long) ||
            off / sizeof(unsigned long) >= LINUX_REGS_COUNT)
            return -EIO;
        proc_debug_regs_t regs;
        int ret = proc_debug_getregs(pid, &regs);
        if (ret)
            return ret;
        unsigned long buf[LINUX_REGS_COUNT];
        linux_regs_export(&regs, buf);
        return (int64_t)buf[off / sizeof(unsigned long)];
    }

    case PTRACE_POKEUSER: {
        unsigned long off = (unsigned long)addr;
        if (off % sizeof(unsigned long) ||
            off / sizeof(unsigned long) >= LINUX_REGS_COUNT)
            return -EIO;
        proc_debug_regs_t regs;
        int ret = proc_debug_getregs(pid, &regs);
        if (ret)
            return ret;
        unsigned long buf[LINUX_REGS_COUNT];
        linux_regs_export(&regs, buf);
        buf[off / sizeof(unsigned long)] = (unsigned long)data;
        int imp = linux_regs_import(&regs, buf);
        if (imp)
            return imp;
        return proc_debug_setregs(pid, &regs);
    }
#endif

    case PTRACE_CONT:
        return proc_debug_resume(pid, (int)(long)data,
                                 PT_DEBUG_RESUME_CONT);

    case PTRACE_SYSCALL:
        return proc_debug_resume(pid, (int)(long)data,
                                 PT_DEBUG_RESUME_SYSCALL);

    case PTRACE_SINGLESTEP:
        return proc_debug_singlestep(pid, (int)(long)data);

    case PTRACE_KILL:
        return proc_debug_kill(pid);

    case PTRACE_ATTACH:
        return proc_debug_attach(pid);

    case PTRACE_DETACH:
        return proc_debug_detach(pid, (int)(long)data);

    case PTRACE_GETREGS: {
        if (!data)
            return -EIO;
        proc_debug_regs_t regs;
        int ret = proc_debug_getregs(pid, &regs);
        if (ret)
            return ret;
        unsigned long buf[LINUX_REGS_COUNT];
        linux_regs_export(&regs, buf);
        return copy_to_user(data, buf, sizeof(buf)) < 0 ? -EFAULT : 0;
    }

    case PTRACE_SETREGS: {
        if (!data)
            return -EIO;
        unsigned long buf[LINUX_REGS_COUNT];
        if (copy_from_user(buf, data, sizeof(buf)) < 0)
            return -EFAULT;
        /* Fetch first: the Linux register layout carries only a subset of
         * the kernel register file (e.g. riscv64 omits sp/pstate); the
         * untouched fields must survive the update. */
        proc_debug_regs_t regs;
        int ret = proc_debug_getregs(pid, &regs);
        if (ret)
            return ret;
        ret = linux_regs_import(&regs, buf);
        if (ret)
            return ret;
        return proc_debug_setregs(pid, &regs);
    }

    case PTRACE_GETFPREGS: {
        if (!data)
            return -EIO;
        proc_debug_regs_t regs;
        int ret = proc_debug_getregs(pid, &regs);
        if (ret)
            return ret;
        unsigned long buf[LINUX_FPREGS_COUNT];
        linux_fpregs_export(&regs, buf);
        return copy_to_user(data, buf, sizeof(buf)) < 0 ? -EFAULT : 0;
    }

    case PTRACE_SETFPREGS: {
        if (!data)
            return -EIO;
        unsigned long buf[LINUX_FPREGS_COUNT];
        if (copy_from_user(buf, data, sizeof(buf)) < 0)
            return -EFAULT;
        proc_debug_regs_t regs;
        int ret = proc_debug_getregs(pid, &regs);
        if (ret)
            return ret;
        linux_fpregs_import(&regs, buf);
        return proc_debug_setregs(pid, &regs);
    }

    case PTRACE_SETOPTIONS: {
        unsigned long opts = (unsigned long)data;
        unsigned long known = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC |
                              PTRACE_O_TRACEEXIT | PTRACE_O_EXITKILL;
        unsigned long mapped = 0;
        if (opts & PTRACE_O_TRACESYSGOOD)
            mapped |= PT_DEBUG_FLAG_SYSGOOD;
        if (opts & PTRACE_O_TRACEEXEC)
            mapped |= PT_DEBUG_FLAG_TRACEEXEC;
        if (opts & PTRACE_O_TRACEEXIT)
            mapped |= PT_DEBUG_FLAG_TRACEEXIT;
        if (opts & PTRACE_O_EXITKILL)
            mapped |= PT_DEBUG_FLAG_EXITKILL;
        if (opts & ~known)
            return -EINVAL;
        return proc_debug_setoptions(pid, mapped);
    }

    case PTRACE_GETEVENTMSG: {
        if (!data)
            return -EIO;
        long msg = 0;
        int ret = proc_debug_geteventmsg(pid, &msg);
        if (ret)
            return ret;
        return copy_to_user(data, &msg, sizeof(msg)) < 0 ? -EFAULT : 0;
    }

    case PTRACE_GETSIGINFO:
    case PTRACE_SETSIGINFO: {
        arch_siginfo_t si;
        if (request == PTRACE_GETSIGINFO) {
            if (!data)
                return -EIO;
            int ret = proc_debug_getsiginfo(pid, &si, sizeof(si));
            if (ret)
                return ret;
            return copy_to_user(data, &si, sizeof(si)) < 0 ? -EFAULT : 0;
        }
        if (!data)
            return -EIO;
        if (copy_from_user(&si, data, sizeof(si)) < 0)
            return -EFAULT;
        return proc_debug_setsiginfo(pid, &si, sizeof(si));
    }

    case PTRACE_GETREGSET:
    case PTRACE_SETREGSET: {
        struct {
            void *base;
            size_t len;
        } iov;
        if (!data || copy_from_user(&iov, data, sizeof(iov)) < 0)
            return -EFAULT;
        int kind = (int)(long)addr;
        if (kind == NT_PRSTATUS) {
            if (request == PTRACE_GETREGSET) {
                size_t n = LINUX_REGS_COUNT * sizeof(unsigned long);
                if (iov.len < n)
                    return -EINVAL;
                proc_debug_regs_t regs;
                int ret = proc_debug_getregs(pid, &regs);
                if (ret)
                    return ret;
                unsigned long buf[LINUX_REGS_COUNT];
                linux_regs_export(&regs, buf);
                if (copy_to_user(iov.base, buf, n) < 0)
                    return -EFAULT;
                iov.len = n;
                return copy_to_user(data, &iov, sizeof(iov)) < 0 ? -EFAULT : 0;
            }
            if (iov.len < LINUX_REGS_COUNT * sizeof(unsigned long))
                return -EINVAL;
            unsigned long buf[LINUX_REGS_COUNT];
            if (copy_from_user(buf, iov.base, sizeof(buf)) < 0)
                return -EFAULT;
            proc_debug_regs_t regs;
            int ret = proc_debug_getregs(pid, &regs);
            if (ret)
                return ret;
            ret = linux_regs_import(&regs, buf);
            if (ret)
                return ret;
            return proc_debug_setregs(pid, &regs);
        }
        if (kind == NT_FPREGSET) {
            if (request == PTRACE_GETREGSET) {
                size_t n = LINUX_FPREGS_COUNT * sizeof(unsigned long);
                if (iov.len < n)
                    return -EINVAL;
                proc_debug_regs_t regs;
                int ret = proc_debug_getregs(pid, &regs);
                if (ret)
                    return ret;
                unsigned long buf[LINUX_FPREGS_COUNT];
                linux_fpregs_export(&regs, buf);
                if (copy_to_user(iov.base, buf, n) < 0)
                    return -EFAULT;
                iov.len = n;
                return copy_to_user(data, &iov, sizeof(iov)) < 0 ? -EFAULT : 0;
            }
            if (iov.len < LINUX_FPREGS_COUNT * sizeof(unsigned long))
                return -EINVAL;
            unsigned long buf[LINUX_FPREGS_COUNT];
            if (copy_from_user(buf, iov.base, sizeof(buf)) < 0)
                return -EFAULT;
            proc_debug_regs_t regs;
            int ret = proc_debug_getregs(pid, &regs);
            if (ret)
                return ret;
            linux_fpregs_import(&regs, buf);
            return proc_debug_setregs(pid, &regs);
        }
        return -EINVAL;
    }

    default:
        return -EIO;
    }
}
