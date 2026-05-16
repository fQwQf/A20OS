/*
 * A20OS Native ABI — Syscall entry types and dispatch interface.
 * Design reference: docs/native-abi/00-overview.md
 */
#ifndef _ABI_NATIVE_SYSCALL_ENTRY_H
#define _ABI_NATIVE_SYSCALL_ENTRY_H

#include "abi/native/types.h"
#include "core/types.h"
#include "core/trap.h"
#include "abi/native/syscall_nr.h"

typedef struct a20_syscall_args {
    uint64_t nr;
    uint64_t arg[6];
    trap_context_t *ctx;
} a20_syscall_args_t;

typedef int64_t (*a20_syscall_handler_t)(const a20_syscall_args_t *args);

typedef struct a20_syscall_entry {
    uint64_t nr;
    const char *name;
    a20_syscall_handler_t handler;
} a20_syscall_entry_t;

#define A20_SYSCALL_TABLE_SIZE  0x10000

const a20_syscall_entry_t *a20_syscall_lookup(uint64_t nr);

#endif
