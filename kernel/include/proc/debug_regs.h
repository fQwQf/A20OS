#ifndef _PROC_DEBUG_REGS_H
#define _PROC_DEBUG_REGS_H

/*
 * Generic register-file layout for the kernel debugging interface.
 *
 * Deliberately dependency-free (only core/types.h) so arch headers can
 * include it without include cycles.  Arch code translates its trap
 * context to/from this layout via arch_ptrace_export_regs /
 * arch_ptrace_import_regs (see each arch's trap_frame.h).
 */

#include "core/types.h"

#define PROC_DEBUG_MAX_REGISTERS   32
#define PROC_DEBUG_MAX_FPREGISTERS 64

typedef struct proc_debug_regs {
    uint64_t regs[PROC_DEBUG_MAX_REGISTERS]; /* GPRs, arch-defined order */
    uint64_t pc;                             /* rip/sepc/elr/era */
    uint64_t sp;                             /* rsp/sp */
    uint64_t status;                         /* rflags/sstatus/pstate */
    uint64_t orig_syscall;                   /* syscall number at trap entry */
    uint64_t fp[PROC_DEBUG_MAX_FPREGISTERS]; /* FP regs, arch-defined order */
    uint64_t fcsr;
} proc_debug_regs_t;

#endif /* _PROC_DEBUG_REGS_H */
