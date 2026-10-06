#ifndef _UAPI_A20_BLOCK_H
#define _UAPI_A20_BLOCK_H

/* Block-device driver statistics, shared by a class driver's ioctl handler
 * and whatever reads the result from userspace.
 *
 * The counters answer one question: how did the driver learn that a command
 * had finished?  A block driver that only spins on the used ring and a driver
 * that parks on the completion interrupt both return 0 from read and write,
 * so the return value cannot tell them apart -- and "it works" is exactly the
 * claim that a silent regression to polling would also satisfy.  irq_count is
 * the observation that cannot be produced by a poll: it is incremented by the
 * interrupt handler, so a non-zero value means the device really raised the
 * line and the platform really dispatched it.
 *
 * irq_mode says which of the three delivery paths the driver settled on, and
 * irq_line is the interrupt number it settled on (-1 when polling).  Together
 * they distinguish "interrupt-driven" from "asked for an interrupt and fell
 * back", which otherwise look identical in every other field.
 *
 * The layout is part of the userspace ABI: a driver that does not implement
 * this ioctl must leave it returning -ENOTTY rather than filling part of the
 * structure, so a reader can tell "no such counter" from "zero events".
 */

typedef __UINT32_TYPE__ a20_blk_u32;
typedef __UINT64_TYPE__ a20_blk_u64;

/* ioctl requests accepted on /dev/diskN.  The first three are the ones the
 * block layer has always dispatched; they are spelled here because a raw
 * block file is the only way to reach a driver's capacity and flush ops
 * without a filesystem on top (fsync(2) on a raw device node has no vnode,
 * so vfs_fsync_vfile() has nothing to sync and returns success without ever
 * asking the driver).  kernel/drivers/core/driver_class.h aliases its
 * internal BLK_IOCTL_* names onto these, so there is one value per request. */

#define A20_BLK_IOCTL_GET_CAPACITY 0x1001U
#define A20_BLK_IOCTL_GET_SECTOR_SZ 0x1002U
#define A20_BLK_IOCTL_SYNC 0x1003U
#define A20_BLK_IOCTL_GET_STATS 0x1004U

/* Values for a20_blk_stats::irq_mode. */
#define A20_BLK_IRQ_POLL 0U /* no interrupt: completion found by polling */
#define A20_BLK_IRQ_INTX 1U /* shared PCI INTx line */
#define A20_BLK_IRQ_MSIX 2U /* message-signalled vector(s) */

#define A20_BLK_STATS_VERSION 1U

typedef struct a20_blk_stats {
    a20_blk_u64 version;
    /* Commands submitted, including the ones issued by the driver's own
     * probe (TEST UNIT READY, READ CAPACITY) and by flush. */
    a20_blk_u64 commands;
    a20_blk_u64 flushes;
    a20_blk_u64 timeouts;
    /* Interrupt handler invocations.  Zero is the honest "no interrupt was
     * delivered", not "no counter". */
    a20_blk_u64 irq_count;
    /* Completions observed after the issuer parked on the interrupt. */
    a20_blk_u64 irq_completions;
    /* Completions observed inside the bounded pre-poll window, before the
     * issuer ever parked. */
    a20_blk_u64 spin_completions;
    a20_blk_u64 irq_mode;
    /* Signed: -1 when polling. */
    a20_blk_u64 irq_line;
} a20_blk_stats_t;

#endif /* _UAPI_A20_BLOCK_H */
