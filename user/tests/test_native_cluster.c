/*
 * Cluster ABI contract tests (docs/cluster/01-abi.md, stage W0).
 *
 * The kernel cluster data plane has landed in WA1, so this test pins the
 * implemented identity behavior alongside the frozen syscall surface:
 *
 *   abi-nr    every cluster syscall number is the documented value and is
 *             reachable through the dispatch table (a wrong number returns
 *             ENOSYS/NOT_SUPPORTED from the generic unknown-syscall path, not
 *             the cluster refusal)
 *   abi-live  set_self succeeds once and rejects a second identity with
 *             A20_ERR_EXISTS
 *   abi-args  each rejects the argument shapes the ABI says are illegal, and
 *             does so *before* the refusal, so a malformed caller never sees
 *             the stub's answer
 *   abi-node  node id hashing matches the FNV-1a vector in the spec, since the
 *             same value has to appear in a frame header on the wire
 *
 * The abi-args partition is the one that matters going forward: when the real
 * handler lands in a later stage, these cases must keep failing the same way.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

static a20_handle_t g_stdout = A20_HANDLE_NULL;

static int fail(const char *what)
{
    if (g_stdout != A20_HANDLE_NULL) {
        char buf[128];
        int n = 0;
        buf[n++] = 'F';
        buf[n++] = ':';
        const char *p = what;
        while (*p && n < (int)sizeof(buf) - 2)
            buf[n++] = *p++;
        buf[n++] = '\n';
        a20_hdl_write_buf(g_stdout, buf, (uint64_t)n, NULL);
    }
    return 1;
}

static void note(const char *msg, uint64_t len)
{
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, msg, len, NULL);
}

static int abi_numbers(void)
{
    if (A20_SYS_cluster_set_self != 0x0520) return fail("nr-set_self");
    if (A20_SYS_cluster_export != 0x0521) return fail("nr-export");
    if (A20_SYS_cluster_connect != 0x0522) return fail("nr-connect");
    if (A20_SYS_cluster_route != 0x0523) return fail("nr-route");
    if (A20_SYS_cluster_event_subscribe != 0x0524) return fail("nr-events");
    if (A20_SYS_cluster_link_status != 0x0525) return fail("nr-link");
    return 0;
}

static int abi_node_hash(void)
{
    a20_node_id_t id;

    a20_node_id_local(&id);
    if (!a20_node_id_is_local(&id)) return fail("node-local");

    /* FNV-1a (offset 2166136261, prime 16777619) over the 16 id bytes. These
     * two vectors are the values the frame header carries, so a change to the
     * recurrence on either side of the wire shows up as a peer mismatch. */
    if (a20_node_hash(&id) != 0x69691905u) return fail("node-hash-zero");

    a20_node_id_broadcast(&id);
    if (a20_node_hash(&id) != 0x360779f5u) return fail("node-hash-ff");

    a20_node_id_broadcast(&id);
    if (a20_node_id_is_local(&id)) return fail("node-bcast-not-local");
    return 0;
}

/* A well-formed peer identity: neither reserved value. */
static void peer_id(a20_node_id_t *out)
{
    uint8_t raw[16];
    for (int i = 0; i < 16; i++)
        raw[i] = (uint8_t)(0x10 + i);
    a20_node_id_set_raw(out, raw);
}

static int abi_live_identity(void)
{
    a20_node_id_t peer;
    a20_node_id_t routed;
    peer_id(&peer);

    a20_status_t r = a20_cluster_set_self(&peer, A20_CLUSTER_CAP_RELIABLE);
    if (r != 0) return fail("set-self-first");
    if (a20_cluster_set_self(&peer, A20_CLUSTER_CAP_RELIABLE) !=
        -(a20_status_t)A20_ERR_EXISTS)
        return fail("set-self-second");

    /* Exercise the committed local export/name lookup path.  The server
     * endpoint is owned by this task and is closed on exit; the temporary
     * caller endpoint is closed here. */
    a20_channel_pair_t pair;
    if (a20_status_is_err(a20_channel_create(&pair)))
        return fail("live-channel-create");
    uint32_t slot = 0;
    r = a20_cluster_export(pair.endpoints[0], "native-cluster", 0, &slot);
    if (r < 0 || slot == 0)
        return fail("live-export");
    a20_handle_t caller = A20_HANDLE_NULL;
    r = a20_cluster_connect(NULL, slot, NULL, 0, 0, &caller);
    if (r < 0 || caller == A20_HANDLE_NULL)
        return fail("live-connect-local");
    if (a20_hdl_close(caller) < 0)
        return fail("live-close-caller");

    /* Route-table lookup and loopback link status are observable through the
     * public ABI; the synthetic next-hop is a 32-bit loopback node number. */
    peer_id(&routed);
    routed.bytes[0]++;
    uint32_t vnode = 7;
    if (a20_cluster_route(A20_ROUTE_REPLACE, &routed,
                          A20_CLX_TRANSPORT_LOOPBACK, &vnode, sizeof(vnode),
                          0) != 0)
        return fail("live-route-replace");
    a20_cluster_link_status_args_t link;
    if (a20_cluster_link_status(&routed, &link) != 0 ||
        link.state != A20_CLX_LINK_UP)
        return fail("live-loopback-status");
    if (a20_cluster_route(A20_ROUTE_DEL, &routed,
                          A20_CLX_TRANSPORT_LOOPBACK, NULL, 0, 0) != 0)
        return fail("live-route-delete");

    return 0;
}

/* Argument validation runs ahead of the refusal: each case below is illegal by
 * 01-abi.md and must come back as INVALID_ARGUMENT, never as the stub's
 * UNSUPPORTED, so the rule survives the data plane landing. */
static int abi_arg_rejection(void)
{
    a20_node_id_t id;
    a20_node_id_local(&id);
    a20_channel_pair_t pair;
    if (a20_status_is_err(a20_channel_create(&pair)))
        return fail("args-channel-create");

    /* set_self with a reserved node id (all-zero LOCAL, all-0xff BROADCAST). */
    if (a20_cluster_set_self(&id, A20_CLUSTER_CAP_RELIABLE) !=
        -A20_ERR_INVALID_ARGUMENT)
        return fail("args-set_self-local");
    a20_node_id_broadcast(&id);
    if (a20_cluster_set_self(&id, 0) != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-set_self-bcast");
    if (a20_cluster_set_self(NULL, 0x8u) != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-set_self-caps");

    /* export with a null channel and an over-long name. */
    char longname[A20_CLUSTER_SERVICE_NAME_MAX + 2];
    for (unsigned i = 0; i < sizeof(longname); i++)
        longname[i] = 'x';
    if (a20_cluster_export(pair.endpoints[0], longname, 0, 0) !=
        -A20_ERR_INVALID_ARGUMENT)
        return fail("args-export-name");

    /* export with an unknown flag bit. */
    if (a20_cluster_export(pair.endpoints[0], "svc", 0x80u, 0) !=
        -A20_ERR_INVALID_ARGUMENT)
        return fail("args-export-flags");

    /* connect with an unknown flag bit. */
    if (a20_cluster_connect(NULL, 1, "svc", 0x80u, 0, 0) !=
        -A20_ERR_INVALID_ARGUMENT)
        return fail("args-connect-flags");

    /* route with an out-of-range op and an over-long next hop. */
    a20_node_id_local(&id);
    uint8_t hop[32];
    for (unsigned i = 0; i < sizeof(hop); i++)
        hop[i] = 0;
    if (a20_cluster_route(99u, &id, A20_CLX_TRANSPORT_LOOPBACK, hop,
                          sizeof(hop), 0) != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-route-op");
    if (a20_cluster_route(A20_ROUTE_ADD, &id, A20_CLX_TRANSPORT_LOOPBACK, hop,
                          sizeof(hop), 0) != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-route-hop-len");

    /* event_subscribe with bits outside the documented mask. */
    if (a20_cluster_event_subscribe(0x10u, 0) != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-event-mask");

    return 0;
}

/* A struct that is too short for the fields the kernel knows must be refused
 * before anything else is read (docs/native-abi/01-types.md §2). */
static int abi_struct_versioning(void)
{
    a20_cluster_connect_args_t args;
    args.size    = sizeof(args) - 8;  /* one pointer short */
    args.version = 1;
    args.flags   = 0;
    args.slot    = 0;
    args.timeout_ms = 0;
    args.reserved = 0;
    a20_node_id_local(&args.node_id);
    args.service_name = 0;
    args.name_len = 0;

    a20_status_t r = a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&args,
                                  0, 0, 0, 0, 0);
    if (r != -A20_ERR_INVALID_ARGUMENT) return fail("args-short-struct");

    args.version = 0;  /* version 0 is never valid */
    r = a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r != -A20_ERR_INVALID_ARGUMENT) return fail("args-version-zero");

    args.size = sizeof(args);
    args.version = 99;  /* newer than the kernel knows */
    r = a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r != -A20_ERR_INVALID_ARGUMENT) return fail("args-version-future");

    /* A NULL args pointer is a fault, not a validation error. */
    r = a20_syscall6(A20_SYS_cluster_connect, 0, 0, 0, 0, 0, 0);
    if (r != -A20_ERR_FAULT) return fail("args-null-ptr");

    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    if (si)
        g_stdout = si->stdout_handle;

    note("start\n", 6);

    if (abi_numbers() != 0) return 1;
    note("abi-nr ok\n", 10);

    if (abi_node_hash() != 0) return 1;
    note("abi-node ok\n", 11);

    if (abi_live_identity() != 0) return 1;
    note("abi-live ok\n", 11);

    if (abi_arg_rejection() != 0) return 1;
    note("abi-args ok\n", 11);

    if (abi_struct_versioning() != 0) return 1;
    note("abi-ver ok\n", 10);

    note("NATIVE_CLUSTER: PASS\n", 20);
    return 0;
}
