/* IPv4 CL adapter. Socket ownership is independent of the configuring task;
 * all I/O runs outside registry locks. One bounded poller owns RX/control.
 * See docs/cluster/07-unified-runtime.md for trust and routing contracts. */
#include "cluster/clx_internal.h"
#include "cluster/udp.h"
#include "core/bootargs.h"
#include "core/errno.h"
#include "core/random.h"
#include "fs/fdtable.h"
#include "net/socket_internal.h"

#ifndef CONFIG_MCU

#define UDP_RX_BUDGET 16u
#define UDP_PING_MS 1000u

typedef struct clx_udp_peer {
    int used;
    uint8_t nh[6];
    a20_node_id_t node;
    uint32_t state;
    uint32_t caps;
    uint8_t version, tier;
    uint64_t remote_nonce;
    uint32_t challenge;
    uint64_t last_rx;
    uint64_t last_tx;
} clx_udp_peer_t;

static spinlock_t g_udp_lock = SPINLOCK_INIT;
static clx_udp_peer_t g_udp_peers[CLX_LIMIT_ROUTES];
static vfile_t *g_udp_file;
static net_socket_t *g_udp_socket;
static int g_udp_starting;
static uint64_t g_udp_nonce;
static uint8_t g_udp_rx[A20_CLX_UDP_MTU + 1];

static void udp_sockaddr(net_sockaddr_in_t *addr, const uint8_t nh[6]) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    memcpy(&addr->sin_addr, nh, 4);
    memcpy(&addr->sin_port, nh + 4, 2);
}

static uint16_t udp_port(void) {
    const char *args = bootargs_get();
    if (!args)
        return A20_CLX_UDP_PORT;
    while (*args) {
        while (*args == ' ')
            args++;
        if (strncmp(args, "clxport=", 8) == 0) {
            uint32_t port = 0;
            args += 8;
            const char *start = args;
            while (*args >= '0' && *args <= '9') {
                port = port * 10 + (uint32_t)(*args++ - '0');
                if (port > 65535)
                    return 0;
            }
            return args != start && (!*args || *args == ' ') ? port : 0;
        }
        while (*args && *args != ' ')
            args++;
    }
    return A20_CLX_UDP_PORT;
}

/* Called in configuration context. Detach the temporary task fd, retaining
 * one vfile reference for the lifetime of the lazy cluster adapter. */
static int udp_start(void) {
    uint64_t irq = spin_lock_irqsave(&g_udp_lock);
    if (g_udp_socket) {
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return 0;
    }
    if (g_udp_starting) {
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return -A20_ERR_BUSY;
    }
    g_udp_starting = 1;
    spin_unlock_irqrestore(&g_udp_lock, irq);

    int result = -A20_ERR_IO;
    uint16_t port = udp_port();
    vfile_t *vf = NULL;
    net_socket_t *s = NULL;
    if (!port) {
        result = -A20_ERR_INVALID_ARGUMENT;
        goto done;
    }
    int fd = net_socket_create(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        goto done;
    vf = fdtable_get_current_file_ref(fd);
    vfs_close(fd);
    if (!vf)
        goto done;
    s = net_socket_from_vfile(vf);
    uint8_t nh[6] = {0, 0, 0, 0, (uint8_t)(port >> 8), (uint8_t)port};
    net_sockaddr_in_t local;
    udp_sockaddr(&local, nh);
    int r = net_bind_sock(s, &local, sizeof(local));
    if (r < 0) {
        result = r == -EADDRINUSE ? -A20_ERR_BUSY : -A20_ERR_IO;
        vfs_put_file(vf);
        vf = NULL;
        s = NULL;
        goto done;
    }
    result = 0;
done:
    irq = spin_lock_irqsave(&g_udp_lock);
    g_udp_file = vf;
    g_udp_socket = s;
    if (s) {
        g_udp_nonce = a20_clx_boot_session();
    }
    g_udp_starting = 0;
    spin_unlock_irqrestore(&g_udp_lock, irq);
    return result;
}

int a20_clx_udp_peer_add(const uint8_t node_id[16], const uint8_t *nh,
                         uint32_t nh_len) {
    if (!node_id || !nh || nh_len != 6 || !(nh[4] | nh[5]) ||
        !(nh[0] | nh[1] | nh[2] | nh[3]) || nh[0] >= 224)
        return -A20_ERR_INVALID_ARGUMENT;
    int r = udp_start();
    if (r < 0)
        return r;
    uint64_t irq = spin_lock_irqsave(&g_udp_lock);
    clx_udp_peer_t *free_peer = NULL;
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        clx_udp_peer_t *p = &g_udp_peers[i];
        if (p->used && memcmp(p->nh, nh, 6) == 0) {
            /* Further routes via this gateway retain its direct identity. */
            spin_unlock_irqrestore(&g_udp_lock, irq);
            return 0;
        }
        if (!p->used && !free_peer)
            free_peer = p;
    }
    if (!free_peer) {
        spin_unlock_irqrestore(&g_udp_lock, irq);
        return -A20_ERR_NO_SPACE;
    }
    memset(free_peer, 0, sizeof(*free_peer));
    free_peer->used = 1;
    free_peer->challenge = (uint32_t)random_u64() | 1u;
    memcpy(free_peer->nh, nh, 6);
    memcpy(free_peer->node.bytes, node_id, 16);
    spin_unlock_irqrestore(&g_udp_lock, irq);
    return 0;
}

void a20_clx_udp_peer_del(const uint8_t *nh, uint32_t nh_len) {
    if (!nh || nh_len != 6)
        return;
    uint64_t irq = spin_lock_irqsave(&g_udp_lock);
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        if (g_udp_peers[i].used && memcmp(g_udp_peers[i].nh, nh, 6) == 0)
            memset(&g_udp_peers[i], 0, sizeof(g_udp_peers[i]));
    }
    spin_unlock_irqrestore(&g_udp_lock, irq);
}

static int udp_wire_send(const uint8_t nh[6], const void *frame, uint32_t len) {
    uint64_t irq = spin_lock_irqsave(&g_udp_lock);
    net_socket_t *s = g_udp_socket;
    spin_unlock_irqrestore(&g_udp_lock, irq);
    if (!s)
        return -A20_ERR_NODE_UNREACHABLE;
    net_sockaddr_in_t addr;
    udp_sockaddr(&addr, nh);
    int n = net_sendto_sock(s, frame, len, MSG_DONTWAIT, &addr, sizeof(addr));
    return n == (int)len ? 0 : -A20_ERR_NODE_UNREACHABLE;
}

static int udp_send(void *ctx, const uint8_t *nh, uint32_t nh_len,
                    const void *frame, uint32_t len) {
    (void)ctx;
    if (!nh || nh_len != 6 || len > A20_CLX_UDP_MTU)
        return -A20_ERR_INVALID_ARGUMENT;
    int ready = 0;
    uint64_t irq = spin_lock_irqsave(&g_udp_lock);
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        if (g_udp_peers[i].used && memcmp(g_udp_peers[i].nh, nh, 6) == 0) {
            ready = g_udp_peers[i].state == A20_CLX_LINK_UP;
            break;
        }
    }
    spin_unlock_irqrestore(&g_udp_lock, irq);
    return ready ? udp_wire_send(nh, frame, len) : -A20_ERR_NODE_UNREACHABLE;
}

static void udp_control(const clx_udp_peer_t *p, uint8_t type,
                        const uint8_t *echo, uint32_t hello_txid) {
    a20_node_id_t self;
    uint32_t caps;
    if (a20_clx_self_get(&self, &caps) < 0)
        return;
    uint8_t payload[32], frame[66];
    a20_frame_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.type = type;
    h.txid = type == A20_CLX_TYPE_HELLO ? p->challenge : hello_txid;
    h.src_hash = a20_clx_node_hash(&self);
    h.dst_hash = a20_clx_node_hash(&p->node);
    h.ttl = A20_CLX_TTL_DEFAULT;
    h.csum_kind = A20_CLX_CSUM_CCITT;
    if (type == A20_CLX_TYPE_HELLO || type == A20_CLX_TYPE_HELLO_ACK) {
        a20_clx_build_hello_payload(self.bytes, 0, A20_CLX_SESSION_VER,
                                    CONFIG_CLUSTER_PROFILE,
                                    (uint8_t)(caps | A20_CLX_CAP_RELIABLE
#if CONFIG_CLUSTER_PROFILE != A20_CLX_PROFILE_MCU
                                              | A20_CLX_CAP_FRAGMENT
#endif
                                              ),
                                    0, g_udp_nonce, payload);
        payload[A20_CLX_HELLO_OFF_LINK_ADDR_LEN] = 6;
        h.payload_len = 32;
    } else {
        h.payload_len = 8;
        if (echo)
            memcpy(payload, echo, 8);
        else
            a20_clx_put_le64(payload, timer_get_ticks());
    }
    int n =
        a20_frame_encode(frame, sizeof(frame), A20_CLX_UDP_MTU, &h, payload);
    if (n > 0)
        udp_wire_send(p->nh, frame, (uint32_t)n);
}

static void udp_receive(const uint8_t nh[6], const uint8_t *frame,
                        uint32_t len) {
    a20_frame_hdr_t h;
    const uint8_t *payload;
    if (a20_frame_decode_session(frame, len, A20_CLX_UDP_MTU, &h, &payload) !=
        A20_FRAME_OK) {
        a20_clx_link_rx_malformed(A20_CLX_TRANSPORT_UDP, nh, 6);
        return;
    }
    clx_udp_peer_t peer;
    uint32_t idx;
    uint64_t irq = spin_lock_irqsave(&g_udp_lock);
    for (idx = 0; idx < CLX_LIMIT_ROUTES; idx++) {
        if (g_udp_peers[idx].used && memcmp(g_udp_peers[idx].nh, nh, 6) == 0)
            break;
    }
    if (idx == CLX_LIMIT_ROUTES) {
        spin_unlock_irqrestore(&g_udp_lock, irq);
        a20_clx_link_rx_drop(A20_CLX_TRANSPORT_UDP, nh, 6);
        return;
    }
    peer = g_udp_peers[idx];
    spin_unlock_irqrestore(&g_udp_lock, irq);

    int hello =
        h.type == A20_CLX_TYPE_HELLO || h.type == A20_CLX_TYPE_HELLO_ACK;
    a20_node_id_t self;
    if (a20_clx_self_get(&self, NULL) < 0)
        return;
    if (h.ttl == 0 ||
        (h.src_hash != a20_clx_node_hash(&peer.node) &&
         !(peer.caps & A20_CLX_CAP_RELAY)) ||
        (hello &&
         (h.src_hash != a20_clx_node_hash(&peer.node) ||
          h.dst_hash != a20_clx_self_hash() ||
          memcmp(payload, peer.node.bytes, 16) != 0 ||
          a20_clx_validate_hello(payload, h.payload_len, h.ver, h.src_hash,
                                 self.bytes, g_udp_nonce, 6) < 0))) {
        a20_clx_link_rx_drop(A20_CLX_TRANSPORT_UDP, nh, 6);
        return;
    }
    if ((!hello && h.ver > peer.version) ||
        (h.flags & A20_CLX_FLAG_BROADCAST) ||
        ((h.flags & A20_CLX_FLAG_FRAGMENTED) &&
         (peer.version < A20_CLX_SESSION_VER ||
          !(peer.caps & A20_CLX_CAP_FRAGMENT)))) {
        a20_clx_link_rx_drop(A20_CLX_TRANSPORT_UDP, nh, 6);
        return;
    }
    if (!hello && peer.state != A20_CLX_LINK_UP &&
        peer.state != A20_CLX_LINK_SUSPECT) {
        a20_clx_link_rx_drop(A20_CLX_TRANSPORT_UDP, nh, 6);
        return;
    }
    if (h.type == A20_CLX_TYPE_HELLO) {
        udp_control(&peer, A20_CLX_TYPE_HELLO_ACK, NULL, h.txid);
        /* A new epoch must answer OUR fresh challenge before adoption.
         * Delayed HELLO/ACK packets cannot roll the peer session backward. */
        uint64_t nonce = a20_clx_get_le64(payload + A20_CLX_HELLO_OFF_NONCE);
        if (nonce != peer.remote_nonce || peer.state == A20_CLX_LINK_DOWN) {
            irq = spin_lock_irqsave(&g_udp_lock);
            if (!g_udp_peers[idx].challenge)
                g_udp_peers[idx].challenge = (uint32_t)random_u64() | 1u;
            peer = g_udp_peers[idx];
            spin_unlock_irqrestore(&g_udp_lock, irq);
            udp_control(&peer, A20_CLX_TYPE_HELLO, NULL, 0);
        }
        return;
    }
    if (h.type == A20_CLX_TYPE_HELLO_ACK &&
        (!peer.challenge || h.txid != peer.challenge)) {
        a20_clx_link_rx_drop(A20_CLX_TRANSPORT_UDP, nh, 6);
        return;
    }
    int restart = hello && peer.remote_nonce &&
                  peer.remote_nonce !=
                      a20_clx_get_le64(payload + A20_CLX_HELLO_OFF_NONCE);
    if (restart)
        a20_clx_link_event(A20_CLX_TRANSPORT_UDP, nh, 6, 0);
    irq = spin_lock_irqsave(&g_udp_lock);
    g_udp_peers[idx].last_rx = timer_get_ticks();
    g_udp_peers[idx].state = A20_CLX_LINK_UP;
    if (hello) {
        g_udp_peers[idx].caps = payload[A20_CLX_HELLO_OFF_CAPS];
        g_udp_peers[idx].tier = payload[A20_CLX_HELLO_OFF_TIER];
        g_udp_peers[idx].version =
            payload[A20_CLX_HELLO_OFF_PROTO_MAX] >= A20_CLX_SESSION_VER
                ? A20_CLX_SESSION_VER
                : A20_CLX_WIRE_VER;
        g_udp_peers[idx].remote_nonce =
            a20_clx_get_le64(payload + A20_CLX_HELLO_OFF_NONCE);
        g_udp_peers[idx].challenge = 0;
    }
    spin_unlock_irqrestore(&g_udp_lock, irq);
    if (restart || peer.state != A20_CLX_LINK_UP)
        a20_clx_link_event(A20_CLX_TRANSPORT_UDP, nh, 6, 1);
    if (h.type == A20_CLX_TYPE_PING && h.dst_hash == a20_clx_self_hash())
        udp_control(&peer, A20_CLX_TYPE_PONG, payload, 0);
    else if (!hello && h.type != A20_CLX_TYPE_PONG)
        a20_clx_rx_frame(A20_CLX_TRANSPORT_UDP, nh, 6, frame, len);
}

static void udp_poll(void *ctx) {
    (void)ctx;
    uint64_t irq = spin_lock_irqsave(&g_udp_lock);
    net_socket_t *s = g_udp_socket;
    spin_unlock_irqrestore(&g_udp_lock, irq);
    if (!s)
        return;
    for (uint32_t i = 0; i < UDP_RX_BUDGET; i++) {
        net_sockaddr_in_t from;
        size_t from_len = sizeof(from);
        int n = net_recvfrom_socket_meta(s, g_udp_rx, sizeof(g_udp_rx),
                                         MSG_DONTWAIT, &from, &from_len, NULL);
        if (n == -EAGAIN)
            break;
        if (n < 0)
            break;
        if (from_len != sizeof(from) || from.sin_family != AF_INET)
            continue;
        uint8_t nh[6];
        memcpy(nh, &from.sin_addr, 4);
        memcpy(nh + 4, &from.sin_port, 2);
        udp_receive(nh, g_udp_rx, (uint32_t)n);
    }
    uint64_t now = timer_get_ticks();
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        clx_udp_peer_t peer;
        irq = spin_lock_irqsave(&g_udp_lock);
        peer = g_udp_peers[i];
        int due = peer.used && (!peer.last_tx ||
                                now - peer.last_tx >= MS_TO_TICKS(UDP_PING_MS));
        if (due) {
            g_udp_peers[i].last_tx = now;
            if (peer.state != A20_CLX_LINK_DOWN &&
                now - peer.last_rx >= MS_TO_TICKS(6000))
                g_udp_peers[i].state = A20_CLX_LINK_DOWN;
            else if (peer.state == A20_CLX_LINK_UP &&
                     now - peer.last_rx >= MS_TO_TICKS(3000))
                g_udp_peers[i].state = A20_CLX_LINK_SUSPECT;
        }
        uint32_t state = g_udp_peers[i].state;
        if (due && state == A20_CLX_LINK_DOWN && !g_udp_peers[i].challenge)
            g_udp_peers[i].challenge = (uint32_t)random_u64() | 1u;
        peer.challenge = g_udp_peers[i].challenge;
        spin_unlock_irqrestore(&g_udp_lock, irq);
        if (!due)
            continue;
        if (state == A20_CLX_LINK_DOWN && peer.state != A20_CLX_LINK_DOWN) {
            kinfo(
                "[CLX-UDP] peer=%u timeout now=%lu last_rx=%lu silent_ms=%lu\n",
                peer.node.bytes[15], (unsigned long)now,
                (unsigned long)peer.last_rx,
                (unsigned long)((now - peer.last_rx) * 1000 / TICKS_PER_SEC));
            a20_clx_link_event(A20_CLX_TRANSPORT_UDP, peer.nh, 6, 0);
        }
        udp_control(&peer,
                    state == A20_CLX_LINK_DOWN ? A20_CLX_TYPE_HELLO
                                               : A20_CLX_TYPE_PING,
                    NULL, 0);
    }
}

static int udp_session(void *ctx, const uint8_t *nh, uint32_t nh_len,
                       const uint8_t node[16], uint64_t *local,
                       uint64_t *remote, uint32_t *caps) {
    (void)ctx;
    int result = -A20_ERR_NODE_UNREACHABLE;
    uint64_t irq = spin_lock_irqsave(&g_udp_lock);
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        clx_udp_peer_t *p = &g_udp_peers[i];
        if (p->used && nh_len == 6 && memcmp(nh, p->nh, 6) == 0 &&
            memcmp(node, p->node.bytes, 16) == 0 &&
            (p->state == A20_CLX_LINK_UP || p->state == A20_CLX_LINK_SUSPECT)) {
            *local = g_udp_nonce;
            *remote = p->remote_nonce;
            *caps = p->version >= A20_CLX_SESSION_VER
                        ? p->caps
                        : p->caps & ~A20_CLX_CAP_RELIABLE;
            result = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&g_udp_lock, irq);
    return result;
}

uint32_t a20_clx_udp_logical_mtu(const uint8_t node_id[16]) {
    uint32_t result = A20_CLX_UDP_MTU;
    uint64_t irq = spin_lock_irqsave(&g_udp_lock);
    for (uint32_t i = 0; i < CLX_LIMIT_ROUTES; i++) {
        clx_udp_peer_t *p = &g_udp_peers[i];
        if (p->used && memcmp(p->node.bytes, node_id, 16) == 0 &&
            p->version >= A20_CLX_SESSION_VER &&
            (p->caps & A20_CLX_CAP_FRAGMENT) &&
            (p->state == A20_CLX_LINK_UP || p->state == A20_CLX_LINK_SUSPECT)) {
            result = p->tier >= A20_CLX_PROFILE_SERVER ? 65590u : 4096u;
            break;
        }
    }
    spin_unlock_irqrestore(&g_udp_lock, irq);
    return result < CLX_LOGICAL_MTU ? result : CLX_LOGICAL_MTU;
}

void a20_clx_udp_init(void) {
    static const a20_clx_transport_t adapter = {
        .transport_id = A20_CLX_TRANSPORT_UDP,
        .mtu = A20_CLX_UDP_MTU,
        .flags = A20_CLX_TFL_POLLING | A20_CLX_TFL_RELIABLE_CAPABLE,
        .send = udp_send,
        .poll = udp_poll,
        .session = udp_session,
    };
    a20_clx_transport_register(&adapter);
}

#endif
