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

/*
 * Which path a TCP connection to a local address takes.
 *
 * "fast" pairs the two sockets and hands payloads straight to the peer's
 * queue, skipping the lwIP state machine.  "lwip" refuses the shortcut and
 * drives the connection through tcp_connect() and the loopback netif, so the
 * real protocol path runs.
 *
 * The switch exists because the two paths are not interchangeable for
 * measurement.  A load generator on the fast path never allocates a pbuf and
 * never reaches the TCP input path, so it cannot measure the data path it
 * appears to exercise -- measured, a 16 MiB run over "fast" moves the pbuf,
 * bottom-half and driver counters by exactly zero.  Both modes stay reachable
 * at runtime so one build can A/B them and so a gate can assert they deliver
 * identical bytes.
 */
typedef enum a20_tcp_path {
    A20_TCP_PATH_FAST = 0,
    A20_TCP_PATH_LWIP = 1,
} a20_tcp_path_t;

extern a20_tcp_path_t g_a20_tcp_path;

/* Apply a "tcpmode fast|lwip" command.  Returns 0 or a negative errno. */
int a20_net_config_write(const char *buf, size_t count);

#endif /* _NET_NET_CONFIG_H */
