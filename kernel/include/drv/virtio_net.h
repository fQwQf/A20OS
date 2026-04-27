#ifndef _VIRTIO_NET_H
#define _VIRTIO_NET_H

#include "core/types.h"

#define VIRTIO_NET_MAX_DEVS 2

int  virtio_net_init(void);
int  virtio_net_ready(int idx);
const uint8_t *virtio_net_mac(int idx);
int  virtio_net_send(int idx, const void *packet, size_t len);
int  virtio_net_recv(int idx, void *packet, size_t maxlen);
void virtio_net_poll_all(void);

#endif /* _VIRTIO_NET_H */
