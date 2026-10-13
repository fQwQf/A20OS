#ifndef _A20_SYNC_H
#define _A20_SYNC_H

#include "a20_types.h"
#include "a20_syscall.h"

static inline a20_status_t a20_futex_wait(uint32_t *addr, uint32_t expected,
                                          uint64_t timeout_ns)
{
    a20_futex_wait_args_t args = {
        .size = sizeof(args),
        .version = 1,
        .addr = (uint64_t)(uintptr_t)addr,
        .expected = expected,
        .flags = 0,
        .timeout_ns = timeout_ns,
    };
    return a20_syscall6(A20_SYS_futex_wait, (uint64_t)(uintptr_t)&args, 0, 0, 0,
                        0, 0);
}

static inline a20_status_t a20_futex_wake(uint32_t *addr, uint32_t count,
                                          uint32_t *out_woken)
{
    a20_futex_wake_args_t args = {
        .size = sizeof(args),
        .version = 1,
        .addr = (uint64_t)(uintptr_t)addr,
        .count = count,
        .flags = 0,
        .out_woken = 0,
        .reserved = 0,
    };
    a20_status_t st = a20_syscall6(A20_SYS_futex_wake,
                                   (uint64_t)(uintptr_t)&args, 0, 0, 0, 0, 0);
    if (st == A20_OK && out_woken)
        *out_woken = args.out_woken;
    return st;
}

/* Every operation is seq_cst. Store/exchange report the previous word; CAS
 * reports the observed word and whether replacement occurred. */
static inline a20_status_t a20_atomic32(uint32_t *addr, uint32_t operation,
                                        uint32_t expected, uint32_t desired,
                                        uint32_t *observed,
                                        uint32_t *exchanged) {
    a20_atomic32_args_t args = {.size = sizeof(args),
                                .version = 1,
                                .addr = (uint64_t)(uintptr_t)addr,
                                .operation = operation,
                                .expected = expected,
                                .desired = desired};
    a20_status_t st =
        a20_syscall6(A20_SYS_atomic32, (uintptr_t)&args, 0, 0, 0, 0, 0);
    if (st == A20_OK) {
        if (observed)
            *observed = args.observed;
        if (exchanged)
            *exchanged = args.exchanged;
    }
    return st;
}
static inline a20_status_t a20_atomic32_load(uint32_t *addr, uint32_t *value) {
    if (!value)
        return -A20_ERR_INVALID_ARGUMENT;
    return a20_atomic32(addr, A20_ATOMIC32_LOAD, 0, 0, value, 0);
}
static inline a20_status_t a20_atomic32_store(uint32_t *addr, uint32_t value) {
    return a20_atomic32(addr, A20_ATOMIC32_STORE, 0, value, 0, 0);
}
#endif
