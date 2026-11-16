/* SPDX-License-Identifier: Apache-2.0
 * Independently implemented from the Allwinner A64 User Manual, SMC chapter,
 * and SD Association Physical Layer Simplified Specification, chapters 4/5.
 * Linux/U-Boot sunxi-mmc were consulted to check register facts and quirks;
 * no upstream implementation is incorporated here. This engine deliberately
 * uses 1-bit, <=400 kHz, single-block PIO: no DMA coherency/descriptor assumptions,
 * voltage switching, tuning, undocumented PLL rates or automatic stop commands.
 */
#include "drivers/block/sunxi_mmc.h"
#include "core/errno.h"
#include "core/string.h"

#define GCTRL 0x00U
#define CLKCR 0x04U
#define TMOUT 0x08U
#define WIDTH 0x0cU
#define BLKSZ 0x10U
#define BCNT 0x14U
#define CMD 0x18U
#define ARG 0x1cU
#define RESP 0x20U
#define IMASK 0x30U
#define RINT 0x38U
#define STATUS 0x3cU
#define NTSR 0x5cU
#define SAMPLE_DELAY 0x144U
#define RESET 7U
#define CPU_ACCESS (1U << 31)
#define CLOCK_ENABLE (1U << 16)
#define START (1U << 31)
#define UPDATE_CLOCK (1U << 21)
#define WAIT_PREVIOUS (1U << 13)
#define RESPONSE (1U << 6)
#define LONG_RESPONSE (1U << 7)
#define CRC (1U << 8)
#define DATA (1U << 9)
#define WRITE (1U << 10)
#define COMMAND_DONE (1U << 2)
#define DATA_DONE (1U << 3)
#define RESPONSE_TIMEOUT (1U << 8)
#define DATA_TIMEOUT (1U << 9)
#define ERRORS 0xbfc2U
#define FIFO_EMPTY (1U << 2)
#define FIFO_FULL (1U << 3)
#define BUSY (1U << 9)
#define R1_ERRORS 0xfff98008U
#define OCR_VOLTAGE 0x00300000U /* fixed 3.3V board supply: 3.2..3.4V */
#define OCR_BUSY (1U << 31)
#define OCR_CCS (1U << 30)
#define APP_COMMAND (1U << 5)
#define SECTOR_SIZE 512U
/* A dead counter is a platform error; a secondary finite poll budget still
 * prevents a permanently wedged controller from hanging the boot forever. */
#define POLL_LIMIT 10000000U

static uint32_t rd(sunxi_mmc_host_t *h, uint32_t reg) {
    return h->io.read(h->io.context, reg);
}
static void wr(sunxi_mmc_host_t *h, uint32_t reg, uint32_t value) {
    h->io.write(h->io.context, reg, value);
}
static uint64_t now(sunxi_mmc_host_t *h) {
    return h->io.ticks(h->io.context);
}
static uint64_t interval(sunxi_mmc_host_t *h, unsigned ms) {
    uint64_t hz = h->io.ticks_per_second;
    uint64_t ticks = (hz / 1000U) * ms + ((hz % 1000U) * ms + 999U) / 1000U;
    return ticks ? ticks : 1;
}
static int cancelled(sunxi_mmc_host_t *h) {
    return __atomic_load_n(&h->closing, __ATOMIC_ACQUIRE);
}
static int delay_ms(sunxi_mmc_host_t *h, unsigned ms) {
    uint64_t begin = now(h), limit = interval(h, ms);
    for (unsigned polls = 0; polls < POLL_LIMIT; polls++) {
        if (cancelled(h))
            return -ENODEV;
        if (now(h) - begin >= limit)
            return 0;
    }
    return -ETIMEDOUT;
}
static int error_status(uint32_t status) {
    if (status == (~0U))
        return -ENODEV;
    if (status & (RESPONSE_TIMEOUT | DATA_TIMEOUT))
        return -ETIMEDOUT;
    return status & ERRORS ? -EIO : 0;
}
static int wait_clear(sunxi_mmc_host_t *h, uint32_t reg, uint32_t mask,
                      unsigned ms, int stopping) {
    uint64_t begin = now(h), limit = interval(h, ms);
    for (unsigned polls = 0; polls < POLL_LIMIT; polls++) {
        if (!stopping && cancelled(h))
            return -ENODEV;
        uint32_t value = rd(h, reg);
        if (value == (~0U))
            return -ENODEV;
        if (!(value & mask))
            return 0;
        if (now(h) - begin >= limit)
            break;
    }
    return -ETIMEDOUT;
}
static int wait_interrupt(sunxi_mmc_host_t *h, uint32_t mask) {
    uint64_t begin = now(h), limit = interval(h, 2000);
    for (unsigned polls = 0; polls < POLL_LIMIT; polls++) {
        if (cancelled(h))
            return -ENODEV;
        uint32_t status = rd(h, RINT);
        int rc = error_status(status);
        if (rc)
            return rc;
        if ((status & mask) == mask)
            return 0;
        if (now(h) - begin >= limit)
            break;
    }
    return -ETIMEDOUT;
}
static int clock_update(sunxi_mmc_host_t *h, int stopping) {
    if (!stopping && cancelled(h))
        return -ENODEV;
    uint32_t clock = rd(h, CLKCR);
    if (clock == (~0U))
        return -ENODEV;
    if (h->mask_data0)
        wr(h, CLKCR, clock | (1U << 31));
    wr(h, RINT, (~0U));
    wr(h, CMD, START | UPDATE_CLOCK | WAIT_PREVIOUS);
    int rc = wait_clear(h, CMD, START, 2000, stopping);
    if (rc == -ENODEV || (!stopping && cancelled(h)))
        return -ENODEV;
    if (!rc)
        rc = error_status(rd(h, RINT));
    if (rc == -ENODEV)
        return rc;
    wr(h, RINT, (~0U));
    if (h->mask_data0)
        wr(h, CLKCR, clock & ~(1U << 31));
    return rc;
}
static int command(sunxi_mmc_host_t *h, unsigned index, uint32_t argument,
                   uint32_t flags, void *data, uint32_t response[4]) {
    int rc = wait_clear(h, STATUS, BUSY, 2000, 0);
    if (rc)
        return rc;
    wr(h, RINT, (~0U));
    if (data) {
        wr(h, GCTRL, CPU_ACCESS | (1U << 1));
        if ((rc = wait_clear(h, GCTRL, 1U << 1, 100, 0)))
            return rc;
        wr(h, BLKSZ, SECTOR_SIZE);
        wr(h, BCNT, SECTOR_SIZE);
        flags |= DATA | WAIT_PREVIOUS;
    }
    wr(h, ARG, argument);
    wr(h, CMD, START | flags | index | (index ? 0 : (1U << 15)));
    if (data) {
        uint8_t *bytes = data;
        uint64_t begin = now(h), limit = interval(h, 2000);
        unsigned word = 0;
        for (unsigned polls = 0; word < SECTOR_SIZE / 4 && polls < POLL_LIMIT;
             polls++) {
            if (cancelled(h))
                return -ENODEV;
            rc = error_status(rd(h, RINT));
            if (rc)
                return rc;
            uint32_t unavailable = flags & WRITE ? FIFO_FULL : FIFO_EMPTY;
            uint32_t status = rd(h, STATUS);
            if (status == (~0U))
                return -ENODEV;
            if (!(status & unavailable)) {
                uint32_t value;
                if (flags & WRITE) {
                    memcpy(&value, bytes + word * 4, 4);
                    wr(h, h->fifo_offset, value);
                } else {
                    value = rd(h, h->fifo_offset);
                    memcpy(bytes + word * 4, &value, 4);
                }
                word++;
            }
            if (now(h) - begin >= limit)
                return -ETIMEDOUT;
        }
        if (word != SECTOR_SIZE / 4)
            return -ETIMEDOUT;
    }
    rc = wait_interrupt(h, COMMAND_DONE | (data ? DATA_DONE : 0));
    if (rc)
        return rc;
    if (response) {
        if (flags & LONG_RESPONSE) {
            for (unsigned i = 0; i < 4; i++)
                response[i] = rd(h, RESP + (3 - i) * 4);
        } else
            response[0] = rd(h, RESP);
    }
    wr(h, RINT, (~0U));
    return 0;
}
static int r1(sunxi_mmc_host_t *h, unsigned cmd, uint32_t arg, uint32_t flags,
              void *data) {
    uint32_t response[4] = {0};
    int rc = command(h, cmd, arg, flags | RESPONSE | CRC, data, response);
    return rc ? rc : response[0] & R1_ERRORS ? -EIO : 0;
}
static int card_ready(sunxi_mmc_host_t *h) {
    uint64_t begin = now(h), limit = interval(h, 2000);
    for (unsigned polls = 0; polls < 2000; polls++) {
        uint32_t response[4] = {0};
        int rc = command(h, 13, h->rca, RESPONSE | CRC, NULL, response);
        if (rc)
            return rc;
        if (response[0] & R1_ERRORS)
            return -EIO;
        if ((response[0] & (1U << 8)) && ((response[0] >> 9) & 15U) == 4)
            return 0;
        if (now(h) - begin >= limit)
            break;
        if ((rc = delay_ms(h, 1)))
            return rc;
    }
    return -ETIMEDOUT;
}
static int capacity(sunxi_mmc_host_t *h, const uint32_t csd[4]) {
    unsigned structure = csd[0] >> 30;
    if (structure == 1 && h->high_capacity) {
        uint32_t size = ((csd[1] & 63U) << 16) | (csd[2] >> 16);
        h->sectors = ((uint64_t)size + 1) * 1024;
    } else if (!structure && !h->high_capacity) {
        unsigned block_bits = (csd[1] >> 16) & 15U;
        unsigned multiplier = (csd[2] >> 15) & 7U;
        uint32_t size = ((csd[1] & 1023U) << 2) | (csd[2] >> 30);
        if (block_bits < 9 || block_bits > 11)
            return -EINVAL;
        h->sectors = ((uint64_t)size + 1) << (multiplier + 2 + block_bits - 9);
        /* SDSC byte addressing is limited to a 32-bit command argument. */
        if (h->sectors > (1ULL << 23))
            return -ERANGE;
    } else
        return -EOPNOTSUPP;
    /* Permanent and temporary card write protection, CSD bits 13 and 12. */
    h->write_protected = (csd[3] & (3U << 12)) != 0;
    return h->sectors ? 0 : -EINVAL;
}
int sunxi_mmc_stop(sunxi_mmc_host_t *h) {
    if (!h || !h->io.read || !h->io.write || !h->io.ticks ||
        !h->io.ticks_per_second)
        return -EINVAL;
    __atomic_store_n(&h->closing, 1, __ATOMIC_RELEASE);
    h->ready = 0;
    if (rd(h, GCTRL) == (~0U))
        return -ENODEV;
    wr(h, IMASK, 0);
    /* Normal PIO is drained; reset/clock acknowledgement must still run with
     * cancellation set. Finite poll budgets cover a stopped timer counter. */
    wr(h, GCTRL, RESET);
    int rc = wait_clear(h, GCTRL, RESET, 100, 1);
    if (rc)
        return rc;
    wr(h, GCTRL, CPU_ACCESS);
    wr(h, IMASK, 0);
    wr(h, CLKCR, 0);
    rc = clock_update(h, 1);
    if (rc)
        return rc;
    uint32_t control = rd(h, GCTRL);
    uint32_t mask = rd(h, IMASK);
    uint32_t clock = rd(h, CLKCR);
    if (control == (~0U) || mask == (~0U) || clock == (~0U))
        return -ENODEV;
    if ((control & (RESET | (1U << 4) | (1U << 5))) ||
        !(control & CPU_ACCESS) || mask || (clock & CLOCK_ENABLE))
        return -ETIMEDOUT;
    return 0;
}
int sunxi_mmc_start(sunxi_mmc_host_t *h, const sunxi_mmc_io_t *io,
                    const sunxi_mmc_platform_t *platform) {
    if (!h || !io || !platform || !io->read || !io->write || !io->ticks ||
        io->ticks_per_second < 1000 || io->ticks_per_second > (~0ULL) / 4 ||
        platform->clock_hz < 100000 || platform->clock_hz > 400000 ||
        (platform->fifo_offset != 0x100 && platform->fifo_offset != 0x200))
        return -EINVAL;
    memset(h, 0, sizeof(*h));
    h->io = *io;
    h->fifo_offset = platform->fifo_offset;
    h->mask_data0 = platform->a64_timing;
    if (rd(h, GCTRL) == (~0U))
        return -ENODEV;
    wr(h, IMASK, 0);
    wr(h, GCTRL, RESET);
    int rc = wait_clear(h, GCTRL, RESET, 100, 0);
    if (rc)
        goto fail;
    wr(h, GCTRL, CPU_ACCESS);
    wr(h, TMOUT, (~0U));
    wr(h, WIDTH, 0); /* 1-bit mode after CMD0; no ACMD6 is issued. */
    wr(h, CLKCR, 0);
    if ((rc = clock_update(h, 0)))
        goto fail;
    if (platform->a64_timing) {
        wr(h, NTSR,
           rd(h, NTSR) | (1U << 31)); /* A64 requires new timing mode */
        wr(h, SAMPLE_DELAY,
           1U << 7); /* documented software delay enable, zero delay */
    }
    wr(h, CLKCR,
       CLOCK_ENABLE); /* divider zero: use the board's <=400kHz module clock */
    if ((rc = clock_update(h, 0)))
        goto fail;
    if ((rc = command(h, 0, 0, 0, NULL, NULL)))
        goto fail;
    uint32_t response[4] = {0};
    rc = command(h, 8, 0x1aa, RESPONSE | CRC, NULL, response);
    int version2 = !rc;
    if (rc && rc != -ETIMEDOUT)
        goto fail;
    if (version2 && response[0] != 0x1aa) {
        rc = -EOPNOTSUPP;
        goto fail;
    }
    uint64_t begin = now(h), limit = interval(h, 2000);
    int powered = 0;
    for (unsigned attempt = 0; attempt < 2000; attempt++) {
        if ((rc = command(h, 55, 0, RESPONSE | CRC, NULL, response)))
            goto fail;
        if ((response[0] & R1_ERRORS) || !(response[0] & APP_COMMAND)) {
            rc = -EOPNOTSUPP;
            goto fail;
        }
        if ((rc = command(h, 41, OCR_VOLTAGE | (version2 ? OCR_CCS : 0),
                          RESPONSE, NULL, response)))
            goto fail;
        if (!(response[0] & OCR_VOLTAGE)) {
            rc = -EOPNOTSUPP;
            goto fail;
        }
        if (response[0] & OCR_BUSY) {
            powered = 1;
            break;
        }
        if (now(h) - begin >= limit)
            break;
        if ((rc = delay_ms(h, 1)))
            goto fail;
    }
    if (!powered) {
        rc = -ETIMEDOUT;
        goto fail;
    }
    h->high_capacity = version2 && (response[0] & OCR_CCS);
    if ((rc = command(h, 2, 0, RESPONSE | LONG_RESPONSE | CRC, NULL, response)))
        goto fail;
    if ((rc = command(h, 3, 0, RESPONSE | CRC, NULL, response)))
        goto fail;
    if ((response[0] & 0xe000U) || !(response[0] & 0xffff0000U)) {
        rc = -EIO;
        goto fail;
    }
    h->rca = response[0] & 0xffff0000U;
    if ((rc = command(h, 9, h->rca, RESPONSE | LONG_RESPONSE | CRC, NULL,
                      response)))
        goto fail;
    if ((rc = capacity(h, response)))
        goto fail;
    if ((rc = r1(h, 7, h->rca, 0, NULL)))
        goto fail;
    if (!h->high_capacity && (rc = r1(h, 16, SECTOR_SIZE, 0, NULL)))
        goto fail;
    if ((rc = card_ready(h)))
        goto fail;
    h->ready = 1;
    return 0;
fail:
    h->ready = 0;
    return rc;
}
int sunxi_mmc_transfer(sunxi_mmc_host_t *h, uint64_t lba, void *buffer,
                       size_t sectors, int write) {
    if (!h || !buffer || !sectors || (write != 0 && write != 1) ||
        sectors > SIZE_MAX / SECTOR_SIZE)
        return -EINVAL;
    if (!h->ready || cancelled(h))
        return -ENODEV;
    if (lba >= h->sectors || sectors > h->sectors - lba)
        return -ERANGE;
    if (write && h->write_protected)
        return -EROFS;
    uint8_t *bytes = buffer;
    for (size_t i = 0; i < sectors; i++) {
        uint64_t argument =
            h->high_capacity ? lba + i : (lba + i) * SECTOR_SIZE;
        if (argument > (~0U))
            return -ERANGE;
        int rc = r1(h, write ? 24 : 17, (uint32_t)argument, write ? WRITE : 0,
                    bytes + i * SECTOR_SIZE);
        if (!rc && write)
            rc = card_ready(h);
        if (rc) {
            /* An interrupted data command leaves card state uncertain. Do not
             * present later I/O as successful without a fresh initialization. */
            h->ready = 0;
            return rc;
        }
    }
    return 0;
}
int sunxi_mmc_flush(sunxi_mmc_host_t *h) {
    if (!h || !h->ready || cancelled(h))
        return -ENODEV;
    int rc = card_ready(h);
    if (rc)
        h->ready = 0;
    return rc;
}
