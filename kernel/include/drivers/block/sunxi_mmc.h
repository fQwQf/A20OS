/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_SUNXI_MMC_H
#define A20_SUNXI_MMC_H
#include "core/types.h"

/* Board-owned electrical preparation. The controller clock must already be
 * running at 100..400 kHz; no PLL, GPIO or regulator address belongs here.
 * A static instance may be attached to the firmware-enumerated device's
 * plat_data before driver_probe_all(). Other controllers remain unclaimed. */
typedef struct sunxi_mmc_platform {
    uint32_t clock_hz;
    uint32_t fifo_offset;
    int a64_timing; /* A64 fixed post-divider/new timing/clock-update DAT0 mask */
} sunxi_mmc_platform_t;

/* The same transaction engine is used by the kernel driver and host fixtures.
 * The caller serializes transactions and supplies a monotonically increasing
 * counter. All register offsets are relative to the mapped controller. */
typedef struct sunxi_mmc_io {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    uint64_t (*ticks)(void *);
    uint64_t ticks_per_second;
} sunxi_mmc_io_t;

typedef struct sunxi_mmc_host {
    sunxi_mmc_io_t io;
    uint64_t sectors;
    uint32_t rca;
    uint32_t fifo_offset;
    int mask_data0;
    int high_capacity;
    int write_protected;
    int ready;
    /* Atomic lifecycle cancellation; other state is caller-serialized. */
    int closing;
} sunxi_mmc_host_t;

int sunxi_mmc_start(sunxi_mmc_host_t *, const sunxi_mmc_io_t *,
                    const sunxi_mmc_platform_t *);
/* Drain PIO execution first. A failed start also needs checked stop before
 * the caller releases its mapped host or callback context. */
int sunxi_mmc_stop(sunxi_mmc_host_t *);
int sunxi_mmc_transfer(sunxi_mmc_host_t *, uint64_t lba, void *, size_t sectors,
                       int write);
int sunxi_mmc_flush(sunxi_mmc_host_t *);
#endif
