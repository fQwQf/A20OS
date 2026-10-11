/*
 * Cluster core internals -- shared declarations of the WA kernel data plane
 * (docs/cluster/03-kernel-impl.md §2 file table).  kernel/cluster/-private:
 * nothing outside the cluster directory may include this.
 *
 * Lock hierarchy (03-§3), outermost -> innermost:
 *   g_clx_lock (self/route/export/proxy registry, one core lock for v0)
 *     -> channel locks via public a20_channel_* calls only
 *   Transport callbacks run in thread context (loopback has no IRQ path in
 *   v0); the RX ring has its own lock and is taken below g_clx_lock.
 *
 * v0 decisions registered in the docs and mirrored here:
 *   - one export namespace per kernel (04-§2 实现期记录): loopback virtual
 *     nodes share it, real per-node tables arrive with WA2;
 *   - remote messages always travel as CALL/CALL_REPLY (02-§2 实现期记录);
 *   - routes: one best route per destination (01-abi §4 实现期记录).
 */
#ifndef _CLX_INTERNAL_H
#define _CLX_INTERNAL_H

#include "core/types.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/string.h"
#include "core/timer.h"
#include "ipc/ipc.h"
#include "abi/native/types.h"
#include "abi/native/resource.h"
#include "cluster/frame.h"
#include "cluster/transport.h"
#include "cluster/transaction.h"

/* Profile-selected caps (01-abi.md 限额; resource.h tier macros). */
#if CONFIG_CLUSTER_PROFILE >= A20_CLX_PROFILE_SERVER
#define CLX_LIMIT_ROUTES A20_LIMIT_CLX_ROUTES_ABSOLUTE
#define CLX_LIMIT_SLOTS A20_LIMIT_CLX_SLOTS_ABSOLUTE
#define CLX_LIMIT_REMOTE_EPS A20_LIMIT_CLX_REMOTE_EPS_ABSOLUTE
#define CLX_LIMIT_INFLIGHT A20_LIMIT_CLX_INFLIGHT_ABSOLUTE
#elif CONFIG_CLUSTER_PROFILE == A20_CLX_PROFILE_MCU
#define CLX_LIMIT_ROUTES A20_LIMIT_CLX_ROUTES_MCU
#define CLX_LIMIT_SLOTS A20_LIMIT_CLX_SLOTS_MCU
#define CLX_LIMIT_REMOTE_EPS A20_LIMIT_CLX_REMOTE_EPS_MCU
#define CLX_LIMIT_INFLIGHT A20_LIMIT_CLX_INFLIGHT_MCU
#else
#define CLX_LIMIT_ROUTES A20_LIMIT_CLX_ROUTES_DEFAULT
#define CLX_LIMIT_SLOTS A20_LIMIT_CLX_SLOTS_DEFAULT
#define CLX_LIMIT_REMOTE_EPS A20_LIMIT_CLX_REMOTE_EPS_DEFAULT
#define CLX_LIMIT_INFLIGHT A20_LIMIT_CLX_INFLIGHT_DEFAULT
#endif

/* Logical frame limit includes header and CRC, independent of wire MTU. */
#if CONFIG_CLUSTER_PROFILE >= A20_CLX_PROFILE_SERVER
#define CLX_LOGICAL_MTU                                                        \
    65590u /* 65536 business + 20 session/status + 34 wire */
#elif CONFIG_CLUSTER_PROFILE == A20_CLX_PROFILE_MCU
#define CLX_LOGICAL_MTU 256u
#else
#define CLX_LOGICAL_MTU 4096u
#endif
uint32_t a20_clx_logical_mtu(const a20_node_id_t *peer);
void a20_clx_fragment_expire(void);
uint8_t *a20_clx_fragment_rx(const a20_node_id_t *src, uint32_t transport,
                             a20_frame_hdr_t *header, const uint8_t *payload);

/* Per-node dedup window for inbound CALLs (02-§4: >= 64 entries). */
#define CLX_DEDUP_WINDOW 64u

/* Pending inbound-CALL FIFO depth per export (v0: single-threaded server
 * assumption, 02-§4 实现期记录; the server's local channel queue provides
 * the real backpressure). */
#define CLX_EXPORT_PENDING 8u

/* Remote CALL deadline: 01-abi §3 fixes the link max_deadline at 30 s and
 * channel_call has no deadline field, so min(caller, link) degenerates to
 * the link constant (registered in 01-abi 落地状态). */
#define CLX_CALL_DEADLINE_MS 30000u

#include "uapi/a20/cluster_snapshot.h"
int a20_clx_route_snapshot(a20_cluster_snapshot_args_t *out);
int a20_clx_export_snapshot(a20_cluster_snapshot_args_t *out);
uint64_t a20_clx_boot_session(void);
void a20_clx_resources(a20_cluster_resource_args_t *out);
void a20_clx_fragment_resources(a20_cluster_resource_args_t *out);
/* These collectors require g_clx_lock, and never allocate or sleep. */
void a20_clx_route_resources(uint64_t *values);
void a20_clx_export_resources(uint64_t *values);
void a20_clx_proxy_resources(uint64_t *values);
void a20_clx_replay_resources(uint64_t *values);
void a20_clx_directory_rx(const a20_node_id_t *src, const a20_frame_hdr_t *h,
                          const uint8_t *input);

/* Cluster event kinds delivered through cluster_event_subscribe (01-abi §5).
 * Delivered as a20_event_notify on the cluster event source with
 * event_type = A20_CLX_EV_BASE + kind, data0 = src/dst node hash,
 * data1 = transport_id | (reason << 8); 01-abi §5 实现期记录. */
#define A20_CLX_EV_BASE 32u
#define A20_CLX_EV_LINK_UP 0u
#define A20_CLX_EV_LINK_DOWN 1u
#define A20_CLX_EV_ROUTE_LOST 2u
#define A20_CLX_EV_EXPORT_DROP 3u

/* ---- core lock + lazy init ------------------------------------------- */

extern spinlock_t g_clx_lock;

/* Idempotent.  Registers the loopback (and, DEFAULT tier, UART) transport
 * and spawns the RX + housekeeping threads.  Syscall context only. */
int a20_clx_core_init(void);
int a20_clx_uart_prepare_route(const uint8_t *nh, uint32_t len);
/* Spawns the boot-gated self-check when the command line carries
 * clxselftest=1 (03-§5 step 6).  Called at the end of core_init. */
void a20_clx_selftest_boot(void);

/* ---- self identity (01-abi §节点标识) -------------------------------- */

int a20_clx_self_is_set(void);
/* Returns 0 and fills out when set_self has run; -A20_ERR_NODE_UNREACHABLE
 * otherwise (01-abi: before set_self every remote operation fails with it). */
int a20_clx_self_get(a20_node_id_t *out_id, uint32_t *out_caps);
/* Only succeeds once; second call -A20_ERR_EXISTS (01-abi 落地状态). */
int a20_clx_self_set(const a20_node_id_t *id, uint32_t caps);
uint32_t a20_clx_self_hash(void);
uint32_t a20_clx_node_hash(const a20_node_id_t *id);

/* ---- routes (03-§2 route.c row) --------------------------------------- */

int a20_clx_route_add(const a20_node_id_t *node, uint32_t transport_id,
                      const uint8_t *next_hop, uint32_t nh_len,
                      uint32_t metric);
int a20_clx_route_del(const a20_node_id_t *node);
int a20_clx_route_del_match(const a20_node_id_t *node, uint32_t transport,
                            const uint8_t *hop, uint32_t len, uint32_t metric);
int a20_clx_route_learn(const a20_node_id_t *node, uint32_t transport,
                        const uint8_t *hop, uint32_t len, uint32_t metric);
int a20_clx_route_replace(const a20_node_id_t *node, uint32_t transport_id,
                          const uint8_t *next_hop, uint32_t nh_len,
                          uint32_t metric);
/* Exact-match lookup, read side never allocates (03-§2).  Returns 0 and
 * fills the out parameters, -A20_ERR_NODE_UNREACHABLE when unknown.  Also
 * resolves the local node itself to the loopback transport (see 04-§2). */
int a20_clx_route_lookup(const a20_node_id_t *node, uint32_t *out_transport,
                         uint8_t *out_nh, uint32_t *out_nh_len);
/* Reverse hash -> node id over self + routed nodes; 0 on hit. */
int a20_clx_node_by_hash(uint32_t hash, a20_node_id_t *out);
void a20_clx_route_link_down(uint32_t transport_id, const uint8_t *nh,
                             uint32_t nh_len);

/* ---- exports (03-§2 export.c row) ------------------------------------- */

typedef struct clx_pending_req {
    a20_node_id_t src;
    uint32_t txid;
    uint64_t src_session, dst_session;
    uint32_t serve_hash; /* the node hosting this export: the CALL's
                                  * dst_hash; replies claim it as src_hash */
} a20_clx_pending_req_t;

typedef struct a20_clx_export {
    int in_use;
    uint32_t slot;  /* monotonic, never reused */
    uint32_t flags; /* A20_EXPORT_* */
    uint32_t name_len;
    char name[A20_CLUSTER_SERVICE_NAME_MAX + 1];
    a20_channel_ep_t *peer;    /* server endpoint's peer; the core
                                          * drains replies from it and injects
                                          * requests through it (03-§1).  Holds
                                          * the only reference once the exporter
                                          * drops its handle. */
    a20_node_id_t last_caller; /* CLOSE best-effort target (04-§2) */
    int last_caller_valid;
    /* FIFO of unanswered inbound CALLs; the reply worker pairs each drained
     * reply with the oldest entry (02-§4 实现期记录: FIFO server assumption). */
    a20_clx_pending_req_t pending[CLX_EXPORT_PENDING];
    uint32_t pending_head, pending_count;
} a20_clx_export_t;

/* Registers name/slot for ep; takes over one peer reference (may be NULL
 * when the endpoint is orphaned -- export still registers, delivery will
 * detect the dead endpoint and revoke).  Returns slot or -errno. */
int64_t a20_clx_export_register(a20_channel_ep_t *ep, const char *name,
                                uint32_t name_len, uint32_t flags,
                                uint32_t *out_slot);
/* Only for a freshly created kernel pair with an uninstalled peer. */
int64_t a20_clx_export_register_owned(a20_channel_ep_t *ep, const char *name,
                                      uint32_t name_len, uint32_t flags,
                                      uint32_t *out_slot);
/* Finds an export by slot or name; g_clx_lock must be held. */
a20_clx_export_t *a20_clx_export_by_slot(uint32_t slot);
a20_clx_export_t *a20_clx_export_by_name(const char *name, uint32_t name_len);
/* Server side died (peer injection failed): revoke, notify EXPORT_DROPPED. */
void a20_clx_export_revoke(a20_clx_export_t *ex, int notify);
void a20_clx_export_revoke_slot(a20_clx_export_t *ex, uint32_t slot,
                                int notify);
/* Remember who called last so revoke can aim CLOSE (best effort).
 * g_clx_lock must be held. */
void a20_clx_export_note_caller(a20_clx_export_t *ex, const a20_node_id_t *src);
/* Inbound CALL dedup window (02-§4).  Returns 1 when (hash, txid) was
 * already seen within the window (duplicate -> drop + dedup_drops).
 * g_clx_lock must be held. */
int a20_clx_export_dedup_seen(uint32_t src_hash, uint32_t txid);
uint64_t a20_clx_export_dedup_drops(void);
/* remote_ep.c: spawns the reply-drain worker for a fresh export.  Binds
 * the worker to THIS registration of the entry (a second reference on
 * ex->peer taken under g_clx_lock) so a REPLACE that retires and reuses
 * the entry before the worker runs cannot mis-bind it to the replacement
 * registration. */
int a20_clx_export_spawn_worker(a20_clx_export_t *ex);
/* connect(LOCAL) resolution (01-abi §3): build a fresh channel pair, inject
 * the connection half into the exported server's queue as a handle-carrying
 * connect request, return the caller half referenced for the new handle.
 * Fully decoupled from the export's remote reply path (落地状态 #10).
 * Returns 0, -A20_ERR_NOT_FOUND when slot/name is unknown, or the channel
 * send error (e.g. -A20_ERR_TYPE_MISMATCH on typed exports that refuse
 * endpoint handles, -A20_ERR_WOULD_BLOCK on a full server queue). */
int a20_clx_export_connect_local(uint32_t slot, const char *name,
                                 uint32_t name_len, a20_channel_ep_t **out);

/* ---- proxies / remote endpoints (03-§2 remote_ep.c row) ---------------- */

/* ---- proxies / remote endpoints (03-§2 remote_ep.c row) ---------------- */

/* One per cluster_connect.  ep is the caller-visible endpoint of the pair;
 * the core owns one reference on the proxy half and the TX worker releases
 * it on exit.  inflight is heap-sized to the profile cap at creation. */
typedef struct a20_clx_proxy {
    int in_use;
    uint32_t id;
    a20_channel_ep_t *ep;    /* proxy half of the connect pair */
    a20_node_id_t peer_node; /* remote node identity */
    uint32_t dst_slot;       /* remote slot; 0 = by name / slot 0 */
    uint32_t reliable;       /* A20_CONNECT_RELIABLE requested */
    uint64_t local_session, remote_session;
    struct a20_clx_inflight {
        uint32_t txid;
        uint64_t deadline_tick;
        uint64_t retry_tick;
        uint8_t *frame;
        uint32_t frame_len, retries;
        int accepted;
    } *inflight;
    uint32_t inflight_cap;
    uint32_t inflight_count;
    uint64_t deadline_tick; /* earliest deadline, 0 = none */
} a20_clx_proxy_t;

/* RX dispatch entry points, called by the RX thread. */
void a20_clx_proxy_rx(uint32_t transport_id, const a20_node_id_t *src,
                      const a20_frame_hdr_t *h, const uint8_t *payload);
void a20_clx_export_rx(uint32_t transport_id, const a20_node_id_t *src,
                       const a20_frame_hdr_t *h, const uint8_t *payload);
/* ERROR frame back to a remote caller (02-§8 errno mapping; remote_ep.c).
 * src_hash on the wire = serve_hash (the node hosting the service, i.e.
 * the inbound CALL's dst_hash; a real kernel's own hash). */
void a20_clx_send_error(const a20_node_id_t *dst, const a20_frame_hdr_t *call,
                        int errno_val);
/* link DOWN or remote CLOSE: every proxy on that node is torn down via
 * a20_channel_ep_peer_shutdown on both pair halves (03-§1, 02-§8). */
void a20_clx_node_down(const a20_node_id_t *node);
/* Deadline scan (housekeeping thread).  Fires peer_shutdown for proxies
 * whose earliest CALL deadline passed; frozen-channel surfacing registered
 * in 01-abi/02-§4 实现期记录. */
void a20_clx_deadline_scan(void);

/* cluster_connect's core half: builds the proxy pair, spawns its TX worker.
 * Returns the caller-side endpoint (one reference, owned by the new handle)
 * or NULL with *err = -errno. */
a20_channel_ep_t *a20_clx_proxy_create(const a20_node_id_t *node, uint32_t slot,
                                       uint32_t reliable, int *err);
a20_channel_ep_t *a20_clx_proxy_create_bound(const a20_node_id_t *node,
                                             uint32_t slot, uint32_t reliable,
                                             uint64_t expected_epoch, int *err);
uint32_t a20_clx_proxy_count(void);
/* In-flight bookkeeping (g_clx_lock held).  Returns 0 or -A20_ERR_NO_SPACE
 * when the pending-CALL cap is hit (01-abi 限额; never blocks). */
int a20_clx_proxy_inflight_add(a20_clx_proxy_t *p, uint32_t txid,
                               uint64_t deadline_tick);
/* (src_hash, txid) -> proxy lookup for CALL_REPLY/ERROR demux. */
a20_clx_proxy_t *a20_clx_proxy_by_txid(uint32_t src_hash, uint32_t txid);
/* (src_hash, dst_slot) -> proxy lookup for CLOSE demux. */
a20_clx_proxy_t *a20_clx_proxy_by_node_slot(uint32_t src_hash, uint32_t slot);
/* Export reply FIFO (g_clx_lock held). */
int a20_clx_export_push_pending(a20_clx_export_t *ex, const a20_node_id_t *src,
                                uint32_t txid, uint32_t serve_hash);
int a20_clx_export_pop_pending(a20_clx_export_t *ex, a20_node_id_t *src,
                               uint32_t *txid, uint32_t *serve_hash,
                               uint64_t *src_session, uint64_t *dst_session);

/* TX path (transport.c): resolve node -> route -> transport -> send with
 * per-link tx counters.  Returns 0 or -errno. */
int a20_clx_core_send_frame(const a20_node_id_t *dst, const uint8_t *frame,
                            uint32_t len);

int a20_clx_peer_session(const a20_node_id_t *node, uint64_t *local,
                         uint64_t *remote, uint32_t *caps);

/* Routed sessions retain direct HELLO binding and confirm the final endpoint. */
int a20_clx_routed_session(const a20_node_id_t *node, uint64_t *local,
                           uint64_t *remote, uint32_t *caps);
void a20_clx_session_rx(const a20_node_id_t *src, const a20_frame_hdr_t *h,
                        const uint8_t *payload);
void a20_clx_session_tick(void);
void a20_clx_session_invalidate(const a20_node_id_t *node);
uint32_t a20_clx_session_mtu(const a20_node_id_t *node);

/* Reliable layer owns the bounded request/reply cache. RX returns one
 * when consumed; otherwise it strips the CALL prefix in the supplied view. */
int a20_clx_reliable_rx(const a20_node_id_t *src, a20_frame_hdr_t *h,
                        const uint8_t **payload);
void a20_clx_reliable_reply(const a20_node_id_t *dst, uint32_t txid,
                            uint32_t serve_hash, uint64_t caller_session,
                            uint64_t server_session, const uint8_t *data,
                            uint32_t len, uint32_t error);
void a20_clx_reliable_receipt(const a20_node_id_t *dst, uint32_t txid,
                              uint64_t local, uint64_t remote);
void a20_clx_link_retransmit(const a20_node_id_t *node);

/* ---- events (01-abi §5) ------------------------------------------------ */

/* Push a cluster event to every subscriber.  Thread context only. */
void a20_clx_event_push(uint32_t kind, uint32_t node_hash,
                        uint32_t transport_id, uint32_t reason);
/* event_subscribe core half: creates the EventQ, watches the cluster event
 * source with the requested mask, returns the queue (handle installed by
 * the ABI layer). */
a20_eventq_t *a20_clx_event_subscribe(uint32_t mask);
uint64_t a20_clx_event_overflows(void);

/* ---- counters for cluster_link_status (02-§10) ------------------------- */

int a20_clx_link_status_fill(const a20_node_id_t *node, uint32_t *state,
                             uint32_t *rtt_us, uint64_t *tx_frames,
                             uint64_t *rx_frames, uint64_t *tx_drops,
                             uint64_t *rx_drops, uint64_t *retransmits,
                             uint64_t *last_hello_age_ms);

#endif /* _CLX_INTERNAL_H */
