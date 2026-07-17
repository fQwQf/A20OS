#ifdef CONFIG_BOARD_STM32F103

#include "sdcard.h"
#include "core/arch.h" /* arch_irq_save/restore — data-phase critical section */
#include "core/string.h"
#include "stm32_uart.h"

#define RCC_AHBENR  (*(volatile uint32_t *)0x40021014UL)
#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define RCC_AHBRSTR (*(volatile uint32_t *)0x40021028UL)

#define GPIOC_CRL (*(volatile uint32_t *)0x40011000UL)
#define GPIOC_CRH (*(volatile uint32_t *)0x40011004UL)
#define GPIOD_CRL (*(volatile uint32_t *)0x40011400UL)

#define SDIO_BASE 0x40018000UL
#define SDIO_POWER  (*(volatile uint32_t *)(SDIO_BASE + 0x00))
#define SDIO_CLKCR  (*(volatile uint32_t *)(SDIO_BASE + 0x04))
#define SDIO_ARG    (*(volatile uint32_t *)(SDIO_BASE + 0x08))
#define SDIO_CMD    (*(volatile uint32_t *)(SDIO_BASE + 0x0C))
#define SDIO_RESP1  (*(volatile uint32_t *)(SDIO_BASE + 0x14))
#define SDIO_RESP2  (*(volatile uint32_t *)(SDIO_BASE + 0x18))
#define SDIO_RESP3  (*(volatile uint32_t *)(SDIO_BASE + 0x1C))
#define SDIO_RESP4  (*(volatile uint32_t *)(SDIO_BASE + 0x20))
#define SDIO_DTIMER (*(volatile uint32_t *)(SDIO_BASE + 0x24))
#define SDIO_DLEN   (*(volatile uint32_t *)(SDIO_BASE + 0x28))
#define SDIO_DCTRL  (*(volatile uint32_t *)(SDIO_BASE + 0x2C))
#define SDIO_STA    (*(volatile uint32_t *)(SDIO_BASE + 0x34))
#define SDIO_ICR    (*(volatile uint32_t *)(SDIO_BASE + 0x38))
#define SDIO_FIFO   (*(volatile uint32_t *)(SDIO_BASE + 0x80))

#define SDIO_CMD_WAITRESP_SHORT (1U << 6)
#define SDIO_CMD_WAITRESP_LONG  (3U << 6)
#define SDIO_CMD_CPSMEN         (1U << 10)

#define SDIO_STA_CCRCFAIL (1U << 0)
#define SDIO_STA_DCRCFAIL (1U << 1)
#define SDIO_STA_CTIMEOUT (1U << 2)
#define SDIO_STA_DTIMEOUT (1U << 3)
#define SDIO_STA_TXUNDERR (1U << 4)
#define SDIO_STA_RXOVERR  (1U << 5)
#define SDIO_STA_CMDREND  (1U << 6)
#define SDIO_STA_CMDSENT  (1U << 7)
#define SDIO_STA_DATAEND  (1U << 8)
#define SDIO_STA_STBITERR (1U << 9)
#define SDIO_STA_TXFIFOHE (1U << 14)
#define SDIO_STA_RXFIFOHF (1U << 15)
#define SDIO_STA_TXFIFOE  (1U << 18)
#define SDIO_STA_RXDAVL   (1U << 21)

#define SDIO_STATIC_FLAGS 0x00C007FFU
#define SDIO_CMD_TIMEOUT_LOOPS 200000U
#define SDIO_CARD_ERROR_MASK 0xFDFFE008U

#define SCB_DEMCR  (*(volatile uint32_t *)0xE000EDFCUL)
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004UL)

static stm32_sdcard_info_t card;
static uint8_t sector_buf[512] __attribute__((aligned(4)));
static block_dev_t sd_block_dev;

/*
 * Data-phase clock ceiling. SDIOCLK = HCLK on the F103, so this is a target the
 * divider rounds down from, not the rate itself. Survives re-init so a runtime
 * `sd clk` choice sticks across `sd retry`.
 */
/*
 * 24MHz — but only because the data phase now runs with interrupts off. Measured
 * on the xuanwu board at 72MHz HCLK via `sd clk`:
 *
 *   without the IRQ guard: 4MHz clean; 6/9/18MHz -> RXOVERR, card dropped;
 *                          24MHz could not even finish identification
 *   with the IRQ guard:    4..24MHz all clean; 24MHz soaked 10x40KB
 *                          write+readback with 0 RXOVERR / 0 retries / 0 drops
 *
 * So the failure was never bandwidth or signal integrity — it was an ISR
 * stealing the CPU mid-block (see sdio_read_one). Faster is also *safer* here:
 * the critical section is one block long, so 24MHz holds interrupts off ~43us
 * versus ~256us at 4MHz.
 */
#define SDIO_DEFAULT_TRANSFER_HZ 24000000U
static uint32_t sdio_transfer_hz = SDIO_DEFAULT_TRANSFER_HZ;
static int sdio_forced_bus_width; /* 0 = negotiate, 1/4 = pinned by console */

/* A single glitched sector used to drop the whole card. Retry first: a bus
 * error is usually transient, and dropping the card turns one bad read into
 * "TF absent" until someone types `sd retry`. */
#define SDIO_IO_ATTEMPTS 3U

/*
 * Longest the data phase may hold interrupts off. A 512-byte block is ~43us at
 * 24MHz/4-bit and ~256us at 4MHz, so this only bites when the card has stopped
 * answering — in which case we hand interrupts back and let the retry/timeout
 * path deal with it rather than stalling SysTick for a full DTIMEOUT.
 */
#define SDIO_DATA_PHASE_MAX_US 4000U

static uint32_t sdio_cycles_per_us(void) {
    uint32_t per_us = stm32_hclk_hz() / 1000000U;
    return per_us ? per_us : 1U;
}

static void sdio_note_error(uint32_t status) {
    card.last_err_sta = status;
    if (status & SDIO_STA_DCRCFAIL)
        card.err_dcrcfail++;
    if (status & SDIO_STA_DTIMEOUT)
        card.err_dtimeout++;
    if (status & SDIO_STA_RXOVERR)
        card.err_rxoverr++;
    if (status & SDIO_STA_TXUNDERR)
        card.err_txunderr++;
    if (status & SDIO_STA_STBITERR)
        card.err_stbiterr++;
}

static int sdio_cmd(uint32_t index, uint32_t arg, uint32_t response,
                    int ignore_crc);

static void sdio_delay_ms(uint32_t ms) {
    uint32_t cycles_per_ms = stm32_hclk_hz() / 1000U;
    if (cycles_per_ms == 0U)
        cycles_per_ms = 1U;
    SCB_DEMCR |= 1U << 24;
    DWT_CTRL |= 1U;
    uint32_t start = DWT_CYCCNT;
    uint32_t duration = cycles_per_ms * ms;
    while ((uint32_t)(DWT_CYCCNT - start) < duration)
        ;
}

static uint32_t sdio_clock_div(uint32_t target_hz) {
    uint32_t hclk = stm32_hclk_hz();
    uint32_t ratio = (hclk + target_hz - 1U) / target_hz;
    uint32_t div = ratio > 2U ? ratio - 2U : 0U;
    return div > 255U ? 255U : div;
}

/* Program CLKCR for the data phase from sdio_transfer_hz + the negotiated bus
 * width, and record what SDIO_CK that actually produced. */
static void sdio_apply_clock(void) {
    uint32_t div = sdio_clock_div(sdio_transfer_hz);
    SDIO_CLKCR = div | (1U << 8) |
                 (card.bus_width == 4 ? (1U << 11) : 0U);
    card.transfer_hz = sdio_transfer_hz;
    card.sdio_ck_hz = stm32_hclk_hz() / (div + 2U);
}

static void gpio_config_pin(volatile uint32_t *crl, volatile uint32_t *crh,
                            unsigned pin, uint32_t mode) {
    volatile uint32_t *reg = pin < 8U ? crl : crh;
    uint32_t shift = (pin & 7U) * 4U;
    uint32_t value = *reg;
    value &= ~(0xFU << shift);
    value |= mode << shift;
    *reg = value;
}

static void sdio_reset(void) {
    RCC_AHBRSTR |= 1U << 10;
    RCC_AHBRSTR &= ~(1U << 10);
}

static void sdio_release_pins(void) {
    for (unsigned pin = 8; pin <= 12; pin++)
        gpio_config_pin(&GPIOC_CRL, &GPIOC_CRH, pin, 0x8U);
    gpio_config_pin(&GPIOD_CRL, &GPIOD_CRL, 2, 0x8U);
}

static void sdio_stop(void) {
    SDIO_DCTRL = 0;
    SDIO_CMD = 0;
    SDIO_ICR = SDIO_STATIC_FLAGS;
    SDIO_CLKCR = 0;
    SDIO_POWER = 0;
    RCC_AHBENR &= ~(1U << 10);
    sdio_release_pins();
}

static int card_status_ready(void) {
    if (!card.rca ||
        sdio_cmd(13, (uint32_t)card.rca << 16,
                 SDIO_CMD_WAITRESP_SHORT, 0) != 0)
        return -1;
    uint32_t status = SDIO_RESP1;
    if (status & SDIO_CARD_ERROR_MASK)
        return -1;
    return ((status & (1U << 8)) && ((status >> 9) & 0xFU) == 4U)
               ? 0
               : -1;
}

static int sdio_cmd(uint32_t index, uint32_t arg, uint32_t response,
                    int ignore_crc) {
    SDIO_ICR = SDIO_STATIC_FLAGS;
    SDIO_ARG = arg;
    SDIO_CMD = (index & 0x3FU) | response | SDIO_CMD_CPSMEN;

    uint32_t timeout = SDIO_CMD_TIMEOUT_LOOPS;
    while (timeout--) {
        uint32_t status = SDIO_STA;
        if (status & SDIO_STA_CTIMEOUT) {
            SDIO_ICR = SDIO_STATIC_FLAGS;
            return -1;
        }
        if (status & SDIO_STA_CCRCFAIL) {
            SDIO_ICR = SDIO_STATIC_FLAGS;
            return ignore_crc ? 0 : -1;
        }
        if (status & (SDIO_STA_CMDREND | SDIO_STA_CMDSENT)) {
            SDIO_ICR = SDIO_STATIC_FLAGS;
            return 0;
        }
    }
    SDIO_ICR = SDIO_STATIC_FLAGS;
    return -1;
}

static int sdio_app_cmd(uint32_t command, uint32_t arg, int ignore_crc) {
    if (sdio_cmd(55, (uint32_t)card.rca << 16,
                 SDIO_CMD_WAITRESP_SHORT, 0) != 0)
        return -1;
    return sdio_cmd(command, arg, SDIO_CMD_WAITRESP_SHORT, ignore_crc);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int sdio_read_one(uint64_t lba, void *buf) {
    if (!card.present || lba >= card.sectors)
        return -1;

    uint32_t arg = card.high_capacity ? (uint32_t)lba :
                   (uint32_t)(lba * 512U);
    SDIO_ICR = SDIO_STATIC_FLAGS;
    SDIO_DTIMER = 0x00FFFFFFU;
    SDIO_DLEN = 512U;
    SDIO_DCTRL = (9U << 4) | (1U << 1) | 1U;

    if (sdio_cmd(17, arg, SDIO_CMD_WAITRESP_SHORT, 0) != 0) {
        SDIO_DCTRL = 0;
        return -1;
    }

    uint32_t *dst = (uint32_t *)buf;
    unsigned words = 0;
    int failed = 0;

    /*
     * Drain the FIFO with interrupts off. The SDIO FIFO is 32 words deep and
     * there is no DMA and no usable hardware flow control (the F103's is
     * erratum'd), so the CPU is the only thing keeping up: one ISR arriving
     * mid-block overruns it and the whole transfer is lost. The IR receiver's
     * EXTI handler busy-waits ~60ms per frame, which a stray edge can trigger
     * at any time, so "an ISR might be slow" is not hypothetical here.
     * Bounded by DWT so a card that stopped answering can't hold interrupts
     * off for the full DTIMEOUT.
     */
    SCB_DEMCR |= 1U << 24;
    DWT_CTRL |= 1U;
    uint32_t start = DWT_CYCCNT;
    uint32_t budget = sdio_cycles_per_us() * SDIO_DATA_PHASE_MAX_US;
    uint32_t irq = arch_irq_save();
    for (;;) {
        uint32_t status = SDIO_STA;
        if (status & (SDIO_STA_DCRCFAIL | SDIO_STA_DTIMEOUT |
                      SDIO_STA_RXOVERR | SDIO_STA_STBITERR)) {
            sdio_note_error(status);
            failed = 1;
            break;
        }
        if (status & SDIO_STA_RXFIFOHF) {
            for (unsigned i = 0; i < 8 && words < 128; i++)
                dst[words++] = SDIO_FIFO;
        } else if (status & SDIO_STA_RXDAVL) {
            if (words < 128)
                dst[words++] = SDIO_FIFO;
            else
                (void)SDIO_FIFO;
        }
        if ((status & SDIO_STA_DATAEND) && words >= 128)
            break;
        if ((uint32_t)(DWT_CYCCNT - start) > budget) {
            failed = 1;
            break;
        }
    }
    arch_irq_restore(irq);

    SDIO_ICR = SDIO_STATIC_FLAGS;
    SDIO_DCTRL = 0;
    return (!failed && words == 128) ? 0 : -1;
}

static int sdio_write_one(uint64_t lba, const void *buf) {
    if (!card.present || lba >= card.sectors)
        return -1;

    uint32_t arg = card.high_capacity ? (uint32_t)lba :
                   (uint32_t)(lba * 512U);
    SDIO_ICR = SDIO_STATIC_FLAGS;
    SDIO_DTIMER = 0x00FFFFFFU;
    SDIO_DLEN = 512U;

    if (sdio_cmd(24, arg, SDIO_CMD_WAITRESP_SHORT, 0) != 0) {
        return -1;
    }
    SDIO_DCTRL = (9U << 4) | 1U;

    const uint32_t *src = (const uint32_t *)buf;
    unsigned words = 0;
    int failed = 0;

    /* Same critical section as the read path — TXUNDERR is the mirror image of
     * RXOVERR: an ISR mid-block starves the FIFO and the write is lost. */
    SCB_DEMCR |= 1U << 24;
    DWT_CTRL |= 1U;
    uint32_t start = DWT_CYCCNT;
    uint32_t budget = sdio_cycles_per_us() * SDIO_DATA_PHASE_MAX_US;
    uint32_t irq = arch_irq_save();
    for (;;) {
        uint32_t status = SDIO_STA;
        if (status & (SDIO_STA_DCRCFAIL | SDIO_STA_DTIMEOUT |
                      SDIO_STA_TXUNDERR | SDIO_STA_STBITERR)) {
            sdio_note_error(status);
            failed = 1;
            break;
        }
        if ((status & SDIO_STA_TXFIFOHE) && words < 128) {
            for (unsigned i = 0; i < 8 && words < 128; i++)
                SDIO_FIFO = src[words++];
        }
        if ((status & SDIO_STA_DATAEND) && words == 128)
            break;
        if ((uint32_t)(DWT_CYCCNT - start) > budget) {
            failed = 1;
            break;
        }
    }
    arch_irq_restore(irq);

    SDIO_ICR = SDIO_STATIC_FLAGS;
    SDIO_DCTRL = 0;
    if (failed || words != 128)
        return -1;

    for (unsigned i = 0; i < 10000; i++)
        if (card_status_ready() == 0)
            return 0;
    return -1;
}

static void copy_volume_label(const uint8_t *src) {
    size_t length = 11;
    while (length && src[length - 1] == ' ')
        length--;
    for (size_t i = 0; i < length; i++) {
        uint8_t ch = src[i];
        card.volume_label[i] = (ch >= 32U && ch <= 126U) ? (char)ch : '?';
    }
    card.volume_label[length] = '\0';
}

static void detect_fat32(void) {
    card.fat32 = 0;
    card.partition_lba = 0;
    card.fat_sectors = 0;
    card.cluster_count = 0;
    card.volume_label[0] = '\0';
    if (sdio_read_one(0, sector_buf) != 0)
        return;
    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA)
        return;

    uint32_t boot_lba = 0;
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t *entry = sector_buf + 446U + i * 16U;
        if (entry[4] == 0x0BU || entry[4] == 0x0CU) {
            boot_lba = read_le32(entry + 8);
            break;
        }
    }

    if (boot_lba && sdio_read_one(boot_lba, sector_buf) != 0)
        return;
    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA)
        return;
    if (sector_buf[11] != 0x00 || sector_buf[12] != 0x02)
        return;

    uint32_t reserved = (uint32_t)sector_buf[14] |
                        ((uint32_t)sector_buf[15] << 8);
    uint32_t fats = sector_buf[16];
    uint32_t root_entries = (uint32_t)sector_buf[17] |
                            ((uint32_t)sector_buf[18] << 8);
    uint32_t sectors_per_cluster = sector_buf[13];
    uint32_t total_sectors = (uint32_t)sector_buf[19] |
                             ((uint32_t)sector_buf[20] << 8);
    if (!total_sectors)
        total_sectors = read_le32(sector_buf + 32);
    uint32_t fat_sectors = (uint32_t)sector_buf[22] |
                           ((uint32_t)sector_buf[23] << 8);
    if (!fat_sectors)
        fat_sectors = read_le32(sector_buf + 36);
    uint32_t root_dir_sectors = ((root_entries * 32U) + 511U) / 512U;
    uint32_t overhead = reserved + fats * fat_sectors + root_dir_sectors;
    if (!sectors_per_cluster || !fats || !fat_sectors ||
        total_sectors <= overhead)
        return;

    uint32_t clusters = (total_sectors - overhead) / sectors_per_cluster;
    if (clusters < 65525U)
        return;

    card.fat32 = 1;
    card.partition_lba = boot_lba;
    card.fat_sectors = fat_sectors;
    card.cluster_count = clusters;
    copy_volume_label(sector_buf + 71);
}

int stm32_sdcard_init(void) {
    memset(&card, 0, sizeof(card));
    memset(&sd_block_dev, 0, sizeof(sd_block_dev));

#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    RCC_APB2ENR |= (1U << 4) | (1U << 5);
    RCC_AHBENR |= 1U << 10;
    sdio_reset();

    for (unsigned pin = 8; pin <= 12; pin++)
        gpio_config_pin(&GPIOC_CRH, &GPIOC_CRH, pin, 0xBU);
    gpio_config_pin(&GPIOD_CRL, &GPIOD_CRL, 2, 0xBU);

    SDIO_POWER = 0;
    SDIO_CLKCR = sdio_clock_div(400000U);
    SDIO_POWER = 3U;
    SDIO_CLKCR |= 1U << 8;
    sdio_delay_ms(2U);

    (void)sdio_cmd(0, 0, 0, 0);
    int v2 = sdio_cmd(8, 0x1AAU, SDIO_CMD_WAITRESP_SHORT, 0) == 0 &&
             (SDIO_RESP1 & 0xFFFU) == 0x1AAU;

    uint32_t ocr = 0;
    for (unsigned i = 0; i < 200; i++) {
        if (sdio_cmd(55, 0, SDIO_CMD_WAITRESP_SHORT, 0) != 0)
            break;
        uint32_t arg = 0x00FF8000U | (v2 ? (1U << 30) : 0U);
        if (sdio_cmd(41, arg, SDIO_CMD_WAITRESP_SHORT, 1) != 0)
            break;
        ocr = SDIO_RESP1;
        if (ocr & (1U << 31))
            break;
        sdio_delay_ms(5U);
    }
    if (!(ocr & (1U << 31)))
        goto absent;
    card.high_capacity = !!(ocr & (1U << 30));

    if (sdio_cmd(2, 0, SDIO_CMD_WAITRESP_LONG, 0) != 0)
        goto absent;
    if (sdio_cmd(3, 0, SDIO_CMD_WAITRESP_SHORT, 0) != 0)
        goto absent;
    card.rca = (uint16_t)(SDIO_RESP1 >> 16);

    if (sdio_cmd(9, (uint32_t)card.rca << 16,
                 SDIO_CMD_WAITRESP_LONG, 0) != 0)
        goto absent;
    uint32_t csd[4] = {SDIO_RESP1, SDIO_RESP2, SDIO_RESP3, SDIO_RESP4};
    if (card.high_capacity) {
        uint32_t csize = ((csd[1] & 0x3FU) << 16) |
                         ((csd[2] >> 16) & 0xFFFFU);
        card.sectors = ((uint64_t)csize + 1U) * 1024U;
    } else {
        uint32_t read_bl_len = (csd[1] >> 16) & 0xFU;
        uint32_t csize = (((csd[1] >> 8) & 0x3U) << 10) |
                         ((csd[1] & 0xFFU) << 2) |
                         ((csd[2] >> 30) & 0x3U);
        uint32_t csize_mult = (csd[2] >> 15) & 0x7U;
        uint64_t blocks = ((uint64_t)csize + 1U) <<
                          (csize_mult + 2U);
        uint64_t block_bytes = 1ULL << read_bl_len;
        card.sectors = (blocks * block_bytes) / 512U;
    }
    if (!card.sectors)
        goto absent;

    if (sdio_cmd(7, (uint32_t)card.rca << 16,
                 SDIO_CMD_WAITRESP_SHORT, 0) != 0)
        goto absent;
    if (!card.high_capacity &&
        sdio_cmd(16, 512, SDIO_CMD_WAITRESP_SHORT, 0) != 0)
        goto absent;

    card.bus_width = 1;
    if (sdio_forced_bus_width != 1 && sdio_app_cmd(6, 2, 0) == 0)
        card.bus_width = 4;
    sdio_apply_clock();

    card.present = 1;
    detect_fat32();
    sd_block_dev.capacity = card.sectors;
    sd_block_dev.sector_size = 512U;
    sd_block_dev.priv = &card;
    return 0;

absent:
    sdio_stop();
    memset(&card, 0, sizeof(card));
    return -1;
#endif
}

void stm32_sdcard_shutdown(void) {
#ifdef CONFIG_STM32_XUANWU
    sdio_stop();
#endif
    memset(&card, 0, sizeof(card));
    memset(&sd_block_dev, 0, sizeof(sd_block_dev));
}

int stm32_sdcard_check(void) {
    if (!card.present)
        return -1;
    if (card_status_ready() == 0)
        return 0;
    stm32_sdcard_shutdown();
    return -1;
}

int stm32_sdcard_read(uint64_t lba, void *buf, size_t count) {
    if (!buf || !card.present || !count ||
        lba >= card.sectors || count > card.sectors - lba)
        return -1;
    for (size_t i = 0; i < count; i++) {
        void *dst = (uint8_t *)buf + i * 512U;
        void *io_buf = ((uintptr_t)dst & 3U) ? sector_buf : dst;
        unsigned attempt;
        for (attempt = 0; attempt < SDIO_IO_ATTEMPTS; attempt++) {
            if (sdio_read_one(lba + i, io_buf) == 0)
                break;
            card.retries++;
            /* Let the card finish/abort the aborted transfer before retrying. */
            (void)card_status_ready();
        }
        if (attempt == SDIO_IO_ATTEMPTS) {
            card.shutdowns++;
            stm32_sdcard_shutdown();
            return -1;
        }
        if (io_buf != dst)
            memcpy(dst, io_buf, 512U);
    }
    return 0;
}

int stm32_sdcard_write(uint64_t lba, const void *buf, size_t count) {
    if (!buf || !card.present || !count || card.write_protected ||
        lba >= card.sectors || count > card.sectors - lba)
        return -1;
    for (size_t i = 0; i < count; i++) {
        const void *src = (const uint8_t *)buf + i * 512U;
        const void *io_buf = src;
        if ((uintptr_t)src & 3U) {
            memcpy(sector_buf, src, 512U);
            io_buf = sector_buf;
        }
        unsigned attempt;
        for (attempt = 0; attempt < SDIO_IO_ATTEMPTS; attempt++) {
            if (sdio_write_one(lba + i, io_buf) == 0)
                break;
            card.retries++;
            (void)card_status_ready();
        }
        if (attempt == SDIO_IO_ATTEMPTS) {
            card.shutdowns++;
            stm32_sdcard_shutdown();
            return -1;
        }
    }
    return 0;
}

int stm32_sdcard_set_transfer_hz(uint32_t target_hz) {
    if (target_hz < 100000U || target_hz > 24000000U)
        return -1;
    sdio_transfer_hz = target_hz;
    if (card.present)
        sdio_apply_clock(); /* takes effect on the next transfer */
    return 0;
}

int stm32_sdcard_set_bus_width(int width) {
    if (width != 1 && width != 4)
        return -1;
    sdio_forced_bus_width = width;
    return 0; /* applied by the next stm32_sdcard_init() */
}

const stm32_sdcard_info_t *stm32_sdcard_info(void) {
    return &card;
}

static int sd_block_read(block_dev_t *dev, uint64_t lba, void *buf,
                         size_t count) {
    (void)dev;
    return stm32_sdcard_read(lba, buf, count);
}

static int sd_block_write(block_dev_t *dev, uint64_t lba, const void *buf,
                          size_t count) {
    (void)dev;
    return stm32_sdcard_write(lba, buf, count);
}

block_dev_t *stm32_sdcard_block_dev(void) {
    if (!card.present)
        return NULL;
    sd_block_dev.read_sector = sd_block_read;
    sd_block_dev.write_sector = sd_block_write;
    return &sd_block_dev;
}

#endif
