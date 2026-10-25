/* Synopsys DesignWare Mobile Storage Host Controller, PIO SD cards.
 *
 * Controller register/protocol facts were checked against Linux v6.12 dw_mmc
 * and U-Boot v2025.10 dw_mmc. This is an independent implementation. Platform
 * clock/reset/pin routing belongs to arch; all resources here are physical and
 * are mapped through the driver core. Each binding owns its host and IRQ.
 */
#include "drivers/block/dw_sdio.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/bootargs.h"
#include "core/cpu.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "proc/proc.h"

#define SDIO_CTRL 0x00
#define SDIO_PWREN 0x04
#define SDIO_CLKDIV 0x08
#define SDIO_CLKSRC 0x0c
#define SDIO_CLKENA 0x10
#define SDIO_TMOUT 0x14
#define SDIO_CTYPE 0x18
#define SDIO_BLKSIZ 0x1c
#define SDIO_BYTCNT 0x20
#define SDIO_INTMASK 0x24
#define SDIO_CMDARG 0x28
#define SDIO_CMD 0x2c
#define SDIO_RESP0 0x30
#define SDIO_RESP1 0x34
#define SDIO_RESP2 0x38
#define SDIO_RESP3 0x3c
#define SDIO_RINTSTS 0x44
#define SDIO_STATUS 0x48
#define SDIO_FIFOTH 0x4c
#define SDIO_VERID 0x6c
#define SDIO_HCON 0x70

#define CTRL_RESET 1U
#define CTRL_FIFO_RESET 2U
#define CTRL_DMA_RESET 4U
#define CTRL_INT_ENABLE (1U << 4)
#define CMD_START (1U << 31)
#define CMD_USE_HOLD_REG (1U << 29)
#define CMD_UPDATE_CLK (1U << 21)
#define CMD_SEND_INIT (1U << 15)
#define CMD_STOP_ABORT (1U << 14)
#define CMD_WAIT_PRVDATA (1U << 13)
#define CMD_WRITE (1U << 10)
#define CMD_DATA_EXPECTED (1U << 9)
#define CMD_CHECK_RESP_CRC (1U << 8)
#define CMD_RESP_LENGTH (1U << 7)
#define CMD_RESP_EXPECT (1U << 6)
#define INT_EBE (1U << 15)
#define INT_SBE (1U << 13)
#define INT_HLE (1U << 12)
#define INT_FRUN (1U << 11)
#define INT_HTO (1U << 10)
#define INT_DRTO (1U << 9)
#define INT_RTO (1U << 8)
#define INT_DCRC (1U << 7)
#define INT_RCRC (1U << 6)
#define INT_RXDR (1U << 5)
#define INT_TXDR (1U << 4)
#define INT_DTO (1U << 3)
#define INT_CD (1U << 2)
#define INT_RE (1U << 1)
#define SDIO_INT_UNMASK (INT_CD | INT_RXDR | INT_TXDR | INT_DTO)
#define SDIO_INT_ALL 0xffffffffU
#define DATA_ERRORS                                                            \
    (INT_EBE | INT_SBE | INT_HLE | INT_FRUN | INT_HTO | INT_DRTO | INT_DCRC)
#define STATUS_DATA_BUSY (1U << 9)
#define STATUS_FIFO_EMPTY (1U << 2)
#define STATUS_FIFO_FULL (1U << 3)
#define SDIO_CMD_TIMEOUT_MS 1000U
#define SDIO_IRQ_PRE_POLL_US 200U
#define SDIO_PARK_CHUNK_MS 5U
/* R1 errors excluding CARD_IS_LOCKED: a locked card fails its data command. */
#define SD_R1_ERRORS 0xfff98008U
#define SD_R1_READY (1U << 8)
#define SD_R1_STATE(v) (((v) >> 9) & 15U)

typedef struct {
    uintptr_t base;
    uint32_t rca, input_hz, max_hz, fifo_offset;
    uint64_t sectors;
    int block_addressed, write_protected, ready, stopped;
    mutex_t lock;
    int irq, irq_registered, poll_only;
    uint32_t irq_status;
    spinlock_t irq_lock;
    wait_queue_t waiters;
} sdio_priv_t;

static inline uint32_t sdio_reg_read(uintptr_t base, uint32_t off) {
    return readl((const volatile void *)(base + off));
}
static inline void sdio_reg_write(uintptr_t base, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)(base + off));
}
static uint64_t sdio_ms_to_ticks(uint32_t ms) {
    uint64_t ticks = clock_ticks_per_sec() * ms / 1000;
    return ticks ? ticks : 1;
}
static uint64_t sdio_deadline(uint32_t ms) {
    return timer_get_ticks() + sdio_ms_to_ticks(ms);
}
/* ------------------------------------------------------------------ *
 * IRQ status latch
 *
 * The handler and the polling loop both feed this, so a completion observed by
 * one is visible to the other and neither can wait on a bit the other already
 * consumed from the hardware.
 * ------------------------------------------------------------------ */

static void sdio_latch_irq(sdio_priv_t *p, uint32_t bits) {
    uint64_t flags = spin_lock_irqsave(&p->irq_lock);
    p->irq_status |= bits;
    spin_unlock_irqrestore(&p->irq_lock, flags);
}

static uint32_t sdio_latched_irq(sdio_priv_t *p) {
    uint64_t flags = spin_lock_irqsave(&p->irq_lock);
    uint32_t seen = p->irq_status;
    spin_unlock_irqrestore(&p->irq_lock, flags);
    return seen;
}

static void sdio_clear_latched(sdio_priv_t *p, uint32_t bits) {
    uint64_t flags = spin_lock_irqsave(&p->irq_lock);
    p->irq_status &= ~bits;
    spin_unlock_irqrestore(&p->irq_lock, flags);
}

/* ------------------------------------------------------------------ *
 * Bounded, optionally interrupt-driven wait
 * ------------------------------------------------------------------ */

/*
 * Park is only legal with a task to park.  probe runs from kernel_main in the
 * embedded profile and from an init kthread in the generic one, and the card
 * bring-up must not sleep in either, so the condition is checked per wait
 * rather than once at probe.
 */
static int sdio_can_park(const sdio_priv_t *p) {
    return p->irq_registered && !p->poll_only && proc_current() != NULL;
}

static void sdio_park_once(sdio_priv_t *p, uint64_t deadline) {
    uint64_t chunk = timer_get_ticks() + sdio_ms_to_ticks(SDIO_PARK_CHUNK_MS);
    if (chunk > deadline)
        chunk = deadline;

    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, chunk);
    wait_queue_entry_t entry = {0};
    wait_queue_link(&p->waiters, &entry, token, 0);
    /* A completion racing queue linkage is observed on the next register
     * check. The bounded chunk also covers a wake that preceded linkage. */
    (void)proc_park_commit(token);
    wait_queue_unlink(&p->waiters, &entry);
    proc_park_finish(token);
}

/*
 * Wait for a controller condition.
 *
 * @mask    SDIO_RINTSTS bits whose latching (by the handler or by this loop's
 *          own read) also shows it;
 * @error   bits that mean the controller failed rather than merely has not
 *          finished yet.
 *
 * Returns 0 for the event, -EIO for CRC/controller failures and -ETIMEDOUT
 * for hardware timeout status or the deadline. Raw status is sampled even
 * with an IRQ, so a missing interrupt cannot hide a completed command.
 */
static int sdio_wait_cond(sdio_priv_t *p, uintptr_t base, uint32_t mask,
                          uint32_t error, uint64_t deadline) {
    uint64_t pre_poll_until =
        timer_get_ticks() +
        (clock_ticks_per_sec() * SDIO_IRQ_PRE_POLL_US) / 1000000;

    for (;;) {
        uint32_t sts = sdio_reg_read(base, SDIO_RINTSTS);
        sdio_latch_irq(p, sts);

        uint32_t seen = sdio_latched_irq(p);
        if (error && (seen & error)) {
            sdio_reg_write(base, SDIO_RINTSTS, seen & error);
            sdio_clear_latched(p, seen & error);
            return (seen & (INT_RTO | INT_DRTO)) ? -ETIMEDOUT : -EIO;
        }
        if (seen & mask) {
            sdio_clear_latched(p, mask);
            return 0;
        }
        /* remove() sets stopped before it takes the IRQ away; a waiter that has
         * not observed the teardown yet must not park on a line that is about
         * to stop delivering.  This is deliberately not `ready`: the card
         * bring-up waits on the same path with ready still 0. */
        if (p->stopped)
            return -ENODEV;

        uint64_t now = timer_get_ticks();
        if (now >= deadline)
            return -ETIMEDOUT;
        if (!sdio_can_park(p) || now < pre_poll_until) {
            cpu_relax();
            __asm__ volatile("" ::: "memory");
            continue;
        }
        sdio_park_once(p, deadline);
    }
}

/* Hardware consumes a command by clearing START. Clock-update commands never
 * raise CMD_DONE, so waiting for that interrupt would always time out. */
static int sdio_wait_clear(sdio_priv_t *p, uint32_t reg, uint32_t mask) {
    uint64_t until = sdio_deadline(SDIO_CMD_TIMEOUT_MS);
    while (sdio_reg_read(p->base, reg) & mask) {
        if (p->stopped)
            return -ENODEV;
        if (timer_get_ticks() >= until)
            return -ETIMEDOUT;
        cpu_relax();
    }
    return 0;
}
static int sdio_reset(sdio_priv_t *p, uint32_t bits) {
    sdio_reg_write(p->base, SDIO_CTRL,
                   sdio_reg_read(p->base, SDIO_CTRL) | bits);
    return sdio_wait_clear(p, SDIO_CTRL, bits);
}
static int sdio_update_clock(sdio_priv_t *p) {
    sdio_reg_write(p->base, SDIO_CMD,
                   CMD_START | CMD_UPDATE_CLK | CMD_WAIT_PRVDATA);
    int rc = sdio_wait_clear(p, SDIO_CMD, CMD_START);
    return rc                                                 ? rc
           : (sdio_reg_read(p->base, SDIO_RINTSTS) & INT_HLE) ? -EIO
                                                              : 0;
}
static int sdio_set_clock(sdio_priv_t *p, uint32_t hz) {
    if (!hz || !p->input_hz)
        return -EINVAL;
    uint64_t div = p->input_hz <= hz
                       ? 0
                       : ((uint64_t)p->input_hz + 2ULL * hz - 1) / (2ULL * hz);
    if (div > 255)
        return -ERANGE;
    sdio_reg_write(p->base, SDIO_RINTSTS, SDIO_INT_ALL);
    sdio_reg_write(p->base, SDIO_CLKENA, 0);
    int rc = sdio_update_clock(p);
    if (rc)
        return rc;
    sdio_reg_write(p->base, SDIO_CLKSRC, 0);
    sdio_reg_write(p->base, SDIO_CLKDIV, (uint32_t)div);
    rc = sdio_update_clock(p);
    if (rc)
        return rc;
    sdio_reg_write(p->base, SDIO_CLKENA, 1);
    return sdio_update_clock(p);
}

/* Response arrays use the controller's natural little-word order: RESP0
 * contains CSD[31:0], RESP3 contains CSD[127:96]. */
static void sdio_response(sdio_priv_t *p, uint32_t response[4]) {
    for (unsigned i = 0; i < 4; i++)
        response[i] = sdio_reg_read(p->base, SDIO_RESP0 + 4 * i);
}
static int sdio_command(sdio_priv_t *p, unsigned index, uint32_t arg,
                        uint32_t flags, uint32_t *response) {
    int rc = sdio_wait_clear(p, SDIO_CMD, CMD_START);
    if (rc)
        return rc;
    sdio_reg_write(p->base, SDIO_RINTSTS, SDIO_INT_ALL);
    sdio_clear_latched(p, SDIO_INT_ALL);
    sdio_reg_write(p->base, SDIO_CMDARG, arg);
    uint32_t cmd = CMD_START | CMD_USE_HOLD_REG | index | flags;
    if (!index)
        cmd |= CMD_SEND_INIT | CMD_STOP_ABORT;
    sdio_reg_write(p->base, SDIO_CMD, cmd);
    uint32_t errors = INT_RTO | INT_RE | INT_HLE;
    if (flags & CMD_CHECK_RESP_CRC)
        errors |= INT_RCRC;
    rc = sdio_wait_cond(p, p->base, INT_CD, errors,
                        sdio_deadline(SDIO_CMD_TIMEOUT_MS));
    if (!rc && response)
        *response = sdio_reg_read(p->base, SDIO_RESP0);
    return rc;
}
#define RESP_R1 (CMD_RESP_EXPECT | CMD_CHECK_RESP_CRC)
static int sdio_r1(sdio_priv_t *p, unsigned cmd, uint32_t arg, uint32_t flags) {
    uint32_t response = 0;
    int rc = sdio_command(p, cmd, arg, flags | RESP_R1, &response);
    return rc ? rc : (response & SD_R1_ERRORS) ? -EIO : 0;
}
static int sdio_app(sdio_priv_t *p, unsigned cmd, uint32_t arg, uint32_t flags,
                    uint32_t *response) {
    uint32_t status = 0;
    int rc = sdio_command(p, 55, p->rca << 16, RESP_R1, &status);
    if (rc)
        return rc;
    if ((status & SD_R1_ERRORS) || !(status & (1U << 5)))
        return -EOPNOTSUPP;
    return sdio_command(p, cmd, arg, flags, response);
}
static uint32_t sdio_csd_bits(const uint32_t csd[4], unsigned low,
                              unsigned count) {
    uint64_t value = csd[low / 32];
    if (low / 32 < 3)
        value |= (uint64_t)csd[low / 32 + 1] << 32;
    return (uint32_t)(value >> (low % 32)) & ((1U << count) - 1);
}
static int sdio_capacity_csd(sdio_priv_t *p, const uint32_t csd[4]) {
    unsigned version = sdio_csd_bits(csd, 126, 2);
    if (version == 1 && p->block_addressed) {
        p->sectors = ((uint64_t)sdio_csd_bits(csd, 48, 22) + 1) * 1024;
    } else if (version == 0 && !p->block_addressed) {
        unsigned block_shift = sdio_csd_bits(csd, 80, 4);
        unsigned multiplier = sdio_csd_bits(csd, 47, 3);
        if (block_shift < 9 || block_shift > 11)
            return -EINVAL;
        p->sectors = ((uint64_t)sdio_csd_bits(csd, 62, 12) + 1)
                     << (multiplier + 2 + block_shift - 9);
    } else
        return -EOPNOTSUPP;
    /* CSD permanent/temporary write protection bits are in RESP0[13:12]. */
    p->write_protected = !!(csd[0] & (3U << 12));
    return p->sectors ? 0 : -EINVAL;
}
static int sdio_wait_card(sdio_priv_t *p) {
    uint64_t until = sdio_deadline(SDIO_CMD_TIMEOUT_MS);
    do {
        uint32_t status;
        int rc = sdio_command(p, 13, p->rca << 16, RESP_R1, &status);
        if (rc)
            return rc;
        if (status & SD_R1_ERRORS)
            return -EIO;
        if ((status & SD_R1_READY) && SD_R1_STATE(status) == 4)
            return 0;
        if (timer_get_ticks() >= until)
            return -ETIMEDOUT;
        mdelay(1);
    } while (!p->stopped);
    return -ENODEV;
}
static int sdio_card_init(sdio_priv_t *p, unsigned fifo_depth) {
    int rc = sdio_reset(p, CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
    if (rc)
        return rc;
    /* This implementation uses 32-bit FIFO accesses only; do not silently
     * corrupt data on synthesized 16- or 64-bit FIFO interfaces. */
    if (((sdio_reg_read(p->base, SDIO_HCON) >> 7) & 7) != 1)
        return -EOPNOTSUPP;
    unsigned version = sdio_reg_read(p->base, SDIO_VERID) & 0xffff;
    p->fifo_offset = version < 0x240a ? 0x100 : 0x200;
    if (!fifo_depth)
        fifo_depth = ((sdio_reg_read(p->base, SDIO_FIFOTH) >> 16) & 0xfff) + 1;
    if (fifo_depth < 2 || fifo_depth > 4096)
        return -EINVAL;
    unsigned half = fifo_depth / 2;
    sdio_reg_write(p->base, SDIO_INTMASK, 0);
    sdio_reg_write(p->base, SDIO_CTRL,
                   0); /* PIO; DMA and controller IRQ disabled. */
    sdio_reg_write(p->base, SDIO_TMOUT, 0xffffffff);
    sdio_reg_write(p->base, SDIO_FIFOTH,
                   (2U << 28) | ((half - 1) << 16) | half);
    sdio_reg_write(p->base, SDIO_PWREN, 1);
    sdio_reg_write(p->base, SDIO_CTYPE,
                   0); /* SD default-speed, one data line. */
    rc = sdio_set_clock(p, 400000);
    if (rc)
        return rc;
    mdelay(1); /* Power/clock stable, then CMD0 generates initial 80 clocks. */
    rc = sdio_command(p, 0, 0, 0, NULL);
    if (rc)
        return rc;
    uint32_t response = 0;
    rc = sdio_command(p, 8, 0x1aa, RESP_R1, &response);
    /* Legacy SDSC cards time out CMD8. Other failures are not an SD v1 card. */
    int v2 = !rc;
    if (rc && rc != -ETIMEDOUT &&
        !(sdio_reg_read(p->base, SDIO_RINTSTS) & INT_RTO))
        return rc;
    if (v2 && (response & 0xfff) != 0x1aa)
        return -EIO;
    uint64_t until = sdio_deadline(1000);
    do {
        rc = sdio_app(p, 41, 0x00300000U | (v2 ? (1U << 30) : 0),
                      CMD_RESP_EXPECT, &response);
        if (rc)
            return rc;
        if (response & (1U << 31))
            break;
        if (timer_get_ticks() >= until)
            return -ETIMEDOUT;
        mdelay(10);
    } while (1);
    if (!(response & 0x00300000U))
        return -EIO;
    p->block_addressed = v2 && !!(response & (1U << 30));
    rc = sdio_command(p, 2, 0, RESP_R1 | CMD_RESP_LENGTH, NULL);
    if (rc)
        return rc;
    rc = sdio_command(p, 3, 0, RESP_R1, &response);
    if (rc)
        return rc;
    if (response & 0xe000U)
        return -EIO; /* R6 CRC/illegal/general error */
    p->rca = response >> 16;
    if (!p->rca)
        return -EIO;
    rc = sdio_command(p, 9, p->rca << 16, RESP_R1 | CMD_RESP_LENGTH, NULL);
    if (rc)
        return rc;
    uint32_t csd[4];
    sdio_response(p, csd);
    rc = sdio_capacity_csd(p, csd);
    if (rc)
        return rc;
    rc = sdio_r1(p, 7, p->rca << 16, 0);
    if (rc)
        return rc;
    rc = sdio_wait_card(p);
    if (rc)
        return rc;
    if (!p->block_addressed && (rc = sdio_r1(p, 16, DW_SDIO_SECTOR_SIZE, 0)))
        return rc;
    /* No CMD6 high-speed switch or voltage switch: never exceed SD default. */
    rc = sdio_set_clock(p, p->max_hz < 25000000 ? p->max_hz : 25000000);
    if (rc)
        return rc;
    p->ready = 1;
    kinfo("[DW-SDIO] Card ready, RCA=0x%x, sectors=%lu, SD %s, 1-bit PIO\n",
          p->rca, (unsigned long)p->sectors,
          p->block_addressed ? "HC/XC" : "SC");
    return 0;
}

static int sdio_irq_handler(int irq, void *data) {
    (void)irq;
    sdio_priv_t *p = data;
    uint32_t bits = sdio_reg_read(p->base, SDIO_RINTSTS);
    if (!bits)
        return 0;
    sdio_latch_irq(p, bits);
    sdio_reg_write(p->base, SDIO_RINTSTS, bits);
    proc_wake_q_t queue;
    proc_wake_q_init(&queue);
    wait_queue_collect_all(&p->waiters, 0, PROC_WAKE_EVENT, &queue, NULL);
    proc_wake_q_flush(&queue);
    return 0;
}
static void sdio_stop(void *data) {
    sdio_priv_t *p = data;
    p->stopped = 1;
    p->ready = 0;
    sdio_reg_write(p->base, SDIO_INTMASK, 0);
    sdio_reg_write(p->base, SDIO_CTRL,
                   sdio_reg_read(p->base, SDIO_CTRL) & ~CTRL_INT_ENABLE);
    if (p->irq_registered)
        free_irq((uint32_t)p->irq, p);
    p->irq_registered = 0;
    proc_wake_q_t queue;
    proc_wake_q_init(&queue);
    wait_queue_collect_all(&p->waiters, 0, PROC_WAKE_EVENT, &queue, NULL);
    proc_wake_q_flush(&queue);
}
static void sdio_release(void *data) {
    sdio_stop(data);
    kfree(data);
}

/* FIFO flags are sampled on every iteration. Latched RXDR/TXDR/DTO events
 * cannot imply a word exists or there is room for a word. Unaligned user
 * buffers are copied bytewise into a register word. */
static int sdio_fifo_word(sdio_priv_t *p, uint8_t *bytes, int write,
                          uint64_t until) {
    for (;;) {
        uint32_t bits = sdio_reg_read(p->base, SDIO_RINTSTS);
        sdio_latch_irq(p, bits);
        if (sdio_latched_irq(p) & DATA_ERRORS)
            return -EIO;
        uint32_t status = sdio_reg_read(p->base, SDIO_STATUS);
        if (!(status & (write ? STATUS_FIFO_FULL : STATUS_FIFO_EMPTY)))
            break;
        if (p->stopped)
            return -ENODEV;
        if (timer_get_ticks() >= until)
            return -ETIMEDOUT;
        if (sdio_can_park(p))
            sdio_park_once(p, until);
        else
            cpu_relax();
    }
    if (write) {
        uint32_t value;
        memcpy(&value, bytes, sizeof(value));
        sdio_reg_write(p->base, p->fifo_offset, value);
    } else {
        uint32_t value = sdio_reg_read(p->base, p->fifo_offset);
        memcpy(bytes, &value, sizeof(value));
    }
    return 0;
}
static int sdio_transfer(sdio_priv_t *p, uint64_t lba, uint8_t *bytes,
                         int write) {
    int rc = sdio_wait_clear(p, SDIO_STATUS, STATUS_DATA_BUSY);
    if (rc)
        return rc;
    rc = sdio_reset(p, CTRL_FIFO_RESET);
    if (rc)
        return rc;
    sdio_reg_write(p->base, SDIO_BLKSIZ, DW_SDIO_SECTOR_SIZE);
    sdio_reg_write(p->base, SDIO_BYTCNT, DW_SDIO_SECTOR_SIZE);
    uint64_t arg = p->block_addressed ? lba : lba * DW_SDIO_SECTOR_SIZE;
    if (arg > 0xffffffffU)
        return -ERANGE;
    rc =
        sdio_r1(p, write ? 24 : 17, (uint32_t)arg,
                CMD_DATA_EXPECTED | CMD_WAIT_PRVDATA | (write ? CMD_WRITE : 0));
    if (rc)
        return rc;
    uint64_t until = sdio_deadline(1000);
    for (unsigned i = 0; i < DW_SDIO_SECTOR_SIZE; i += 4) {
        rc = sdio_fifo_word(p, bytes + i, write, until);
        if (rc)
            return rc;
    }
    rc = sdio_wait_cond(p, p->base, INT_DTO, DATA_ERRORS, until);
    if (rc)
        return rc;
    sdio_reg_write(p->base, SDIO_RINTSTS, INT_DTO);
    return write ? sdio_wait_card(p) : 0;
}
static int sdio_io(device_t *dev, uint64_t lba, void *buf, size_t count,
                   int write) {
    sdio_priv_t *p = dev->drv_priv;
    if (!p || !buf || !count)
        return -EINVAL;
    mutex_lock(&p->lock);
    int rc = 0;
    if (!p->ready || p->stopped)
        rc = -ENODEV;
    else if (write && p->write_protected)
        rc = -EROFS;
    else if (lba >= p->sectors || count > p->sectors - lba ||
             count > SIZE_MAX / 512)
        rc = -EINVAL;
    else
        for (size_t i = 0; i < count; i++) {
            rc = sdio_transfer(p, lba + i, (uint8_t *)buf + i * 512, write);
            if (rc)
                break;
        }
    if (rc && rc != -EINVAL && rc != -EROFS) {
        /* Do not issue further requests into an unknown card/controller data
         * state. Rebind explicitly re-enumerates the card after an I/O error. */
        p->ready = 0;
        sdio_reg_write(p->base, SDIO_INTMASK, 0);
    }
    mutex_unlock(&p->lock);
    return rc;
}
static int sdio_read(device_t *dev, uint64_t lba, void *buf, size_t count) {
    return sdio_io(dev, lba, buf, count, 0);
}
static int sdio_write(device_t *dev, uint64_t lba, const void *buf,
                      size_t count) {
    return sdio_io(dev, lba, (void *)buf, count, 1);
}
static uint64_t sdio_capacity(device_t *dev) {
    return ((sdio_priv_t *)dev->drv_priv)->sectors;
}
static uint32_t sdio_sector_size(device_t *dev) {
    (void)dev;
    return DW_SDIO_SECTOR_SIZE;
}
static const block_dev_ops_t sdio_ops = {
    .read = sdio_read,
    .write = sdio_write,
    .capacity = sdio_capacity,
    .sector_size = sdio_sector_size,
};
static int sdio_probe(device_t *dev) {
    resource_t *resource = device_get_resource(dev, RES_MMIO, 0);
    if (!resource || resource->end < resource->start ||
        resource->end - resource->start < 0x203)
        return -ENODEV;
    const dw_sdio_platform_data_t *config = dev->plat_data;
    uint32_t hz = config ? config->input_hz : 0;
    uint32_t max_hz = config ? config->max_hz : 25000000;
    uint32_t depth = config ? config->fifo_depth : 0;
    int property;
    if (!hz) {
        property =
            fw_node_read_u32(&dev->firmware_node, "clock-frequency", &hz);
        if (property && property != -ENOENT)
            return property;
    }
    if (!max_hz)
        max_hz = 25000000;
    property = fw_node_read_u32(&dev->firmware_node, "max-frequency", &max_hz);
    if (property && property != -ENOENT)
        return property;
    if (!depth) {
        property = fw_node_read_u32(&dev->firmware_node, "fifo-depth", &depth);
        if (property && property != -ENOENT)
            return property;
    }
    /* A generic host cannot infer its CIU clock from a compatible string. */
    if (!hz || !max_hz)
        return -EINVAL;
    sdio_priv_t *p = kcalloc(1, sizeof(*p));
    if (!p)
        return -ENOMEM;
    p->base = (uintptr_t)device_map_mmio(dev, 0);
    if (!p->base) {
        kfree(p);
        return -EIO;
    }
    p->input_hz = hz;
    p->max_hz = max_hz;
    p->irq = platform_device_irq(dev);
    if (p->irq < 0 && p->irq != -ENODEV) {
        int error = p->irq;
        kfree(p);
        return error;
    }
    mutex_init(&p->lock);
    spin_init(&p->irq_lock);
    wait_queue_init(&p->waiters);
    int rc = device_add_resource_action(dev, sdio_release, p);
    if (rc) {
        kfree(p);
        return rc;
    }
    rc = sdio_card_init(p, depth);
    if (rc) {
        kinfo("[DW-SDIO] '%s' card initialization failed (%d)\n", dev->name,
              rc);
        return rc;
    }
    int read_only = fw_node_has_property(&dev->firmware_node, "read-only");
    if (read_only < 0)
        return read_only;
    if (read_only)
        p->write_protected = 1;
#ifdef CONFIG_STORAGE_READ_ONLY
    p->write_protected = 1;
#endif
    p->poll_only = dw_sdio_poll_forced(bootargs_get());
    if (dw_sdio_pick_irq_mode(p->irq, p->poll_only) &&
        !request_irq((uint32_t)p->irq, sdio_irq_handler, 0, p)) {
        p->irq_registered = 1;
        sdio_reg_write(p->base, SDIO_CTRL, CTRL_INT_ENABLE);
        sdio_reg_write(p->base, SDIO_INTMASK,
                       SDIO_INT_UNMASK | DATA_ERRORS | INT_RTO | INT_RCRC |
                           INT_RE);
    }
    dev->drv_priv = p;
    kinfo("[DW-SDIO] '%s' %s completion\n", dev->name,
          p->irq_registered ? "IRQ" : "polling");
    return 0;
}
static int sdio_remove(device_t *dev) {
    if (dev->drv_priv)
        sdio_stop(dev->drv_priv);
    dev->drv_priv = NULL;
    return 0;
}
static const firmware_device_id_t sdio_ids[] = {
    {.id = "starfive,jh7110-mmc"},
    {.id = "rockchip,rk3568-dw-mshc"},
    {.id = "rockchip,rk3288-dw-mshc"},
    {0},
};
static driver_t dw_sdio_driver = {
    .name = "dw-sdio",
    .bus = &platform_bus,
    .firmware_ids = sdio_ids,
    .probe = sdio_probe,
    .remove = sdio_remove,
    .class_ops = &sdio_ops,
    .class_type = DEV_CLASS_BLOCK,
    .irq_optional = 1,
};
DRIVER_REGISTER(dw_sdio_driver);
