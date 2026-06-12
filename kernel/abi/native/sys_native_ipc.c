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
#include "abi/native/objects.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
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
extern void a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
extern void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);

/* ===== IPC (0x0500) ===== */

int64_t sys_a20_event_queue_create(const a20_syscall_args_t *args)
{
    a20_event_queue_create_args_t *uargs = (a20_event_queue_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_event_queue_create_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    a20_eventq_t *eq = a20_eventq_create(kargs.capacity_hint);
    if (!eq) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { a20_eventq_release(eq); return -A20_ERR_BAD_HANDLE; }

    int64_t h = a20_handle_install(ht, eq, A20_OBJ_EVENT_QUEUE,
                                   A20_RIGHT_READ | A20_RIGHT_STAT |
                                   A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                   A20_RIGHT_CONTROL);
    if (h < 0) a20_eventq_release(eq);
    return h;
}

int64_t sys_a20_event_watch(const a20_syscall_args_t *args)
{
    a20_event_watch_args_t *uargs = (a20_event_watch_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_event_watch_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t eq_entry;
    int64_t r = a20_handle_lookup_internal(ht, kargs.queue, A20_OBJ_EVENT_QUEUE,
                                           A20_RIGHT_READ, &eq_entry);
    if (r < 0) return r;

    a20_handle_entry_t tgt_entry;
    r = a20_handle_lookup_internal(ht, kargs.target, A20_OBJ_INVALID,
                                   A20_RIGHTS_NONE, &tgt_entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)eq_entry.object;
    return a20_eventq_watch(eq, kargs.target, tgt_entry.object,
                            tgt_entry.type, kargs.event_mask, kargs.user_data);
}

int64_t sys_a20_event_wait(const a20_syscall_args_t *args)
{
    a20_handle_t queue_h = (a20_handle_t)A20_ARG(0);
    a20_pending_event_t *out = (a20_pending_event_t *)A20_ARG(1);
    uint64_t timeout_ns = A20_ARG(2);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, queue_h, A20_OBJ_EVENT_QUEUE,
                                           A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)entry.object;
    a20_pending_event_t ev;
    r = a20_eventq_wait(eq, &ev, timeout_ns);
    if (r < 0) return r;
    if (copy_to_user(out, &ev, sizeof(ev)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_event_cancel(const a20_syscall_args_t *args)
{
    a20_handle_t queue_h = (a20_handle_t)A20_ARG(0);
    a20_handle_t target_h = (a20_handle_t)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, queue_h, A20_OBJ_EVENT_QUEUE,
                                           A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)entry.object;
    return a20_eventq_cancel(eq, target_h);
}

int64_t sys_a20_channel_create(const a20_syscall_args_t *args)
{
    a20_channel_create_args_t *uargs = (a20_channel_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_channel_create_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    a20_channel_ep_t *ep0 = a20_channel_create(kargs.msg_capacity, NULL);
    if (!ep0) return -A20_ERR_NO_MEMORY;
    a20_channel_ep_t *ep1 = ep0->peer;
    refcount_inc(&ep1->refcount);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) {
        a20_channel_ep_release(ep0);
        a20_channel_ep_release(ep1);
        return -A20_ERR_BAD_HANDLE;
    }

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER;
    int64_t h0 = a20_handle_install(ht, ep0, A20_OBJ_CHANNEL_ENDPOINT, rights);
    int64_t h1 = a20_handle_install(ht, ep1, A20_OBJ_CHANNEL_ENDPOINT, rights);

    a20_handle_t result[2];
    result[0] = (h0 >= 0) ? (a20_handle_t)h0 : A20_HANDLE_NULL;
    result[1] = (h1 >= 0) ? (a20_handle_t)h1 : A20_HANDLE_NULL;
    if (copy_to_user(uargs->out_endpoints, result, sizeof(result)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_channel_send(const a20_syscall_args_t *args)
{
    a20_msg_send_args_t *uargs = (a20_msg_send_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_msg_send_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, kargs.channel,
                                           A20_OBJ_CHANNEL_ENDPOINT,
                                           A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;

    if (a20_ht_get_label(ht) > entry.security_label)
        return -A20_ERR_ACCESS;

    a20_channel_ep_t *ep = (a20_channel_ep_t *)entry.object;

    a20_ch_handle_info_t hinfos[A20_CH_MAX_HANDLES];
    uint32_t actual_hcount = 0;

    if (kargs.handle_count > 0 && kargs.handles) {
        if (kargs.handle_count > A20_CH_MAX_HANDLES)
            return -A20_ERR_INVALID_ARGUMENT;

        a20_handle_t user_handles[A20_CH_MAX_HANDLES];
        a20_rights_t user_rights[A20_CH_MAX_HANDLES];
        if (copy_from_user(user_handles, (void *)kargs.handles,
                           kargs.handle_count * sizeof(a20_handle_t)) < 0)
            return -A20_ERR_FAULT;

        uint32_t rights_count = 0;
        if (kargs.transfer_rights) {
            if (copy_from_user(user_rights, (void *)kargs.transfer_rights,
                               kargs.handle_count * sizeof(a20_rights_t)) < 0)
                return -A20_ERR_FAULT;
            rights_count = kargs.handle_count;
        }

        for (uint32_t i = 0; i < kargs.handle_count; i++) {
            a20_handle_entry_t he;
            r = a20_handle_lookup_internal(ht, user_handles[i], A20_OBJ_INVALID,
                                           A20_RIGHT_TRANSFER, &he);
            if (r < 0) return r;

            a20_rights_t xfer_rights = (rights_count > 0)
                                       ? user_rights[i] : he.rights;
            /* docs/native-abi/06-security.md §4.1: ρ_recv = ρ_send ∩ ρ_transfer */
            hinfos[actual_hcount].object = he.object;
            hinfos[actual_hcount].type = he.type;
            hinfos[actual_hcount].transfer_rights = he.rights & xfer_rights;
            if (hinfos[actual_hcount].transfer_rights == 0)
                return -A20_ERR_ACCESS;
            actual_hcount++;
        }
    }

    return a20_channel_send(ep, (const void *)kargs.data, kargs.data_len,
                            hinfos, actual_hcount, ht);
}

int64_t sys_a20_channel_recv(const a20_syscall_args_t *args)
{
    a20_msg_recv_args_t *uargs = (a20_msg_recv_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_msg_recv_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, kargs.channel,
                                           A20_OBJ_CHANNEL_ENDPOINT,
                                           A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    if (a20_ht_get_label(ht) < entry.security_label)
        return -A20_ERR_ACCESS;

    a20_channel_ep_t *ep = (a20_channel_ep_t *)entry.object;

    a20_ch_handle_info_t hinfos[A20_CH_MAX_HANDLES];
    uint32_t out_hcount = kargs.handle_buf_count;
    if (out_hcount > A20_CH_MAX_HANDLES)
        out_hcount = A20_CH_MAX_HANDLES;

    uint32_t out_len = kargs.data_buf_len;
    r = a20_channel_recv(ep, (void *)kargs.data_buf, &out_len,
                         hinfos, &out_hcount, ht);
    if (r < 0) return r;

    /* Install received handles into receiver's handle table */
    a20_handle_t out_handles[A20_CH_MAX_HANDLES];
    a20_rights_t out_rights[A20_CH_MAX_HANDLES];
    for (uint32_t i = 0; i < out_hcount; i++) {
        out_rights[i] = hinfos[i].transfer_rights;
        int64_t nh = a20_handle_install_temporal(ht, hinfos[i].object,
                                                  hinfos[i].type,
                                                  hinfos[i].transfer_rights,
                                                  0, 0, 0, 0);
        out_handles[i] = (nh >= 0) ? (a20_handle_t)nh : A20_HANDLE_NULL;
    }

    /* Copy results to user */
    kargs.out_data_len = out_len;
    kargs.out_handle_count = out_hcount;
    if (kargs.handle_buf && out_hcount > 0) {
        uint32_t copy_count = out_hcount < kargs.handle_buf_count
                              ? out_hcount : kargs.handle_buf_count;
        if (copy_to_user((void *)kargs.handle_buf, out_handles,
                         copy_count * sizeof(a20_handle_t)) < 0)
            return -A20_ERR_FAULT;
    }
    if (kargs.out_rights_buf && out_hcount > 0) {
        if (copy_to_user((void *)kargs.out_rights_buf, out_rights,
                         out_hcount * sizeof(a20_rights_t)) < 0)
            return -A20_ERR_FAULT;
    }
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return (int64_t)out_len;
}

int64_t sys_a20_event_watch_fs(const a20_syscall_args_t *args)
{
    a20_handle_t dir_h = (a20_handle_t)A20_ARG(0);
    a20_handle_t queue_h = (a20_handle_t)A20_ARG(1);
    uint32_t event_mask = (uint32_t)A20_ARG(2);
    uint64_t user_data = A20_ARG(3);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t dir_entry;
    int64_t r = a20_handle_lookup_internal(ht, dir_h, A20_OBJ_DIRECTORY,
                                            A20_RIGHT_READ, &dir_entry);
    if (r < 0) return r;

    a20_handle_entry_t eq_entry;
    r = a20_handle_lookup_internal(ht, queue_h, A20_OBJ_EVENT_QUEUE,
                                    A20_RIGHT_WRITE, &eq_entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)eq_entry.object;
    return a20_eventq_watch(eq, dir_h, dir_entry.object,
                             dir_entry.type, event_mask, user_data);
}

