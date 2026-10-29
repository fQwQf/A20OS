/* Upstream input hooks run before the forwarder's decrement. Preserve local
 * delivery semantics while preventing a zero TTL/hop limit wrapping to 255. */
#include "native.h"
#include "lwip/netif.h"
#include "lwip/ip4.h"
#include "lwip/inet_chksum.h"
#include "core/lock.h"
#include "core/timer.h"
#include "core/string.h"

/* Bounded exception emission, independent of CT timeouts and flow ownership.
 * The small lock is released before allocation/output or protocol callbacks. */
static spinlock_t exception_lock = SPINLOCK_INIT;
static uint32_t exception_ms;
static unsigned exception_tokens = 16;
static int exception_credit(void) {
    uint64_t ticks = timer_get_ticks();
    uint32_t now = (uint32_t)((ticks / TICKS_PER_SEC) * 1000 +
                              (ticks % TICKS_PER_SEC) * 1000 / TICKS_PER_SEC);
    uint64_t flags = spin_lock_irqsave(&exception_lock);
    uint32_t refill = (now - exception_ms) / 100;
    if (refill) {
        exception_tokens = refill >= 16 || exception_tokens + refill >= 16
            ? 16 : exception_tokens + refill;
        exception_ms = now;
    }
    int allowed = exception_tokens != 0;
    if (allowed) --exception_tokens;
    spin_unlock_irqrestore(&exception_lock, flags);
    return allowed;
}
static __attribute__((unused)) void ipv4_exception(struct pbuf *original, struct netif *ingress,
                           const uint8_t *header, unsigned ihl,
                           uint8_t type, uint8_t code, uint16_t mtu) {
    ip4_addr_t source, destination;
    memcpy(&destination.addr, header + 12, 4);
    source = *netif_ip4_addr(ingress);
    if (ip4_addr_isany(&source) || ip4_addr_isany(&destination) ||
        ip4_addr_ismulticast(&destination) || ip4_addr_isbroadcast(&destination, ingress) ||
        (header[12] == 0 || header[12] == 127 || header[12] >= 224) || ((header[6] & 31) || header[7])) return;
    if (header[9] == 1) {
        uint8_t icmp_type;
        if (pbuf_copy_partial(original, &icmp_type, 1, (u16_t)ihl) != 1 ||
            icmp_type == 3 || icmp_type == 4 || icmp_type == 5 ||
            icmp_type == 11 || icmp_type == 12) return;
    }
    if (!exception_credit()) return;
    unsigned length = ((unsigned)header[2] << 8) | header[3];
    /* RFC 1812: quote as much as fits a 576-byte IPv4 exception. */
    unsigned quote = length < 548 ? length : 548;
    struct pbuf *reply = pbuf_alloc(PBUF_IP, (u16_t)(8 + quote), PBUF_RAM);
    if (!reply) return;
    uint8_t *bytes = reply->payload;
    memset(bytes, 0, 8);
    bytes[0] = type; bytes[1] = code;
    bytes[6] = (uint8_t)(mtu >> 8); bytes[7] = (uint8_t)mtu;
    if (pbuf_copy_partial(original, bytes + 8, (u16_t)quote, 0) == quote) {
        uint16_t check = inet_chksum(bytes, (u16_t)(8 + quote));
        memcpy(bytes + 2, &check, 2);
        ip4_output(reply, &source, &destination, 64, 0, 1);
    }
    pbuf_free(reply);
}
int a20_lwip_input_guard4(void *packet, void *input) {
    struct pbuf *p = packet;
    struct netif *ingress = input;
    uint8_t header[60];
    if (pbuf_copy_partial(p, header, 20, 0) != 20 || header[0] >> 4 != 4) return 0;
    unsigned ihl = (header[0] & 15) * 4;
    unsigned length = ((unsigned)header[2] << 8) | header[3];
    if (ihl < 20 || length < ihl || length > p->tot_len ||
        pbuf_copy_partial(p, header, (u16_t)ihl, 0) != ihl ||
        inet_chksum(header, (u16_t)ihl) != 0) return 0;
    ip4_addr_t destination, source;
    memcpy(&destination.addr, header + 16, 4); memcpy(&source.addr, header + 12, 4);
    if (ip4_addr_ismulticast(&destination) || ip4_addr_isbroadcast(&destination, ingress)) return 0;
    for (struct netif *n = netif_list; n; n = n->next)
        if (ip4_addr_cmp(&destination, netif_ip4_addr(n))) return 0;
    if (header[8] == 0) { pbuf_free(p); return 1; }
#if IP_FORWARD
    struct netif *egress = ip4_route_src(&source, &destination);
    if (!egress || !netif_is_up(egress)) return 0;
#if !IP_FORWARD_ALLOW_TX_ON_RX_NETIF
    if (egress == ingress) return 0;
#endif
    if (header[8] == 1) {
        ipv4_exception(p, ingress, header, ihl, 11, 0, 0);
        pbuf_free(p); return 1;
    }
    if ((header[6] & 0x40) && egress->mtu && length > egress->mtu) {
        ipv4_exception(p, ingress, header, ihl, 3, 4, egress->mtu);
        pbuf_free(p); return 1;
    }
#endif
    return 0;
}

int a20_lwip_input_guard6(void *packet, void *input) {
    (void)input;
    struct pbuf *p = packet;
    uint8_t header[40];
    if (pbuf_copy_partial(p, header, sizeof(header), 0) != sizeof(header) ||
        header[0] >> 4 != 6 || header[7] != 0 || header[24] == 0xff) return 0;
    for (struct netif *n = netif_list; n; n = n->next)
        for (unsigned i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i)
            if (ip6_addr_isvalid(netif_ip6_addr_state(n, i)) &&
                !memcmp(netif_ip6_addr(n, i)->addr, header + 24, 16)) return 0;
    pbuf_free(p);
    return 1;
}
