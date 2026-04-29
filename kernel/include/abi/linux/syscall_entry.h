#ifndef _ABI_LINUX_SYSCALL_ENTRY_H
#define _ABI_LINUX_SYSCALL_ENTRY_H

#include "core/types.h"
#include "core/trap.h"
#include "abi/linux/syscall_nr.h"

typedef struct linux_syscall_args {
    uint64_t nr;
    uint64_t arg[6];
    trap_context_t *ctx;
} linux_syscall_args_t;

typedef int64_t (*linux_syscall_handler_t)(const linux_syscall_args_t *args);

typedef struct linux_syscall_entry {
    uint64_t nr;
    const char *name;
    linux_syscall_handler_t handler;
    int restores_context;
} linux_syscall_entry_t;

const linux_syscall_entry_t *linux_syscall_lookup(uint64_t nr);

#endif /* _ABI_LINUX_SYSCALL_ENTRY_H */
