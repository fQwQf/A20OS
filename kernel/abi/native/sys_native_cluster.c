/*
 * A20OS Native ABI — Cluster syscalls (0x0520).
 *
 * Every entry point here is currently a stub: it validates its arguments with
 * the same rules the rest of the Native ABI uses and then refuses with
 * A20_ERR_CLUSTER_UNSUPPORTED.  That is deliberate.  The ABI is frozen first
 * (docs/cluster/impl-prompts.md W0) so that the kernel data plane, the MCU
 * leaf firmware and the userspace clusterd can be written against a fixed
 * surface; the routing tables, remote endpoints and transports land in later
 * stages and replace these bodies.
 *
 * Argument validation runs before the refusal on purpose: a stub that skipped
 * it would let a malformed call look like a valid one until the day the real
 * handler arrived, and the size/version contract
 * (docs/native-abi/01-types.md §2, docs/cluster/01-abi.md §通用约定) is part of
 * the frozen surface, not an implementation detail.
 *
 * Design reference: docs/cluster/01-abi.md
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
#include "sys_validate.h"

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

    return -A20_ERR_CLUSTER_UNSUPPORTED;
}

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
    if (!kargs.service_name && kargs.name_len)
        return -A20_ERR_INVALID_ARGUMENT;

    return -A20_ERR_CLUSTER_UNSUPPORTED;
}

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
    if (!kargs.service_name && kargs.name_len)
        return -A20_ERR_INVALID_ARGUMENT;

    return -A20_ERR_CLUSTER_UNSUPPORTED;
}

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

    return -A20_ERR_CLUSTER_UNSUPPORTED;
}

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

    return -A20_ERR_CLUSTER_UNSUPPORTED;
}

int64_t sys_a20_cluster_link_status(const a20_syscall_args_t *args)
{
    a20_cluster_link_status_args_t *uargs =
        (a20_cluster_link_status_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_cluster_link_status_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.reserved) return -A20_ERR_INVALID_ARGUMENT;
    /* node_id is an input here; the link counters below it are outputs, so
     * there is nothing else to validate until the routing table exists. */

    return -A20_ERR_CLUSTER_UNSUPPORTED;
}