#ifndef A20_NET_BSD_TUN_H
#define A20_NET_BSD_TUN_H
#include "core/types.h"
#include "net/interface.h"
#include "net/tun.h"
#include "net/profile.h"

/* Public indices: 1 primary NIC, 2 loopback, then extra NIC slots, then TUN. */
#define A20_BSD_TUN_BASE (2 + NET_PROFILE_NETIF_MAX_DEVS)

int a20_bsd_tun_create(net_tun_emit_t emit, void *ctx);
void a20_bsd_tun_destroy(unsigned index);
int a20_bsd_tun_inject(unsigned index, const uint8_t *packet, size_t length);
int a20_bsd_tun_is_index(unsigned index);
int a20_bsd_tun_up(unsigned index);
int a20_bsd_tun_mtu(unsigned index);
int a20_bsd_tun_set_mtu(unsigned index, uint16_t mtu);
int a20_bsd_tun_set_flags(unsigned index, uint32_t value, uint32_t mask);
int a20_bsd_tun_get_addr(unsigned index, uint8_t address[4], uint8_t mask[4],
                         uint8_t gateway[4]);
int a20_bsd_tun_set_addr(unsigned index, const uint8_t address[4],
                         const uint8_t mask[4], int add);
int a20_bsd_tun_route4(unsigned index, const uint8_t destination[4],
                       uint8_t prefix, int remove);
int a20_bsd_tun_set_addr6(unsigned index, const uint8_t address[16],
                          uint8_t prefix, int add);
int a20_bsd_tun_del_addr6(unsigned index, const uint8_t address[16]);
int a20_bsd_tun_snapshot(nl_link_out_t *out, int max);
int a20_bsd_tun_interface_ipv4(unsigned index);
int a20_bsd_tun_owns_ipv4(uint32_t address);
#endif
