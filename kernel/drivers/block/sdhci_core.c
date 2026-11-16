/* SPDX-License-Identifier: Apache-2.0 */
/* SDHCI PIO / SD memory protocol, independently implemented from the SDHCI
 * register contract. Board clock/pad mechanisms are supplied by arch code. */
#include "drivers/block/sdhci.h"
#include "core/errno.h"

enum {
    BLOCK_SIZE = 0x04,
    BLOCK_COUNT = 0x06,
    ARGUMENT = 0x08,
    TRANSFER_MODE = 0x0c,
    COMMAND = 0x0e,
    RESPONSE = 0x10,
    BUFFER = 0x20,
    PRESENT_STATE = 0x24,
    HOST_CONTROL = 0x28,
    POWER_CONTROL = 0x29,
    CLOCK_CONTROL = 0x2c,
    TIMEOUT_CONTROL = 0x2e,
    SOFTWARE_RESET = 0x2f,
    INT_STATUS = 0x30,
    INT_ENABLE = 0x34,
    SIGNAL_ENABLE = 0x38,
};
#define CMD_DONE 1U
#define DATA_DONE 2U
#define WRITE_READY (1U << 4)
#define READ_READY (1U << 5)
#define ERROR (1U << 15)
#define ERROR_BITS 0xffff0000U
#define CMD_CRC (1U << 3)
#define CMD_INDEX (1U << 4)
#define CMD_DATA (1U << 5)
#define RESP_136 1U
#define RESP_48 2U
#define RESP_BUSY 3U
/* Errors in the R1 card status; READY_FOR_DATA/state/APP_CMD are not errors. */
#define CARD_ERRORS 0xfff98008U

static uint32_t rd(sdhci_host_t *h, unsigned reg, unsigned width) {
    return h->io.read(h->io.context, reg, width);
}
static void wr(sdhci_host_t *h, unsigned reg, unsigned width, uint32_t value) {
    h->io.write(h->io.context, reg, width, value);
}
static int expired(sdhci_host_t *h, uint64_t start, unsigned ms) {
    return h->io.time_ms(h->io.context) - start >= ms;
}
static int cancelled(sdhci_host_t *h) {
    return __atomic_load_n(&h->closing, __ATOMIC_ACQUIRE);
}
static int wait_mask(sdhci_host_t *h, unsigned reg, unsigned width,
                     uint32_t mask, uint32_t wanted, unsigned ms) {
    uint64_t start = h->io.time_ms(h->io.context);
    do {
        if (cancelled(h))
            return -ENODEV;
        uint32_t value = rd(h, reg, width);
        uint32_t absent = width == 1 ? 0xffU : width == 2 ? 0xffffU : 0xffffffffU;
        if (value == absent)
            return -ENODEV;
        if ((value & mask) == wanted)
            return 0;
        h->io.delay_us(h->io.context, 10);
    } while (!expired(h, start, ms));
    return -ETIMEDOUT;
}
static int reset(sdhci_host_t *h, unsigned mask) {
    if (cancelled(h) || rd(h, SOFTWARE_RESET, 1) == 0xffU)
        return -ENODEV;
    wr(h, SOFTWARE_RESET, 1, mask);
    return wait_mask(h, SOFTWARE_RESET, 1, mask, 0, 100);
}
static int recover(sdhci_host_t *h, int error) {
    if (cancelled(h) || error == -ENODEV) {
        h->ready = 0;
        return -ENODEV;
    }
    int rc = reset(h, 6);
    if (rc)
        h->ready = 0;
    if (rc == -ENODEV)
        return rc;
    wr(h, INT_STATUS, 4, 0xffffffffU);
    return error;
}
static int wait_interrupt(sdhci_host_t *h, uint32_t wanted, unsigned ms) {
    uint64_t start = h->io.time_ms(h->io.context);
    do {
        if (cancelled(h))
            return -ENODEV;
        uint32_t status = rd(h, INT_STATUS, 4);
        if (status == 0xffffffffU)
            return -ENODEV;
        if (status & (ERROR | ERROR_BITS)) {
            /* Command and data timeout status have distinct error bits. */
            int error = status & ((1U << 16) | (1U << 20)) ? -ETIMEDOUT : -EIO;
            return recover(h, error);
        }
        if (status & wanted) {
            wr(h, INT_STATUS, 4, wanted);
            return 0;
        }
        h->io.delay_us(h->io.context, 10);
    } while (!expired(h, start, ms));
    return recover(h, -ETIMEDOUT);
}

int sdhci_clock_divider(uint32_t input, uint32_t requested, unsigned version,
                        uint16_t *encoded, uint32_t *actual) {
    if (!input || !requested || !encoded || !actual || version > 5)
        return -EINVAL;
    uint32_t divisor = 1;
    uint32_t limit = version >= 2 ? 2046 : 256;
    if (input > requested) {
        if (version >= 2) {
            divisor = (uint32_t)(((uint64_t)input + 2ULL * requested - 1) /
                                 (2ULL * requested)) *
                      2;
        } else {
            while (divisor < limit && (uint64_t)requested * divisor < input)
                divisor *= 2;
        }
    }
    if (divisor > limit || (uint64_t)requested * divisor < input)
        return -ERANGE;
    uint32_t field = divisor == 1 ? 0 : divisor / 2;
    *encoded = (uint16_t)(((field & 0xff) << 8) | ((field & 0x300) >> 2));
    *actual = input / divisor;
    return 0;
}
static int set_clock(sdhci_host_t *h, uint32_t hz) {
    uint16_t divider;
    uint32_t actual;
    int rc = sdhci_clock_divider(h->input_hz, hz, h->spec_version, &divider,
                                 &actual);
    if (rc)
        return rc;
    wr(h, CLOCK_CONTROL, 2, 0);
    wr(h, CLOCK_CONTROL, 2, divider | 1U);
    rc = wait_mask(h, CLOCK_CONTROL, 2, 2, 2, 100);
    if (!rc) {
        wr(h, CLOCK_CONTROL, 2, divider | 5U);
        h->actual_hz = actual;
    }
    return rc;
}
static int command(sdhci_host_t *h, unsigned index, uint32_t argument,
                   unsigned flags, uint32_t response[4]) {
    /* Busy responses also require DATA_INHIBIT clear before launch. */
    uint32_t mask = (flags & CMD_DATA) || (flags & 3) == RESP_BUSY ? 3 : 1;
    int rc = wait_mask(h, PRESENT_STATE, 4, mask, 0, 1000);
    if (rc)
        return recover(h, rc);
    wr(h, INT_STATUS, 4, 0xffffffffU);
    wr(h, ARGUMENT, 4, argument);
    wr(h, COMMAND, 2, (index << 8) | flags);
    rc = wait_interrupt(h, CMD_DONE, 1000);
    if (rc)
        return rc;
    if (response) {
        if ((flags & 3) == RESP_136) {
            /* SDHCI strips the CRC/end byte: reconstruct CSD/CID bit positions. */
            for (unsigned i = 0; i < 4; ++i) {
                unsigned offset = RESPONSE + (3 - i) * 4;
                response[i] = rd(h, offset, 4) << 8;
                if (i != 3)
                    response[i] |= rd(h, offset - 4, 4) >> 24;
            }
        } else {
            response[0] = rd(h, RESPONSE, 4);
        }
    }
    if ((flags & 3) == RESP_BUSY) {
        /* BL808 has NO_BUSY_IRQ: DAT0/inhibit is the completion contract. */
        rc = wait_mask(h, PRESENT_STATE, 4, 2, 0, 1000);
        if (rc)
            return recover(h, rc);
    }
    return 0;
}
static int r1(sdhci_host_t *h, unsigned index, uint32_t arg, unsigned flags) {
    uint32_t response[4];
    int rc = command(h, index, arg, flags | CMD_CRC | CMD_INDEX, response);
    return rc ? rc : response[0] & CARD_ERRORS ? -EIO : 0;
}

int sdhci_card_initialize(sdhci_host_t *h) {
    if (!h || !h->io.read || !h->io.write || !h->io.time_ms ||
        !h->io.delay_us || !h->input_hz || !h->max_hz)
        return -EINVAL;
    h->ready = 0;
    h->sectors = 0;
    h->write_protected = 0;
    if (cancelled(h))
        return -ENODEV;
    int rc = reset(h, 1);
    if (rc)
        return rc;
    wr(h, SIGNAL_ENABLE, 4,
       0); /* Poll status; never depend on an IRQ domain. */
    wr(h, INT_ENABLE, 4, 0x03ff007fU);
    wr(h, INT_STATUS, 4, 0xffffffffU);
    wr(h, HOST_CONTROL, 1, 0); /* one data line, default speed, PIO */
    wr(h, TIMEOUT_CONTROL, 1, 0x0e);
    wr(h, POWER_CONTROL, 1,
       0x0e); /* 3.3V selection before power-on (BL808 quirk) */
    wr(h, POWER_CONTROL, 1, 0x0f);
    h->io.delay_us(h->io.context, 10000);
    rc = set_clock(h, 400000);
    if (rc)
        return rc;
    /* At least 74 clocks with power stable, then optional vendor pad clocks. */
    h->io.delay_us(h->io.context, 1000);
    if (h->before_idle && (rc = h->before_idle(h->vendor_context)))
        return rc;
    if ((rc = command(h, 0, 0, 0, NULL)))
        return rc;
    uint32_t response[4];
    rc = command(h, 8, 0x1aa, RESP_48 | CMD_CRC | CMD_INDEX, response);
    int version2 = !rc;
    if (rc && rc != -ETIMEDOUT)
        return rc;
    if (version2 && (response[0] & 0xfff) != 0x1aa)
        return -EIO;
    uint64_t start = h->io.time_ms(h->io.context);
    uint32_t ocr;
    do {
        if ((rc = r1(h, 55, 0, RESP_48)))
            return rc;
        /* This host supplies 3.3V: advertise only its 3.2..3.4V window. */
        rc = command(h, 41, 0x00300000U | (version2 ? 1U << 30 : 0), RESP_48,
                     response);
        if (rc)
            return rc;
        ocr = response[0];
        if (!(ocr & 0x00300000U))
            return -EOPNOTSUPP;
        if (ocr & (1U << 31))
            break;
        h->io.delay_us(h->io.context, 10000);
    } while (!expired(h, start, 1500));
    if (!(ocr & (1U << 31)))
        return -ETIMEDOUT;
    h->block_addressed = version2 && (ocr & (1U << 30));
    if ((rc = command(h, 2, 0, RESP_136 | CMD_CRC, response)))
        return rc;
    if ((rc = command(h, 3, 0, RESP_48 | CMD_CRC | CMD_INDEX, response)))
        return rc;
    if (response[0] & 0xe000U)
        return -EIO;
    h->rca = (uint16_t)(response[0] >> 16);
    if (!h->rca)
        return -EIO;
    if ((rc = command(h, 9, (uint32_t)h->rca << 16, RESP_136 | CMD_CRC,
                      response)))
        return rc;
    unsigned structure = response[0] >> 30;
    if (structure == 1 && h->block_addressed) {
        uint32_t csize = ((response[1] & 0x3f) << 16) | (response[2] >> 16);
        h->sectors = ((uint64_t)csize + 1) * 1024;
    } else if (structure == 0 && !h->block_addressed) {
        unsigned read_bl_len = (response[1] >> 16) & 15;
        uint32_t csize = ((response[1] & 0x3ff) << 2) | (response[2] >> 30);
        unsigned mult = (response[2] >> 15) & 7;
        if (read_bl_len < 9 || read_bl_len > 11)
            return -EOPNOTSUPP;
        h->sectors = ((uint64_t)csize + 1) << (mult + 2 + read_bl_len - 9);
    } else {
        return -EOPNOTSUPP;
    }
    /* CSD permanent and temporary card write protection are bits 13/12. */
    h->write_protected = !!(response[3] & (3U << 12));
    if ((rc = r1(h, 7, (uint32_t)h->rca << 16, RESP_BUSY)))
        return rc;
    if (!h->block_addressed && (rc = r1(h, 16, 512, RESP_48)))
        return rc;
    rc = set_clock(h, h->max_hz < 25000000 ? h->max_hz : 25000000);
    if (!rc)
        h->ready = 1;
    return rc;
}

static int transfer_sector(sdhci_host_t *h, uint64_t lba, unsigned char *buffer,
                           int write) {
    if (lba > (h->block_addressed ? 0xffffffffULL : 0xffffffffULL / 512))
        return -ERANGE;
    int rc = wait_mask(h, PRESENT_STATE, 4, 3, 0, 1000);
    if (rc)
        return recover(h, rc);
    wr(h, BLOCK_SIZE, 2, 512);
    wr(h, BLOCK_COUNT, 2, 1);
    wr(h, TRANSFER_MODE, 2, write ? 0 : 1U << 4);
    uint32_t response[4];
    rc = command(h, write ? 24 : 17,
                 (uint32_t)(h->block_addressed ? lba : lba * 512),
                 RESP_48 | CMD_CRC | CMD_INDEX | CMD_DATA, response);
    if (rc)
        return rc;
    if (response[0] & CARD_ERRORS)
        return recover(h, -EIO);
    rc = wait_interrupt(h, write ? WRITE_READY : READ_READY, 1000);
    if (rc)
        return rc;
    /* Byte packing permits unaligned filesystem buffers on every architecture. */
    for (unsigned offset = 0; offset < 512; offset += 4) {
        if (cancelled(h))
            return -ENODEV;
        if (write) {
            uint32_t value = buffer[offset] |
                             (uint32_t)buffer[offset + 1] << 8 |
                             (uint32_t)buffer[offset + 2] << 16 |
                             (uint32_t)buffer[offset + 3] << 24;
            wr(h, BUFFER, 4, value);
        } else {
            uint32_t value = rd(h, BUFFER, 4);
            for (unsigned i = 0; i < 4; ++i)
                buffer[offset + i] = (unsigned char)(value >> (i * 8));
        }
    }
    rc = wait_interrupt(h, DATA_DONE, 3000);
    if (!rc && write) {
        /* CMD24 may finish while the card is programming. Poll R1 until TRAN. */
        uint64_t start = h->io.time_ms(h->io.context);
        do {
            rc = command(h, 13, (uint32_t)h->rca << 16,
                         RESP_48 | CMD_CRC | CMD_INDEX, response);
            if (rc)
                return rc;
            if (response[0] & CARD_ERRORS)
                return -EIO;
            if ((response[0] & ((15U << 9) | (1U << 8))) ==
                ((4U << 9) | (1U << 8)))
                return 0;
            h->io.delay_us(h->io.context, 1000);
        } while (!expired(h, start, 3000));
        return recover(h, -ETIMEDOUT);
    }
    return rc;
}

int sdhci_card_transfer(sdhci_host_t *h, uint64_t lba, void *buffer,
                        size_t count, int write) {
    if (!h || !buffer || !count || (write != 0 && write != 1))
        return -EINVAL;
    if (!h->ready || cancelled(h))
        return -ENODEV;
    if (lba >= h->sectors || count > h->sectors - lba ||
        count > (size_t)-1 / 512)
        return -ERANGE;
    if (write && h->write_protected)
        return -EROFS;
    for (size_t i = 0; i < count; ++i) {
        int rc = transfer_sector(h, lba + i, (unsigned char *)buffer + i * 512,
                                 write);
        if (rc) {
            /* Command/data-line reset cannot prove the card left DATA/RCV or
             * programming state. A new binding must enumerate it again. */
            h->ready = 0;
            return rc;
        }
    }
    return 0;
}
int sdhci_card_stop(sdhci_host_t *h) {
    if (!h || !h->io.read || !h->io.write || !h->io.delay_us)
        return -EINVAL;
    __atomic_store_n(&h->closing, 1, __ATOMIC_RELEASE);
    h->ready = 0;
    if (rd(h, SOFTWARE_RESET, 1) == 0xffU)
        return -ENODEV;
    wr(h, SIGNAL_ENABLE, 4, 0);
    wr(h, INT_ENABLE, 4, 0);
    /* This raw, bounded reset deliberately ignores the cancellation flag:
     * normal PIO has exited, but hardware acknowledgement is still required.
     * Do not depend on timer IRQ progress during shutdown. */
    wr(h, SOFTWARE_RESET, 1, 1);
    for (unsigned attempt = 0; ; attempt++) {
        uint32_t value = rd(h, SOFTWARE_RESET, 1);
        if (value == 0xffU)
            return -ENODEV;
        if (!(value & 7U))
            break;
        if (attempt == 9999)
            return -ETIMEDOUT;
        h->io.delay_us(h->io.context, 10);
    }
    wr(h, SIGNAL_ENABLE, 4, 0);
    wr(h, INT_ENABLE, 4, 0);
    wr(h, CLOCK_CONTROL, 2, 0);
    uint32_t signals = rd(h, SIGNAL_ENABLE, 4);
    uint32_t interrupts = rd(h, INT_ENABLE, 4);
    uint32_t clock = rd(h, CLOCK_CONTROL, 2);
    if (signals == 0xffffffffU || interrupts == 0xffffffffU || clock == 0xffffU)
        return -ENODEV;
    if (signals || interrupts || (clock & 5U))
        return -ETIMEDOUT;
    h->actual_hz = 0;
    return 0;
}
