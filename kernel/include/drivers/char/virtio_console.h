/*
 * virtio-console (virtio-serial) user ABI.
 *
 * Shared by the kernel driver (kernel/drivers/char/virtio_console.c) and by
 * user-space clients, so it is written the way kernel/mm/swap.h is: only
 * fixed-width types from <stdint.h>, no kernel headers.  The ioctl numbers
 * and the stats layout below are the whole contract; nothing here may change
 * shape without a version bump, because a stale user program would then be
 * talking to a kernel that reads a different struct.
 */
#ifndef _DRIVERS_CHAR_VIRTIO_CONSOLE_H
#define _DRIVERS_CHAR_VIRTIO_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#define VPORT_IOCTL_VERSION      1U

/* arg: int32_t* in user space.  Non-zero attaches received bytes to the
 * kernel console input path (uart_receive_char), zero detaches.  Attached is
 * the default; a client that reads the port itself detaches first, otherwise
 * every byte it consumes is also delivered to the shell as typed input. */
#define VPORT_IOCTL_SET_CONSOLE  0x9001
/* arg: struct vport_stats* in user space. */
#define VPORT_IOCTL_GET_STATS    0x9002

struct vport_stats {
    uint32_t version;
    uint32_t rx_bytes;      /* bytes the receive pump moved into the byte ring */
    uint32_t rx_dropped;    /* bytes discarded: byte ring full, or console-mirror
                             * staging buffer full while mirroring was on */
    uint32_t tx_bytes;      /* bytes the device accepted from write() */
    uint32_t tx_timeouts;   /* transmit completions that never arrived */
    uint32_t console_mirror;
} __attribute__((packed));

#endif /* _DRIVERS_CHAR_VIRTIO_CONSOLE_H */