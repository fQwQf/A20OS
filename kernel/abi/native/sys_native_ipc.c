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
#include "fs/pipe.h"
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
#include "channel_recv_txn.h"

#define A20_ARG(n) (args->arg[(n)])

typedef struct a20_recv_publish_context {
    void *user_args;
    void *kernel_args;
    int is_call;
} a20_recv_publish_context_t;

static int a20_channel_recv_publish(void *opaque, uint32_t data_len,
                                    uint32_t handle_count)
{
    a20_recv_publish_context_t *context = opaque;
    if (!context || !context->user_args || !context->kernel_args)
        return -1;
    if (context->is_call) {
        a20_channel_call_args_t *kargs = context->kernel_args;
        kargs->out_reply_len = data_len;
        kargs->out_reply_handles = handle_count;
        return a20_copy_struct_to_user(context->user_args, kargs,
                                       sizeof(*kargs)) < 0 ? -1 : 0;
    }
    a20_msg_recv_args_t *kargs = context->kernel_args;
    kargs->out_data_len = data_len;
    kargs->out_handle_count = handle_count;
    return a20_copy_struct_to_user(context->user_args, kargs,
                                   sizeof(*kargs)) < 0 ? -1 : 0;
}

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
/* Cluster proxy errno mapping (kernel/abi/native/sys_native_cluster.c,
 * docs/zh/cluster/03-kernel-impl.md §1). */
extern int64_t a20_clx_map_send_err(int64_t r, const a20_channel_ep_t *ep);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);
extern int64_t a20_native_vfs_result(int r);
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
    a20_event_queue_create_args_t *uargs = (a20_event_queue_create_args_t *)(uintptr_t)A20_ARG(0);
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
    a20_event_watch_args_t *uargs = (a20_event_watch_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_event_watch_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);
    if (kargs.flags & ~A20_WATCH_LEVEL)
        return -A20_ERR_INVALID_ARGUMENT;

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
                         tgt_entry.type, kargs.event_mask, kargs.user_data,
                         (uint16_t)kargs.flags);
    a20_object_release(tgt_entry.object, tgt_entry.type);
    a20_object_release(eq_entry.object, eq_entry.type);
    return r;
}

int64_t sys_a20_event_wait(const a20_syscall_args_t *args)
{
    a20_event_wait_args_t *uargs = (a20_event_wait_args_t *)(uintptr_t)A20_ARG(0);
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
    if (copy_to_user((void *)(uintptr_t)kargs.events, evbuf,
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
    a20_channel_create_args_t *uargs = (a20_channel_create_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_channel_create_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.flags & ~(A20_CHANNEL_BYTE_STREAM | A20_CHANNEL_NONBLOCK))
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.flags & A20_CHANNEL_BYTE_STREAM) {
        if (kargs.type || kargs.msg_capacity)
            return -A20_ERR_INVALID_ARGUMENT;
        struct a20_ht_internal *ht = task_get_a20_ht(proc_current());
        if (!ht) return -A20_ERR_BAD_HANDLE;
        vfile_t *rd, *wr;
        int error = pipe_create_vfiles(&rd, &wr,
                                      kargs.flags & A20_CHANNEL_NONBLOCK ? O_NONBLOCK : 0);
        if (error < 0) return a20_native_vfs_result(error);
        a20_rights_t common = A20_RIGHT_STAT | A20_RIGHT_DUP |
                              A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL;
        int64_t h0 = a20_handle_install(ht, rd, A20_OBJ_PIPE_ENDPOINT, common | A20_RIGHT_READ);
        int64_t h1 = h0 >= 0 ? a20_handle_install(ht, wr, A20_OBJ_PIPE_ENDPOINT,
                                                common | A20_RIGHT_WRITE) : h0;
        if (h0 < 0 || h1 < 0) {
            if (h0 >= 0) a20_handle_remove(ht, (a20_handle_t)h0);
            else vfs_put_file(rd);
            vfs_put_file(wr);
            return h0 < 0 ? h0 : h1;
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
    if (kargs.flags)
        return -A20_ERR_INVALID_ARGUMENT;

    /* Typed channel (docs/zh/native-abi/05-ipc.md §2.3): copy the type
     * signature from userspace and normalize its limits. */
    a20_channel_type_t ktype;
    const a20_channel_type_t *typep = NULL;
    if (kargs.type) {
        if (copy_from_user(&ktype, (const void *)(uintptr_t)kargs.type, sizeof(ktype)) < 0)
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

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
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
    a20_msg_send_args_t *uargs = (a20_msg_send_args_t *)(uintptr_t)A20_ARG(0);
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
        if (copy_from_user(kdata, (const void *)(uintptr_t)kargs.data,
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
        if (copy_from_user(user_handles, (void *)(uintptr_t)kargs.handles,
                           kargs.handle_count * sizeof(a20_handle_t)) < 0) {
            r = -A20_ERR_FAULT;
            goto out_data;
        }

        uint32_t rights_count = 0;
        if (kargs.transfer_rights) {
            if (copy_from_user(user_rights, (void *)(uintptr_t)kargs.transfer_rights,
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
            /* docs/zh/native-abi/06-security.md §4.1: ρ_recv = ρ_send ∩ ρ_transfer */
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
    /* Cluster proxy endpoints (docs/zh/cluster/03-kernel-impl.md §1): the
     * typed-channel refusal of a cross-node handle maps to
     * A20_ERR_CLUSTER_UNSUPPORTED instead of TYPE_MISMATCH. */
    r = a20_clx_map_send_err(r, ep);

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
    a20_msg_recv_args_t *uargs = (a20_msg_recv_args_t *)(uintptr_t)A20_ARG(0);
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

    a20_recv_publish_context_t publish = {uargs, &kargs, 0};
    r = a20_channel_recv_txn(
        ep, ht, kargs.flags, 0,
        (void *)(uintptr_t)kargs.data_buf, kargs.data_buf_len,
        (void *)(uintptr_t)kargs.handle_buf, kargs.handle_buf_count,
        (void *)(uintptr_t)kargs.out_rights_buf, &publish,
        a20_channel_recv_publish);
    a20_object_release(entry.object, entry.type);
    return r;
out_ep:
    a20_object_release(entry.object, entry.type);
    return r;
}

/*
 * sys_a20_channel_call — fused RPC (docs/zh/hybrid-kernel/00-design.md §4.1).
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
    a20_channel_call_args_t *uargs = (a20_channel_call_args_t *)(uintptr_t)A20_ARG(0);
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
        if (copy_from_user(kdata, (const void *)(uintptr_t)kargs.data,
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
        if (copy_from_user(user_handles, (void *)(uintptr_t)kargs.handles,
                           kargs.handle_count * sizeof(a20_handle_t)) < 0) {
            r = -A20_ERR_FAULT;
            goto out_data;
        }

        uint32_t rights_count = 0;
        if (kargs.transfer_rights) {
            if (copy_from_user(user_rights, (void *)(uintptr_t)kargs.transfer_rights,
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
    /* Cluster proxy mapping, see sys_a20_channel_send. */
    r = a20_clx_map_send_err(r, ep);

out_handles:
    for (uint32_t i = 0; i < actual_hcount; i++)
        a20_object_release(hinfos[i].object, hinfos[i].type);
out_data:
    kfree(kdata);
    if (r < 0) goto out_ep; /* send stage failed: request not delivered */

    /* ---- Reply stage (mirrors sys_a20_channel_recv) ---- */
    a20_recv_publish_context_t publish = {uargs, &kargs, 1};
    r = a20_channel_recv_txn(
        ep, ht, kargs.flags, 1,
        (void *)(uintptr_t)kargs.reply_buf, kargs.reply_buf_len,
        (void *)(uintptr_t)kargs.reply_handle_buf,
        kargs.reply_handle_buf_count,
        (void *)(uintptr_t)kargs.reply_rights_buf, &publish,
        a20_channel_recv_publish);
    a20_object_release(entry.object, entry.type);
    return r;

out_ep:
    a20_object_release(entry.object, entry.type);
    return r;
}

int64_t sys_a20_event_watch_fs(const a20_syscall_args_t *args)
{
    a20_event_watch_fs_args_t *uargs =
        (a20_event_watch_fs_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_event_watch_fs_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    uint32_t fs_mask = (1u << A20_EVENT_FS_CREATE) |
                       (1u << A20_EVENT_FS_DELETE) |
                       (1u << A20_EVENT_FS_MODIFY) |
                       (1u << A20_EVENT_FS_RENAME);
    if (kargs.event_mask == 0 || (kargs.event_mask & ~fs_mask))
        return -A20_ERR_INVALID_ARGUMENT;

    char prefix[256] = {0};
    if (kargs.path_len >= sizeof(prefix))
        return -A20_ERR_NAME_TOO_LONG;
    if (kargs.path_len) {
        if (!kargs.path ||
            copy_from_user(prefix, (const void *)(uintptr_t)kargs.path,
                           kargs.path_len) < (long)kargs.path_len)
            return -A20_ERR_FAULT;
        /* Paths here filter direct child names. Recursive traversal would
         * require watches on each descendant directory, not a string match. */
        for (uint32_t i = 0; i < kargs.path_len; i++) {
            if (!prefix[i] || prefix[i] == '/')
                return -A20_ERR_INVALID_ARGUMENT;
        }
    }

    a20_handle_t dir_h = kargs.dir;
    a20_handle_t queue_h = kargs.queue;
    uint32_t event_mask = kargs.event_mask;
    uint64_t user_data = kargs.user_data;

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

    /* FS watches are keyed on the directory vnode so every handle referencing
     * the same directory receives the same change events (docs/zh/native-abi/
     * 09-native-abi-deepening.md §6).  The vnode is referenced by the fd
     * throughout the watch; vnode_put() cleans up the watch on release. */
    vfile_t *vf = (vfile_t *)dir_entry.object;
    if (!vf || !vf->vnode) {
        a20_object_release(eq_entry.object, eq_entry.type);
        a20_object_release(dir_entry.object, dir_entry.type);
        return -A20_ERR_BAD_HANDLE;
    }
    vnode_t *vn = vf->vnode;

    a20_eventq_t *eq = (a20_eventq_t *)eq_entry.object;
    r = a20_eventq_watch_fs(eq, dir_h, vn, (uint64_t)event_mask,
                            user_data, prefix);
    a20_object_release(eq_entry.object, eq_entry.type);
    a20_object_release(dir_entry.object, dir_entry.type);
    return r;

}
