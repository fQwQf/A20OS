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
#include "abi/linux/ptrace_layout.h"
#include "core/errno.h"

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

/* Per-arch Linux register-file layouts live in kernel/arch/<arch>/platform/
 * arch_hooks.c (linux_arch_* / linux_arch_user_offset); common code only
 * calls those hooks.  Buffer sizes use the worst-case counts so the caller
 * never needs arch branches. */

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

    case PTRACE_PEEKUSER: {
        if (!linux_arch_has_peekuser())
            return -EIO;
        unsigned long off = (unsigned long)addr;
        if (off % sizeof(unsigned long) ||
            off / sizeof(unsigned long) >= linux_arch_regs_count())
            return -EIO;
        proc_debug_regs_t regs;
        int ret = proc_debug_getregs(pid, &regs);
        if (ret)
            return ret;
        unsigned long buf[LINUX_ARCH_MAX_REGS];
        linux_arch_regs_export(&regs, buf);
        return (int64_t)buf[off / sizeof(unsigned long)];
    }

    case PTRACE_POKEUSER: {
        if (!linux_arch_has_peekuser())
            return -EIO;
        unsigned long off = (unsigned long)addr;
        if (off % sizeof(unsigned long) ||
            off / sizeof(unsigned long) >= linux_arch_regs_count())
            return -EIO;
        proc_debug_regs_t regs;
        int ret = proc_debug_getregs(pid, &regs);
        if (ret)
            return ret;
        unsigned long buf[LINUX_ARCH_MAX_REGS];
        linux_arch_regs_export(&regs, buf);
        buf[off / sizeof(unsigned long)] = (unsigned long)data;
        int imp = linux_arch_regs_import(&regs, buf);
        if (imp)
            return imp;
        return proc_debug_setregs(pid, &regs);
    }

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
        unsigned long buf[LINUX_ARCH_MAX_REGS];
        linux_arch_regs_export(&regs, buf);
        size_t n = (size_t)linux_arch_regs_count() * sizeof(unsigned long);
        return copy_to_user(data, buf, n) < 0 ? -EFAULT : 0;
    }

    case PTRACE_SETREGS: {
        if (!data)
            return -EIO;
        unsigned long buf[LINUX_ARCH_MAX_REGS];
        size_t n = (size_t)linux_arch_regs_count() * sizeof(unsigned long);
        if (copy_from_user(buf, data, n) < 0)
            return -EFAULT;
        /* Fetch first: the Linux register layout carries only a subset of
         * the kernel register file (e.g. riscv64 omits sp/pstate); the
         * untouched fields must survive the update. */
        proc_debug_regs_t regs;
        int ret = proc_debug_getregs(pid, &regs);
        if (ret)
            return ret;
        ret = linux_arch_regs_import(&regs, buf);
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
        unsigned long buf[LINUX_ARCH_MAX_FPREGS];
        linux_arch_fpregs_export(&regs, buf);
        size_t n = (size_t)linux_arch_fpregs_count() * sizeof(unsigned long);
        return copy_to_user(data, buf, n) < 0 ? -EFAULT : 0;
    }

    case PTRACE_SETFPREGS: {
        if (!data)
            return -EIO;
        unsigned long buf[LINUX_ARCH_MAX_FPREGS];
        size_t n = (size_t)linux_arch_fpregs_count() * sizeof(unsigned long);
        if (copy_from_user(buf, data, n) < 0)
            return -EFAULT;
        proc_debug_regs_t regs;
        int ret = proc_debug_getregs(pid, &regs);
        if (ret)
            return ret;
        linux_arch_fpregs_import(&regs, buf);
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
                size_t n = linux_arch_regs_count() * sizeof(unsigned long);
                if (iov.len < n)
                    return -EINVAL;
                proc_debug_regs_t regs;
                int ret = proc_debug_getregs(pid, &regs);
                if (ret)
                    return ret;
                unsigned long buf[LINUX_ARCH_MAX_REGS];
                linux_arch_regs_export(&regs, buf);
                if (copy_to_user(iov.base, buf, n) < 0)
                    return -EFAULT;
                iov.len = n;
                return copy_to_user(data, &iov, sizeof(iov)) < 0 ? -EFAULT : 0;
            }
            size_t n = linux_arch_regs_count() * sizeof(unsigned long);
            if (iov.len < n)
                return -EINVAL;
            unsigned long buf[LINUX_ARCH_MAX_REGS];
            if (copy_from_user(buf, iov.base, n) < 0)
                return -EFAULT;
            proc_debug_regs_t regs;
            int ret = proc_debug_getregs(pid, &regs);
            if (ret)
                return ret;
            ret = linux_arch_regs_import(&regs, buf);
            if (ret)
                return ret;
            return proc_debug_setregs(pid, &regs);
        }
        if (kind == NT_FPREGSET) {
            if (request == PTRACE_GETREGSET) {
                size_t n = linux_arch_fpregs_count() * sizeof(unsigned long);
                if (iov.len < n)
                    return -EINVAL;
                proc_debug_regs_t regs;
                int ret = proc_debug_getregs(pid, &regs);
                if (ret)
                    return ret;
                unsigned long buf[LINUX_ARCH_MAX_FPREGS];
                linux_arch_fpregs_export(&regs, buf);
                if (copy_to_user(iov.base, buf, n) < 0)
                    return -EFAULT;
                iov.len = n;
                return copy_to_user(data, &iov, sizeof(iov)) < 0 ? -EFAULT : 0;
            }
            size_t n = linux_arch_fpregs_count() * sizeof(unsigned long);
            if (iov.len < n)
                return -EINVAL;
            unsigned long buf[LINUX_ARCH_MAX_FPREGS];
            if (copy_from_user(buf, iov.base, n) < 0)
                return -EFAULT;
            proc_debug_regs_t regs;
            int ret = proc_debug_getregs(pid, &regs);
            if (ret)
                return ret;
            linux_arch_fpregs_import(&regs, buf);
            return proc_debug_setregs(pid, &regs);
        }
        return -EINVAL;
    }

    default:
        return -EIO;
    }
}
