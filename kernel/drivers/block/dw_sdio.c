/*
 * A20OS DW SDIO host driver.
 *
 * LOCK_ORDER: This driver uses no private spinlock. All command/data
 * transfers are synchronous busy-polls against a single global sdio_priv_t
 * instance (g_sdio). Future concurrent or IRQ-driven versions must add a
 * private lock and document it in docs/drivers/lock-order.md.
 */
#include "drivers/block/dw_sdio.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/timer.h"

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

static inline uint32_t sdio_reg_read(uintptr_t base, uint32_t off) {
    return readl((volatile void *)(base + off));
}

static inline void sdio_reg_write(uintptr_t base, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)(base + off));
}

static int sdio_wait_cmd(uintptr_t base, uint32_t timeout_ms) {
    uint64_t start = timer_get_ticks();
    uint64_t tmo = (clock_ticks_per_sec() * timeout_ms) / 1000;
    if (tmo == 0) tmo = clock_ticks_per_sec();

    while (1) {
        uint32_t sts = sdio_reg_read(base, SDIO_RINTSTS);
        if (sts & INT_CD) {
            sdio_reg_write(base, SDIO_RINTSTS, INT_CD);
            return 0;
        }
        if (sts & (INT_RTO | INT_RCRC | INT_RE)) {
            sdio_reg_write(base, SDIO_RINTSTS, sts);
            return -1;
        }
        if (timer_get_ticks() - start > tmo)
            return -1;
    }
}

static int sdio_send_cmd_raw(uintptr_t base, uint32_t idx, uint32_t arg, uint32_t flags) {
    sdio_reg_write(base, SDIO_RINTSTS, 0xFFFFFFFF);
    sdio_reg_write(base, SDIO_CMDARG, arg);

    uint32_t cmd = (idx & 0x3F) | CMD_START | CMD_USE_HOLD_REG;
    if (flags & CMD_FLAG_RESP_EXPECT)  cmd |= CMD_RESP_EXPECT;
    if (flags & CMD_FLAG_RESP_LONG)    cmd |= CMD_RESP_LENGTH;
    if (flags & CMD_FLAG_DATA_EXPECTED) cmd |= CMD_DATA_EXPECTED;
    if (flags & CMD_FLAG_WRITE)        cmd |= CMD_WRITE;

    sdio_reg_write(base, SDIO_CMD, cmd);
    return sdio_wait_cmd(base, 1000);
}

static int sdio_update_clock(uintptr_t base) {
    uint32_t cmd = CMD_START | CMD_USE_HOLD_REG | CMD_UPDATE_CLK |
                   CMD_WAIT_PRVDATA;
    sdio_reg_write(base, SDIO_CMD, cmd);
    return sdio_wait_cmd(base, 100);
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

typedef struct {
    uintptr_t base;
    uint32_t  rca;
    int       ready;
    uint64_t  sectors;
} sdio_priv_t;

static sdio_priv_t g_sdio;

static int sdio_reset(uintptr_t base) {
    sdio_reg_write(base, SDIO_CTRL, 1);
    while (sdio_reg_read(base, SDIO_CTRL) & 1);
    return 0;
}

static int sdio_init_clock(uintptr_t base, uint32_t div) {
    sdio_reg_write(base, SDIO_CLKENA, 0);
    sdio_update_clock(base);
    sdio_reg_write(base, SDIO_CLKDIV, div);
    sdio_reg_write(base, SDIO_CLKENA, 1);
    return sdio_update_clock(base);
}

static int sdio_wait_data_idle(uintptr_t base) {
    uint64_t start = timer_get_ticks();
    while (sdio_reg_read(base, SDIO_STATUS) & STATUS_DATA_BUSY) {
        if (timer_get_ticks() - start > clock_ticks_per_sec())
            return -1;
    }
    return 0;
}

int dw_sdio_init_dev(uintptr_t base) {
    g_sdio.base = base;
    g_sdio.ready = 0;
    g_sdio.sectors = 0;

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
    sdio_reg_write(base, SDIO_INTMASK, 0);

    /* initial clock ~400kHz: div = ceil(clk_in / (2 * 400k)) - 1 */
    sdio_init_clock(base, 124);

    /* CMD0 */
    sdio_send_cmd_raw(base, 0, 0, CMD_FLAG_NONE);

    /* CMD8: check SD v2.0+ */
    if (sdio_send_cmd_raw(base, 8, 0x1AA, CMD_FLAG_RESP_EXPECT) != 0)
        return -1;

    uint32_t resp[4];
    sdio_read_resp(base, resp, 0);
    if ((resp[0] & 0xFF) != 0xAA)
        return -1;

    /* ACMD41: wait card ready */
    uint32_t ocr = 0;
    for (int i = 0; i < 1000; i++) {
        sdio_send_cmd_raw(base, 55, 0, CMD_FLAG_RESP_EXPECT);
        sdio_send_cmd_raw(base, 41, 0x40FF8000, CMD_FLAG_RESP_EXPECT);
        sdio_read_resp(base, resp, 0);
        ocr = resp[0];
        if (ocr & (1U << 31))
            break;
        mdelay(10);
    }
    if (!(ocr & (1U << 31)))
        return -1;

    /* CMD2: get CID */
    sdio_send_cmd_raw(base, 2, 0, CMD_FLAG_RESP_EXPECT | CMD_FLAG_RESP_LONG);

    /* CMD3: get RCA */
    sdio_send_cmd_raw(base, 3, 0, CMD_FLAG_RESP_EXPECT);
    sdio_read_resp(base, resp, 0);
    g_sdio.rca = (resp[0] >> 16) & 0xFFFF;

    /* CMD9: get CSD */
    sdio_send_cmd_raw(base, 9, g_sdio.rca << 16,
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
    sdio_send_cmd_raw(base, 7, g_sdio.rca << 16, CMD_FLAG_RESP_EXPECT);

    /* CMD16: set block len */
    sdio_send_cmd_raw(base, 16, DW_SDIO_SECTOR_SIZE, CMD_FLAG_RESP_EXPECT);

    /* switch to high-speed clock (~25MHz) */
    sdio_init_clock(base, 1);

    g_sdio.ready = 1;
    kinfo("[DW-SDIO] Card ready, RCA=0x%04X, sectors=%lu\n",
          g_sdio.rca, (unsigned long)g_sdio.sectors);
    return 0;
}

static int sdio_xfer_block(uintptr_t base, uint32_t cmd_idx, uint32_t arg,
                           void *buf, int write) {
    if (!g_sdio.ready)
        return -1;

    sdio_wait_data_idle(base);

    sdio_reg_write(base, SDIO_BLKSIZ, DW_SDIO_SECTOR_SIZE);
    sdio_reg_write(base, SDIO_BYTCNT, DW_SDIO_SECTOR_SIZE);

    uint32_t flags = CMD_FLAG_DATA_EXPECTED;
    if (write) flags |= CMD_FLAG_WRITE;

    if (sdio_send_cmd_raw(base, cmd_idx, arg, flags | CMD_FLAG_RESP_EXPECT) != 0)
        return -1;

    uint32_t *fifo = (uint32_t *)(base + SDIO_FIFO);
    uint32_t words = DW_SDIO_SECTOR_SIZE / 4;

    if (write) {
        const uint32_t *src = buf;
        for (uint32_t i = 0; i < words; i++) {
            while (sdio_reg_read(base, SDIO_STATUS) & STATUS_FIFO_FULL)
                ;
            writel(src[i], fifo);
        }
    } else {
        uint32_t *dst = buf;
        for (uint32_t i = 0; i < words; i++) {
            while (sdio_reg_read(base, SDIO_STATUS) & STATUS_FIFO_EMPTY)
                ;
            dst[i] = readl(fifo);
        }
    }

    /* wait for data transfer over */
    uint64_t start = timer_get_ticks();
    while (!(sdio_reg_read(base, SDIO_RINTSTS) & INT_DTO)) {
        if (timer_get_ticks() - start > clock_ticks_per_sec())
            return -1;
    }
    sdio_reg_write(base, SDIO_RINTSTS, INT_DTO);

    return 0;
}

int dw_sdio_read_sector(uintptr_t base, uint64_t lba, void *buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (sdio_xfer_block(base, 17, (uint32_t)(lba + i),
                           (char *)buf + i * DW_SDIO_SECTOR_SIZE, 0) != 0)
            return -1;
    }
    return 0;
}

int dw_sdio_write_sector(uintptr_t base, uint64_t lba, const void *buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (sdio_xfer_block(base, 24, (uint32_t)(lba + i),
                           (char *)buf + i * DW_SDIO_SECTOR_SIZE, 1) != 0)
            return -1;
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

/* ============================================================
 * driver_t integration
 * ============================================================ */

typedef struct {
    sdio_priv_t    sdio;
    block_dev_ops_t ops;
} dw_sdio_drv_t;

static dw_sdio_drv_t g_drv;

static int dw_sdio_driver_probe(device_t *dev) {
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res) return -1;

    if (dw_sdio_init_dev(res->start) != 0) {
        kinfo("[DW-SDIO] Failed to init card at 0x%lx\n", (unsigned long)res->start);
        return -1;
    }

    dev->drv_priv = &g_drv;
    kinfo("[DW-SDIO] Probed '%s' at 0x%lx\n", dev->name, (unsigned long)res->start);
    return 0;
}

static int dw_sdio_driver_remove(device_t *dev) {
    (void)dev;
    return 0;
}

static int dw_sdio_class_read(struct device *dev, uint64_t lba, void *buf, size_t count) {
    dw_sdio_drv_t *priv = (dw_sdio_drv_t *)dev->drv_priv;
    return dw_sdio_read_sector(priv->sdio.base, lba, buf, count);
}

static int dw_sdio_class_write(struct device *dev, uint64_t lba, const void *buf, size_t count) {
    dw_sdio_drv_t *priv = (dw_sdio_drv_t *)dev->drv_priv;
    return dw_sdio_write_sector(priv->sdio.base, lba, buf, count);
}

static uint64_t dw_sdio_class_capacity(struct device *dev) {
    dw_sdio_drv_t *priv = (dw_sdio_drv_t *)dev->drv_priv;
    return dw_sdio_capacity(priv->sdio.base);
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

static driver_t dw_sdio_driver = {
    .name       = "dw-sdio",
    .id_table   = NULL,
    .bus        = NULL,
    .probe      = dw_sdio_driver_probe,
    .remove     = dw_sdio_driver_remove,
    .class_ops  = &dw_sdio_block_ops,
    .class_type = DEV_CLASS_BLOCK,
};

DRIVER_REGISTER(dw_sdio_driver);
