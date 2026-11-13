/*
 * A20OS Native ABI — Phase 2 syscall implementations.
 *
 * This file is part of the mechanically split Native Phase 2 ABI.
 * See sys_phase2.c for shared helpers and forward declarations.
 */
#include "core/types.h"
#include "core/klog.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/version.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "core/cpu.h"
#include "core/smp.h"
#include "core/random.h"
#include "core/kexec.h"
#include "uapi/a20/kexec.h"
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
    a20_system_info_t *out = (a20_system_info_t *)(uintptr_t)A20_ARG(0);
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
    strncpy(info.sysname, A20OS_SYSNAME, sizeof(info.sysname));
    strncpy(info.nodename, A20OS_NODENAME, sizeof(info.nodename));
    strncpy(info.release, A20OS_VERSION, sizeof(info.release));
    strncpy(info.version, A20OS_VERSION_FULL, sizeof(info.version));
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
    void *buf = (void *)(uintptr_t)A20_ARG(0);
    size_t len = (size_t)A20_ARG(1);
    if (!buf || len > 4096) return -A20_ERR_INVALID_ARGUMENT;

    char kbuf[4096];
    random_fill(kbuf, len);
    if (copy_to_user(buf, kbuf, len) < 0) return -A20_ERR_FAULT;
    return (int64_t)len;
}

static int64_t kexec_native_status(int rc)
{
    switch (rc) {
    case 0: return A20_OK;
    case -EPERM: return -A20_ERR_PERM;
    case -ESRCH:
    case -EBADF:
    case -EINVAL:
    case -ENOEXEC: return -A20_ERR_INVALID_ARGUMENT;
    case -EFAULT: return -A20_ERR_FAULT;
    case -ENOMEM: return -A20_ERR_NO_MEMORY;
    case -EBUSY: return -A20_ERR_BUSY;
    case -EIO: return -A20_ERR_IO;
    case -EADDRNOTAVAIL: return -A20_ERR_ADDRESS_NOT_AVAILABLE;
    case -ETIMEDOUT: return -A20_ERR_TIMED_OUT;
    case -ENOSYS:
    case -EOPNOTSUPP: return -A20_ERR_NOT_SUPPORTED;
    default: return -A20_ERR_IO;
    }
}

int64_t sys_a20_system_kexec(const a20_syscall_args_t *args)
{
    uint32_t op = (uint32_t)A20_ARG(0);
    if (op == A20_KEXEC_STATUS) {
        a20_kexec_status_t *user = (void *)(uintptr_t)A20_ARG(1);
        if (!user)
            return -A20_ERR_FAULT;
        uint32_t size;
        if (copy_from_user(&size, &user->size, sizeof(size)) < 0)
            return -A20_ERR_FAULT;
        if (size < sizeof(a20_kexec_status_t))
            return -A20_ERR_INVALID_ARGUMENT;
        kexec_status_t status;
        kexec_get_status(&status);
        a20_kexec_status_t out = {
            .size = sizeof(out), .struct_version = 1,
            .state = status.state, .backend = status.backend,
            .entry = status.entry, .nr_segments = status.nr_segments,
        };
        return copy_to_user(user, &out, sizeof(out)) < 0
               ? -A20_ERR_FAULT : A20_OK;
    }
    if (op == A20_KEXEC_EXECUTE)
        return kexec_native_status(kexec_execute());
    if (op == A20_KEXEC_DISCARD)
        return kexec_native_status(kexec_discard());
    if (op == A20_KEXEC_LOAD_IMAGE) {
        const void *user = (const void *)(uintptr_t)A20_ARG(1);
        uint64_t size = A20_ARG(2);
        if (A20_ARG(3) || !size || size > KEXEC_MAX_IMAGE_SIZE)
            return -A20_ERR_INVALID_ARGUMENT;
        if (!user)
            return -A20_ERR_FAULT;
        void *image = kmalloc((size_t)size);
        if (!image)
            return -A20_ERR_NO_MEMORY;
        int rc = copy_from_user(image, user, (size_t)size) < 0
                 ? -EFAULT : kexec_load_image(image, size);
        kfree(image);
        return kexec_native_status(rc);
    }
    if (op == A20_KEXEC_LOAD_SEGMENTS) {
        uint64_t entry = A20_ARG(1);
        const a20_kexec_segment_t *user = (const void *)(uintptr_t)A20_ARG(2);
        uint64_t count = A20_ARG(3);
        if (A20_ARG(4) || !count || count > KEXEC_MAX_SEGMENTS)
            return -A20_ERR_INVALID_ARGUMENT;
        if (!user)
            return -A20_ERR_FAULT;
        a20_kexec_segment_t *wire = kmalloc((size_t)count * sizeof(*wire));
        kexec_segment_t *segments = kmalloc((size_t)count * sizeof(*segments));
        if (!wire || !segments) {
            kfree(wire);
            kfree(segments);
            return -A20_ERR_NO_MEMORY;
        }
        int rc = copy_from_user(wire, user, (size_t)count * sizeof(*wire)) < 0
                 ? -EFAULT : 0;
        for (uint64_t i = 0; !rc && i < count; i++) {
            segments[i].buf = (const void *)(uintptr_t)wire[i].buf;
            segments[i].bufsz = (size_t)wire[i].bufsz;
            segments[i].mem = (const void *)(uintptr_t)wire[i].mem;
            segments[i].memsz = (size_t)wire[i].memsz;
            if (wire[i].bufsz > SIZE_MAX || wire[i].memsz > SIZE_MAX)
                rc = -EINVAL;
        }
        if (!rc)
            rc = kexec_load_segment_array(entry, count, segments);
        kfree(wire);
        kfree(segments);
        return kexec_native_status(rc);
    }
    return -A20_ERR_INVALID_ARGUMENT;
}

int64_t sys_a20_system_reboot(const a20_syscall_args_t *args)
{
    uint32_t cmd = (uint32_t)A20_ARG(0);
    task_t *task = proc_current();
    if (!task)
        return -A20_ERR_NO_ENTRY;
    if (!proc_has_cap(task, CAP_SYS_BOOT) && task->cred.euid != 0)
        return -A20_ERR_PERM;
    if (cmd == 2)
        return kexec_native_status(kexec_execute());
    if (cmd > 1)
        return -A20_ERR_INVALID_ARGUMENT;
    klog_disk_mark_clean();
    int rc = vfs_system_shutdown();
    if (rc < 0)
        return kexec_native_status(rc);
    if (cmd == 0)
        firmware_shutdown();
    firmware_reboot();
    return A20_OK;
}
