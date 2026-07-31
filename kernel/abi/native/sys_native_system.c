/*
 * A20OS Native ABI — Phase 2 syscall implementations.
 *
 * This file is part of the mechanically split Native Phase 2 ABI.
 * See sys_phase2.c for shared helpers and forward declarations.
 */
#include "core/types.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/version.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "core/cpu.h"
#include "core/smp.h"
#include "core/random.h"
#include "trap_frame.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "fs/xattr.h"
#include "net/socket.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "sys_validate.h"
#include "abi/native/startup.h"
#include "abi/native/vmo.h"
#include "abi/native/vmar.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/resource.h"

#define A20_ARG(n) (args->arg[(n)])

extern struct a20_ht_internal *task_get_a20_ht(task_t *t);
extern int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                                  uint16_t type, a20_rights_t rights);
extern int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                           uint16_t type, a20_rights_t rights,
                                           uint64_t expiry_tick, uint32_t remaining_ops,
                                           uint32_t temporal_flags, uint8_t security_label);
extern int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                          uint16_t expected_type, a20_rights_t required_rights,
                                          a20_handle_entry_t *out);
extern int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
extern void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);

/* ===== System (0x0A00) ===== */

int64_t sys_a20_system_info(const a20_syscall_args_t *args)
{
    a20_system_info_t *out = (a20_system_info_t *)A20_ARG(0);
    if (!out) return -A20_ERR_FAULT;

    uint32_t user_size;
    if (copy_from_user(&user_size, out, sizeof(user_size)) < 0)
        return -A20_ERR_FAULT;
    size_t v1_size = offsetof(a20_system_info_t, configured_cpus);
    if (user_size == 0)
        user_size = v1_size;
    if (user_size < v1_size)
        return -A20_ERR_INVALID_ARGUMENT;

    a20_system_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.struct_version = 2;
    strncpy(info.sysname, "A20OS", sizeof(info.sysname));
    strncpy(info.nodename, "a20", sizeof(info.nodename));
    strncpy(info.release, VERSION, sizeof(info.release));
    strncpy(info.version, "Native ABI", sizeof(info.version));
    strncpy(info.machine, ARCH_NAME, sizeof(info.machine));
    info.total_ram = (uint64_t)pfa.total_frames * 4096;
    info.free_ram = (uint64_t)pfa.free_frames * 4096;
    info.total_swap = 0;
    info.free_swap = 0;
    info.num_procs = (uint16_t)proc_pid_max();
    info.configured_cpus = smp_configured_cpu_count();
    info.online_cpus = smp_online_cpu_count();
    info.current_cpu = cpu_current_id();
    info.page_size = 4096;
    info.uptime_ns = timer_get_ticks() * (1000000000ULL / TICKS_PER_SEC);

    size_t copy_size = user_size < sizeof(info) ? user_size : sizeof(info);
    if (copy_to_user(out, &info, copy_size) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_system_random(const a20_syscall_args_t *args)
{
    void *buf = (void *)A20_ARG(0);
    size_t len = (size_t)A20_ARG(1);
    if (!buf || len > 4096) return -A20_ERR_INVALID_ARGUMENT;

    char kbuf[4096];
    random_fill(kbuf, len);
    if (copy_to_user(buf, kbuf, len) < 0) return -A20_ERR_FAULT;
    return (int64_t)len;
}

int64_t sys_a20_system_reboot(const a20_syscall_args_t *args)
{
    uint32_t cmd = (uint32_t)A20_ARG(0);
    (void)A20_ARG(1);

    switch (cmd) {
    case 0: firmware_shutdown(); break;
    case 1: firmware_reboot(); break;
    default: while (1) {} break;
    }
    return A20_OK;
}
