/* Shared Native clock entry.
 *
 * Kept separate from the full sys_core.c so an MCU profile can link exactly
 * one time syscall without dragging exec, VFS and signal code in with it.
 */
#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "core/types.h"
#include "core/timekeeping.h"
#include "sys/usercopy.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"

int64_t sys_a20_clock_get(const a20_syscall_args_t *args)
{
    uint32_t clock_id = (uint32_t)args->arg[0];
    a20_time_ns_t *out = (a20_time_ns_t *)(uintptr_t)args->arg[1];
    if (!out)
        return -A20_ERR_FAULT;

    uint64_t ts[2]; /* [seconds, nanoseconds] */
    switch (clock_id) {
    case 0: /* CLOCK_REALTIME */
    case 5: /* CLOCK_REALTIME_COARSE */
        timekeeping_get_realtime(ts);
        break;
    case 1: /* CLOCK_MONOTONIC */
    case 4: /* CLOCK_MONOTONIC_RAW */
    case 6: /* CLOCK_MONOTONIC_COARSE */
        timekeeping_get_monotonic(ts);
        break;
    default:
        return -A20_ERR_INVALID_ARGUMENT;
    }

    a20_time_ns_t ns = ts[0] * 1000000000ULL + ts[1];
    if (copy_to_user(out, &ns, sizeof(ns)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}
#endif
