/*
 * A20OS Native ABI — Pager, Monitor and task memory syscalls.
 *
 * Pager  (0x0D00): user-space paging for PAGED VMOs.
 * Monitor (0x0D10): perf-style software-event counter objects.
 * task_mem (0x0211/0x0212): rights-checked cross-process memory access.
 *
 * Design reference: docs/native-abi/09-native-abi-deepening.md §2-§4.
 */
#include "core/types.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/version.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "core/random.h"
#include "trap_frame.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "mm/vmo.h"
#include "mm/process_vm.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "fs/xattr.h"
#include "net/socket.h"
#include "sys/usercopy.h"
#include "core/klog.h"

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
extern int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                               a20_handle_t h,
                                               uint16_t expected_type,
                                               a20_rights_t required_rights,
                                               a20_handle_entry_t *out);
extern int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern void a20_object_release(void *object, uint16_t type);
extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);

/* ===== Pager (0x0D00) ===== */

int64_t sys_a20_pager_create(const a20_syscall_args_t *args)
{
    a20_pager_create_args_t *uargs = (a20_pager_create_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_pager_create_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.flags != 0)
        return -A20_ERR_INVALID_ARGUMENT;

    a20_channel_ep_t *ep0 = a20_channel_create(A20_CH_DEFAULT_CAP, NULL);
    if (!ep0) return -A20_ERR_NO_MEMORY;
    a20_channel_ep_t *ep1 = ep0->peer;

    a20_pager_t *pager = a20_pager_create(ep0);
    if (!pager) {
        a20_channel_ep_release(ep0);
        a20_channel_ep_release(ep1);
        return -A20_ERR_NO_MEMORY;
    }

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) {
        a20_pager_put(pager);
        a20_channel_ep_release(ep1);
        return -A20_ERR_BAD_HANDLE;
    }

    a20_rights_t pager_rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                A20_RIGHT_CONTROL;
    int64_t hp = a20_handle_install(ht, pager, A20_OBJ_PAGER, pager_rights);
    if (hp < 0) {
        a20_pager_put(pager);
        a20_channel_ep_release(ep1);
        return hp;
    }

    a20_rights_t req_rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                              A20_RIGHT_DUP | A20_RIGHT_TRANSFER;
    int64_t hr = a20_handle_install(ht, ep1, A20_OBJ_CHANNEL_ENDPOINT, req_rights);
    if (hr < 0) {
        a20_handle_remove(ht, (a20_handle_t)hp);
        a20_channel_ep_release(ep1);
        return hr;
    }

    kargs.out_pager = (a20_handle_t)hp;
    kargs.out_requests = (a20_handle_t)hr;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)hp);
        a20_handle_remove(ht, (a20_handle_t)hr);
        return -A20_ERR_FAULT;
    }
    return A20_OK;
}

int64_t sys_a20_pager_vmo_attach(const a20_syscall_args_t *args)
{
    a20_pager_vmo_args_t *uargs = (a20_pager_vmo_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_pager_vmo_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t pe;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.pager, A20_OBJ_PAGER,
                                               A20_RIGHT_CONTROL, &pe);
    if (r < 0) return r;

    a20_handle_entry_t ve;
    r = a20_handle_lookup_ref_internal(ht, kargs.vmo, A20_OBJ_MEMORY,
                                       A20_RIGHT_CONTROL, &ve);
    if (r < 0) {
        a20_object_release(pe.object, pe.type);
        return r;
    }

    r = a20_pager_attach_vmo((a20_pager_t *)pe.object, (struct vmo *)ve.object);
    a20_object_release(ve.object, ve.type);
    a20_object_release(pe.object, pe.type);
    return r;
}

int64_t sys_a20_pager_supply_pages(const a20_syscall_args_t *args)
{
    a20_pager_supply_args_t *uargs = (a20_pager_supply_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_pager_supply_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t pe;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.pager, A20_OBJ_PAGER,
                                               A20_RIGHT_WRITE, &pe);
    if (r < 0) return r;

    a20_handle_entry_t ve;
    r = a20_handle_lookup_ref_internal(ht, kargs.vmo, A20_OBJ_MEMORY,
                                       A20_RIGHT_WRITE, &ve);
    if (r < 0) {
        a20_object_release(pe.object, pe.type);
        return r;
    }

    a20_handle_entry_t se;
    r = a20_handle_lookup_ref_internal(ht, kargs.source, A20_OBJ_MEMORY,
                                       A20_RIGHT_READ, &se);
    if (r < 0) {
        a20_object_release(ve.object, ve.type);
        a20_object_release(pe.object, pe.type);
        return r;
    }

    int64_t supplied = a20_pager_supply_pages(
        (a20_pager_t *)pe.object, (struct vmo *)ve.object,
        (struct vmo *)se.object, kargs.vmo_offset, kargs.source_offset,
        kargs.len);

    a20_object_release(se.object, se.type);
    a20_object_release(ve.object, ve.type);
    a20_object_release(pe.object, pe.type);
    if (supplied < 0) return supplied;

    kargs.out_supplied = (uint64_t)supplied;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return supplied;
}

/* ===== Monitor (0x0D10) ===== */

int64_t sys_a20_monitor_create(const a20_syscall_args_t *args)
{
    a20_monitor_create_args_t *uargs = (a20_monitor_create_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_monitor_create_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.kind < A20_MONITOR_TASK_CPU_TIME ||
        kargs.kind > A20_MONITOR_SYS_CTX_SWITCH)
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    task_t *target = NULL;
    if (kargs.target != A20_HANDLE_NULL) {
        a20_handle_entry_t te;
        int64_t r = a20_handle_lookup_task_like(ht, kargs.target,
                                               A20_RIGHT_STAT, &te);
        if (r < 0) return r;
        target = proc_find_get((int)(uintptr_t)te.object);
        if (!target) return -A20_ERR_BAD_HANDLE;
    }

    a20_monitor_t *mon = a20_monitor_create(kargs.kind, kargs.flags, target,
                                            kargs.period_ns);
    if (!mon) {
        if (target) proc_put(target);
        return -A20_ERR_NO_MEMORY;
    }

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_DUP |
                          A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL;
    int64_t h = a20_handle_install(ht, mon, A20_OBJ_MONITOR, rights);
    if (h < 0) {
        a20_monitor_release(mon);
        return h;
    }
    if (kargs.period_ns)
        a20_monitor_register(mon);

    kargs.out_monitor = (a20_handle_t)h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)h);
        return -A20_ERR_FAULT;
    }
    return A20_OK;
}

int64_t sys_a20_monitor_query(const a20_syscall_args_t *args)
{
    a20_handle_t mon_h = (a20_handle_t)A20_ARG(0);
    uint64_t out_ptr = A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t me;
    int64_t r = a20_handle_lookup_ref_internal(ht, mon_h, A20_OBJ_MONITOR,
                                               A20_RIGHT_READ, &me);
    if (r < 0) return r;

    a20_monitor_t *mon = (a20_monitor_t *)me.object;
    int64_t v = a20_monitor_sample(mon);
    a20_object_release(me.object, me.type);
    if (v < 0) return v;

    a20_monitor_value_t val;
    memset(&val, 0, sizeof(val));
    val.size = sizeof(val);
    val.version = 1;
    val.kind = mon->kind;
    val.flags = 0;
    val.count = (uint64_t)v;
    uint64_t now_ns = timer_get_ticks() * 1000000000ULL / TICKS_PER_SEC;
    val.time_active_ns = now_ns >= mon->time_start_ns
                             ? now_ns - mon->time_start_ns : 0;
    val.prev = mon->last_sample;
    if (copy_to_user((void *)(uintptr_t)out_ptr, &val, sizeof(val)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

/* ===== task_mem (0x0211 / 0x0212) ===== */

static int64_t a20_task_mem_rw(const a20_syscall_args_t *args, int write)
{
    a20_task_mem_args_t *uargs = (a20_task_mem_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_task_mem_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.flags != 0 || kargs.local_iov_count == 0 ||
        kargs.remote_iov_count == 0)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.local_iov_count > 64 || kargs.remote_iov_count > 64)
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_rights_t need = write ? A20_RIGHT_WRITE : A20_RIGHT_READ;
    a20_handle_entry_t te;
    int64_t r = a20_handle_lookup_ref_task_like(ht, kargs.task,
                                               need, &te);
    if (r < 0) return r;

    int target_pid = (int)(uintptr_t)te.object;
    task_t *target = proc_find_get(target_pid);
    a20_object_release(te.object, te.type);
    if (!target) return -A20_ERR_BAD_HANDLE;
    if (!target->mm) {
        proc_put(target);
        return -A20_ERR_BAD_HANDLE;
    }

    a20_iovec_t liov[64];
    a20_iovec_t riov[64];
    if (copy_from_user(liov, (const void *)(uintptr_t)kargs.local_iov,
                       kargs.local_iov_count * sizeof(a20_iovec_t)) < 0 ||
        copy_from_user(riov, (const void *)(uintptr_t)kargs.remote_iov,
                       kargs.remote_iov_count * sizeof(a20_iovec_t)) < 0) {
        proc_put(target);
        return -A20_ERR_FAULT;
    }

    uint64_t total = 0;
    uint64_t budget = 0;
    for (uint32_t i = 0; i < kargs.local_iov_count; i++)
        budget += liov[i].len;
    if (budget > 64 * 1024) {
        proc_put(target);
        return -A20_ERR_INVALID_ARGUMENT;
    }

    /* Sequential iovec pairs: copy up to min(local, remote) per pair. */
    for (uint32_t i = 0; i < kargs.local_iov_count && i < kargs.remote_iov_count; i++) {
        uint64_t n = liov[i].len < riov[i].len ? liov[i].len : riov[i].len;
        if (n == 0) continue;
        if (liov[i].base + n < liov[i].base || riov[i].base + n < riov[i].base)
            continue;
        if (liov[i].base + n > USER_VA_LIMIT || riov[i].base + n > USER_VA_LIMIT)
            continue;

        char kbuf[512];
        uint64_t done = 0;
        while (done < n) {
            size_t chunk = n - done;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            uint64_t lva = liov[i].base + done;
            uint64_t rva = riov[i].base + done;

            if (write) {
                if (copy_from_user(kbuf, (const void *)(uintptr_t)lva, chunk) < 0)
                    break;
                if (process_vm_write_kernel(target, (void *)(uintptr_t)rva, kbuf,
                                            chunk) < 0)
                    break;
            } else {
                if (process_vm_read_kernel(target, (const void *)(uintptr_t)rva, kbuf,
                                           chunk) < 0)
                    break;
                if (copy_to_user((void *)(uintptr_t)lva, kbuf, chunk) < 0)
                    break;
            }
            done += chunk;
            total += chunk;
        }
    }

    proc_put(target);
    kargs.out_transferred = total;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return (int64_t)total;
}

int64_t sys_a20_task_mem_read(const a20_syscall_args_t *args)
{
    return a20_task_mem_rw(args, 0);
}

int64_t sys_a20_task_mem_write(const a20_syscall_args_t *args)
{
    return a20_task_mem_rw(args, 1);
}
