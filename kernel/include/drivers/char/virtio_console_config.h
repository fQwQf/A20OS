#ifndef _VIRTIO_CONSOLE_CONFIG_H
#define _VIRTIO_CONSOLE_CONFIG_H

/* Standalone builds retain the throughput defaults. The module build injects
 * a generated header with preset values and any explicit instance overrides. */
#ifndef CONFIG_VIRTIO_CONSOLE_RX_SLOTS
#define CONFIG_VIRTIO_CONSOLE_RX_SLOTS 8
#endif
#ifndef CONFIG_VIRTIO_CONSOLE_TX_SLOTS
#define CONFIG_VIRTIO_CONSOLE_TX_SLOTS 16
#endif
#ifndef CONFIG_VIRTIO_CONSOLE_BUFFER_SIZE
#define CONFIG_VIRTIO_CONSOLE_BUFFER_SIZE 4096
#endif
#ifndef CONFIG_VIRTIO_CONSOLE_RING_SIZE
#define CONFIG_VIRTIO_CONSOLE_RING_SIZE 4096
#endif

#if CONFIG_VIRTIO_CONSOLE_RX_SLOTS < 1 || CONFIG_VIRTIO_CONSOLE_RX_SLOTS > 16 || \
    (CONFIG_VIRTIO_CONSOLE_RX_SLOTS & (CONFIG_VIRTIO_CONSOLE_RX_SLOTS - 1))
#error "virtio-console RX slots must be a power of two in [1,16]"
#endif
#if CONFIG_VIRTIO_CONSOLE_TX_SLOTS < 1 || CONFIG_VIRTIO_CONSOLE_TX_SLOTS > 16 || \
    (CONFIG_VIRTIO_CONSOLE_TX_SLOTS & (CONFIG_VIRTIO_CONSOLE_TX_SLOTS - 1))
#error "virtio-console TX slots must be a power of two in [1,16]"
#endif
#if CONFIG_VIRTIO_CONSOLE_BUFFER_SIZE < 64 || CONFIG_VIRTIO_CONSOLE_BUFFER_SIZE > 4096 || \
    (CONFIG_VIRTIO_CONSOLE_BUFFER_SIZE & (CONFIG_VIRTIO_CONSOLE_BUFFER_SIZE - 1))
#error "virtio-console buffer size must be a power of two in [64,4096]"
#endif
#if CONFIG_VIRTIO_CONSOLE_RING_SIZE < 128 || CONFIG_VIRTIO_CONSOLE_RING_SIZE > 65536 || \
    (CONFIG_VIRTIO_CONSOLE_RING_SIZE & (CONFIG_VIRTIO_CONSOLE_RING_SIZE - 1))
#error "virtio-console byte ring size must be a power of two in [128,65536]"
#endif
#endif
