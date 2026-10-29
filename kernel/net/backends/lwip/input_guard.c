/* Upstream input hooks run before the forwarder's decrement. Preserve local
 * delivery semantics while preventing a zero TTL/hop limit wrapping to 255. */
#include "native.h"
#include "lwip/netif.h"
#include "core/string.h"

int a20_lwip_input_guard4(void *packet, void *input) {
    struct pbuf *p = packet;
    struct netif *ingress = input;
    uint8_t header[20];
    if (pbuf_copy_partial(p, header, sizeof(header), 0) != sizeof(header) ||
        header[0] >> 4 != 4 || header[8] != 0) return 0;
    ip4_addr_t destination;
    memcpy(&destination.addr, header + 16, 4);
    if (ip4_addr_ismulticast(&destination) || ip4_addr_isbroadcast(&destination, ingress))
        return 0;
    for (struct netif *n = netif_list; n; n = n->next)
        if (ip4_addr_cmp(&destination, netif_ip4_addr(n))) return 0;
    pbuf_free(p);
    return 1;
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
