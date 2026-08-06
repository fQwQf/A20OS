#ifndef _A20_DEBUG_H
#define _A20_DEBUG_H

/*
 * Native ABI debugging API (Debug 0x0900 partition).
 *
 * Thin user-side wrappers over sys_native_debug.c.  A debug session is an
 * A20_OBJ_DEBUG handle; the traced task must be stopped before register or
 * memory access (see docs/native-abi/06-security.md §8.1).
 */

#include "a20_types.h"
#include "a20_syscall.h"

/* Stop kinds / events / resume modes mirror the kernel ABI (types.h). */
#define A20_DEBUG_STOP_SIGNAL        1
#define A20_DEBUG_STOP_EVENT         2
#define A20_DEBUG_STOP_SYSCALL_ENTRY 3
#define A20_DEBUG_STOP_SYSCALL_EXIT  4
#define A20_DEBUG_EVENT_EXEC         4
#define A20_DEBUG_EVENT_EXIT         6
#define A20_DEBUG_RESUME_CONT        0
#define A20_DEBUG_RESUME_SYSCALL     1

static inline a20_status_t a20_debug_traceme(void)
{
    return a20_syscall6(A20_SYS_debug_traceme, 0, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_debug_attach(a20_handle_t task,
                                             a20_handle_t *out)
{
    a20_status_t st = a20_syscall6(A20_SYS_debug_attach, task, 0, 0, 0, 0, 0);
    if (st >= 0 && out)
        *out = (a20_handle_t)st;
    return st < 0 ? st : A20_OK;
}

/* Block until the traced task stops, or timeout_us elapses
 * (A20_TIMEOUT_INFINITE for no timeout, 0 for a non-blocking poll). */
static inline a20_status_t a20_debug_wait(a20_handle_t dbg, uint64_t timeout_us,
                                           a20_debug_event_info_t *out)
{
    return a20_syscall6(A20_SYS_debug_wait, dbg, timeout_us, (uint64_t)out,
                        0, 0, 0);
}

static inline a20_status_t a20_debug_event(a20_handle_t dbg,
                                            a20_debug_event_info_t *out)
{
    return a20_syscall6(A20_SYS_debug_event, dbg, (uint64_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_debug_resume(a20_handle_t dbg, uint32_t mode)
{
    return a20_syscall6(A20_SYS_debug_resume, dbg, mode, 0, 0, 0, 0);
}

static inline a20_status_t a20_debug_detach(a20_handle_t dbg)
{
    return a20_syscall6(A20_SYS_debug_detach, dbg, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_debug_kill(a20_handle_t dbg)
{
    return a20_syscall6(A20_SYS_debug_kill, dbg, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_debug_read(a20_handle_t dbg, uint64_t addr,
                                           void *buf, uint64_t len)
{
    a20_status_t st = a20_syscall6(A20_SYS_debug_read, dbg, addr, len,
                                   (uint64_t)buf, 0, 0);
    return st < 0 ? st : A20_OK;
}

static inline a20_status_t a20_debug_write(a20_handle_t dbg, uint64_t addr,
                                            const void *buf, uint64_t len)
{
    a20_status_t st = a20_syscall6(A20_SYS_debug_write, dbg, addr, len,
                                   (uint64_t)buf, 0, 0);
    return st < 0 ? st : A20_OK;
}

static inline a20_status_t a20_debug_read_regs(a20_handle_t dbg,
                                                a20_regs_t *out)
{
    return a20_syscall6(A20_SYS_debug_read_regs, dbg, (uint64_t)out,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_debug_write_regs(a20_handle_t dbg,
                                                 const a20_regs_t *in)
{
    return a20_syscall6(A20_SYS_debug_write_regs, dbg, (uint64_t)in,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_debug_map_memory(a20_handle_t dbg,
                                                 uint64_t addr, uint64_t len,
                                                 uint32_t prot,
                                                 uint64_t *out_local)
{
    a20_status_t st = a20_syscall6(A20_SYS_debug_map_memory, dbg, addr, len,
                                   prot, 0, 0);
    if (st >= 0 && out_local)
        *out_local = (uint64_t)st;
    return st < 0 ? st : A20_OK;
}

#endif /* _A20_DEBUG_H */
