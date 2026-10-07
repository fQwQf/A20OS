/*
 * A20OS Native ABI — Cluster syscalls (0x0520, docs/cluster/01-abi.md).
 *
 * W0 froze the six entry points as validating stubs; this file now wires
 * them to the kernel cluster core (kernel/cluster/, WA1).  Argument
 * validation is unchanged from the stub stage: same rules, same order, so
 * everything the frozen surface rejected before is rejected identically
 * now.
 *
 * Authority (01-abi §3: cluster_set_self/export/route need
 * A20_RIGHT_CLUSTER_ADMIN): the tree has no task-level A20 rights bitmask
 * -- rights live on handles -- so the admin authority is carried by
 * effective uid 0 (clusterd runs as root); registered in 01-abi 落地状态
 * (2026-10).  connect/event_subscribe/link_status stay unprivileged.
 *
 * errno mapping follows 01-abi 落地状态: ALREADY_EXISTS->A20_ERR_EXISTS(10),
 * RESOURCE_LIMIT->A20_ERR_NO_SPACE(13), INVALID_ARGS->A20_ERR_INVALID_ARGUMENT(12).
 */
#include "core/types.h"
#include "core/defs.h"
#include "core/string.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/resource.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/ipc_internal.h"
#include "sys_validate.h"
#include "ipc/ipc.h"
#include "cluster/clx_internal.h"
#include "proc/proc.h"

#define A20_ARG(n) (args->arg[(n)])

/* Reserved node identities from docs/cluster/01-abi.md §节点标识. */
static int clx_node_id_is_reserved(const a20_node_id_t *id)
{
    int all_zero = 1;
    int all_ff = 1;
    for (int i = 0; i < 16; i++) {
        if (id->bytes[i] != 0x00) all_zero = 0;
        if (id->bytes[i] != 0xff) all_ff = 0;
    }
    return all_zero || all_ff;
}

static int clx_reserved_is_zero(const uint64_t *reserved, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        if (reserved[i]) return 0;
    return 1;
}

static int clx_node_is_local(const a20_node_id_t *id)
{
    for (int i = 0; i < 16; i++)
        if (id->bytes[i]) return 0;
    return 1;
}

/* clusterd authority: see file header (01-abi 落地状态). */
static int clx_admin_ok(void)
{
    task_t *cur = proc_current();

    return cur && cur->cred.euid == 0;
}

static int clx_is_cluster_remote_send_err(int64_t r, const a20_channel_ep_t *ep)
{
    if (r != -A20_ERR_TYPE_MISMATCH || !ep || !ep->chan_type)
        return 0;
    return (ep->chan_type->flags & A20_CHAN_TYPE_REMOTE) != 0;
}

int64_t a20_clx_map_send_err(int64_t r, const a20_channel_ep_t *ep)
{
    /* 03-§1: 跨机 handle 传递在现成 ch_check_send_types 检查点上被拒后，
     * 在 ABI 层映射为 A20_ERR_CLUSTER_UNSUPPORTED（消息不发出）。 */
    if (clx_is_cluster_remote_send_err(r, ep))
        return -A20_ERR_CLUSTER_UNSUPPORTED;
    return r;
}

/* ---- 1. cluster_set_self ---------------------------------------------------- */

int64_t sys_a20_cluster_set_self(const a20_syscall_args_t *args)
{
    a20_cluster_set_self_args_t *uargs =
        (a20_cluster_set_self_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_cluster_set_self_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (!clx_reserved_is_zero(kargs.reserved, 2))
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.caps & ~A20_CLUSTER_CAPS_ALL)
        return -A20_ERR_INVALID_ARGUMENT;
    if (clx_node_id_is_reserved(&kargs.node_id))
        return -A20_ERR_INVALID_ARGUMENT;
    if (!clx_admin_ok())
        return -A20_ERR_PERM;

    int r = a20_clx_core_init();
    if (r < 0) return r;
    r = a20_clx_self_set(&kargs.node_id, kargs.caps);
    return r;
}

/* ---- 2. cluster_export ------------------------------------------------------- */

int64_t sys_a20_cluster_export(const a20_syscall_args_t *args)
{
    a20_cluster_export_args_t *uargs =
        (a20_cluster_export_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_cluster_export_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.flags & ~A20_EXPORT_FLAGS_ALL)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.reserved) return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.channel == A20_HANDLE_NULL)
        return -A20_ERR_BAD_HANDLE;
    if (kargs.name_len > A20_CLUSTER_SERVICE_NAME_MAX)
        return -A20_ERR_INVALID_ARGUMENT;
    if (!kargs.service_name || kargs.name_len == 0)
        return -A20_ERR_INVALID_ARGUMENT;
    if (!a20_clx_self_is_set())
        return -A20_ERR_NODE_UNREACHABLE;   /* 01-abi §2 */
    if (!clx_admin_ok())
        return -A20_ERR_PERM;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.channel,
                                               A20_OBJ_CHANNEL_ENDPOINT,
                                               A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    char name[A20_CLUSTER_SERVICE_NAME_MAX + 1];
    if (copy_from_user(name, (const void *)(uintptr_t)kargs.service_name,
                       kargs.name_len) < 0) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_FAULT;
    }
    name[kargs.name_len] = '\0';

    uint32_t slot = 0;
    r = a20_clx_export_register((a20_channel_ep_t *)entry.object, name,
                                kargs.name_len, kargs.flags, &slot);
    a20_object_release(entry.object, entry.type);
    if (r < 0) return r;
    return (int64_t)slot;                   /* >= 0: the 32-bit slot (01-abi §2) */
}

/* ---- 3. cluster_connect ------------------------------------------------------- */

int64_t sys_a20_cluster_connect(const a20_syscall_args_t *args)
{
    a20_cluster_connect_args_t *uargs =
        (a20_cluster_connect_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_cluster_connect_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.flags & ~A20_CONNECT_FLAGS_ALL)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.reserved) return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.name_len > A20_CLUSTER_SERVICE_NAME_MAX)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.service_name && kargs.name_len == 0)
        return -A20_ERR_INVALID_ARGUMENT;

    int r = a20_clx_core_init();
    if (r < 0) return r;

    char name[A20_CLUSTER_SERVICE_NAME_MAX + 1];
    int have_name = 0;
    if (kargs.service_name && kargs.name_len) {
        if (copy_from_user(name, (const void *)(uintptr_t)kargs.service_name,
                           kargs.name_len) < 0)
            return -A20_ERR_FAULT;
        name[kargs.name_len] = '\0';
        have_name = 1;
    }

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    /* Rights: cluster_connect needs no A20_RIGHT_CLUSTER_ADMIN (01-abi
     * §3 权限位与标签). */
    static const a20_rights_t ep_rights = A20_RIGHT_READ | A20_RIGHT_WRITE |
                                          A20_RIGHT_STAT | A20_RIGHT_DUP |
                                          A20_RIGHT_TRANSFER;

    a20_channel_ep_t *ep = NULL;
    if (clx_node_is_local(&kargs.node_id)) {
        /* LOCAL: local fast-path semantics, bit-identical (01-abi §4).
         * Resolution hands the caller a fresh pair half; the connection
         * half is delivered to the exported server as a handle-carrying
         * connect request, decoupled from the remote reply path
         * (01-abi 落地状态 #10). */
        if (have_name) {
            r = a20_clx_export_connect_local(0, name, kargs.name_len, &ep);
            if (r < 0) return r;
        } else if (kargs.slot) {
            r = a20_clx_export_connect_local(kargs.slot, NULL, 0, &ep);
            if (r < 0) return r;
        } else {
            return -A20_ERR_INVALID_ARGUMENT;   /* nothing to resolve */
        }
    } else {
        if (clx_node_id_is_reserved(&kargs.node_id))
            return -A20_ERR_INVALID_ARGUMENT;   /* BROADCAST via connect */
        if (!a20_clx_self_is_set())
            return -A20_ERR_NODE_UNREACHABLE;
        if (have_name) {
            /* Remote name resolution is clusterd's job (05-userspace.md);
             * the v0 kernel resolves slots only. */
            return -A20_ERR_CLUSTER_UNSUPPORTED;
        }
        uint32_t transport_id, nh_len;
        uint8_t nh[A20_CLX_LINK_ADDR_MAX];
        if (a20_clx_route_lookup(&kargs.node_id, &transport_id, nh,
                                 &nh_len) < 0)
            return -A20_ERR_NODE_UNREACHABLE;
        int err = 0;
        ep = a20_clx_proxy_create(&kargs.node_id, kargs.slot, kargs.flags,
                                  &err);
        if (!ep) return err;
    }

    int64_t h = a20_handle_install(ht, ep, A20_OBJ_CHANNEL_ENDPOINT,
                                   ep_rights);
    if (h < 0) {
        /* ep came referenced: from proxy_create (caller-half ownership) or
         * from export_connect_local (fresh pair's caller half). */
        a20_channel_ep_release(ep);
        return h;
    }
    return h;
}

/* ---- 4. cluster_route ----------------------------------------------------------- */

int64_t sys_a20_cluster_route(const a20_syscall_args_t *args)
{
    a20_cluster_route_args_t *uargs =
        (a20_cluster_route_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_cluster_route_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.op != A20_ROUTE_ADD && kargs.op != A20_ROUTE_DEL &&
        kargs.op != A20_ROUTE_REPLACE)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.reserved) return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.next_hop_len > sizeof(kargs.next_hop))
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.op != A20_ROUTE_DEL && clx_node_id_is_reserved(&kargs.node_id))
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.transport_id != A20_CLX_TRANSPORT_LOOPBACK &&
        kargs.transport_id != A20_CLX_TRANSPORT_UDP &&
        kargs.transport_id != A20_CLX_TRANSPORT_UART)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.op != A20_ROUTE_DEL && kargs.next_hop_len == 0)
        return -A20_ERR_INVALID_ARGUMENT;
    if (!clx_admin_ok())
        return -A20_ERR_PERM;

    int r = a20_clx_core_init();
    if (r < 0) return r;

    if (kargs.op == A20_ROUTE_DEL)
        return a20_clx_route_del(&kargs.node_id);
    if (kargs.op == A20_ROUTE_ADD)
        return a20_clx_route_add(&kargs.node_id, kargs.transport_id,
                                 kargs.next_hop, kargs.next_hop_len,
                                 kargs.metric);
    return a20_clx_route_replace(&kargs.node_id, kargs.transport_id,
                                 kargs.next_hop, kargs.next_hop_len,
                                 kargs.metric);
}

/* ---- 5. cluster_event_subscribe --------------------------------------------------- */

int64_t sys_a20_cluster_event_subscribe(const a20_syscall_args_t *args)
{
    a20_cluster_event_subscribe_args_t *uargs =
        (a20_cluster_event_subscribe_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_cluster_event_subscribe_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.mask & ~A20_CLX_EVENT_MASK_ALL)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.reserved) return -A20_ERR_INVALID_ARGUMENT;

    int r = a20_clx_core_init();
    if (r < 0) return r;

    a20_eventq_t *eq = a20_clx_event_subscribe(kargs.mask);
    if (!eq) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) {
        a20_eventq_release(eq);
        return -A20_ERR_BAD_HANDLE;
    }
    int64_t h = a20_handle_install(ht, eq, A20_OBJ_EVENT_QUEUE,
                                   A20_RIGHT_READ | A20_RIGHT_WRITE);
    if (h < 0) {
        a20_eventq_release(eq);
        return h;
    }
    return h;
}

/* ---- 6. cluster_link_status --------------------------------------------------------- */

int64_t sys_a20_cluster_link_status(const a20_syscall_args_t *args)
{
    a20_cluster_link_status_args_t *uargs =
        (a20_cluster_link_status_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_cluster_link_status_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.reserved) return -A20_ERR_INVALID_ARGUMENT;
    /* node_id is an input; the counters below it are outputs, so nothing
     * else is validated (01-abi 落地状态: 输出字段不参与入参校验). */

    int r = a20_clx_core_init();
    if (r < 0) return r;

    r = a20_clx_link_status_fill(&kargs.node_id, &kargs.state, &kargs.rtt_us,
                                 &kargs.tx_frames, &kargs.rx_frames,
                                 &kargs.tx_drops, &kargs.rx_drops,
                                 &kargs.retransmits,
                                 &kargs.last_hello_age_ms);
    if (r < 0) return r;

    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}
