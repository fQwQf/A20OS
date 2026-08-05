/*
 * Slot/protocol conventions for the user-space virtio-blk driver (M4).
 * The second virtio-mmio slot on QEMU virt is reserved for this driver
 * (kernel enumerate skips user-owned slots).
 */
#ifndef _A20_UBD_PROTO_H
#define _A20_UBD_PROTO_H

#include "a20_types.h"

#define A20_UBD_EP_SLOT   (A20_NATIVE_FD_HANDLE_BASE + 44u)
#define A20_UBD_EP_HANDLE ((a20_handle_t)A20_UBD_EP_SLOT)

/* QEMU virt: virtio-mmio slot 3 = bus.3, IRQ = irq_base(1) + slot = 4
 * (slot 1 is used by smoke-vfs-stress's ext4 drive, slot 2 by its isofs). */
#define UBD_MMIO_BASE     0x10004000ULL
#define UBD_MMIO_SIZE     0x1000ULL
#define UBD_MMIO_IRQ      4u

#define UBD_SECTOR_SIZE   512u
#define UBD_QUEUE_SIZE    64u

/* Request protocol on the service channel:
 *   'R' + u64 sector          -> reply { 512 bytes data }
 *   'W' + u64 sector + 512B   -> reply { i64 status }
 *   'C'                       -> exit(42) (crash)
 */
#define UBD_REQ_READ      'R'
#define UBD_REQ_WRITE     'W'
#define UBD_REQ_CRASH     'C'

#endif
