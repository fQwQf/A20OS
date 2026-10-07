/*
 * Cluster subsystem self-check (docs/cluster/03-kernel-impl.md §5 step 6).
 *
 * Boot-gated with "clxselftest=1" on the kernel command line (bootargs
 * convention, same as CHTRACE_BOOTARG): a20_clx_core_init() spawns the
 * check thread, which drives the WA1 acceptance list against the internal
 * API on one kernel with a virtual node B:
 *
 *   1. A/B echo round trip       send(ep_A) -> CALL -> export -> server
 *                                -> CALL_REPLY -> recv(ep_A)
 *   1a. deadline cleared         a completed CALL zeroes the proxy's
 *                                deadline cache; a scan leaves it alive
 *   1b. connect(self)            inbound CALL dispatches to the export
 *                                table, never to the local proxy demux
 *   1c. link_status              LOCAL (all-zero) aggregates every link;
 *                                routed node UP; unknown UNREACHABLE
 *   1d. export REPLACE           same-name export retired, fresh slot
 *   1d2. REPLACE vs old worker   old server handle released AFTER the
 *         exit                   replace: the parked old worker's exit
 *                                must not clear the new export's peer;
 *                                a remote CALL on the new slot gets a
 *                                normal REPLY, not REMOTE_CLOSED
 *   1e. connect(LOCAL)           fresh pair + handle-carrying connect
 *                                request; decoupled from the remote
 *                                reply path
 *   2. handle rejection          typed-channel refusal on the proxy pair
 *                                (raw TYPE_MISMATCH here; the ABI layer
 *                                maps it to A20_ERR_CLUSTER_UNSUPPORTED)
 *   3. in-flight over-limit      cap refuses with -A20_ERR_NO_SPACE
 *   3a. CLOSE encodes            empty-payload frames are legal on the
 *                                wire (payload == NULL, len == 0)
 *   3b. CLOSE export branch      handle release -> CLOSE(dst_slot) on the
 *                                wire -> export drops pending pairings
 *   3c. CLOSE proxy branch       remote CLOSE tears the proxy down; the
 *                                caller wakes like a local peer close
 *   4. remote CLOSE teardown     server endpoint released -> injection
 *                                fails -> ERROR(REMOTE_CLOSED) -> both-half
 *                                peer_shutdown -> caller wakes CANCELED
 *   5. deadline scan             an expired in-flight txid tears the proxy
 *                                down (the 30 s deadline itself is not
 *                                waited out)
 *
 * Every case prints "[CLX-TEST] <name>: PASS/FAIL"; the summary line is
 * "clx-selftest: <n>/<total>".
 */
#include "cluster/clx_internal.h"
#include "cluster/loopback.h"

#include "core/bootargs.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "proc/proc.h"

static int g_st_pass, g_st_total;

static void st_report(const char *name, int ok)
{
    g_st_total++;
    if (ok)
        g_st_pass++;
    printf("[CLX-TEST] %s: %s\n", name, ok ? "PASS" : "FAIL");
}

/* Virtual identities; hashes are pinned in tools/cluster-ref/MANIFEST.json. */
static const a20_node_id_t st_node_a = {
    { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10 }
};
static const a20_node_id_t st_node_b = {
    { 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
      0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0 }
};
static const uint8_t st_nh_b[4] = { 0x02, 0, 0, 0 };

/* ---- server side: the exported echo endpoint ------------------------------- */

static a20_channel_ep_t *st_server_ep;      /* ep_S, owned by the server */
static volatile int st_server_done;
static volatile int st_server_should_close;

/* Per-connection echo threads (01-abi 落地状态 #10): a LOCAL connect shows
 * up on the exported endpoint as a zero-length message carrying one
 * A20_OBJ_CHANNEL_ENDPOINT handle; the server serves that connection on
 * the received endpoint, decoupled from the remote reply path.  proc_alloc
 * entries take no argument, so the endpoint passes through a spawn slot. */
#define ST_CONN_SLOTS 4
static a20_channel_ep_t *st_conn_spawn[ST_CONN_SLOTS];
static spinlock_t st_conn_spawn_lock = SPINLOCK_INIT;

static void st_conn_thread(void)
{
    a20_channel_ep_t *ep = NULL;
    uint8_t buf[256];

    uint64_t flags = spin_lock_irqsave(&st_conn_spawn_lock);
    for (int i = 0; i < ST_CONN_SLOTS; i++) {
        if (st_conn_spawn[i]) {
            ep = st_conn_spawn[i];
            st_conn_spawn[i] = NULL;
            break;
        }
    }
    spin_unlock_irqrestore(&st_conn_spawn_lock, flags);
    if (!ep)
        return;
    proc_set_name(proc_current(), "clx-st-conn");

    for (;;) {
        uint32_t len = 0, handles = 0;
        int64_t r = a20_channel_recv_begin(ep, 0, &len, &handles);
        if (r < 0)
            break;                          /* caller went away */
        if (len > sizeof(buf) || handles) {
            a20_channel_recv_abort(ep);
            break;
        }
        uint32_t got = len;
        r = a20_channel_recv_finish(ep, buf, &got, NULL, &handles);
        if (r < 0)
            break;
        if (a20_channel_send(ep, buf, got, NULL, 0, NULL, 0) < 0)
            break;
    }
    a20_channel_ep_release(ep);
}

static int st_conn_queue(a20_channel_ep_t *ep)
{
    uint64_t flags = spin_lock_irqsave(&st_conn_spawn_lock);
    for (int i = 0; i < ST_CONN_SLOTS; i++) {
        if (!st_conn_spawn[i]) {
            st_conn_spawn[i] = ep;
            spin_unlock_irqrestore(&st_conn_spawn_lock, flags);
            if (proc_alloc(st_conn_thread) >= 0)
                return 0;
            flags = spin_lock_irqsave(&st_conn_spawn_lock);
            st_conn_spawn[i] = NULL;
            spin_unlock_irqrestore(&st_conn_spawn_lock, flags);
            return -1;
        }
    }
    spin_unlock_irqrestore(&st_conn_spawn_lock, flags);
    return -1;
}

static void st_server_thread(void)
{
    proc_set_name(proc_current(), "clx-st-srv");
    uint8_t *buf = kmalloc(2048);

    for (;;) {
        uint32_t len = 0, handles = 0;
        a20_ch_handle_info_t hinfo[1];
        /* Blocking recv on the server endpoint (03-§1: an ordinary
         * endpoint; requests were injected through its peer). */
        int64_t r = a20_channel_recv_begin(st_server_ep, 0, &len, &handles);
        if (r < 0)
            break;                          /* closed: stop serving */
        if (!buf || len > 2048 || handles > 1) {
            a20_channel_recv_abort(st_server_ep);
            break;
        }
        uint32_t got = len;
        uint32_t hcount = 1;
        r = a20_channel_recv_finish(st_server_ep, buf, &got, hinfo, &hcount);
        if (r < 0)
            break;
        if (hcount == 1) {
            /* LOCAL connect request (01-abi 落地状态 #10): the received
             * endpoint is a fresh connection -- serve it on its own
             * thread.  The handle reference moves to the conn thread. */
            if (hinfo[0].type != A20_OBJ_CHANNEL_ENDPOINT ||
                st_conn_queue((a20_channel_ep_t *)hinfo[0].object) < 0)
                a20_channel_ep_release((a20_channel_ep_t *)hinfo[0].object);
            continue;
        }
        /* Echo service: same bytes back.  With should_close armed the
         * check thread wants this to be the LAST exchange: echo it, then
         * release the endpoint so the export revokes and the remote
         * caller sees the CLOSE path. */
        (void)a20_channel_send(st_server_ep, buf, got, NULL, 0, NULL, 0);
        if (st_server_should_close)
            break;
    }
    /* Server "exits": release ep_S.  The export revokes on the next
     * failed injection; its reply worker sees the peer close and exits. */
    a20_channel_ep_release(st_server_ep);
    st_server_ep = NULL;
    st_server_done = 1;
    if (buf)
        kfree(buf);
}

/* ---- check thread ----------------------------------------------------------- */

static void st_sleep_ms(uint32_t ms)
{
    proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(ms));
}

static void st_check_thread(void)
{
    proc_set_name(proc_current(), "clx-selftest");

    /* The check drives the internal API directly; bring the core up the
     * same way a first cluster syscall would (idempotent). */
    if (a20_clx_core_init() < 0) {
        printf("[CLX-TEST] core init failed\n");
        return;
    }
    st_sleep_ms(200);                       /* let the boot finish printing */

    /* setup: self, route to virtual node B */
    st_report("set_self", a20_clx_self_set(&st_node_a, 0) == 0);
    st_report("set_self twice -> EXISTS",
              a20_clx_self_set(&st_node_a, 0) == -A20_ERR_EXISTS);
    st_report("route_add B", a20_clx_route_add(&st_node_b,
                                               A20_CLX_TRANSPORT_LOOPBACK,
                                               st_nh_b, 4, 0) == 0);
    st_report("route_add B twice -> EXISTS",
              a20_clx_route_add(&st_node_b, A20_CLX_TRANSPORT_LOOPBACK,
                                st_nh_b, 4, 0) == -A20_ERR_EXISTS);
    {
        uint32_t tid = 0xdead, nh_len = 0;
        uint8_t nh[16];
        st_report("route lookup B",
                  a20_clx_route_lookup(&st_node_b, &tid, nh, &nh_len) == 0 &&
                  tid == A20_CLX_TRANSPORT_LOOPBACK);
        {
            a20_node_id_t unknown = {{
                0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
                0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee }};
            st_report("route lookup unknown -> UNREACHABLE",
                      a20_clx_route_lookup(&unknown, &tid, nh,
                                           &nh_len) ==
                          -A20_ERR_NODE_UNREACHABLE);
        }
        st_report("route lookup self -> loopback",
                  a20_clx_route_lookup(&st_node_a, &tid, nh, &nh_len) == 0 &&
                  tid == A20_CLX_TRANSPORT_LOOPBACK);
    }

    /* export: fresh channel; the server owns ep_s, export_register keeps
     * the peer (injection path + reply drain). */
    a20_channel_ep_t *ep_s = a20_channel_create(8, NULL);
    uint32_t slot = 0;
    int64_t er = ep_s ? a20_clx_export_register(ep_s, "echo", 4, 0, &slot)
                      : -1;
    st_report("export register", er > 0);
    st_server_ep = ep_s;
    int server_pid = proc_alloc(st_server_thread);
    st_report("server thread", server_pid >= 0);

    int err = 0;
    a20_channel_ep_t *ep_a = a20_clx_proxy_create(&st_node_b, slot, 0, &err);
    st_report("proxy create", ep_a != NULL);

    if (ep_a) {
        /* 1. echo round trip */
        const char *req = "ping-echo";
        int64_t sr = a20_channel_send(ep_a, req, 10, NULL, 0, NULL, 0);
        st_sleep_ms(50);
        uint32_t rlen;
        uint8_t rbuf[64];
        rlen = (uint32_t)sizeof(rbuf);
        int64_t rr = a20_channel_recv(ep_a, rbuf, &rlen, NULL, NULL, NULL, 0);
        st_report("A/B echo round trip",
                  sr == 0 && rr == 0 && rlen == 10 &&
                  memcmp(rbuf, req, 10) == 0);

        /* 1a. completed CALL clears the proxy deadline: after the round
         * trip above nothing is in flight, so deadline_tick must be 0 and
         * a deadline scan must leave the proxy alone (pre-fix the stale
         * 30 s deadline tore every proxy down after its first call). */
        {
            uint64_t flags = spin_lock_irqsave(&g_clx_lock);
            a20_clx_proxy_t *p = a20_clx_proxy_by_node_slot(
                a20_clx_node_hash(&st_node_b), slot);
            int cleared = p && p->inflight_count == 0 &&
                          p->deadline_tick == 0;
            spin_unlock_irqrestore(&g_clx_lock, flags);
            a20_clx_deadline_scan();
            st_sleep_ms(20);
            uint64_t f2 = spin_lock_irqsave(&g_clx_lock);
            a20_clx_proxy_t *p2 = a20_clx_proxy_by_node_slot(
                a20_clx_node_hash(&st_node_b), slot);
            int alive = p2 && p2->in_use;
            spin_unlock_irqrestore(&g_clx_lock, f2);
            st_report("CALL done -> deadline cleared, scan keeps proxy",
                      cleared && alive);
        }

        /* 1b. connect to one's own node id: the inbound CALL must be
         * dispatched to the export table, never to the local proxy demux
         * (pre-fix the CALL matched the proxy's own in-flight txid and
         * tore the endpoint down on its first call). */
        {
            int err3 = 0;
            a20_channel_ep_t *ep_self = a20_clx_proxy_create(&st_node_a, slot,
                                                             0, &err3);
            const char *sreq = "self-call";
            int self_ok = 0, self_alive = 0;
            if (ep_self) {
                int64_t ss = a20_channel_send(ep_self, sreq, 9, NULL, 0,
                                              NULL, 0);
                st_sleep_ms(50);
                rlen = (uint32_t)sizeof(rbuf);
                int64_t srr = a20_channel_recv(ep_self, rbuf, &rlen, NULL,
                                               NULL, NULL, 0);
                self_ok = ss == 0 && srr == 0 && rlen == 9 &&
                          memcmp(rbuf, sreq, 9) == 0;
                uint64_t flags = spin_lock_irqsave(&g_clx_lock);
                a20_clx_proxy_t *p = a20_clx_proxy_by_node_slot(
                    a20_clx_node_hash(&st_node_a), slot);
                self_alive = p && p->in_use;
                spin_unlock_irqrestore(&g_clx_lock, flags);
            }
            st_report("connect(self) inbound CALL stays on export path",
                      ep_self != NULL && self_ok && self_alive);
            if (ep_self) {
                a20_channel_ep_release(ep_self);
                /* The release puts CLOSE(self->self, slot) on the wire;
                 * flush it before later tests arm pending state. */
                st_sleep_ms(50);
            }
        }

        /* 1c. link_status: LOCAL (all-zero) aggregates every link
         * (01-abi §6), a routed node reports its own link, an unknown
         * node is NODE_UNREACHABLE. */
        {
            a20_node_id_t local = {{ 0 }};
            a20_node_id_t unknown = {{
                0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
                0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77 }};
            uint32_t lst = 0, lrtt = 0;
            uint32_t bst = 0, brtt = 0;
            uint32_t ust = 0, urtt = 0;
            uint64_t ltx = 0, lrx = 0, ltd = 0, lrd = 0, lrt = 0, llh = 0;
            uint64_t btx = 0, brx = 0, btd = 0, brd = 0, brt = 0, blh = 0;
            uint64_t utx = 0, urx = 0, utd = 0, urd = 0, urt = 0, ulh = 0;
            int64_t r_loc = a20_clx_link_status_fill(&local, &lst, &lrtt,
                                                     &ltx, &lrx, &ltd, &lrd,
                                                     &lrt, &llh);
            int64_t r_b = a20_clx_link_status_fill(&st_node_b, &bst, &brtt,
                                                   &btx, &brx, &btd, &brd,
                                                   &brt, &blh);
            int64_t r_unk = a20_clx_link_status_fill(&unknown, &ust, &urtt,
                                                     &utx, &urx, &utd, &urd,
                                                     &urt, &ulh);
            st_report("link_status LOCAL aggregates / B UP / unknown UNREACH",
                      r_loc == 0 && ltx > 0 && lrx > 0 &&
                      lst == A20_CLX_LINK_UP &&
                      r_b == 0 && bst == A20_CLX_LINK_UP && btx > 0 &&
                      r_unk == -A20_ERR_NODE_UNREACHABLE);
        }

        /* 1d. A20_EXPORT_REPLACE: same-name export without REPLACE is
         * EXISTS; with REPLACE the old export is retired and the new one
         * takes the name on a fresh slot (01-abi §2).  Each attempt needs
         * its own channel: a failed register consumes the pair's peer
         * half (the endpoint is left orphaned). */
        {
            uint32_t slot_r1 = 0, slot_r2 = 0, slot_r3 = 0;
            a20_channel_ep_t *ep_r1 = a20_channel_create(8, NULL);
            a20_channel_ep_t *ep_r2 = a20_channel_create(8, NULL);
            a20_channel_ep_t *ep_r3 = a20_channel_create(8, NULL);
            int64_t er1 = ep_r1 ? a20_clx_export_register(ep_r1, "repl-svc",
                                                          8, 0, &slot_r1)
                                : -1;
            int64_t er2 = ep_r2 ? a20_clx_export_register(ep_r2, "repl-svc",
                                                          8, 0, &slot_r2)
                                : -1;
            int64_t er3 = ep_r3 ? a20_clx_export_register(
                                      ep_r3, "repl-svc", 8,
                                      A20_EXPORT_REPLACE, &slot_r3)
                                : -1;
            uint64_t flags = spin_lock_irqsave(&g_clx_lock);
            a20_clx_export_t *old_ex = a20_clx_export_by_slot(slot_r1);
            a20_clx_export_t *new_ex = a20_clx_export_by_name("repl-svc", 8);
            int swapped = !old_ex && new_ex && new_ex->slot == slot_r3;
            spin_unlock_irqrestore(&g_clx_lock, flags);
            st_report("export REPLACE retires same-name export",
                      er1 > 0 && er2 == -A20_ERR_EXISTS && er3 > 0 &&
                      slot_r3 != slot_r1 && swapped);
            if (ep_r1)
                a20_channel_ep_release(ep_r1);
            if (ep_r2)
                a20_channel_ep_release(ep_r2);
            if (ep_r3)
                a20_channel_ep_release(ep_r3);
        }

        /* 1d2. REPLACE vs old worker exit (export worker lifecycle race):
         * the old export's reply worker is PARKED on the old peer's recv
         * when REPLACE retires and reuses its table entry.  Releasing the
         * old server handle only now runs the old worker's exit path
         * against an entry that already serves the NEW registration: a
         * correct exit leaves the new peer untouched, so a remote CALL on
         * the new slot gets a normal REPLY (pre-fix the exit path cleared
         * and released the NEW export's peer, leaving an
         * in_use/peer==NULL zombie whose CALLs answered
         * ERROR(REMOTE_CLOSED) -- and 1d's asserts ran before the old
         * handle was released, so they could not observe it). */
        {
            uint32_t slot_w1 = 0, slot_w2 = 0;
            a20_channel_ep_t *ep_w1 = a20_channel_create(8, NULL);
            int64_t ew1 = ep_w1 ? a20_clx_export_register(ep_w1,
                                                          "repl-race", 9,
                                                          0, &slot_w1)
                                : -1;
            st_sleep_ms(50);        /* old worker parks on the old peer */
            a20_channel_ep_t *ep_w2 = a20_channel_create(8, NULL);
            int64_t ew2 = ep_w2 ? a20_clx_export_register(
                                      ep_w2, "repl-race", 9,
                                      A20_EXPORT_REPLACE, &slot_w2)
                                : -1;
            /* The trigger 1d never waited for: the OLD server handle
             * dies only after the entry already carries the NEW
             * registration, running the old worker's exit path. */
            if (ep_w1)
                a20_channel_ep_release(ep_w1);
            st_sleep_ms(100);
            uint64_t flags = spin_lock_irqsave(&g_clx_lock);
            a20_clx_export_t *ex_w = a20_clx_export_by_slot(slot_w2);
            int peer_ok = ex_w && ex_w->in_use && ex_w->peer;
            spin_unlock_irqrestore(&g_clx_lock, flags);
            /* Serve the replacement export (the conn thread takes over
             * the ep_w2 reference) and drive a remote CALL through it. */
            int served = ep_w2 && st_conn_queue(ep_w2) == 0;
            int call_ok = 0;
            a20_channel_ep_t *ep_c = NULL;
            if (served) {
                int err7 = 0;
                ep_c = a20_clx_proxy_create(&st_node_b, slot_w2, 0, &err7);
                if (ep_c) {
                    const char *wreq = "race-probe";
                    if (a20_channel_send(ep_c, wreq, 10, NULL, 0, NULL,
                                         0) == 0) {
                        st_sleep_ms(100);
                        rlen = (uint32_t)sizeof(rbuf);
                        int64_t wr = a20_channel_recv(ep_c, rbuf, &rlen,
                                                      NULL, NULL, NULL, 0);
                        call_ok = wr == 0 && rlen == 10 &&
                                  memcmp(rbuf, wreq, 10) == 0;
                    }
                }
            }
            st_report("REPLACE survives old server-handle release",
                      ew1 > 0 && ew2 > 0 && slot_w2 != slot_w1 &&
                      peer_ok && served && call_ok);
            /* Cleanup: drop the caller (CLOSE), revoke the export, then
             * mark the server half closed via its peer so the conn
             * thread exits; its last release of ep_w2 closes the export
             * peer the reply worker is parked on, so it exits too. */
            if (ep_c)
                a20_channel_ep_release(ep_c);
            uint64_t f3 = spin_lock_irqsave(&g_clx_lock);
            a20_clx_export_t *ex_w2 = a20_clx_export_by_slot(slot_w2);
            spin_unlock_irqrestore(&g_clx_lock, f3);
            if (ex_w2)
                a20_clx_export_revoke(ex_w2, 0);
            if (served) {
                a20_channel_ep_t *wpeer = a20_channel_ep_peer_ref(ep_w2);
                if (wpeer) {
                    a20_channel_ep_peer_shutdown(wpeer);
                    a20_channel_ep_release(wpeer);
                }
            } else if (ep_w2) {
                a20_channel_ep_release(ep_w2);
            }
            st_sleep_ms(50);        /* let the workers/conn threads exit */
        }

        /* 1e. connect(LOCAL): a fresh pair whose connection half the
         * server accepts as a handle-carrying request -- decoupled from
         * the export's remote reply path, so a local call cannot hang
         * behind the reply worker (01-abi 落地状态 #10). */
        {
            a20_channel_ep_t *ep_l = NULL;
            int lr = a20_clx_export_connect_local(0, "echo", 4, &ep_l);
            const char *lreq = "local-echo";
            const char *rreq = "remote-after-local";
            int local_ok = 0, remote_ok = 0;
            if (lr == 0 && ep_l) {
                st_sleep_ms(50);        /* server accepts the connection */
                int64_t ls = a20_channel_send(ep_l, lreq, 10, NULL, 0,
                                              NULL, 0);
                st_sleep_ms(50);
                rlen = (uint32_t)sizeof(rbuf);
                int64_t lrr = a20_channel_recv(ep_l, rbuf, &rlen, NULL,
                                               NULL, NULL, 0);
                local_ok = ls == 0 && lrr == 0 && rlen == 10 &&
                           memcmp(rbuf, lreq, 10) == 0;
                /* the remote path still works on the same export */
                int64_t rs = a20_channel_send(ep_a, rreq, 18, NULL, 0,
                                              NULL, 0);
                st_sleep_ms(50);
                rlen = (uint32_t)sizeof(rbuf);
                int64_t rrr = a20_channel_recv(ep_a, rbuf, &rlen, NULL,
                                               NULL, NULL, 0);
                remote_ok = rs == 0 && rrr == 0 && rlen == 18 &&
                            memcmp(rbuf, rreq, 18) == 0;
            }
            st_report("LOCAL connect decoupled from remote reply path",
                      lr == 0 && local_ok && remote_ok);
            if (ep_l)
                a20_channel_ep_release(ep_l);
        }

        /* 2. handle rejection at the typed-channel checkpoint (03-§1). */
        a20_ch_handle_info_t hi;
        memset(&hi, 0, sizeof(hi));
        hi.type = 1;
        int64_t hr = a20_channel_send(ep_a, "x", 1, &hi, 1, NULL, 0);
        st_report("handle rejection", hr == -A20_ERR_TYPE_MISMATCH);

        /* 3. in-flight over-limit refuses without blocking (01-abi 限额). */
        {
            uint64_t flags = spin_lock_irqsave(&g_clx_lock);
            a20_clx_proxy_t *p = a20_clx_proxy_by_node_slot(
                a20_clx_node_hash(&st_node_b), slot);
            int full_ok = 0;
            if (p) {
                int filled = 0;
                while (a20_clx_proxy_inflight_add(
                           p, 0x1000u + (uint32_t)filled,
                           timer_get_ticks() + 1000) == 0)
                    filled++;
                full_ok = (filled == (int)p->inflight_cap);
                for (uint32_t j = 0; j < p->inflight_cap; j++)
                    p->inflight[j].deadline_tick = 0;
                p->inflight_count = 0;
                p->deadline_tick = 0;
            }
            spin_unlock_irqrestore(&g_clx_lock, flags);
            st_report("inflight over-limit -> NO_SPACE", full_ok);
        }

        /* 3a. empty-payload frames encode (02-§8 CLOSE was undeliverable
         * while the encoder rejected payload == NULL). */
        {
            uint8_t fbuf[A20_CLX_HDR_LEN + A20_CLX_CRC_LEN];
            a20_frame_hdr_t fh, fd;
            const uint8_t *fpl = NULL;
            memset(&fh, 0, sizeof(fh));
            fh.ver = (uint8_t)A20_CLX_WIRE_VER;
            fh.type = A20_CLX_TYPE_CLOSE;
            fh.src_hash = a20_clx_node_hash(&st_node_a);
            fh.dst_hash = a20_clx_node_hash(&st_node_b);
            fh.dst_slot = slot;
            fh.ttl = A20_CLX_TTL_DEFAULT;
            fh.csum_kind = A20_CLX_CSUM_CCITT;
            int fn = a20_frame_encode(fbuf, sizeof(fbuf), 65536, &fh, NULL);
            st_report("CLOSE frame encodes with NULL payload",
                      fn == (int)(A20_CLX_HDR_LEN + A20_CLX_CRC_LEN) &&
                      a20_frame_decode(fbuf, (uint32_t)fn, 65536, &fd,
                                       &fpl) == A20_FRAME_OK &&
                      fd.type == A20_CLX_TYPE_CLOSE && fd.payload_len == 0);
        }

        /* 3b. CLOSE export branch: a live proxy whose caller half is
         * released puts CLOSE(dst_slot) on the wire (02-§8); on RX the
         * export side drops the slot's unanswered request pairings. */
        {
            uint64_t flags = spin_lock_irqsave(&g_clx_lock);
            a20_clx_export_t *ex = a20_clx_export_by_slot(slot);
            int armed = 0;
            if (ex)
                armed = a20_clx_export_push_pending(
                            ex, &st_node_b, 0x7777u,
                            a20_clx_node_hash(&st_node_b)) == 0;
            spin_unlock_irqrestore(&g_clx_lock, flags);

            int err4 = 0;
            a20_channel_ep_t *ep_x = a20_clx_proxy_create(&st_node_b, slot,
                                                          0, &err4);
            if (ep_x) {
                st_sleep_ms(20);            /* worker parks on recv */
                a20_channel_ep_release(ep_x);   /* 02-§8: -> CLOSE wire */
            }
            st_sleep_ms(100);               /* CLOSE round trip */
            uint64_t f2 = spin_lock_irqsave(&g_clx_lock);
            a20_clx_export_t *ex2 = a20_clx_export_by_slot(slot);
            int cleared = armed && ex2 && ex2->pending_count == 0;
            spin_unlock_irqrestore(&g_clx_lock, f2);
            st_report("CLOSE rx clears export pending (export branch)",
                      ep_x != NULL && cleared);
        }

        /* 3c. CLOSE proxy branch: a CLOSE from the remote node naming one
         * of our proxies' (node, slot) tears the endpoint down and wakes
         * the caller exactly like a local peer close.  Slot 77 has no
         * export, so the export branch is a no-op here. */
        {
            int err5 = 0;
            a20_channel_ep_t *ep_z = a20_clx_proxy_create(&st_node_b, 77, 0,
                                                          &err5);
            if (ep_z) {
                uint8_t fbuf[A20_CLX_HDR_LEN + A20_CLX_CRC_LEN];
                a20_frame_hdr_t fh;
                memset(&fh, 0, sizeof(fh));
                fh.ver = (uint8_t)A20_CLX_WIRE_VER;
                fh.type = A20_CLX_TYPE_CLOSE;
                fh.src_hash = a20_clx_node_hash(&st_node_b);
                fh.dst_hash = a20_clx_node_hash(&st_node_a);
                fh.dst_slot = 77;
                fh.ttl = A20_CLX_TTL_DEFAULT;
                fh.csum_kind = A20_CLX_CSUM_CCITT;
                int fn = a20_frame_encode(fbuf, sizeof(fbuf), 65536, &fh,
                                          NULL);
                if (fn > 0)
                    a20_clx_rx_frame(A20_CLX_TRANSPORT_LOOPBACK, st_nh_b, 4,
                                     fbuf, (uint32_t)fn);
                st_sleep_ms(100);
                uint64_t flags = spin_lock_irqsave(&g_clx_lock);
                a20_clx_proxy_t *pz = a20_clx_proxy_by_node_slot(
                    a20_clx_node_hash(&st_node_b), 77);
                int gone = !pz || !pz->in_use;
                spin_unlock_irqrestore(&g_clx_lock, flags);
                rlen = (uint32_t)sizeof(rbuf);
                int64_t zr = a20_channel_recv(ep_z, rbuf, &rlen, NULL, NULL,
                                              NULL, 0);
                st_report("CLOSE rx tears proxy down (proxy branch)",
                          fn > 0 && gone && zr == -A20_ERR_CANCELED);
                a20_channel_ep_release(ep_z);
            } else {
                st_report("CLOSE rx tears proxy down (proxy branch)", 0);
            }
        }

        /* 4. remote CLOSE path: arm the server's final exchange, then the
         * next request fails injection -> ERROR(REMOTE_CLOSED) -> both-half
         * shutdown -> the parked caller wakes like a local peer close. */
        st_server_should_close = 1;
        (void)a20_channel_send(ep_a, "last", 4, NULL, 0, NULL, 0);
        st_sleep_ms(100);
        rlen = (uint32_t)sizeof(rbuf);
        (void)a20_channel_recv(ep_a, rbuf, &rlen, NULL, NULL, NULL, 0);
        st_sleep_ms(100);
        int64_t cr = a20_channel_send(ep_a, "after-close", 11, NULL, 0,
                                      NULL, 0);
        st_sleep_ms(100);
        rlen = (uint32_t)sizeof(rbuf);
        int64_t cr2 = a20_channel_recv(ep_a, rbuf, &rlen, NULL, NULL, NULL, 0);
        st_report("remote CLOSE -> peer-closed wakeup",
                  cr == 0 && cr2 == -A20_ERR_CANCELED);

        /* 5. deadline scan: synthesize an expired in-flight txid on a
         * fresh proxy, run one scan, expect the proxy torn down. */
        int err2 = 0;
        a20_channel_ep_t *ep_a2 = a20_clx_proxy_create(&st_node_b, 0, 0,
                                                       &err2);
        if (ep_a2) {
            uint64_t flags = spin_lock_irqsave(&g_clx_lock);
            a20_clx_proxy_t *p = a20_clx_proxy_by_node_slot(
                a20_clx_node_hash(&st_node_b), 0);
            int armed = 0;
            if (p)
                armed = a20_clx_proxy_inflight_add(
                            p, 0xBEEFu, timer_get_ticks() - 1) == 0;
            spin_unlock_irqrestore(&g_clx_lock, flags);
            a20_clx_deadline_scan();
            st_sleep_ms(20);
            uint64_t f2 = spin_lock_irqsave(&g_clx_lock);
            a20_clx_proxy_t *p2 = a20_clx_proxy_by_node_slot(
                a20_clx_node_hash(&st_node_b), 0);
            int gone = armed && (!p2 || !p2->in_use);
            spin_unlock_irqrestore(&g_clx_lock, f2);
            st_report("deadline scan teardown", gone);
            a20_channel_ep_release(ep_a2);
        } else {
            st_report("deadline scan teardown", 0);
        }
        a20_channel_ep_release(ep_a);
    }

    for (int i = 0; i < 100 && !st_server_done; i++)
        st_sleep_ms(10);
    printf("[CLX-TEST] clx-selftest: %d/%d checks passed\n", g_st_pass,
           g_st_total);
}

/* ---- boot hook ------------------------------------------------------------------ */

void a20_clx_selftest_boot(void)
{
    const char *args = bootargs_get();

    if (!args || !strstr(args, "clxselftest=1"))
        return;
    if (proc_alloc(st_check_thread) < 0)
        printf("[CLX-TEST] failed to spawn check thread\n");
}
