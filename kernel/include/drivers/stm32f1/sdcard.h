#ifndef _STM32F103_SDCARD_H
#define _STM32F103_SDCARD_H

#include "core/types.h"
#include "drivers/block/block_dev.h"

typedef struct stm32_sdcard_info {
    int present;
    int high_capacity;
    int fat32;
    int bus_width;
    int write_protected;
    uint16_t rca;
    uint64_t sectors;
    uint32_t partition_lba;
    uint32_t fat_sectors;
    uint32_t cluster_count;
    char volume_label[12];

    /*
     * Transfer diagnostics. SDIOCLK is HCLK on the F103, so SDIO_CK moves with
     * the core clock: the 8MHz->72MHz switch multiplied the SD bus clock by 6
     * without anyone choosing to. These counters say which failure mode a card
     * actually hits, since the fixes differ:
     *   rxoverr  -> the polled FIFO reader lost the race (clock too fast for
     *               the CPU, or an ISR stole too long)
     *   dcrcfail -> bits arrived corrupted (signal integrity / clock too fast
     *               for the wiring)
     *   dtimeout -> the card never answered
     */
    uint32_t sdio_ck_hz;      /* the SDIO_CK actually in effect right now */
    uint32_t transfer_hz;     /* the target it was configured from        */
    uint32_t err_dcrcfail;
    uint32_t err_dtimeout;
    uint32_t err_rxoverr;
    uint32_t err_txunderr;
    uint32_t err_stbiterr;
    uint32_t err_cmd;
    uint32_t retries;         /* transient errors survived by a retry     */
    uint32_t shutdowns;       /* times a transfer gave up and dropped the card */
    uint32_t last_err_sta;    /* raw SDIO_STA at the last data failure    */
} stm32_sdcard_info_t;

int stm32_sdcard_init(void);
/* Probe/recover the card through the A20 driver model. */
int stm32_sdcard_recover(void);
void stm32_sdcard_shutdown(void);
int stm32_sdcard_check(void);
int stm32_sdcard_read(uint64_t lba, void *buf, size_t count);
int stm32_sdcard_write(uint64_t lba, const void *buf, size_t count);
const stm32_sdcard_info_t *stm32_sdcard_info(void);
block_dev_t *stm32_sdcard_block_dev(void);

/* Board enumeration hook: publishes the SDIO host as an A20 block device. */
int stm32_sdcard_register_device(void);

/*
 * Retune the data-phase clock at runtime (console `sd clk <mhz>`), so the safe
 * ceiling for this board+card+wiring can be found without a reflash per guess.
 * Persists across stm32_sdcard_init(), so `sd retry` keeps the chosen rate.
 * target_hz is a ceiling; the real SDIO_CK lands at HCLK/(CLKDIV+2).
 */
int stm32_sdcard_set_transfer_hz(uint32_t target_hz);

/* Force the data bus width (1 or 4). 1-bit quarters the FIFO pressure and is
 * the other axis worth bisecting when 4-bit is unreliable. */
int stm32_sdcard_set_bus_width(int width);

#endif
