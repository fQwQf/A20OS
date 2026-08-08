#ifndef _ABI_LINUX_PTRACE_LAYOUT_H
#define _ABI_LINUX_PTRACE_LAYOUT_H

/*
 * Linux ABI ptrace register-file layouts.
 *
 * Each arch exposes its own Linux user_regs_struct/user_fpregs_struct
 * translation in kernel/arch/<arch>/platform/arch_hooks.c; common code only
 * calls these hooks.  No CONFIG_<arch> branches may live here.
 */

#include "core/types.h"

struct proc_debug_regs;

#define LINUX_ARCH_MAX_REGS     44
#define LINUX_ARCH_MAX_FPREGS   66

int  linux_arch_regs_count(void);
int  linux_arch_fpregs_count(void);
void linux_arch_regs_export(const struct proc_debug_regs *in,
                            unsigned long *out);
int  linux_arch_regs_import(struct proc_debug_regs *in,
                            const unsigned long *r);
void linux_arch_fpregs_export(const struct proc_debug_regs *in,
                              unsigned long *out);
void linux_arch_fpregs_import(struct proc_debug_regs *in,
                              const unsigned long *r);

/* PTRACE_PEEKUSER/POKEUSER word offsets (byte offsets inside struct user).
 * Only x86_64 supports them today; other arches return 0 for has_peekuser. */
enum {
    LINUX_USER_OFFSET_ORIG_RAX = 0,
    LINUX_USER_OFFSET_RIP,
    LINUX_USER_OFFSET_EFLAGS,
    LINUX_USER_OFFSET_RSP,
    LINUX_USER_OFFSET_FS_BASE,
    LINUX_USER_OFFSET_GS_BASE,
    LINUX_USER_OFFSET_LAST,
};

int linux_arch_has_peekuser(void);
unsigned long linux_arch_user_offset(int field);

#endif /* _ABI_LINUX_PTRACE_LAYOUT_H */
