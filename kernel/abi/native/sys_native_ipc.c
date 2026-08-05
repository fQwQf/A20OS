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
extern int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                           uint16_t type, a20_rights_t rights,
                                           uint64_t expiry_tick, uint32_t remaining_ops,
                                           uint32_t temporal_flags, uint8_t security_label);
extern int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                          uint16_t expected_type, a20_rights_t required_rights,
                                          a20_handle_entry_t *out);
extern int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                               a20_handle_t h,
                                               uint16_t expected_type,
                                               a20_rights_t required_rights,
                                               a20_handle_entry_t *out);
extern int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
extern void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);
extern int64_t a20_handle_reserve_many(struct a20_ht_internal *ht,
                                       a20_handle_t *handles, uint32_t count);
extern void a20_handle_abort_reserved(struct a20_ht_internal *ht,
                                      a20_handle_t *handles, uint32_t count);
extern int64_t a20_handle_commit_reserved_temporal(struct a20_ht_internal *ht,
                                                   a20_handle_t h, void *object,
                                                   uint16_t type, a20_rights_t rights,
                                                   uint64_t expiry_tick,
                                                   uint32_t remaining_ops,
                                                   uint32_t temporal_flags,
                                                   uint8_t security_label);
extern void a20_object_release(void *object, uint16_t type);

/* ===== IPC (0x0500) ===== */

int64_t sys_a20_event_queue_create(const a20_syscall_args_t *args)
{
    a20_event_queue_create_args_t *uargs = (a20_event_queue_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_event_queue_create_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    a20_eventq_t *eq = a20_eventq_create(kargs.capacity_hint);
    if (!eq) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { a20_eventq_release(eq); return -A20_ERR_BAD_HANDLE; }

    int64_t h = a20_handle_install(ht, eq, A20_OBJ_EVENT_QUEUE,
                                   A20_RIGHT_READ | A20_RIGHT_STAT |
                                   A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                   A20_RIGHT_CONTROL);
    if (h < 0) {
        a20_eventq_release(eq);
        return h;
    }
    kargs.out_queue = (a20_handle_t)h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)h);
        return -A20_ERR_FAULT;
    }
    return A20_OK;
}

int64_t sys_a20_event_watch(const a20_syscall_args_t *args)
{
    a20_event_watch_args_t *uargs = (a20_event_watch_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_event_watch_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t eq_entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.queue,
                                               A20_OBJ_EVENT_QUEUE,
                                               A20_RIGHT_READ, &eq_entry);
    if (r < 0) return r;

    a20_handle_entry_t tgt_entry;
    r = a20_handle_lookup_ref_internal(ht, kargs.target, A20_OBJ_INVALID,
                                       A20_RIGHTS_NONE, &tgt_entry);
    if (r < 0) {
        a20_object_release(eq_entry.object, eq_entry.type);
        return r;
    }

    a20_eventq_t *eq = (a20_eventq_t *)eq_entry.object;
    r = a20_eventq_watch(eq, kargs.target, tgt_entry.object,
                         tgt_entry.type, kargs.event_mask, kargs.user_data);
    a20_object_release(tgt_entry.object, tgt_entry.type);
    a20_object_release(eq_entry.object, eq_entry.type);
    return r;
}

int64_t sys_a20_event_wait(const a20_syscall_args_t *args)
{
    a20_event_wait_args_t *uargs = (a20_event_wait_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_event_wait_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.max_events == 0 || !kargs.events)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.max_events > 64)
        kargs.max_events = 64;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.queue,
                                               A20_OBJ_EVENT_QUEUE,
                                               A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)entry.object;
    a20_pending_event_t evbuf[64];
    r = a20_eventq_wait(eq, evbuf, kargs.max_events, kargs.timeout_ns);
    a20_object_release(entry.object, entry.type);
    if (r < 0) return r;

    uint32_t n = (uint32_t)r;
    if (copy_to_user((void *)kargs.events, evbuf,
                     n * sizeof(a20_pending_event_t)) < 0)
        return -A20_ERR_FAULT;
    kargs.out_count = n;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return (int64_t)n;
}

int64_t sys_a20_event_cancel(const a20_syscall_args_t *args)
{
    a20_handle_t queue_h = (a20_handle_t)A20_ARG(0);
    a20_handle_t target_h = (a20_handle_t)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, queue_h,
                                               A20_OBJ_EVENT_QUEUE,
                                               A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)entry.object;
    r = a20_eventq_cancel(eq, target_h);
    a20_object_release(entry.object, entry.type);
    return r;
}

int64_t sys_a20_channel_create(const a20_syscall_args_t *args)
{
    a20_channel_create_args_t *uargs = (a20_channel_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_channel_create_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    /* Typed channel (docs/native-abi/05-ipc.md §2.3): copy the type
     * signature from userspace and normalize its limits. */
    a20_channel_type_t ktype;
    const a20_channel_type_t *typep = NULL;
    if (kargs.type) {
        if (copy_from_user(&ktype, (const void *)kargs.type, sizeof(ktype)) < 0)
            return -A20_ERR_FAULT;
        if (ktype.version > 1)
            return -A20_ERR_INVALID_ARGUMENT;
        if (ktype.flags & ~(A20_CHAN_TYPE_ORDERED | A20_CHAN_TYPE_STRICT))
            return -A20_ERR_INVALID_ARGUMENT;
        if (ktype.max_data_size == 0 || ktype.max_data_size > A20_CH_MAX_DATA)
            ktype.max_data_size = A20_CH_MAX_DATA;
        if (ktype.max_handles == 0 || ktype.max_handles > A20_CH_MAX_HANDLES)
            ktype.max_handles = A20_CH_MAX_HANDLES;
        typep = &ktype;
    }

    a20_channel_ep_t *ep0 = a20_channel_create(kargs.msg_capacity, typep);
    if (!ep0) return -A20_ERR_NO_MEMORY;
    a20_channel_ep_t *ep1 = ep0->peer;
    /* Each endpoint's initial reference is owned by its installed handle. */

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
    int64_t h1 = (h0 >= 0)
                 ? a20_handle_install(ht, ep1, A20_OBJ_CHANNEL_ENDPOINT, rights)
                 : -A20_ERR_NO_SPACE;

    if (h0 < 0 || h1 < 0) {
        if (h0 >= 0) a20_handle_remove(ht, (a20_handle_t)h0);
        else         a20_channel_ep_release(ep0);
        a20_channel_ep_release(ep1);
        return (h0 < 0) ? h0 : h1;
    }

    kargs.out_endpoints[0] = (a20_handle_t)h0;
    kargs.out_endpoints[1] = (a20_handle_t)h1;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)h0);
        a20_handle_remove(ht, (a20_handle_t)h1);
        return -A20_ERR_FAULT;
    }
    return A20_OK;
}

int64_t sys_a20_channel_send(const a20_syscall_args_t *args)
{
    a20_msg_send_args_t *uargs = (a20_msg_send_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_msg_send_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.channel,
                                               A20_OBJ_CHANNEL_ENDPOINT,
                                               A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;

    if (a20_ht_get_label(ht) > entry.security_label) {
        r = -A20_ERR_ACCESS;
        goto out_ep;
    }
    a20_channel_ep_t *ep = (a20_channel_ep_t *)entry.object;

    void *kdata = NULL;
    if (kargs.data_len > 0) {
        if (!kargs.data || kargs.data_len > A20_CH_MAX_DATA) {
            r = kargs.data_len > A20_CH_MAX_DATA
                ? -A20_ERR_INVALID_ARGUMENT : -A20_ERR_FAULT;
            goto out_ep;
        }
        kdata = kmalloc(kargs.data_len);
        if (!kdata) {
            r = -A20_ERR_NO_MEMORY;
            goto out_ep;
        }
        if (copy_from_user(kdata, (const void *)kargs.data,
                           kargs.data_len) < 0) {
            r = -A20_ERR_FAULT;
            goto out_data;
        }
    }

    a20_ch_handle_info_t hinfos[A20_CH_MAX_HANDLES];
    uint32_t actual_hcount = 0;

    if (kargs.handle_count > 0 && kargs.handles) {
        if (kargs.handle_count > A20_CH_MAX_HANDLES) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_data;
        }

        a20_handle_t user_handles[A20_CH_MAX_HANDLES];
        a20_rights_t user_rights[A20_CH_MAX_HANDLES];
        if (copy_from_user(user_handles, (void *)kargs.handles,
                           kargs.handle_count * sizeof(a20_handle_t)) < 0) {
            r = -A20_ERR_FAULT;
            goto out_data;
        }

        uint32_t rights_count = 0;
        if (kargs.transfer_rights) {
            if (copy_from_user(user_rights, (void *)kargs.transfer_rights,
                               kargs.handle_count * sizeof(a20_rights_t)) < 0) {
                r = -A20_ERR_FAULT;
                goto out_data;
            }
            rights_count = kargs.handle_count;
        }

        for (uint32_t i = 0; i < kargs.handle_count; i++) {
            a20_handle_entry_t he;
            r = a20_handle_lookup_ref_internal(ht, user_handles[i],
                                               A20_OBJ_INVALID,
                                               A20_RIGHT_TRANSFER, &he);
            if (r < 0) goto out_handles;

            a20_rights_t xfer_rights = (rights_count > 0)
                                       ? user_rights[i] : he.rights;
            /* docs/native-abi/06-security.md §4.1: ρ_recv = ρ_send ∩ ρ_transfer */
            hinfos[actual_hcount].object = he.object;
            hinfos[actual_hcount].type = he.type;
            hinfos[actual_hcount].transfer_rights = he.rights & xfer_rights;
            hinfos[actual_hcount].expiry_tick = he.expiry_tick;
            hinfos[actual_hcount].remaining_ops = he.remaining_ops;
            hinfos[actual_hcount].temporal_flags = he.temporal_flags;
            hinfos[actual_hcount].security_label = he.security_label;
            actual_hcount++;
            if (hinfos[actual_hcount - 1].transfer_rights == 0) {
                r = -A20_ERR_ACCESS;
                goto out_handles;
            }
        }
    } else if (kargs.handle_count > 0 || kargs.handles) {
        r = -A20_ERR_INVALID_ARGUMENT;
        goto out_data;
    }

    r = a20_channel_send(ep, kdata, kargs.data_len,
                         hinfos, actual_hcount, ht, kargs.flags);

out_handles:
    for (uint32_t i = 0; i < actual_hcount; i++)
        a20_object_release(hinfos[i].object, hinfos[i].type);
out_data:
    kfree(kdata);
out_ep:
    a20_object_release(entry.object, entry.type);
    return r;
}

int64_t sys_a20_channel_recv(const a20_syscall_args_t *args)
{
    a20_msg_recv_args_t *uargs = (a20_msg_recv_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_msg_recv_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.channel,
                                               A20_OBJ_CHANNEL_ENDPOINT,
                                               A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    if (a20_ht_get_label(ht) < entry.security_label) {
        r = -A20_ERR_ACCESS;
        goto out_ep;
    }
    a20_channel_ep_t *ep = (a20_channel_ep_t *)entry.object;

    /* Phase 1: wait for a message; ep->lock is held on success. */
    uint32_t msg_data_len = 0;
    uint32_t msg_handles = 0;
    r = a20_channel_recv_begin(ep, kargs.flags, &msg_data_len, &msg_handles);
    if (r < 0) goto out_ep;

    if ((msg_data_len > 0 && (!kargs.data_buf ||
                              kargs.data_buf_len < msg_data_len)) ||
        (msg_handles > 0 && (!kargs.handle_buf ||
                             kargs.handle_buf_count < msg_handles))) {
        a20_channel_recv_abort(ep);
        r = -A20_ERR_NO_SPACE;
        goto out_ep;
    }

    /* Phase 2: reserve receiver slots BEFORE dequeuing so a full table
     * yields NO_SPACE with the message still queued (docs/native-abi/
     * 05-ipc.md §2.6: no partial delivery). */
    a20_handle_t reserved[A20_CH_MAX_HANDLES];
    uint32_t nreserved = 0;
    if (msg_handles > 0) {
        r = a20_handle_reserve_many(ht, reserved, msg_handles);
        if (r < 0) {
            a20_channel_recv_abort(ep);
            goto out_ep;
        }
        nreserved = msg_handles;
    }

    void *kdata = NULL;
    if (msg_data_len > 0) {
        kdata = kmalloc(msg_data_len);
        if (!kdata) {
            a20_handle_abort_reserved(ht, reserved, nreserved);
            a20_channel_recv_abort(ep);
            r = -A20_ERR_NO_MEMORY;
            goto out_ep;
        }
    }

    a20_ch_handle_info_t hinfos[A20_CH_MAX_HANDLES];
    uint32_t out_hcount = msg_handles;
    uint32_t out_len = msg_data_len;
    r = a20_channel_recv_finish(ep, kdata, &out_len, hinfos, &out_hcount);
    if (r < 0) {
        kfree(kdata);
        a20_handle_abort_reserved(ht, reserved, nreserved);
        goto out_ep;
    }

    /* Install received handles into the reserved slots; temporal limits and
     * security labels are inherited from the sender's entries. */
    a20_handle_t out_handles[A20_CH_MAX_HANDLES];
    a20_rights_t out_rights[A20_CH_MAX_HANDLES];
    uint32_t installed = 0;
    for (uint32_t i = 0; i < out_hcount; i++) {
        out_rights[i] = hinfos[i].transfer_rights;
        int64_t cr = a20_handle_commit_reserved_temporal(
            ht, reserved[i], hinfos[i].object, hinfos[i].type,
            hinfos[i].transfer_rights,
            hinfos[i].expiry_tick, hinfos[i].remaining_ops,
            hinfos[i].temporal_flags, hinfos[i].security_label);
        if (cr < 0) {
            a20_object_release(hinfos[i].object, hinfos[i].type);
            for (uint32_t j = i + 1; j < out_hcount; j++)
                a20_object_release(hinfos[j].object, hinfos[j].type);
            for (uint32_t j = 0; j < installed; j++)
                a20_handle_remove(ht, out_handles[j]);
            a20_handle_abort_reserved(ht, &reserved[i], out_hcount - i);
            kfree(kdata);
            r = cr;
            goto out_ep;
        }
        out_handles[i] = reserved[i];
        installed++;
    }

    /* Copy results to user.  If a late fault still happens, roll back the
     * freshly installed handles rather than leaking unnamed capabilities. */
    kargs.out_data_len = out_len;
    kargs.out_handle_count = installed;
    if (out_len > 0 &&
        copy_to_user((void *)kargs.data_buf, kdata, out_len) < 0) {
        r = -A20_ERR_FAULT;
        goto out_rollback;
    }
    if (installed > 0 &&
        copy_to_user((void *)kargs.handle_buf, out_handles,
                     installed * sizeof(a20_handle_t)) < 0) {
        r = -A20_ERR_FAULT;
        goto out_rollback;
    }
    if (kargs.out_rights_buf && installed > 0 &&
        copy_to_user((void *)kargs.out_rights_buf, out_rights,
                     installed * sizeof(a20_rights_t)) < 0) {
        r = -A20_ERR_FAULT;
        goto out_rollback;
    }
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        r = -A20_ERR_FAULT;
        goto out_rollback;
    }

    kfree(kdata);
    a20_object_release(entry.object, entry.type);
    return (int64_t)out_len;

out_rollback:
    for (uint32_t i = 0; i < installed; i++)
        a20_handle_remove(ht, out_handles[i]);
    kfree(kdata);
out_ep:
    a20_object_release(entry.object, entry.type);
    return r;
}

/*
 * sys_a20_channel_call — fused RPC (docs/hybrid-kernel/00-design.md §4.1).
 *
 * One trap performs channel_send(request) + channel_recv(reply) on the same
 * endpoint with a single handle lookup (READ|WRITE) and a single args
 * validation.  Compared to the two-syscall sequence this halves the trap
 * count of an RPC round trip; the peer wakeup still goes through the
 * wait_queue priority-preempt path, so a blocked server is scheduled
 * immediately after the request lands.
 *
 * Error partition: a failure from the send stage means the request was not
 * delivered.  A failure from the reply stage (only possible with
 * A20_MSG_NONBLOCK, or on interrupt) means the request may already be
 * delivered and a reply may arrive later.
 */
int64_t sys_a20_channel_call(const a20_syscall_args_t *args)
{
    a20_channel_call_args_t *uargs = (a20_channel_call_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_channel_call_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.flags & ~A20_MSG_NONBLOCK)
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.channel,
                                               A20_OBJ_CHANNEL_ENDPOINT,
                                               A20_RIGHT_READ | A20_RIGHT_WRITE,
                                               &entry);
    if (r < 0) return r;

    /* Combined BLP: write requires no-write-down... the send and recv
     * checks together reduce to label equality with the endpoint. */
    if (a20_ht_get_label(ht) != entry.security_label) {
        r = -A20_ERR_ACCESS;
        goto out_ep;
    }
    a20_channel_ep_t *ep = (a20_channel_ep_t *)entry.object;

    /* ---- Send stage (mirrors sys_a20_channel_send) ---- */
    void *kdata = NULL;
    if (kargs.data_len > 0) {
        if (!kargs.data || kargs.data_len > A20_CH_MAX_DATA) {
            r = kargs.data_len > A20_CH_MAX_DATA
                ? -A20_ERR_INVALID_ARGUMENT : -A20_ERR_FAULT;
            goto out_ep;
        }
        kdata = kmalloc(kargs.data_len);
        if (!kdata) {
            r = -A20_ERR_NO_MEMORY;
            goto out_ep;
        }
        if (copy_from_user(kdata, (const void *)kargs.data,
                           kargs.data_len) < 0) {
            r = -A20_ERR_FAULT;
            goto out_data;
        }
    }

    a20_ch_handle_info_t hinfos[A20_CH_MAX_HANDLES];
    uint32_t actual_hcount = 0;

    if (kargs.handle_count > 0 && kargs.handles) {
        if (kargs.handle_count > A20_CH_MAX_HANDLES) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out_data;
        }

        a20_handle_t user_handles[A20_CH_MAX_HANDLES];
        a20_rights_t user_rights[A20_CH_MAX_HANDLES];
        if (copy_from_user(user_handles, (void *)kargs.handles,
                           kargs.handle_count * sizeof(a20_handle_t)) < 0) {
            r = -A20_ERR_FAULT;
            goto out_data;
        }

        uint32_t rights_count = 0;
        if (kargs.transfer_rights) {
            if (copy_from_user(user_rights, (void *)kargs.transfer_rights,
                               kargs.handle_count * sizeof(a20_rights_t)) < 0) {
                r = -A20_ERR_FAULT;
                goto out_data;
            }
            rights_count = kargs.handle_count;
        }

        for (uint32_t i = 0; i < kargs.handle_count; i++) {
            a20_handle_entry_t he;
            r = a20_handle_lookup_ref_internal(ht, user_handles[i],
                                               A20_OBJ_INVALID,
                                               A20_RIGHT_TRANSFER, &he);
            if (r < 0) goto out_handles;

            a20_rights_t xfer_rights = (rights_count > 0)
                                       ? user_rights[i] : he.rights;
            hinfos[actual_hcount].object = he.object;
            hinfos[actual_hcount].type = he.type;
            hinfos[actual_hcount].transfer_rights = he.rights & xfer_rights;
            hinfos[actual_hcount].expiry_tick = he.expiry_tick;
            hinfos[actual_hcount].remaining_ops = he.remaining_ops;
            hinfos[actual_hcount].temporal_flags = he.temporal_flags;
            hinfos[actual_hcount].security_label = he.security_label;
            actual_hcount++;
            if (hinfos[actual_hcount - 1].transfer_rights == 0) {
                r = -A20_ERR_ACCESS;
                goto out_handles;
            }
        }
    } else if (kargs.handle_count > 0 || kargs.handles) {
        r = -A20_ERR_INVALID_ARGUMENT;
        goto out_data;
    }

    r = a20_channel_send_dwc(ep, kdata, kargs.data_len,
                             hinfos, actual_hcount, ht, kargs.flags, 1);

out_handles:
    for (uint32_t i = 0; i < actual_hcount; i++)
        a20_object_release(hinfos[i].object, hinfos[i].type);
out_data:
    kfree(kdata);
    if (r < 0) goto out_ep; /* send stage failed: request not delivered */

    /* ---- Reply stage (mirrors sys_a20_channel_recv) ---- */
    uint32_t msg_data_len = 0;
    uint32_t msg_handles = 0;
    r = a20_channel_recv_begin_donate(ep, kargs.flags, &msg_data_len,
                                      &msg_handles);
    if (r < 0) goto out_ep;

    if ((msg_data_len > 0 && (!kargs.reply_buf ||
                              kargs.reply_buf_len < msg_data_len)) ||
        (msg_handles > 0 && (!kargs.reply_handle_buf ||
                             kargs.reply_handle_buf_count < msg_handles))) {
        a20_channel_recv_abort(ep);
        r = -A20_ERR_NO_SPACE;
        goto out_ep;
    }

    a20_handle_t reserved[A20_CH_MAX_HANDLES];
    uint32_t nreserved = 0;
    if (msg_handles > 0) {
        r = a20_handle_reserve_many(ht, reserved, msg_handles);
        if (r < 0) {
            a20_channel_recv_abort(ep);
            goto out_ep;
        }
        nreserved = msg_handles;
    }

    void *rdata = NULL;
    if (msg_data_len > 0) {
        rdata = kmalloc(msg_data_len);
        if (!rdata) {
            a20_handle_abort_reserved(ht, reserved, nreserved);
            a20_channel_recv_abort(ep);
            r = -A20_ERR_NO_MEMORY;
            goto out_ep;
        }
    }

    a20_ch_handle_info_t rhinfos[A20_CH_MAX_HANDLES];
    uint32_t out_hcount = msg_handles;
    uint32_t out_len = msg_data_len;
    r = a20_channel_recv_finish(ep, rdata, &out_len, rhinfos, &out_hcount);
    if (r < 0) {
        kfree(rdata);
        a20_handle_abort_reserved(ht, reserved, nreserved);
        goto out_ep;
    }

    a20_handle_t out_handles[A20_CH_MAX_HANDLES];
    a20_rights_t out_rights[A20_CH_MAX_HANDLES];
    uint32_t installed = 0;
    for (uint32_t i = 0; i < out_hcount; i++) {
        out_rights[i] = rhinfos[i].transfer_rights;
        int64_t cr = a20_handle_commit_reserved_temporal(
            ht, reserved[i], rhinfos[i].object, rhinfos[i].type,
            rhinfos[i].transfer_rights,
            rhinfos[i].expiry_tick, rhinfos[i].remaining_ops,
            rhinfos[i].temporal_flags, rhinfos[i].security_label);
        if (cr < 0) {
            a20_object_release(rhinfos[i].object, rhinfos[i].type);
            for (uint32_t j = i + 1; j < out_hcount; j++)
                a20_object_release(rhinfos[j].object, rhinfos[j].type);
            for (uint32_t j = 0; j < installed; j++)
                a20_handle_remove(ht, out_handles[j]);
            a20_handle_abort_reserved(ht, &reserved[i], out_hcount - i);
            kfree(rdata);
            r = cr;
            goto out_ep;
        }
        out_handles[i] = reserved[i];
        installed++;
    }

    kargs.out_reply_len = out_len;
    kargs.out_reply_handles = installed;
    if (out_len > 0 &&
        copy_to_user((void *)kargs.reply_buf, rdata, out_len) < 0) {
        r = -A20_ERR_FAULT;
        goto out_call_rollback;
    }
    if (installed > 0 &&
        copy_to_user((void *)kargs.reply_handle_buf, out_handles,
                     installed * sizeof(a20_handle_t)) < 0) {
        r = -A20_ERR_FAULT;
        goto out_call_rollback;
    }
    if (kargs.reply_rights_buf && installed > 0 &&
        copy_to_user((void *)kargs.reply_rights_buf, out_rights,
                     installed * sizeof(a20_rights_t)) < 0) {
        r = -A20_ERR_FAULT;
        goto out_call_rollback;
    }
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        r = -A20_ERR_FAULT;
        goto out_call_rollback;
    }

    kfree(rdata);
    a20_object_release(entry.object, entry.type);
    return (int64_t)out_len;

out_call_rollback:
    for (uint32_t i = 0; i < installed; i++)
        a20_handle_remove(ht, out_handles[i]);
    kfree(rdata);
    goto out_ep;

out_ep:
    a20_object_release(entry.object, entry.type);
    return r;
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
    int64_t r = a20_handle_lookup_ref_internal(ht, dir_h, A20_OBJ_DIRECTORY,
                                               A20_RIGHT_READ, &dir_entry);
    if (r < 0) return r;

    a20_handle_entry_t eq_entry;
    r = a20_handle_lookup_ref_internal(ht, queue_h, A20_OBJ_EVENT_QUEUE,
                                       A20_RIGHT_READ, &eq_entry);
    if (r < 0) {
        a20_object_release(dir_entry.object, dir_entry.type);
        return r;
    }

    a20_eventq_t *eq = (a20_eventq_t *)eq_entry.object;
    r = a20_eventq_watch(eq, dir_h, dir_entry.object,
                         dir_entry.type, event_mask, user_data);
    a20_object_release(eq_entry.object, eq_entry.type);
    a20_object_release(dir_entry.object, dir_entry.type);
    return r;

}
