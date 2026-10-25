/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_SDHCI_H
#define A20_SDHCI_H

#include "core/types.h"

struct device;

/* Board mechanisms stay outside the SD/SDHCI protocol driver. prepare() must
 * establish the actual source rate, pins and power before the host is reset.
 * It may install additional device-managed release actions. */
typedef struct sdhci_platform_data {
    uint32_t input_hz;
    uint32_t max_hz;
    unsigned spec_version; /* SDHCI version encoding: 0=1.0, 1=2.0, 2=3.0. */
    int (*prepare)(struct device *device, void *mapped_host);
    int (*before_idle)(struct device *device, void *mapped_host);
} sdhci_platform_data_t;

/* Pure protocol engine. Access width is explicit; the FIFO always uses 32-bit
 * accesses. No DMA is issued, so ordinary caller buffers need no cache ops. */
typedef struct sdhci_io {
    void *context;
    uint32_t (*read)(void *context, unsigned offset, unsigned width);
    void (*write)(void *context, unsigned offset, unsigned width,
                  uint32_t value);
    uint64_t (*time_ms)(void *context);
    void (*delay_us)(void *context, unsigned usecs);
} sdhci_io_t;

typedef struct sdhci_host {
    sdhci_io_t io;
    uint64_t sectors;
    uint32_t input_hz;
    uint32_t max_hz;
    uint32_t actual_hz;
    uint16_t rca;
    unsigned spec_version;
    int block_addressed;
    int write_protected;
    int ready;
    /* Optional vendor hook after host reset/initial clock, before CMD0. */
    int (*before_idle)(void *context);
    void *vendor_context;
} sdhci_host_t;

int sdhci_clock_divider(uint32_t input_hz, uint32_t requested_hz,
                        unsigned spec_version, uint16_t *encoded,
                        uint32_t *actual_hz);
int sdhci_card_initialize(sdhci_host_t *host);
int sdhci_card_transfer(sdhci_host_t *host, uint64_t lba, void *buffer,
                        size_t count, int write);
void sdhci_card_stop(sdhci_host_t *host);

#endif
