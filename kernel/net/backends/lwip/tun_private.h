#ifndef A20_LWIP_TUN_PRIVATE_H
#define A20_LWIP_TUN_PRIVATE_H
#include "net/tun.h"
struct netif;
int a20_lwip_netif_is_tun(struct netif *);
int a20_lwip_tun_create(net_tun_emit_t, void *);
void a20_lwip_tun_destroy(unsigned);
int a20_lwip_tun_inject(unsigned, const uint8_t *, size_t);
#endif
