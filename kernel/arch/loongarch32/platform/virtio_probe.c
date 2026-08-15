#include "drivers/bus/virtio_transport.h"

/* The NaiLoong SoC has no virtio transport; these always report no device. */

int arch_virtio_blk_probe(int index, virtio_transport_t *vt) {
    (void)index;
    (void)vt;
    return -1;
}

int arch_virtio_net_probe(int index, virtio_transport_t *vt) {
    (void)index;
    (void)vt;
    return -1;
}
