/*
 * TUN — Linux /dev/net/tun, kernel-service form.
 *
 * Driver-form rationale and the ABI coverage table live in
 * docs/drivers/classes/tun.md.  Short version: TUN has no hardware to probe,
 * its instances are created by open() + TUNSETIFF, and its peer is the kernel
 * network stack, so it is a kernel service (like pty), not a DriverStore
 * module. Protocol interface slots belong to the selected backend; this file
 * owns only the Linux ABI, packet queues and service instance state.
 */
#include "core/defs.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/poll.h"
#include "core/fcntl.h"
#include "fs/vfs.h"
#include "fs/readiness.h"
#include "mm/mm.h"
#include "proc/proc.h"
#include "proc/park.h"
#include "proc/signal.h"
#include "proc/resource_ns.h"
#include "proc/userns.h"
#include "net/stack.h"
#include "net/profile.h"
#include "net/tun.h"


#define TUN_MAX          NET_PROFILE_TUN_CONTROL_SLOTS
/* Separate bounded control credits let a full set of detached persistent
 * interfaces be reopened. Protocol interface capacity stays backend-owned. */
#define TUN_NAME_LEN     16
#define TUN_RX_DEPTH     NET_PROFILE_TUN_RX_DEPTH

/* Linux TUN ABI (values are architecture-independent for these). */
#define TUNSETIFF        0x400454caUL
#define TUNGETIFF        0x800454d2UL
#define TUNGETFEATURES   0x800454cfUL
#define TUNSETVNETHDRSZ  0x400454d8UL
#define TUNGETVNETHDRSZ  0x800454d7UL
#define TUNSETOFFLOAD    0x400454d0UL
#define TUNSETPERSIST    0x400454cbUL
#define TUNSETOWNER      0x400454ccUL
#define TUNSETGROUP      0x400454ceUL
#define TUNSETQUEUE      0x400454d9UL
#define SIOCGIFMTU       0x8921UL
#define SIOCSIFMTU       0x8922UL
#define SIOCGIFINDEX     0x8933UL
#define SIOCGIFNAME      0x8910UL
#define SIOCGIFFLAGS     0x8913UL
#define SIOCSIFFLAGS     0x8914UL
#define SIOCGIFHWADDR    0x8927UL

/* struct virtio_net_hdr (legacy, no MRG_RXBUF): flags, gso_type, hdr_len,
 * gso_size, csum_start, csum_offset.  wireguard-go reserves exactly this much
 * in front of every packet when IFF_VNET_HDR is in effect. */
#define TUN_VNET_HDR_LEN 10

#define IFF_TUN          0x0001
#define IFF_TAP          0x0002
#define IFF_NO_PI        0x1000
#define IFF_MULTI_QUEUE  0x0100
#define IFF_VNET_HDR     0x4000

/* struct ifreq is a 16-byte name followed by a 24-byte union on every 64-bit
 * target; the union is where ifr_flags and ifr_mtu both live (offset 16). */
struct tun_ifreq {
    char ifr_name[TUN_NAME_LEN];
    union {
        short ifr_flags;
        int   ifr_mtu;
    } ifr_u;
    char ifr_tail[20];
};

typedef struct {
    int      used;
    int      attached;
    char     name[TUN_NAME_LEN];
    int      flags;
    int      vnet_hdr;
    int      vnet_hdr_sz;
    int      persist;
    int      owner;
    int      group;
    uint32_t offload;
    unsigned ifindex;
    /* Kernel -> user: one entry per received packet.  The output callback is
     * called with the backend execution lock held, so it only copies and enqueues; a full
     * queue drops and counts, like Linux's tx_dropped. */
    uint8_t *rx[TUN_RX_DEPTH];
    uint16_t rx_len[TUN_RX_DEPTH];
    int      rx_tail;
    int      rx_count;
    size_t   rx_bytes;
    uint64_t rx_dropped;
    spinlock_t lock;
    wait_queue_t rd_wait;
    int      readers_waiting;
} tun_t;

static tun_t g_tuns[TUN_MAX];
static spinlock_t g_tun_table_lock = SPINLOCK_INIT;
/* Process-context control is serialized; never held by protocol callbacks.
 * Backend -> queue/table spinlocks; control mutex -> backend, never inverse. */
static mutex_t g_tun_control = MUTEX_INIT;
_Static_assert(sizeof(g_tuns) + sizeof(g_tun_table_lock) + sizeof(g_tun_control) <=
               NET_PROFILE_TUN_CONTROL_BUDGET, "TUN control storage exceeds profile budget");

static tun_t *tun_from_vfile(vfile_t *vf)
{
    tun_t *t = (tun_t *)vf->priv;
    if (!t || t < &g_tuns[0] || t >= &g_tuns[TUN_MAX] || !t->used)
        return NULL;
    return t;
}

static int tun_name_in_use(const char *name, const tun_t *self)
{
    for (int i = 0; i < TUN_MAX; i++) {
        if (&g_tuns[i] != self && g_tuns[i].used &&
            strcmp(g_tuns[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static void tun_alloc_name(tun_t *t, char *out)
{
    for (int n = 0; n < TUN_MAX; n++) {
        snprintf(out, TUN_NAME_LEN, "tun%d", n);
        if (!tun_name_in_use(out, t)) {
            snprintf(t->name, sizeof(t->name), "%s", out);
            return;
        }
    }
    snprintf(out, TUN_NAME_LEN, "tun%d", TUN_MAX - 1);
    snprintf(t->name, sizeof(t->name), "%s", out);
}

/* ---------------------------------------------------------------- data path */

static int tun_emit(void *context, size_t len, net_tun_reader_t reader, void *source)
{
    tun_t *t = context;
    if (!len || len > 65535) return -EINVAL;
    if (len > NET_PROFILE_TUN_RX_BYTES) {
        __atomic_add_fetch(&t->rx_dropped, 1, __ATOMIC_RELAXED);
        return 0;
    }
    uint8_t *copy = (uint8_t *)kmalloc(len);
    if (!copy) {
        __atomic_add_fetch(&t->rx_dropped, 1, __ATOMIC_RELAXED);
        return -ENOMEM;
    }
    if (reader(source, copy, len) != len) {
        kfree(copy);
        __atomic_add_fetch(&t->rx_dropped, 1, __ATOMIC_RELAXED);
        return -EIO;
    }

    uint64_t flags = spin_lock_irqsave(&t->lock);
    if (!t->attached || t->rx_count >= TUN_RX_DEPTH ||
        len > NET_PROFILE_TUN_RX_BYTES - t->rx_bytes) {
        spin_unlock_irqrestore(&t->lock, flags);
        kfree(copy);
        __atomic_add_fetch(&t->rx_dropped, 1, __ATOMIC_RELAXED);
        /* Queue full is a drop, not an interface error: the link is fine. */
        return 0;
    }
    int slot = (t->rx_tail + t->rx_count) % TUN_RX_DEPTH;
    t->rx[slot] = copy;
    t->rx_len[slot] = len;
    t->rx_count++;
    t->rx_bytes += len;
    spin_unlock_irqrestore(&t->lock, flags);

    wait_queue_wake_all(&t->rd_wait, 0, PROC_WAKE_EVENT);
    return 0;
}

/* Block until a packet is queued (or a signal/timeout).  Lock is held on entry
 * and on exit unless the return value is negative; mirrors pty's waiter. */
static int tun_wait_locked(tun_t *t, uint64_t *flags)
{
    task_t *task = proc_current();
    if (!task) {
        spin_unlock_irqrestore(&t->lock, *flags);
        proc_yield();
        *flags = spin_lock_irqsave(&t->lock);
        return 0;
    }
    if (signal_task_has_unblocked(task))
        return -ERESTARTSYS;

    spin_unlock_irqrestore(&t->lock, *flags);
    proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
    *flags = spin_lock_irqsave(&t->lock);
    if (!token.task)
        return 0;

    int should_wait = (t->rx_count == 0 && t->used);
    int interrupted = signal_task_has_unblocked(task);
    if (!should_wait || interrupted) {
        spin_unlock_irqrestore(&t->lock, *flags);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        *flags = spin_lock_irqsave(&t->lock);
        return interrupted ? -ERESTARTSYS : 0;
    }

    wait_queue_entry_t entry = {0};
    bool linked = wait_queue_link(&t->rd_wait, &entry, token, 0);
    t->readers_waiting++;

    spin_unlock_irqrestore(&t->lock, *flags);
    proc_wake_reason_t reason = PROC_WAKE_CANCEL;
    if (linked)
        reason = proc_park_commit(token);
    else
        (void)proc_park_cancel(token);
    wait_queue_unlink(&t->rd_wait, &entry);
    proc_park_finish(token);
    *flags = spin_lock_irqsave(&t->lock);
    t->readers_waiting--;

    if (proc_wake_reason_is_task_interrupt(reason))
        return -ERESTARTSYS;
    if (reason == PROC_WAKE_TIMEOUT)
        return -EAGAIN;
    return 0;
}

/* ------------------------------------------------------------------ vfs ops */

int tun_file_open(vfile_t *vf)
{
    tun_t *t = NULL;
    uint64_t flags = spin_lock_irqsave(&g_tun_table_lock);
    for (int i = 0; i < TUN_MAX; i++) {
        if (!g_tuns[i].used) {
            t = &g_tuns[i];
            t->used = 1;
            t->attached = 1;
            t->name[0] = 0;
            t->flags = 0;
            t->vnet_hdr = 0;
            t->vnet_hdr_sz = TUN_VNET_HDR_LEN;
            t->persist = 0;
            t->owner = 0;
            t->group = 0;
            t->offload = 0;
            t->ifindex = 0;
            t->rx_tail = t->rx_count = 0;
            t->rx_bytes = 0;
            t->rx_dropped = 0;
            spin_init(&t->lock); /* fresh instance: no waiter can exist yet */
            wait_queue_init(&t->rd_wait);
            t->readers_waiting = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&g_tun_table_lock, flags);

    if (!t)
        return -ENFILE;
    vf->priv = t;
    return 0;
}

static int tun_setiff(vfile_t *vf, tun_t *t, void *arg)
{
    if (resource_ns_is_initial(resource_ns_task_get(NULL, PROCNS_NET)) == 0)
        return -EOPNOTSUPP; /* per-netns netifs are not designed yet */

    struct tun_ifreq ifr;
    if (copy_from_user(&ifr, arg, sizeof(ifr)) < 0)
        return -EFAULT;

    int want_flags = (unsigned short)ifr.ifr_u.ifr_flags;
    if ((want_flags & IFF_TUN) == 0 || (want_flags & (IFF_TAP | IFF_MULTI_QUEUE)))
        return -EOPNOTSUPP;
    if ((want_flags & ~(IFF_TUN | IFF_NO_PI | IFF_VNET_HDR)) != 0)
        return -EOPNOTSUPP;
    if (t->ifindex)
        return -EBUSY;
    if (ifr.ifr_name[TUN_NAME_LEN - 1]) return -EINVAL;

    char name[TUN_NAME_LEN];
    memset(name, 0, sizeof(name));
    int have_name = 0;
    for (int i = 0; i < TUN_NAME_LEN && ifr.ifr_name[i]; i++) {
        name[i] = ifr.ifr_name[i];
        have_name = 1;
    }
    if (have_name) {
        uint64_t tf = spin_lock_irqsave(&g_tun_table_lock);
        tun_t *existing = NULL;
        for (int i = 0; i < TUN_MAX; ++i)
            if (&g_tuns[i] != t && g_tuns[i].used && !strcmp(g_tuns[i].name, name)) {
                existing = &g_tuns[i]; break;
            }
        if (existing) {
            spin_unlock_irqrestore(&g_tun_table_lock, tf);
            if (!existing->persist || existing->attached) return -EBUSY;
            if (existing->flags != want_flags) return -EINVAL;
            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", existing->name);
            ifr.ifr_u.ifr_flags = (short)existing->flags;
            if (copy_to_user(arg, &ifr, sizeof(ifr)) < 0) return -EFAULT;
            uint64_t qf = spin_lock_irqsave(&existing->lock);
            existing->attached = 1;
            spin_unlock_irqrestore(&existing->lock, qf);
            tf = spin_lock_irqsave(&g_tun_table_lock);
            t->used = 0;
            vf->priv = existing;
            spin_unlock_irqrestore(&g_tun_table_lock, tf);
            return 0;
        }
        snprintf(t->name, sizeof(t->name), "%s", name);
        spin_unlock_irqrestore(&g_tun_table_lock, tf);
    } else {
        uint64_t tf = spin_lock_irqsave(&g_tun_table_lock);
        tun_alloc_name(t, name);
        spin_unlock_irqrestore(&g_tun_table_lock, tf);
    }

    int index = net_stack_tun_create(tun_emit, t);
    if (index < 0) {
        uint64_t tf = spin_lock_irqsave(&g_tun_table_lock);
        t->name[0] = 0;
        spin_unlock_irqrestore(&g_tun_table_lock, tf);
        return index;
    }
    uint64_t tf = spin_lock_irqsave(&g_tun_table_lock);
    t->ifindex = (unsigned)index;
    spin_unlock_irqrestore(&g_tun_table_lock, tf);
    t->flags = want_flags;
    t->vnet_hdr = (want_flags & IFF_VNET_HDR) != 0;
    t->vnet_hdr_sz = TUN_VNET_HDR_LEN;

    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", t->name);
    ifr.ifr_u.ifr_flags = (short)t->flags;
    if (copy_to_user(arg, &ifr, sizeof(ifr)) < 0) {
        net_stack_tun_destroy(t->ifindex);
        tf = spin_lock_irqsave(&g_tun_table_lock);
        t->ifindex = 0;
        t->name[0] = 0;
        spin_unlock_irqrestore(&g_tun_table_lock, tf);
        return -EFAULT;
    }
    return 0;
}

static int tun_ioctl_locked(vfile_t *vf, unsigned long req, void *arg)
{
    tun_t *t = tun_from_vfile(vf);
    if (!t)
        return -EIO;

    switch (req) {
    case TUNSETIFF:
        return tun_setiff(vf, t, arg);
    case TUNGETIFF: {
        if (!t->ifindex)
            return -EBADFD;
        struct tun_ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", t->name);
        ifr.ifr_u.ifr_flags = (short)(t->flags | (t->persist ? 0x0800 : 0));
        return copy_to_user(arg, &ifr, sizeof(ifr)) < 0 ? -EFAULT : 0;
    }
    case TUNGETFEATURES: {
        int features = IFF_TUN | IFF_NO_PI | IFF_VNET_HDR;
        return copy_to_user(arg, &features, sizeof(features)) < 0 ?
               -EFAULT : 0;
    }
    case TUNSETVNETHDRSZ:
    case TUNGETVNETHDRSZ: {
        int sz = 0;
        if (req == TUNSETVNETHDRSZ) {
            if (copy_from_user(&sz, arg, sizeof(sz)) < 0)
                return -EFAULT;
            if (sz != TUN_VNET_HDR_LEN)
                return -EINVAL;
            t->vnet_hdr_sz = sz;
            return 0;
        }
        return copy_to_user(arg, &t->vnet_hdr_sz, sizeof(sz)) < 0 ?
               -EFAULT : 0;
    }
    case TUNSETOFFLOAD: {
        uint32_t requested = (uint32_t)(uintptr_t)arg;
        /* IPv4 checksum completion/TSO/USO only. Unsupported IPv6 GSO is
         * rejected at negotiation, rather than silently losing its packets. */
        if (requested & ~0x2bu) return -EINVAL;
        t->offload = requested;
        return 0;
    }
    case TUNSETPERSIST:
        if (!t->ifindex) return -EBADFD;
        t->persist = (uintptr_t)arg != 0;
        return 0;
    case TUNSETOWNER: case TUNSETGROUP:
        /* Delegated ownership needs userns/credential checks on reattach. */
        return -EOPNOTSUPP;
    case TUNSETQUEUE:
        /* Single queue; multi-queue attach is not implemented. */
        return -EINVAL;
    case SIOCGIFMTU: case SIOCSIFMTU: case SIOCGIFINDEX:
    case SIOCGIFFLAGS: case SIOCSIFFLAGS: case SIOCGIFHWADDR: case SIOCGIFNAME: {
        struct tun_ifreq ifr;
        if (copy_from_user(&ifr, arg, sizeof(ifr)) < 0) return -EFAULT;
        if (!t->ifindex) return -ENODEV;
        if (req == SIOCSIFFLAGS)
            return net_stack_if_set_flags(t->ifindex, ifr.ifr_u.ifr_flags, 0x1);
        if (req == SIOCSIFMTU) {
            if (ifr.ifr_u.ifr_mtu < 68 || ifr.ifr_u.ifr_mtu > 65535) return -EINVAL;
            int rc = net_stack_if_set_mtu(t->ifindex, (uint16_t)ifr.ifr_u.ifr_mtu);
            return rc;
        }
        if (req == SIOCGIFNAME) {
            if (ifr.ifr_u.ifr_mtu != (int)t->ifindex) return -ENODEV;
            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", t->name);
        } else if (req == SIOCGIFINDEX) ifr.ifr_u.ifr_mtu = (int)t->ifindex;
        else if (req == SIOCGIFMTU) {
            int mtu = net_stack_if_mtu(t->ifindex);
            if (mtu < 0) return mtu;
            ifr.ifr_u.ifr_mtu = mtu;
        }
        else if (req == SIOCGIFFLAGS)
            ifr.ifr_u.ifr_flags = (short)(IFF_TUN | (t->flags & IFF_NO_PI) |
                                         (net_stack_if_up(t->ifindex) ? 1 : 0));
        else memset(&ifr.ifr_u, 0, sizeof(ifr.ifr_u));
        return copy_to_user(arg, &ifr, sizeof(ifr)) < 0 ? -EFAULT : 0;
    }
    default:
        return -EINVAL;
    }
}

static int tun_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    mutex_lock(&g_tun_control);
    resource_namespace_t *ns = resource_ns_current(PROCNS_NET);
    int modifies = req == TUNSETIFF || req == TUNSETPERSIST ||
                   req == TUNSETOWNER || req == TUNSETGROUP ||
                   req == SIOCSIFMTU || req == SIOCSIFFLAGS;
    int rc = modifies && (!ns || !userns_capable(proc_current(), ns->owner, CAP_NET_ADMIN))
        ? -EPERM : tun_ioctl_locked(vf, req, arg);
    mutex_unlock(&g_tun_control);
    return rc;
}

static int tun_read(vfile_t *vf, char *buf, size_t count)
{
    tun_t *t = tun_from_vfile(vf);
    if (!t)
        return -EIO;
    if (count == 0)
        return 0;

    uint8_t *pkt;
    uint16_t len;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&t->lock);
        while (t->rx_count == 0) {
            if (vf->flags & O_NONBLOCK) {
                spin_unlock_irqrestore(&t->lock, flags);
                return -EAGAIN;
            }
            int r = tun_wait_locked(t, &flags);
            if (r < 0) {
                spin_unlock_irqrestore(&t->lock, flags);
                return r;
            }
        }
        pkt = t->rx[t->rx_tail];
        len = t->rx_len[t->rx_tail];
        t->rx[t->rx_tail] = NULL;
        t->rx_tail = (t->rx_tail + 1) % TUN_RX_DEPTH;
        t->rx_count--;
        t->rx_bytes -= len;
        spin_unlock_irqrestore(&t->lock, flags);
        if (len != 0 && pkt)
            break;
        /* Never hand a malformed or empty record to the reader: it is not an
         * IP datagram, and consumers treat the virtio header as a real packet
         * ("end of checksum offset ... exceeds packet length (0)"). */
        if (pkt)
            kfree(pkt);
    }

    /* Linux PI precedes an optional virtio header. The protocol field is
     * Ethernet order, flags are native order; truncation sets TUN_PKT_STRIP. */
    size_t pi = (t->flags & IFF_NO_PI) ? 0 : 4;
    if (count < pi + (t->vnet_hdr ? TUN_VNET_HDR_LEN : 0)) { kfree(pkt); return -EINVAL; }
    if (pi) {
        uint16_t flags = len > count - pi - (t->vnet_hdr ? TUN_VNET_HDR_LEN : 0) ? 1 : 0;
        memcpy(buf, &flags, 2);
        buf[2] = (pkt[0] >> 4) == 6 ? (char)0x86 : 8;
        buf[3] = (pkt[0] >> 4) == 6 ? (char)0xdd : 0;
        buf += pi; count -= pi;
    }
    /* Linux truncates to the caller's buffer rather than reporting EMSGSIZE. */
    if (t->vnet_hdr) {
        if (count < TUN_VNET_HDR_LEN) {
            kfree(pkt);
            return -EINVAL;
        }
        memset(buf, 0, TUN_VNET_HDR_LEN); /* no offload requested */
        size_t room = count - TUN_VNET_HDR_LEN;
        size_t n = len < room ? len : room;
        memcpy(buf + TUN_VNET_HDR_LEN, pkt, n);
        kfree(pkt);
        return (int)(n + TUN_VNET_HDR_LEN + pi);
    }
    size_t n = len < count ? len : count;
    memcpy(buf, pkt, n);
    kfree(pkt);
    return (int)(n + pi);
}

/* ---------------------------------------------------- virtio-net offload */

/* wireguard-go requests IFF_VNET_HDR and then TUNSETOFFLOAD(CSUM|TSO4|TSO6),
 * so the packets it writes carry a struct virtio_net_hdr and may ask the kernel
 * to finish a transport checksum or to segment a coalesced TCP/UDP packet.  A
 * driver that advertises the feature but rejects the packets breaks every
 * inbound packet, so both operations are implemented here.
 *
 * The header fields are native-endian u16 (little-endian on every target this
 * kernel builds for).  `flags` bit 0 is NEEDS_CSUM: the transport checksum
 * field holds the pseudo-header partial sum and the kernel must add the
 * header+payload sum (this is exactly what Linux's CHECKSUM_PARTIAL means). */

struct tun_vnet_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
};

#define TUN_VNET_F_NEEDS_CSUM 0x01
#define TUN_VNET_GSO_TCPV4    1
#define TUN_VNET_GSO_TCPV6    4
#define TUN_VNET_GSO_UDP_L4   5
#define TUN_VNET_GSO_ECN      0x80

static uint16_t tun_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* One's-complement sum over big-endian 16-bit words. */
static uint32_t tun_csum_add(const uint8_t *p, size_t n, uint32_t sum)
{
    while (n > 1) {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p += 2;
        n -= 2;
    }
    if (n)
        sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t tun_csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)(~sum & 0xffff);
}

static void tun_csum_put(uint8_t *p, uint16_t c)
{
    if (c == 0)
        c = 0xffff; /* zero means "no checksum" for UDP; never emit it */
    p[0] = (uint8_t)(c >> 8);
    p[1] = (uint8_t)(c & 0xff);
}

static int tun_ip_csum(uint8_t *pkt, size_t ihl)
{
    if (ihl < 20 || ihl > 60)
        return -EINVAL;
    pkt[10] = pkt[11] = 0;
    uint16_t c = tun_csum_fold(tun_csum_add(pkt, ihl, 0));
    if (c == 0)
        c = 0xffff;
    pkt[10] = (uint8_t)(c >> 8);
    pkt[11] = (uint8_t)(c & 0xff);
    return 0;
}

/* Compute the transport checksum of an IPv4 packet whose checksum field
 * already holds the pseudo-header partial sum.  `len` is the number of bytes
 * actually present; the IPv4 total-length field bounds the region. */
static int tun_finish_csum4(uint8_t *pkt, size_t len, uint16_t csum_start,
                            uint16_t csum_offset)
{
    if (len < 20 || (pkt[0] >> 4) != 4)
        return -EINVAL;
    size_t ihl = (size_t)(pkt[0] & 0x0f) * 4;
    if (ihl < 20 || ihl > len)
        return -EINVAL;
    size_t total = ((size_t)pkt[2] << 8) | pkt[3];
    if (total < ihl || total > len)
        return -EINVAL;
    if (csum_start < ihl || (size_t)csum_start + csum_offset + 2 > total)
        return -EINVAL;
    if (csum_start != ihl ||
        !((pkt[9] == 6 && csum_offset == 16 && total >= ihl + 20) ||
          (pkt[9] == 17 && csum_offset == 6 && total >= ihl + 8))) return -EINVAL;
    pkt[csum_start + csum_offset] = pkt[csum_start + csum_offset + 1] = 0;
    uint32_t sum = tun_csum_add(pkt + 12, 8, 0);      /* src + dst */
    sum += (uint32_t)pkt[9];                          /* protocol */
    sum += (uint32_t)(total - ihl);                   /* transport length */
    sum = tun_csum_add(pkt + csum_start, total - csum_start, sum);
    tun_csum_put(pkt + csum_start + csum_offset, tun_csum_fold(sum));
    return 0;
}

static int tun_inject(tun_t *t, const uint8_t *packet, size_t length) {
    return net_stack_tun_inject(t->ifindex, packet, length);
}

/* Segment one TCP-over-IPv4 GSO packet into MTU-sized segments.  Each segment
 * gets its own IP id, total length, sequence number and full checksums, so the
 * receiving stack sees ordinary packets. */
static int tun_gso_tcp4(tun_t *t, uint8_t *pkt, size_t len, size_t hdr_len,
                        size_t mss)
{
    if ((pkt[0] >> 4) != 4 || len < 40)
        return -EINVAL;
    size_t ihl = (size_t)(pkt[0] & 0x0f) * 4;
    if (ihl < 20 || ihl + 20 > len || pkt[9] != 6)
        return -EINVAL;
    size_t total = ((size_t)pkt[2] << 8) | pkt[3];
    if (total == 0 || total > len)
        total = len;
    if (hdr_len < ihl + 20 || hdr_len > total)
        return -EINVAL;
    size_t payload = total - hdr_len;
    if (mss == 0 || mss > 0xffff)
        return -EINVAL;
    /* Even a single segment needs its checksum rebuilt: the coalesced packet
     * only carries the pseudo-header partial sum.  Fall through so the loop
     * below emits it with full checksums. */
    uint8_t *seg = kmalloc(hdr_len + mss);
    if (!seg)
        return -ENOMEM;
    uint32_t seq = ((uint32_t)pkt[ihl + 4] << 24) |
                   ((uint32_t)pkt[ihl + 5] << 16) |
                   ((uint32_t)pkt[ihl + 6] << 8) | pkt[ihl + 7];
    uint16_t ipid = (uint16_t)(((uint16_t)pkt[4] << 8) | pkt[5]);
    size_t off = 0;
    int rc = 0;
    while (off < payload) {
        size_t chunk = payload - off;
        if (chunk > mss)
            chunk = mss;
        memcpy(seg, pkt, hdr_len);
        memcpy(seg + hdr_len, pkt + hdr_len + off, chunk);
        size_t seglen = hdr_len + chunk;
        uint16_t sl = (uint16_t)seglen;
        seg[2] = (uint8_t)(sl >> 8);
        seg[3] = (uint8_t)sl;
        ipid++;
        seg[4] = (uint8_t)(ipid >> 8);
        seg[5] = (uint8_t)ipid;
        uint32_t s = seq + (uint32_t)off;
        seg[ihl + 4] = (uint8_t)(s >> 24);
        seg[ihl + 5] = (uint8_t)(s >> 16);
        seg[ihl + 6] = (uint8_t)(s >> 8);
        seg[ihl + 7] = (uint8_t)s;
        if (off + chunk >= payload)
            seg[ihl + 13] |= 0x08; /* PSH on the final segment */
        else
            seg[ihl + 13] &= (uint8_t)~0x08;
        tun_ip_csum(seg, ihl);
        seg[ihl + 16] = seg[ihl + 17] = 0;
        uint32_t sum = tun_csum_add(seg + 12, 8, 0);
        sum += 6; /* IPPROTO_TCP */
        sum += (uint32_t)(seglen - ihl);
        sum = tun_csum_add(seg + ihl, seglen - ihl, sum);
        tun_csum_put(seg + ihl + 16, tun_csum_fold(sum));
        if (tun_inject(t, seg, seglen) != 0) {
            rc = -EIO;
            break;
        }
        off += chunk;
    }
    kfree(seg);
    return rc;
}

/* UDP_L4 GSO: `gso_size` is the payload size of each datagram (headers are
 * repeated per segment, exactly as wireguard-go's UDP GRO accounting writes). */
static int tun_gso_udp4(tun_t *t, uint8_t *pkt, size_t len, size_t hdr_len,
                        size_t gso_size)
{
    if ((pkt[0] >> 4) != 4 || len < 28)
        return -EINVAL;
    size_t ihl = (size_t)(pkt[0] & 0x0f) * 4;
    if (ihl < 20 || ihl + 8 > len || pkt[9] != 17)
        return -EINVAL;
    size_t total = ((size_t)pkt[2] << 8) | pkt[3];
    if (total == 0 || total > len)
        total = len;
    if (hdr_len < ihl + 8 || hdr_len > total)
        return -EINVAL;
    size_t payload = total - hdr_len;
    if (gso_size == 0 || gso_size > 0xffff)
        return -EINVAL;
    /* Single-segment case still needs a full checksum; see tun_gso_tcp4(). */
    uint8_t *seg = kmalloc(hdr_len + gso_size);
    if (!seg)
        return -ENOMEM;
    uint16_t ipid = (uint16_t)(((uint16_t)pkt[4] << 8) | pkt[5]);
    size_t off = 0;
    int rc = 0;
    while (off < payload) {
        size_t chunk = payload - off;
        if (chunk > gso_size)
            chunk = gso_size;
        memcpy(seg, pkt, hdr_len);
        memcpy(seg + hdr_len, pkt + hdr_len + off, chunk);
        size_t seglen = hdr_len + chunk;
        uint16_t sl = (uint16_t)seglen;
        seg[2] = (uint8_t)(sl >> 8);
        seg[3] = (uint8_t)sl;
        ipid++;
        seg[4] = (uint8_t)(ipid >> 8);
        seg[5] = (uint8_t)ipid;
        uint16_t ul = (uint16_t)(8 + chunk);
        seg[ihl + 4] = (uint8_t)(ul >> 8);
        seg[ihl + 5] = (uint8_t)ul;
        tun_ip_csum(seg, ihl);
        seg[ihl + 6] = seg[ihl + 7] = 0;
        uint32_t sum = tun_csum_add(seg + 12, 8, 0);
        sum += 17; /* IPPROTO_UDP */
        sum += (uint32_t)(seglen - ihl);
        sum = tun_csum_add(seg + ihl, seglen - ihl, sum);
        tun_csum_put(seg + ihl + 6, tun_csum_fold(sum));
        if (tun_inject(t, seg, seglen) != 0) {
            rc = -EIO;
            break;
        }
        off += chunk;
    }
    kfree(seg);
    return rc;
}

static int tun_write(vfile_t *vf, const char *buf, size_t count)
{
    tun_t *t = tun_from_vfile(vf);
    if (!t || !t->ifindex)
        return -EIO;
    if (count == 0) {
        /* wireguard-go's status probe.  Its routineHackListener maps EINVAL to
         * "up" and EIO to "down" and gives up on anything else. */
        int up = net_stack_if_up(t->ifindex);
        return up ? -EINVAL : -EIO;
    }

    const uint8_t *pkt = (const uint8_t *)buf;
    size_t pkt_len = count;
    if (!(t->flags & IFF_NO_PI)) {
        if (pkt_len < 4) return -EINVAL;
        if (!((pkt[2] == 8 && pkt[3] == 0) || (pkt[2] == 0x86 && pkt[3] == 0xdd))) return -EINVAL;
        pkt += 4; pkt_len -= 4;
    }
    if (t->vnet_hdr) {
        if (pkt_len < TUN_VNET_HDR_LEN)
            return -EINVAL;
        struct tun_vnet_hdr h;
        h.flags = pkt[0];
        h.gso_type = pkt[1];
        h.hdr_len = tun_rd16(pkt + 2);
        h.gso_size = tun_rd16(pkt + 4);
        h.csum_start = tun_rd16(pkt + 6);
        h.csum_offset = tun_rd16(pkt + 8);
        pkt += TUN_VNET_HDR_LEN;
        pkt_len -= TUN_VNET_HDR_LEN;
        /* A header with no packet behind it is what wireguard-go emits for an
         * empty payload; Linux's tun_get_user() accepts and drops it. */
        if (pkt_len == 0)
            return (int)count;
        if (pkt_len > 0xffff)
            return -EMSGSIZE;

        uint8_t type = (uint8_t)(h.gso_type & (uint8_t)~TUN_VNET_GSO_ECN);
        if (type != 0) {
            /* The packet is mutable: it lives in the ABI's scratch buffer. */
            uint8_t *mp = (uint8_t *)pkt;
            int rc;
            if (type == TUN_VNET_GSO_TCPV4) {
                size_t ihl = (size_t)(mp[0] & 0x0f) * 4;
                size_t thl = (ihl + 20 <= pkt_len) ?
                             (size_t)(mp[ihl + 12] >> 4) * 4 : 0;
                size_t hdr = h.hdr_len;
                if (hdr < ihl + thl || hdr > pkt_len) {
                    if (ihl + thl > pkt_len)
                        return -EINVAL;
                    hdr = ihl + thl;
                }
                rc = tun_gso_tcp4(t, mp, pkt_len, hdr, h.gso_size);
                if (rc < 0)
                    return rc;
                return (int)count;
            }
            if (type == TUN_VNET_GSO_UDP_L4) {
                size_t ihl = (size_t)(mp[0] & 0x0f) * 4;
                size_t hdr = h.hdr_len;
                if (hdr < ihl + 8 || hdr > pkt_len) {
                    if (ihl + 8 > pkt_len)
                        return -EINVAL;
                    hdr = ihl + 8;
                }
                rc = tun_gso_udp4(t, mp, pkt_len, hdr, h.gso_size);
                if (rc < 0)
                    return rc;
                return (int)count;
            }
            /* IPv6 GSO: this stack has no IPv6 data path.  Report it rather
             * than inject an unusable packet. */
            return -EOPNOTSUPP;
        }
        if (h.flags & TUN_VNET_F_NEEDS_CSUM) {
            if ((pkt[0] >> 4) != 4)
                return -EOPNOTSUPP;
            int rc = tun_finish_csum4((uint8_t *)pkt, pkt_len,
                                      h.csum_start, h.csum_offset);
            if (rc < 0)
                return rc;
        }
    }
    if (pkt_len > 0xffff)
        return -EMSGSIZE;
    if (tun_inject(t, pkt, pkt_len) != 0)
        return -EIO;
    return (int)count;
}

static int tun_poll(vfile_t *vf, short events)
{
    tun_t *t = tun_from_vfile(vf);
    if (!t)
        return POLLERR;
    int ready = 0;
    if (events & POLLIN) {
        uint64_t flags = spin_lock_irqsave(&t->lock);
        if (t->rx_count > 0)
            ready |= POLLIN;
        spin_unlock_irqrestore(&t->lock, flags);
    }
    if (events & POLLOUT)
        ready |= POLLOUT;
    return ready;
}

static size_t tun_poll_sources(vfile_t *vf, short events,
                               readiness_source_t *sources, size_t max)
{
    tun_t *t = tun_from_vfile(vf);
    if (!t || !sources || max == 0 || !(events & POLLIN))
        return 0;
    sources[0] = (readiness_source_t){ &t->rd_wait, 0, 0 };
    return 1;
}

static int tun_close(vfile_t *vf)
{
    mutex_lock(&g_tun_control);
    tun_t *t = tun_from_vfile(vf);
    if (!t) { mutex_unlock(&g_tun_control); return 0; }
    if (t->ifindex && !t->persist) {
        net_stack_tun_destroy(t->ifindex);
        uint64_t tf = spin_lock_irqsave(&g_tun_table_lock);
        t->ifindex = 0;
        spin_unlock_irqrestore(&g_tun_table_lock, tf);
    }
    uint64_t flags = spin_lock_irqsave(&t->lock);
    for (int i = 0; i < TUN_RX_DEPTH; i++) {
        if (t->rx[i]) {
            kfree(t->rx[i]);
            t->rx[i] = NULL;
        }
    }
    t->rx_count = 0;
    t->rx_bytes = 0;
    t->attached = 0;
    t->rx_tail = 0;
    spin_unlock_irqrestore(&t->lock, flags);

    uint64_t tf = spin_lock_irqsave(&g_tun_table_lock);
    if (!t->persist) { t->used = 0; t->name[0] = 0; }
    spin_unlock_irqrestore(&g_tun_table_lock, tf);
    vf->priv = NULL;
    mutex_unlock(&g_tun_control);
    return 0;
}

vfile_ops_t g_tun_ops = {
    .read = tun_read,
    .write = tun_write,
    .ioctl = tun_ioctl,
    .poll = tun_poll,
    .poll_sources = tun_poll_sources,
    .close = tun_close,
};

int net_tun_interface_name(unsigned index, char *out, unsigned cap) {
    if (!index || !out || !cap) return 0;
    int found = 0;
    uint64_t flags = spin_lock_irqsave(&g_tun_table_lock);
    for (int i = 0; i < TUN_MAX; ++i)
        if (g_tuns[i].used && g_tuns[i].ifindex == index) {
            snprintf(out, cap, "%s", g_tuns[i].name); found = 1; break;
        }
    spin_unlock_irqrestore(&g_tun_table_lock, flags);
    return found;
}
unsigned net_tun_interface_by_name(const char *name) {
    if (!name || !name[0]) return 0;
    unsigned index = 0;
    uint64_t flags = spin_lock_irqsave(&g_tun_table_lock);
    for (int i = 0; i < TUN_MAX; ++i)
        if (g_tuns[i].used && !strcmp(g_tuns[i].name, name)) {
            index = g_tuns[i].ifindex; break;
        }
    spin_unlock_irqrestore(&g_tun_table_lock, flags);
    return index;
}

int net_tun_format(char *out, size_t capacity) {
    if (!out || !capacity) return 0;
    unsigned controls = 0, interfaces = 0, attached = 0, packets = 0;
    size_t bytes = 0; uint64_t dropped = 0;
    uint64_t flags = spin_lock_irqsave(&g_tun_table_lock);
    for (int i = 0; i < TUN_MAX; ++i) {
        tun_t *t = &g_tuns[i];
        if (!t->used) continue;
        ++controls;
        if (t->ifindex) ++interfaces;
        uint64_t qf = spin_lock_irqsave(&t->lock);
        attached += t->attached != 0;
        packets += (unsigned)t->rx_count; bytes += t->rx_bytes;
        dropped += __atomic_load_n(&t->rx_dropped, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&t->lock, qf);
    }
    spin_unlock_irqrestore(&g_tun_table_lock, flags);
    int used = snprintf(out, capacity,
        "tun: controls=%u/%u interfaces=%u/%u attached=%u queued=%u bytes=%lu dropped=%llu static=%lu queue_quota=%lu\n",
        controls, (unsigned)TUN_MAX, interfaces, (unsigned)NET_PROFILE_TUN_MAX_DEVS,
        attached, packets, (unsigned long)bytes, (unsigned long long)dropped,
        (unsigned long)(sizeof(g_tuns) + sizeof(g_tun_table_lock) + sizeof(g_tun_control)),
        (unsigned long)(NET_PROFILE_TUN_MAX_DEVS * NET_PROFILE_TUN_RX_BYTES));
    return used < 0 ? 0 : (size_t)used >= capacity ? (int)capacity - 1 : used;
}
