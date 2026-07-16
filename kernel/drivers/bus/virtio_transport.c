#include "drivers/bus/virtio_transport.h"

__attribute__((weak)) int arch_virtio_gpu_probe(int index, virtio_transport_t *vt) {
    (void)index;
    (void)vt;
    return -1;
}

__attribute__((weak)) int arch_virtio_input_probe(int index, virtio_transport_t *vt) {
    (void)index;
    (void)vt;
    return -1;
}
