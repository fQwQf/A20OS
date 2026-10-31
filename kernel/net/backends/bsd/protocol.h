/* A20OS-facing declarations for the namespace-isolated BSD-private adapters.
 * Only values and borrowed buffers cross this boundary; no BSD socket, mbuf,
 * ifnet or thread type is visible to the common frontend. */
#ifndef A20_NET_BSD_PROTOCOL_H
#define A20_NET_BSD_PROTOCOL_H

#include "core/types.h"

int a20_bsd_port_socket_create(int domain, int type, int protocol);
int a20_bsd_port_socket_bind(int fd, const void *address, size_t length);
int a20_bsd_port_socket_connect(int fd, const void *address, size_t length);
int a20_bsd_port_socket_listen(int fd, int backlog);
int a20_bsd_port_socket_accept(int fd, void *address, size_t *length);
int a20_bsd_port_socket_close(int fd);
int a20_bsd_port_socket_shutdown(int fd, int receive, int transmit);
int a20_bsd_port_socket_send(int fd, const void *buffer, size_t length,
                             int flags, const void *address, size_t address_length);
int a20_bsd_port_socket_recv(int fd, void *buffer, size_t length,
                             int flags, void *address, size_t *address_length);

#include "protocol_abi.h"

/* Readiness multiplexing.  One kqueue per backend lets the executor touch
 * only sockets that actually have data, instead of issuing a non-blocking
 * recv() per open socket on every progress pass.  a20_bsd_ready_t is declared
 * in protocol_abi.h so the BSD namespace can fill it. */
int a20_bsd_port_readiness_init(void);
int a20_bsd_port_readiness_add(int fd, void *tag);
void a20_bsd_port_readiness_remove(int fd);
int a20_bsd_port_readiness_wait(a20_bsd_ready_t *events, int maximum);


int a20_bsd_port_socket_recv_meta(int fd, void *buffer, size_t length,
                                  int flags, void *address,
                                  size_t *address_length,
                                  a20_bsd_pktinfo_t *info);
int a20_bsd_port_socket_send_control(int fd, const void *buffer, size_t length,
                                     int flags, const void *address,
                                     size_t address_length,
                                     uint32_t ifindex,
                                     const uint8_t source_v4[4]);
int a20_bsd_port_socket_error(int fd);
int a20_bsd_port_socket_poll(int fd, int events);
int a20_bsd_port_socket_getpeername(int fd, void *address, size_t *length);
int a20_bsd_port_socket_getsockname(int fd, void *address, size_t *length);
int a20_bsd_port_socket_set_int(int fd, int level, int option, int value);
int a20_bsd_port_socket_enable_pktinfo(int fd, int domain);
int a20_bsd_port_socket_multicast(int fd, uint32_t group, uint32_t ifaddr,
                                  int join);
int a20_bsd_port_socket_get_int_freebsd(int fd, int level, int option,
                                        int *value);
int a20_bsd_port_socket_set_str_freebsd(int fd, int level, int option,
                                        const char *value);
int a20_bsd_port_socket_set_int_freebsd(int fd, int level, int option,
                                        int value);
void a20_bsd_port_protocol_progress(void);
void a20_bsd_port_hardclock(void);

typedef struct a20_bsd_nic_config {
    int port_id;
    char ifname[16];
    uint8_t mac[6];
    char address[16];
    char netmask[16];
    char broadcast[16];
    char gateway[16];
} a20_bsd_nic_config_t;

typedef struct a20_bsd_nic_stats {
    uint64_t rx_frames, tx_frames, rx_drops, tx_drops, tx_errors, rx_starved;
} a20_bsd_nic_stats_t;

void *a20_bsd_port_nic_attach(const a20_bsd_nic_config_t *config);
void a20_bsd_port_nic_progress(void *context, unsigned budget);
void a20_bsd_port_nic_stats(void *context, a20_bsd_nic_stats_t *out);
int a20_bsd_port_nic_set_mtu(void *context, int mtu);
int a20_bsd_port_nic_set_up(void *context, int up);
/* Replace the primary IPv4 address. old_* is the address currently installed
 * in FreeBSD; a failed add restores it. gateway may be NULL to keep the
 * existing default route. */
int a20_bsd_port_nic_set_ipv4(void *context,
                              const uint8_t address[4],
                              const uint8_t mask[4],
                              const uint8_t gateway[4],
                              const uint8_t old_address[4],
                              const uint8_t old_mask[4],
                              const uint8_t old_gateway[4]);
int a20_bsd_port_nic_route4(void *context, const uint8_t destination[4],
                            uint8_t prefix, const uint8_t gateway[4],
                            int remove);
int a20_bsd_port_nic_set_ipv6(void *context, const uint8_t address[16],
                              uint8_t prefix, int add);
int a20_bsd_port_nic_send_raw(void *context, const void *frame, size_t length);
/* Insert (or refresh) a static Ethernet neighbour entry.  DHCP servers use
 * SIOCSARP for this because the client has no address to answer ARP with yet;
 * without it the OFFER/ACK is queued behind an ARP that can never resolve. */
int a20_bsd_port_nic_set_neighbor(void *context, uint32_t address,
                                  const uint8_t mac[6]);
int a20_bsd_port_nic_ifindex(void *context);
/* Router builds install the kernel forwarding sysctls in this port's VNET. */
void a20_bsd_port_nic_set_forwarding(int ipv4, int ipv6);

/* TUN (L3) interface.  The returned pointer is opaque to the common layer. */
void *a20_bsd_port_tun_create(int unit, a20_bsd_tun_emit_t emit, void *ctx);
void a20_bsd_port_tun_destroy(void *context);
int a20_bsd_port_tun_inject(void *context, const void *packet, size_t length);
int a20_bsd_port_tun_set_mtu(void *context, uint16_t mtu);
int a20_bsd_port_tun_set_flags(void *context, uint32_t value, uint32_t mask);
int a20_bsd_port_tun_get_addr(void *context, uint8_t address[4],
                              uint8_t mask[4], uint8_t gateway[4]);
int a20_bsd_port_tun_set_addr(void *context, const uint8_t address[4],
                              const uint8_t mask[4], int add);
int a20_bsd_port_tun_set_addr6(void *context, const uint8_t address[16],
                               uint8_t prefix, int add);
int a20_bsd_port_tun_snapshot(void *context, a20_bsd_tun_info_t *out);
const char *a20_bsd_port_tun_name(void *context);

#endif
