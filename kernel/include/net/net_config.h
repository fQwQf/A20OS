#ifndef _NET_NET_CONFIG_H
#define _NET_NET_CONFIG_H

#include "core/types.h"
#include "lwip/ip4_addr.h"
#include "lwip/dns.h"

typedef struct a20_net_config {
    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    ip4_addr_t dns[DNS_MAX_SERVERS];
    int        dns_count;
    int        dhcp_enable;
    char       hostname[64];
} a20_net_config_t;

/* Global effective runtime network configuration.  Populated during early boot
 * from the kernel command line and updated by DHCP when enabled. */
extern a20_net_config_t g_a20_net_config;

/* Parse a20.* keys from the kernel command line and initialize g_a20_net_config. */
void a20_net_config_init(void);

/* Format the effective runtime configuration into a key=value text buffer. */
int a20_net_config_format(char *buf, size_t bufsz);

/* Synchronize g_a20_net_config from current lwIP netif/DNS state.  Caller must
 * hold g_lwip_lock (via a20_lwip_lock()). */
void a20_net_config_sync_from_lwip(void);

#endif /* _NET_NET_CONFIG_H */
