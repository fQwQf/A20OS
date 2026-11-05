#ifndef A20_NET_INTERFACE_H
#define A20_NET_INTERFACE_H
#include "core/types.h"
#define NLRT_IF_FLAGS_UP 0x1
#define NLRT_IF_FLAGS_LOOPBACK 0x8
#define NLRT_IF_FLAGS_RUNNING 0x40
/* The Rust `NlrtLink`, filled by the snapshot primitive.  Same layout as the
 * Rust struct; the offsets the Rust side reads are pinned below. */
typedef struct {
    char     name[16];
    uint32_t index;
    uint32_t mtu;
    uint32_t flags;
    uint8_t  mac[6];
    uint8_t  loopback;
    uint8_t  has_ip;
    uint8_t  has_gw;
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
} nl_link_out_t;

typedef struct {
    uint32_t index;
    uint8_t address[16];
    uint8_t prefix, flags, scope, reserved;
} nl_addr6_out_t;

void net_netlink_link_notify_one(unsigned index, int up);

#endif
