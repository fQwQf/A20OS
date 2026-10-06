/*
 * A20OS DW SDIO host driver.
 *
 * LOCK_ORDER: one global sdio_priv_t instance (g_sdio).  `lock` is a sleepable
 * mutex, not the spinlock this driver used to hold: the completion path parks
 * on `waiters` when a platform IRQ resource exists, and parking under a
 * spinlock is forbidden (docs/drivers/guide/lock-order.md).  `irq_lock` is a
 * spinlock that protects only `irq_status`; the IRQ handler takes it, never
 * `lock`, so a handler can run while a transfer holds the mutex and parks.  The
 * two are never held together -- see the DW SDIO section of lock-order.md,
 * which now records this order.
 *
 * Completion has two shapes and one body.  Without a claimed IRQ line -- the
 * board published no RES_IRQ, a20.dw-sdio.poll=1 asked for polling,
 * request_irq() refused the line, or a timeout already downgraded the instance
 * -- every wait is the bounded spin this driver always did, so a device with no
 * IRQ resource behaves exactly as before.  With a line, the wait spins briefly
 * and then parks until the handler wakes it; every park is bounded by the same
 * deadline as the spin, so a card or a route that never raises the line fails
 * with a timeout instead of hanging the caller, and the first timeout
 * downgrades the instance to polling for good.
 *
 * Card enumeration stays polled even when an IRQ is claimed: probe runs from
 * kernel_main (embedded) or an init kthread (generic), and neither is a context
 * that may park for the whole card bring-up.
 */
#include "drivers/block/dw_sdio.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/bootargs.h"
#include "core/cpu.h"
#include "core/defs.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"

#define SDIO_CTRL     0x00
#define SDIO_PWREN    0x04
#define SDIO_CLKDIV   0x08
#define SDIO_CLKENA   0x10
#define SDIO_TMOUT    0x14
#define SDIO_CTYPE    0x18
#define SDIO_BLKSIZ   0x1C
#define SDIO_BYTCNT   0x20
#define SDIO_INTMASK  0x24
#define SDIO_CMDARG   0x28
#define SDIO_CMD      0x2C
#define SDIO_RESP0    0x30
#define SDIO_RESP1    0x34
#define SDIO_RESP2    0x38
#define SDIO_RESP3    0x3C
#define SDIO_RINTSTS  0x44
#define SDIO_STATUS   0x48
#define SDIO_FIFOTH   0x4C
#define SDIO_FIFO     0x200

#define CMD_START         (1U << 31)
#define CMD_USE_HOLD_REG  (1U << 29)
#define CMD_UPDATE_CLK    (1U << 21)
#define CMD_SEND_INIT     (1U << 15)
#define CMD_STOP_ABORT    (1U << 14)
#define CMD_WAIT_PRVDATA  (1U << 13)
#define CMD_SEND_AUTO_STOP (1U << 12)
#define CMD_STREAM_MODE   (1U << 11)
#define CMD_WRITE         (1U << 10)
#define CMD_DATA_EXPECTED (1U << 9)
#define CMD_CHECK_RESP_CRC (1U << 8)
#define CMD_RESP_LENGTH   (1U << 7)
#define CMD_RESP_EXPECT   (1U << 6)

#define INT_SDIO_INTR     (1U << 16)
#define INT_EBE           (1U << 15)
#define INT_ACD           (1U << 14)
#define INT_SBE           (1U << 13)
#define INT_HLE           (1U << 12)
#define INT_FRUN          (1U << 11)
#define INT_HTO           (1U << 10)
#define INT_DRTO          (1U << 9)
#define INT_RTO           (1U << 8)
#define INT_DCRC          (1U << 7)
#define INT_RCRC          (1U << 6)
#define INT_RXDR          (1U << 5)
#define INT_TXDR          (1U << 4)
#define INT_DTO           (1U << 3)
#define INT_CD            (1U << 2)
#define INT_RE            (1U << 1)

/* Interrupt sources unmasked once a line is claimed.  Command done and data
 * transfer over end a transfer; RXDR/TXDR are the FIFO watermarks that let the
 * 512-byte data phase advance without spinning on SDIO_STATUS. */
#define SDIO_INT_UNMASK (INT_CD | INT_RXDR | INT_TXDR | INT_DTO)
#define SDIO_INT_ALL    0xFFFFFFFFU

#define CMD_FLAG_NONE          0
#define CMD_FLAG_RESP_EXPECT   1
#define CMD_FLAG_RESP_LONG     2
#define CMD_FLAG_DATA_EXPECTED 4
#define CMD_FLAG_WRITE         8

#define FIFO_RX_WMARK_SHIFT 16
#define FIFO_TX_WMARK_SHIFT 0
#define FIFO_MSIZE_SHIFT    28

#define STATUS_DATA_BUSY (1U << 9)
#define STATUS_FIFO_EMPTY (1U << 2)
#define STATUS_FIFO_FULL  (1U << 3)

#define SD_OCR_MASK 0x40FF8000

/* Command timeout and per-sector data timeout, both in milliseconds.  Every
 * bounded wait derives its deadline from these, so no path can spin forever. */
#define SDIO_CMD_TIMEOUT_MS    1000U
#define SDIO_CLOCK_TIMEOUT_MS   100U
#define SDIO_SECTOR_TIMEOUT_MS 1000U

/* Spin this long before the first park.  Arming an interrupt and taking it
 * costs about as much as re-reading one register, and a 512-byte sector at the
 * card's rate finishes sooner than the setup does, so parking immediately would
 * usually cost more than it saved. */
#define SDIO_IRQ_PRE_POLL_US 200U

/* A park is bounded even so, so a wake that never comes turns into a re-check
 * rather than a stall. */
#define SDIO_PARK_CHUNK_MS 5U

typedef struct {
    uintptr_t  base;
    uint32_t   rca;
    int        ready;         /* card enumerated: transfers may run */
    int        stopped;       /* remove() has begun: nothing may park */
    uint64_t   sectors;
    mutex_t    lock;          /* sleepable: serialises one transfer at a time */
    /* IRQ completion channel.  irq is the line platform_device_irq() returned,
     * or -1 when the board published none.  irq_registered says request_irq()
     * accepted it.  poll_only is the sticky downgrade: set by a completion
     * timeout, after which no path parks again for the life of the instance. */
    int        irq;
    int        irq_registered;
    int        poll_only;
    uint32_t   irq_status;    /* SDIO_RINTSTS bits observed, latched */
    spinlock_t irq_lock;      /* protects irq_status only */
    wait_queue_t waiters;
} sdio_priv_t;

static sdio_priv_t g_sdio;

static inline uint32_t sdio_reg_read(uintptr_t base, uint32_t off) {
    return readl((volatile void *)(base + off));
}

static inline void sdio_reg_write(uintptr_t base, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)(base + off));
}

static uint64_t sdio_ms_to_ticks(uint32_t ms)
{
    uint64_t tmo = (clock_ticks_per_sec() * ms) / 1000;
    return tmo ? tmo : 1;
}

/* ------------------------------------------------------------------ *
 * IRQ status latch
 *
 * The handler and the polling loop both feed this, so a completion observed by
 * one is visible to the other and neither can wait on a bit the other already
 * consumed from the hardware.
 * ------------------------------------------------------------------ */

static void sdio_latch_irq(sdio_priv_t *p, uint32_t bits)
{
    uint64_t flags = spin_lock_irqsave(&p->irq_lock);
    p->irq_status |= bits;
    spin_unlock_irqrestore(&p->irq_lock, flags);
}

static uint32_t sdio_latched_irq(sdio_priv_t *p)
{
    uint64_t flags = spin_lock_irqsave(&p->irq_lock);
    uint32_t seen = p->irq_status;
    spin_unlock_irqrestore(&p->irq_lock, flags);
    return seen;
}

static void sdio_clear_latched(sdio_priv_t *p, uint32_t bits)
{
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
static int sdio_can_park(const sdio_priv_t *p)
{
    return p->irq_registered && !p->poll_only && proc_current() != NULL;
}

static void sdio_park_once(sdio_priv_t *p, uint64_t deadline)
{
    uint64_t chunk = timer_get_ticks() + sdio_ms_to_ticks(SDIO_PARK_CHUNK_MS);
    if (chunk > deadline)
        chunk = deadline;

    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, chunk);
    wait_queue_entry_t entry = {0};
    wait_queue_link(&p->waiters, &entry, token, 0);
    /* No re-check between link and commit: the caller re-reads the registers
     * and re-tests `ready` at the top of the loop, so a completion that raced
     * the link is seen immediately rather than slept through. */
    (void)proc_park_commit(token);
    wait_queue_unlink(&p->waiters, &entry);
    proc_park_finish(token);
}

/*
 * Wait for a controller condition.
 *
 * @ready   a direct register read already shows the condition;
 * @mask    SDIO_RINTSTS bits whose latching (by the handler or by this loop's
 *          own read) also shows it;
 * @error   bits that mean the controller failed rather than merely has not
 *          finished yet.
 *
 * Returns 0 when satisfied, -EIO on @error, -ETIMEDOUT on deadline.  The two
 * failures are distinguished because only a timeout means the interrupt
 * arrival assumption was wrong; a CRC or timeout bit is a property of the card
 * or the transfer, and must not cost the instance its interrupt.
 */
static int sdio_wait_cond(sdio_priv_t *p, uint32_t base, int ready,
                          uint32_t mask, uint32_t error, uint64_t deadline)
{
    uint64_t pre_poll_until = timer_get_ticks() +
                              (clock_ticks_per_sec() * SDIO_IRQ_PRE_POLL_US) / 1000000;

    for (;;) {
        uint32_t sts = sdio_reg_read(base, SDIO_RINTSTS);
        sdio_latch_irq(p, sts);

        uint32_t seen = sdio_latched_irq(p);
        if (error && (seen & error)) {
            sdio_reg_write(base, SDIO_RINTSTS, seen & error);
            sdio_clear_latched(p, seen & error);
            return -EIO;
        }
        if (ready || (seen & mask)) {
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

/* Command finished (INT_CD) or failed (RTO/RCRC/RE). */
static int sdio_wait_cmd(sdio_priv_t *p, uint32_t base, uint32_t timeout_ms)
{
    return sdio_wait_cond(p, base, 0, INT_CD,
                          INT_RTO | INT_RCRC | INT_RE,
                          timer_get_ticks() + sdio_ms_to_ticks(timeout_ms));
}

static int sdio_send_cmd_raw(sdio_priv_t *p, uint32_t idx, uint32_t arg,
                             uint32_t flags) {
    sdio_reg_write(p->base, SDIO_RINTSTS, SDIO_INT_ALL);
    sdio_clear_latched(p, SDIO_INT_ALL);
    sdio_reg_write(p->base, SDIO_CMDARG, arg);

    uint32_t cmd = (idx & 0x3F) | CMD_START | CMD_USE_HOLD_REG;
    if (flags & CMD_FLAG_RESP_EXPECT)  cmd |= CMD_RESP_EXPECT;
    if (flags & CMD_FLAG_RESP_LONG)    cmd |= CMD_RESP_LENGTH;
    if (flags & CMD_FLAG_DATA_EXPECTED) cmd |= CMD_DATA_EXPECTED;
    if (flags & CMD_FLAG_WRITE)        cmd |= CMD_WRITE;

    sdio_reg_write(p->base, SDIO_CMD, cmd);
    return sdio_wait_cmd(p, p->base, SDIO_CMD_TIMEOUT_MS);
}

static int sdio_update_clock(sdio_priv_t *p) {
    uint32_t cmd = CMD_START | CMD_USE_HOLD_REG | CMD_UPDATE_CLK |
                   CMD_WAIT_PRVDATA;
    sdio_reg_write(p->base, SDIO_CMD, cmd);
    return sdio_wait_cmd(p, p->base, SDIO_CLOCK_TIMEOUT_MS);
}

static int sdio_read_resp(uintptr_t base, uint32_t *resp, int long_resp) {
    resp[0] = sdio_reg_read(base, SDIO_RESP0);
    if (long_resp) {
        resp[1] = sdio_reg_read(base, SDIO_RESP1);
        resp[2] = sdio_reg_read(base, SDIO_RESP2);
        resp[3] = sdio_reg_read(base, SDIO_RESP3);
    }
    return 0;
}

static int sdio_reset(uintptr_t base) {
    sdio_reg_write(base, SDIO_CTRL, 1);
    while (sdio_reg_read(base, SDIO_CTRL) & 1);
    return 0;
}

static int sdio_init_clock(sdio_priv_t *p, uint32_t div) {
    sdio_reg_write(p->base, SDIO_CLKENA, 0);
    sdio_update_clock(p);
    sdio_reg_write(p->base, SDIO_CLKDIV, div);
    sdio_reg_write(p->base, SDIO_CLKENA, 1);
    return sdio_update_clock(p);
}

/* The previous data phase must have drained before the next BLKSIZ/BYTCNT
 * pair is written; a stale busy bit makes the new command a no-op. */
static int sdio_wait_data_idle(sdio_priv_t *p, uint32_t base, uint64_t deadline)
{
    while (sdio_reg_read(base, SDIO_STATUS) & STATUS_DATA_BUSY) {
        if (p->stopped)
            return -ENODEV;
        if (timer_get_ticks() >= deadline)
            return -ETIMEDOUT;
        if (!sdio_can_park(p)) {
            cpu_relax();
            continue;
        }
        sdio_park_once(p, deadline);
    }
    return 0;
}

int dw_sdio_init_dev(uintptr_t base) {
    g_sdio.base    = base;
    g_sdio.ready   = 0;
    g_sdio.stopped = 0;
    g_sdio.sectors = 0;
    g_sdio.irq     = -1;
    g_sdio.irq_registered = 0;
    g_sdio.poll_only = 0;
    g_sdio.irq_status  = 0;
    mutex_init(&g_sdio.lock);
    spin_init(&g_sdio.irq_lock);
    wait_queue_init(&g_sdio.waiters);

    sdio_reset(base);

    sdio_reg_write(base, SDIO_TMOUT, 0xFFFFFFFF);
    sdio_reg_write(base, SDIO_FIFOTH,
                   (2U << FIFO_MSIZE_SHIFT) |
                   (511 << FIFO_RX_WMARK_SHIFT) |
                   (0 << FIFO_TX_WMARK_SHIFT));
    sdio_reg_write(base, SDIO_PWREN, 1);
    sdio_reg_write(base, SDIO_CTYPE, 0);
    sdio_reg_write(base, SDIO_BLKSIZ, DW_SDIO_SECTOR_SIZE);
    sdio_reg_write(base, SDIO_BYTCNT, 0);
    /* Interrupts stay masked for the whole bring-up: probe cannot park, so the
     * card enumeration below runs on the polling path by construction. */
    sdio_reg_write(base, SDIO_INTMASK, 0);

    /* initial clock ~400kHz: div = ceil(clk_in / (2 * 400k)) - 1 */
    sdio_init_clock(&g_sdio, 124);

    /* CMD0 */
    sdio_send_cmd_raw(&g_sdio, 0, 0, CMD_FLAG_NONE);

    /* CMD8: check SD v2.0+ */
    if (sdio_send_cmd_raw(&g_sdio, 8, 0x1AA, CMD_FLAG_RESP_EXPECT) != 0)
        return -EIO;

    uint32_t resp[4];
    sdio_read_resp(base, resp, 0);
    if ((resp[0] & 0xFF) != 0xAA)
        return -EIO;

    /* ACMD41: wait card ready */
    uint32_t ocr = 0;
    for (int i = 0; i < 1000; i++) {
        sdio_send_cmd_raw(&g_sdio, 55, 0, CMD_FLAG_RESP_EXPECT);
        sdio_send_cmd_raw(&g_sdio, 41, SD_OCR_MASK, CMD_FLAG_RESP_EXPECT);
        sdio_read_resp(base, resp, 0);
        ocr = resp[0];
        if (ocr & (1U << 31))
            break;
        mdelay(10);
    }
    if (!(ocr & (1U << 31)))
        return -ETIMEDOUT;

    /* CMD2: get CID */
    sdio_send_cmd_raw(&g_sdio, 2, 0, CMD_FLAG_RESP_EXPECT | CMD_FLAG_RESP_LONG);

    /* CMD3: get RCA */
    sdio_send_cmd_raw(&g_sdio, 3, 0, CMD_FLAG_RESP_EXPECT);
    sdio_read_resp(base, resp, 0);
    g_sdio.rca = (resp[0] >> 16) & 0xFFFF;

    /* CMD9: get CSD */
    sdio_send_cmd_raw(&g_sdio, 9, g_sdio.rca << 16,
                      CMD_FLAG_RESP_EXPECT | CMD_FLAG_RESP_LONG);
    sdio_read_resp(base, resp, 1);

    /* parse CSD v2.0 for capacity */
    uint32_t csd_structure = (resp[3] >> 30) & 0x3;
    if (csd_structure == 1) {
        uint32_t csize = ((resp[1] & 0x3F) << 16) | ((resp[2] >> 16) & 0xFFFF);
        g_sdio.sectors = ((uint64_t)(csize + 1)) * 1024;
    } else {
        g_sdio.sectors = 0;
    }

    /* CMD7: select card */
    sdio_send_cmd_raw(&g_sdio, 7, g_sdio.rca << 16, CMD_FLAG_RESP_EXPECT);

    /* CMD16: set block len */
    sdio_send_cmd_raw(&g_sdio, 16, DW_SDIO_SECTOR_SIZE, CMD_FLAG_RESP_EXPECT);

    /* switch to high-speed clock (~25MHz) */
    sdio_init_clock(&g_sdio, 1);

    g_sdio.ready = 1;
    kinfo("[DW-SDIO] Card ready, RCA=0x%04X, sectors=%lu\n",
          g_sdio.rca, (unsigned long)g_sdio.sectors);
    return 0;
}

/*
 * IRQ completion.  Top half only: read the status, record and write-clear it,
 * then wake whoever is parked.  It takes irq_lock and never `lock`, so it is
 * safe to run while a transfer holds the sleepable mutex -- that is what lets
 * the transfer park instead of spinning.
 *
 * The wake queue is flushed here, after irq_lock is released and while no
 * object lock is held, which is the order lock-order.md requires of every
 * driver completion path.
 */
static int sdio_irq_handler(int irq, void *priv)
{
    (void)irq;
    sdio_priv_t *p = (sdio_priv_t *)priv;
    if (!p)
        return 0;

    uint32_t sts = sdio_reg_read(p->base, SDIO_RINTSTS);
    if (!sts)
        return 0;

    sdio_latch_irq(p, sts);
    /* RINTSTS is write-1-to-clear.  Clearing only what was read keeps a
     * completion that arrives between the read and this write pending. */
    sdio_reg_write(p->base, SDIO_RINTSTS, sts);

    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    (void)wait_queue_collect_all(&p->waiters, 0, PROC_WAKE_EVENT, &wake_q, NULL);
    (void)proc_wake_q_flush(&wake_q);
    return 0;
}

/*
 * Give up on the interrupt for the life of this instance.
 *
 * Reached only when an IRQ-driven wait hit its deadline: the line the board
 * published did not deliver.  That is exactly the case a board gets wrong
 * (an interrupt number from a DTS that is not the one wired), and the whole
 * point of the polling fallback is that such a board keeps booting on the path
 * that worked before.  Mask the controller, release the line, and never park
 * again -- so a bad line costs performance and is visible in the log, not
 * correctness.
 *
 * Caller must hold p->lock and must not be the handler itself: free_irq() waits
 * for in-flight handlers.
 */
static void sdio_downgrade_to_poll(sdio_priv_t *p, const char *what)
{
    if (p->poll_only)
        return;
    p->poll_only = 1;
    sdio_clear_latched(p, SDIO_INT_ALL);
    sdio_reg_write(p->base, SDIO_INTMASK, 0);
    if (p->irq_registered) {
        free_irq((uint32_t)p->irq, p);
        p->irq_registered = 0;
    }
    kwarn("[DW-SDIO] %s timed out on IRQ %d; downgraded to polling\n",
          what, p->irq);
}

static int sdio_xfer_block(uintptr_t base, uint32_t cmd_idx, uint32_t arg,
                           void *buf, int write) {
    sdio_priv_t *p = &g_sdio;
    uint64_t deadline = timer_get_ticks() +
                        sdio_ms_to_ticks(SDIO_SECTOR_TIMEOUT_MS);
    int ret = -EIO;

    mutex_lock(&p->lock);
    if (!p->ready || !buf)
        goto out;

    if (sdio_wait_data_idle(p, base, deadline) != 0) {
        ret = -EIO;
        goto out;
    }

    sdio_reg_write(base, SDIO_BLKSIZ, DW_SDIO_SECTOR_SIZE);
    sdio_reg_write(base, SDIO_BYTCNT, DW_SDIO_SECTOR_SIZE);

    uint32_t cmd_flags = CMD_FLAG_DATA_EXPECTED | CMD_FLAG_RESP_EXPECT;
    if (write) cmd_flags |= CMD_FLAG_WRITE;

    ret = sdio_send_cmd_raw(p, cmd_idx, arg, cmd_flags);
    if (ret != 0)
        goto out;

    uint32_t *fifo = (uint32_t *)(base + SDIO_FIFO);
    uint32_t words = DW_SDIO_SECTOR_SIZE / 4;

    if (write) {
        const uint32_t *src = buf;
        for (uint32_t i = 0; i < words; i++) {
            ret = sdio_wait_cond(p, base,
                                 !(sdio_reg_read(base, SDIO_STATUS) &
                                   STATUS_FIFO_FULL),
                                 INT_TXDR, INT_EBE | INT_SBE,
                                 deadline);
            if (ret != 0)
                goto out;
            writel(src[i], fifo);
        }
    } else {
        uint32_t *dst = buf;
        for (uint32_t i = 0; i < words; i++) {
            /* DTO is accepted as "data arrived" because the controller can
             * assert it with the last word still in the FIFO; the caller still
             * reads exactly `words` words. */
            ret = sdio_wait_cond(p, base,
                                 !(sdio_reg_read(base, SDIO_STATUS) &
                                   STATUS_FIFO_EMPTY),
                                 INT_RXDR | INT_DTO, INT_EBE | INT_DCRC,
                                 deadline);
            if (ret != 0)
                goto out;
            dst[i] = readl(fifo);
        }
    }

    /* wait for data transfer over */
    ret = sdio_wait_cond(p, base, 0, INT_DTO,
                         INT_EBE | INT_DCRC | INT_DRTO, deadline);
    if (ret != 0)
        goto out;
    sdio_reg_write(base, SDIO_RINTSTS, INT_DTO);
    sdio_clear_latched(p, INT_DTO);
    ret = 0;

out:
    /* Only a timeout says anything about the interrupt; a CRC or card error is
     * a property of this transfer and must not cost the instance its line. */
    if (ret == -ETIMEDOUT && p->irq_registered)
        sdio_downgrade_to_poll(p, "transfer");
    mutex_unlock(&p->lock);
    return ret;
}

int dw_sdio_read_sector(uintptr_t base, uint64_t lba, void *buf, size_t count) {
    if (!buf)
        return -EINVAL;
    for (size_t i = 0; i < count; i++) {
        int ret = sdio_xfer_block(base, 17, (uint32_t)(lba + i),
                                  (char *)buf + i * DW_SDIO_SECTOR_SIZE, 0);
        if (ret != 0)
            return ret;
    }
    return 0;
}

int dw_sdio_write_sector(uintptr_t base, uint64_t lba, const void *buf, size_t count) {
    if (!buf)
        return -EINVAL;
    for (size_t i = 0; i < count; i++) {
        int ret = sdio_xfer_block(base, 24, (uint32_t)(lba + i),
                                  (char *)(uintptr_t)buf +
                                      i * DW_SDIO_SECTOR_SIZE, 1);
        if (ret != 0)
            return ret;
    }
    return 0;
}

uint64_t dw_sdio_capacity(uintptr_t base) {
    (void)base;
    return g_sdio.sectors;
}

int dw_sdio_card_ready(uintptr_t base) {
    (void)base;
    return g_sdio.ready;
}

int dw_sdio_using_irq(void) {
    return g_sdio.irq_registered && !g_sdio.poll_only;
}

/* ============================================================
 * driver_t integration
 * ============================================================ */

/*
 * Claim the line the platform device published, if it published one and the
 * command line did not ask for polling.  Runs after the card is enumerated,
 * because probe cannot park and the bring-up therefore stays polled; the
 * transfer path is the first thing that can use the interrupt.
 *
 * A line already owned by another driver comes back -EBUSY from request_irq()
 * and simply leaves this instance polling -- which is what keeps a second
 * driver from stealing a line the board meant for someone else.
 */
static void sdio_claim_irq(int irq_line, int force_poll)
{
    if (!dw_sdio_pick_irq_mode(irq_line, force_poll))
        return;

    g_sdio.irq = irq_line;
    if (request_irq((uint32_t)irq_line, sdio_irq_handler, 0, &g_sdio) != 0) {
        kinfo("[DW-SDIO] IRQ %d registration failed; using polling\n",
              irq_line);
        g_sdio.irq = -1;
        return;
    }
    g_sdio.irq_registered = 1;
    /* Unmask the controller only with the handler in place: a source that
     * asserts before a handler exists would storm the line. */
    sdio_reg_write(g_sdio.base, SDIO_INTMASK, SDIO_INT_UNMASK);
    kinfo("[DW-SDIO] completion driven by IRQ %d\n", irq_line);
}

static int dw_sdio_driver_probe(device_t *dev) {
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res) return -ENODEV;

    int irq_line = platform_device_irq(dev);
    int force_poll = dw_sdio_poll_forced(bootargs_get());

    if (dw_sdio_init_dev(res->start) != 0) {
        kinfo("[DW-SDIO] Failed to init card at 0x%lx\n",
              (unsigned long)res->start);
        return -EIO;
    }

    dev->drv_priv = &g_sdio;
    sdio_claim_irq(irq_line, force_poll);

    /* Say which mode is live and, when it is not the interrupt, why.  A board
     * whose IRQ resource is missing or unusable must be identifiable from the
     * log alone, otherwise "the driver fell back" is indistinguishable from
     * "the driver never tried". */
    if (dw_sdio_using_irq()) {
        kinfo("[DW-SDIO] Probed '%s' at 0x%lx\n", dev->name,
              (unsigned long)res->start);
        return 0;
    }

    const char *why;
    if (force_poll)
        why = DW_SDIO_POLL_KEY "1";
    else if (irq_line == -ENODEV)
        why = "platform device published no RES_IRQ";
    else if (irq_line == -EINVAL)
        why = "platform RES_IRQ is a range or malformed";
    else if (irq_line == -ERANGE)
        why = "platform RES_IRQ names a line the IRQ registry cannot address";
    else
        why = "request_irq() refused the line";
    kinfo("[DW-SDIO] Probed '%s' at 0x%lx, polling completion (%s)\n",
          dev->name, (unsigned long)res->start, why);
    return 0;
}

static int dw_sdio_driver_remove(device_t *dev) {
    sdio_priv_t *p = (sdio_priv_t *)(dev ? dev->drv_priv : NULL);
    if (!p)
        return 0;

    /* Order matters: stop the source first, so the line cannot re-enter a
     * handler whose priv is about to be dropped.  `stopped` also makes any
     * parked transfer return immediately instead of waiting out its deadline. */
    p->stopped = 1;
    p->ready = 0;
    sdio_reg_write(p->base, SDIO_INTMASK, 0);
    if (p->irq_registered) {
        free_irq((uint32_t)p->irq, p);
        p->irq_registered = 0;
    }
    p->irq = -1;

    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    (void)wait_queue_collect_all(&p->waiters, 0, PROC_WAKE_EVENT, &wake_q, NULL);
    (void)proc_wake_q_flush(&wake_q);

    sdio_clear_latched(p, SDIO_INT_ALL);
    dev->drv_priv = NULL;
    return 0;
}

static int dw_sdio_class_read(struct device *dev, uint64_t lba, void *buf, size_t count) {
    sdio_priv_t *priv = (sdio_priv_t *)dev->drv_priv;
    if (!priv || !buf || !count)
        return -EINVAL;
    if (lba >= priv->sectors || count > priv->sectors - lba)
        return -EINVAL;
    return dw_sdio_read_sector(priv->base, lba, buf, count);
}

static int dw_sdio_class_write(struct device *dev, uint64_t lba, const void *buf, size_t count) {
    sdio_priv_t *priv = (sdio_priv_t *)dev->drv_priv;
    if (!priv || !buf || !count)
        return -EINVAL;
    if (lba >= priv->sectors || count > priv->sectors - lba)
        return -EINVAL;
    return dw_sdio_write_sector(priv->base, lba, buf, count);
}

static uint64_t dw_sdio_class_capacity(struct device *dev) {
    sdio_priv_t *priv = (sdio_priv_t *)dev->drv_priv;
    return priv ? priv->sectors : 0;
}

static uint32_t dw_sdio_class_sector_size(struct device *dev) {
    (void)dev;
    return DW_SDIO_SECTOR_SIZE;
}

static block_dev_ops_t dw_sdio_block_ops = {
    .read        = dw_sdio_class_read,
    .write       = dw_sdio_class_write,
    .capacity    = dw_sdio_class_capacity,
    .sector_size = dw_sdio_class_sector_size,
};

static const device_id_t dw_sdio_ids[] = {
    { .vendor = DW_SDIO_PLATFORM_VENDOR, .device = DW_SDIO_PLATFORM_DEVICE,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t dw_sdio_driver = {
    .name       = "dw-sdio",
    .id_table   = dw_sdio_ids,
    .bus        = &platform_bus,
    /* jh7110.dtsi: mmc1 "starfive,jh7110-mmc" */
    .of_compatible = "starfive,jh7110-mmc",
    .probe      = dw_sdio_driver_probe,
    .remove     = dw_sdio_driver_remove,
    .class_ops  = &dw_sdio_block_ops,
    .class_type = DEV_CLASS_BLOCK,
};

DRIVER_REGISTER(dw_sdio_driver);