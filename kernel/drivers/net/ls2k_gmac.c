/*
 * A20OS Loongson-2K GMAC driver.
 *
 * LOCK_ORDER: This driver uses no private spinlock. send/recv are register-
 * polling paths against a single global ls2k_gmac_priv_t instance
 * (g_ls2k_gmac). Future IRQ-driven or SMP-safe versions must add a private
 * lock and document it in docs/drivers/lock-order.md.
 */
#include "drivers/net/ls2k_gmac.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "mm/mm.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/timer.h"

#define GMAC_MAC_BASE  0x0000
#define GMAC_DMA_BASE  0x1000

#define MAC_CONFIGURATION  (GMAC_MAC_BASE + 0x0000)
#define MAC_FRAME_FILTER   (GMAC_MAC_BASE + 0x0004)
#define MAC_MII_ADDR       (GMAC_MAC_BASE + 0x0010)
#define MAC_MII_DATA       (GMAC_MAC_BASE + 0x0014)
#define MAC_FLOW_CTRL      (GMAC_MAC_BASE + 0x0018)
#define MAC_VERSION        (GMAC_MAC_BASE + 0x0020)
#define MAC_INTERRUPT_STATUS (GMAC_MAC_BASE + 0x0038)
#define MAC_INTERRUPT_MASK   (GMAC_MAC_BASE + 0x003C)
#define MAC_ADDR0_HIGH     (GMAC_MAC_BASE + 0x0040)
#define MAC_ADDR0_LOW      (GMAC_MAC_BASE + 0x0044)
#define MAC_RGMII_STATUS   (GMAC_MAC_BASE + 0x00D8)

#define MII_ADDR_GB     (1U << 0)
#define MII_ADDR_MW     (1U << 1)
#define MII_ADDR_CR_SHIFT 2
#define MII_ADDR_GR_SHIFT 6
#define MII_ADDR_PA_SHIFT 11

#define DMA_BUS_MODE      (GMAC_DMA_BASE + 0x0000)
#define DMA_TX_POLL_DEMAND (GMAC_DMA_BASE + 0x0004)
#define DMA_RX_POLL_DEMAND (GMAC_DMA_BASE + 0x0008)
#define DMA_RX_BASE_ADDR  (GMAC_DMA_BASE + 0x000C)
#define DMA_TX_BASE_ADDR  (GMAC_DMA_BASE + 0x0010)
#define DMA_STATUS        (GMAC_DMA_BASE + 0x0014)
#define DMA_CONTROL       (GMAC_DMA_BASE + 0x0018)
#define DMA_INTR_ENABLE   (GMAC_DMA_BASE + 0x001C)

#define DMA_STATUS_TI     (1U << 0)
#define DMA_STATUS_TPS    (1U << 1)
#define DMA_STATUS_TU     (1U << 2)
#define DMA_STATUS_TJT    (1U << 3)
#define DMA_STATUS_OVF    (1U << 4)
#define DMA_STATUS_UNF    (1U << 5)
#define DMA_STATUS_RI     (1U << 6)
#define DMA_STATUS_RU     (1U << 7)
#define DMA_STATUS_RPS    (1U << 8)
#define DMA_STATUS_RWT    (1U << 9)
#define DMA_STATUS_ETI    (1U << 10)
#define DMA_STATUS_FBI    (1U << 13)
#define DMA_STATUS_AIS    (1U << 14)
#define DMA_STATUS_NIS    (1U << 15)

#define DMA_BUS_MODE_SWR  (1U << 0)

#define MAC_CONF_TE       (1U << 3)
#define MAC_CONF_RE       (1U << 2)
#define MAC_CONF_DM       (1U << 11)
#define MAC_CONF_IPC      (1U << 10)
#define MAC_CONF_PS       (1U << 15)
#define MAC_CONF_FES      (1U << 14)

#define RGMII_STATUS_LINK_UP (1U << 3)

#define GMAC_DESC_NUM 16
#define GMAC_BUF_SIZE 1536

typedef struct {
    uint32_t status;
    uint32_t length;
    uint32_t buffer1;
    uint32_t buffer2;
} gmac_desc_t;

#define DESC_OWN (1U << 31)
#define DESC_IOC (1U << 30)
#define DESC_FD  (1U << 29)
#define DESC_LD  (1U << 28)
#define DESC_ES  (1U << 15)
#define DESC_LS  (1U << 8)
#define DESC_FS  (1U << 9)

typedef struct {
    uintptr_t base;
    int       valid;
    gmac_desc_t tx_desc[GMAC_DESC_NUM] ALIGNED(16);
    gmac_desc_t rx_desc[GMAC_DESC_NUM] ALIGNED(16);
    uint8_t     tx_buf[GMAC_DESC_NUM][GMAC_BUF_SIZE] ALIGNED(64);
    uint8_t     rx_buf[GMAC_DESC_NUM][GMAC_BUF_SIZE] ALIGNED(64);
    uint32_t    tx_busy;
    uint32_t    rx_busy;
    uint8_t     mac[6];
} ls2k_gmac_priv_t;

static ls2k_gmac_priv_t g_ls2k_gmac;

static inline uint32_t gmac_read(uintptr_t base, uint32_t off) {
    return readl((volatile void *)(base + off));
}

static inline void gmac_write(uintptr_t base, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)(base + off));
}

static int gmac_mdio_wait(uintptr_t base) {
    uint64_t start = timer_get_ticks();
    while (gmac_read(base, MAC_MII_ADDR) & MII_ADDR_GB) {
        if (timer_get_ticks() - start > clock_ticks_per_sec() / 100)
            return -1;
    }
    return 0;
}

static uint16_t gmac_mdio_read(uintptr_t base, int phy_addr, int reg) {
    if (gmac_mdio_wait(base) != 0) return 0xFFFF;
    uint32_t val = MII_ADDR_GB |
                   (phy_addr << MII_ADDR_PA_SHIFT) |
                   (reg << MII_ADDR_GR_SHIFT) |
                   (4 << MII_ADDR_CR_SHIFT);
    gmac_write(base, MAC_MII_ADDR, val);
    if (gmac_mdio_wait(base) != 0) return 0xFFFF;
    return (uint16_t)gmac_read(base, MAC_MII_DATA);
}

static void gmac_mdio_write(uintptr_t base, int phy_addr, int reg, uint16_t data) {
    if (gmac_mdio_wait(base) != 0) return;
    gmac_write(base, MAC_MII_DATA, data);
    uint32_t val = MII_ADDR_GB | MII_ADDR_MW |
                   (phy_addr << MII_ADDR_PA_SHIFT) |
                   (reg << MII_ADDR_GR_SHIFT) |
                   (4 << MII_ADDR_CR_SHIFT);
    gmac_write(base, MAC_MII_ADDR, val);
    gmac_mdio_wait(base);
}

static void ls2k_gmac_init_desc(uintptr_t base, ls2k_gmac_priv_t *priv) {
    memset(priv->tx_desc, 0, sizeof(priv->tx_desc));
    memset(priv->rx_desc, 0, sizeof(priv->rx_desc));

    paddr_t tx_desc_pa = va_to_pa((const void *)priv->tx_desc);
    paddr_t rx_desc_pa = va_to_pa((const void *)priv->rx_desc);

    for (int i = 0; i < GMAC_DESC_NUM; i++) {
        paddr_t tx_buf_pa = va_to_pa((const void *)priv->tx_buf[i]);
        paddr_t rx_buf_pa = va_to_pa((const void *)priv->rx_buf[i]);

        priv->tx_desc[i].status = DESC_IOC | DESC_FD | DESC_LD;
        priv->tx_desc[i].buffer1 = (uint32_t)tx_buf_pa;

        priv->rx_desc[i].status = DESC_OWN | DESC_IOC | DESC_FD | DESC_LD;
        priv->rx_desc[i].length = (1 << 16) | GMAC_BUF_SIZE;
        priv->rx_desc[i].buffer1 = (uint32_t)rx_buf_pa;
    }

    gmac_write(base, DMA_TX_BASE_ADDR, (uint32_t)tx_desc_pa);
    gmac_write(base, DMA_RX_BASE_ADDR, (uint32_t)rx_desc_pa);

    priv->tx_busy = 0;
    priv->rx_busy = 0;
}

static int ls2k_gmac_phy_init(uintptr_t base) {
    int phy_addr = 0;

    uint16_t id1 = gmac_mdio_read(base, phy_addr, 2);
    uint16_t id2 = gmac_mdio_read(base, phy_addr, 3);
    uint32_t phy_id = ((uint32_t)id1 << 16) | id2;
    kinfo("[LS2K-GMAC] PHY ID: 0x%08X\n", phy_id);

    gmac_mdio_write(base, phy_addr, 0, 0x8000);
    uint64_t start = timer_get_ticks();
    while (1) {
        uint16_t val = gmac_mdio_read(base, phy_addr, 0);
        if (!(val & 0x8000)) break;
        if (timer_get_ticks() - start > clock_ticks_per_sec() * 2) {
            kinfo("[LS2K-GMAC] PHY reset timeout\n");
            return -1;
        }
    }

    gmac_mdio_write(base, phy_addr, 0, 0x1200);
    mdelay(100);

    start = timer_get_ticks();
    while (1) {
        uint16_t val = gmac_mdio_read(base, phy_addr, 1);
        if (val & 0x0004) break;
        if (timer_get_ticks() - start > clock_ticks_per_sec() * 5) {
            kinfo("[LS2K-GMAC] PHY link up timeout\n");
            return -1;
        }
        mdelay(10);
    }

    kinfo("[LS2K-GMAC] PHY link up\n");
    return 0;
}

int ls2k_gmac_init(uintptr_t base) {
    g_ls2k_gmac.base = base;
    g_ls2k_gmac.valid = 0;
    memcpy(g_ls2k_gmac.mac, (uint8_t[]){0x00, 0x55, 0x7B, 0xB5, 0x7D, 0xF7}, 6);

    gmac_write(base, DMA_BUS_MODE, DMA_BUS_MODE_SWR);
    while (gmac_read(base, DMA_BUS_MODE) & DMA_BUS_MODE_SWR);
    mdelay(10);

    gmac_write(base, MAC_CONFIGURATION, 0);
    ls2k_gmac_init_desc(base, &g_ls2k_gmac);

    gmac_write(base, MAC_FRAME_FILTER, 0x80000001);
    gmac_write(base, MAC_FLOW_CTRL, 0);

    uint32_t high = (g_ls2k_gmac.mac[5] << 8) | g_ls2k_gmac.mac[4] | (1U << 31);
    uint32_t low  = (g_ls2k_gmac.mac[3] << 24) | (g_ls2k_gmac.mac[2] << 16) |
                    (g_ls2k_gmac.mac[1] << 8)  | g_ls2k_gmac.mac[0];
    gmac_write(base, MAC_ADDR0_HIGH, high);
    gmac_write(base, MAC_ADDR0_LOW, low);

    if (ls2k_gmac_phy_init(base) != 0)
        return -1;

    gmac_write(base, MAC_CONFIGURATION,
               MAC_CONF_RE | MAC_CONF_TE | MAC_CONF_DM | MAC_CONF_IPC);

    gmac_write(base, DMA_CONTROL, 0x00202000);
    gmac_write(base, DMA_INTR_ENABLE, 0x00010043);

    g_ls2k_gmac.valid = 1;
    kinfo("[LS2K-GMAC] Initialized at 0x%lx\n", (unsigned long)base);
    return 0;
}

int ls2k_gmac_send(uintptr_t base, const void *pkt, size_t len) {
    if (!g_ls2k_gmac.valid || len > GMAC_BUF_SIZE - 4) return -1;

    uint32_t idx = g_ls2k_gmac.tx_busy;
    gmac_desc_t *desc = &g_ls2k_gmac.tx_desc[idx];

    if (desc->status & DESC_OWN) return -1;

    memcpy(g_ls2k_gmac.tx_buf[idx], pkt, len);
    if (len < 60) {
        memset(g_ls2k_gmac.tx_buf[idx] + len, 0, 60 - len);
        len = 60;
    }

    desc->length = (1 << 16) | (uint32_t)len;
    desc->status = DESC_OWN | DESC_IOC | DESC_FD | DESC_LD | DESC_FS | DESC_LS;

    gmac_write(base, DMA_TX_POLL_DEMAND, 1);

    g_ls2k_gmac.tx_busy = (idx + 1) % GMAC_DESC_NUM;
    return 0;
}

int ls2k_gmac_recv(uintptr_t base, void *buf, size_t maxlen) {
    if (!g_ls2k_gmac.valid) return -1;

    uint32_t idx = g_ls2k_gmac.rx_busy;
    gmac_desc_t *desc = &g_ls2k_gmac.rx_desc[idx];

    if (desc->status & DESC_OWN) return -1;

    if (desc->status & DESC_ES) {
        desc->status = DESC_OWN | DESC_IOC | DESC_FD | DESC_LD;
        g_ls2k_gmac.rx_busy = (idx + 1) % GMAC_DESC_NUM;
        return -1;
    }

    uint32_t len = ((desc->status >> 16) & 0x3FFF) - 4;
    if (len > maxlen) len = maxlen;
    if (len > 0) memcpy(buf, g_ls2k_gmac.rx_buf[idx], len);

    desc->status = DESC_OWN | DESC_IOC | DESC_FD | DESC_LD;
    desc->length = (1 << 16) | GMAC_BUF_SIZE;
    gmac_write(base, DMA_RX_POLL_DEMAND, 1);

    g_ls2k_gmac.rx_busy = (idx + 1) % GMAC_DESC_NUM;
    return (int)len;
}

void ls2k_gmac_get_mac(uintptr_t base, uint8_t *mac) {
    (void)base;
    memcpy(mac, g_ls2k_gmac.mac, 6);
}

int ls2k_gmac_poll(uintptr_t base) {
    uint32_t status = gmac_read(base, DMA_STATUS);
    if (status) gmac_write(base, DMA_STATUS, status);
    return 0;
}

/* ============================================================
 * driver_t integration
 * ============================================================ */

typedef struct {
    ls2k_gmac_priv_t gmac;
    net_dev_ops_t    ops;
} ls2k_gmac_drv_t;

static ls2k_gmac_drv_t g_ls2k_drv;

static int ls2k_gmac_driver_probe(device_t *dev) {
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res) return -1;

    if (ls2k_gmac_init(res->start) != 0) {
        kinfo("[LS2K-GMAC] Failed to init at 0x%lx\n", (unsigned long)res->start);
        return -1;
    }

    dev->drv_priv = &g_ls2k_drv;
    kinfo("[LS2K-GMAC] Probed '%s' at 0x%lx\n", dev->name, (unsigned long)res->start);
    return 0;
}

static int ls2k_gmac_driver_remove(device_t *dev) {
    (void)dev;
    return 0;
}

static int ls2k_gmac_class_send(struct device *dev, const void *pkt, size_t len) {
    ls2k_gmac_drv_t *priv = (ls2k_gmac_drv_t *)dev->drv_priv;
    return ls2k_gmac_send(priv->gmac.base, pkt, len);
}

static int ls2k_gmac_class_recv(struct device *dev, void *buf, size_t maxlen) {
    ls2k_gmac_drv_t *priv = (ls2k_gmac_drv_t *)dev->drv_priv;
    return ls2k_gmac_recv(priv->gmac.base, buf, maxlen);
}

static const uint8_t *ls2k_gmac_class_mac(struct device *dev) {
    ls2k_gmac_drv_t *priv = (ls2k_gmac_drv_t *)dev->drv_priv;
    return priv->gmac.mac;
}

static void ls2k_gmac_class_poll(struct device *dev) {
    ls2k_gmac_drv_t *priv = (ls2k_gmac_drv_t *)dev->drv_priv;
    ls2k_gmac_poll(priv->gmac.base);
}

static net_dev_ops_t ls2k_gmac_net_ops = {
    .send = ls2k_gmac_class_send,
    .recv = ls2k_gmac_class_recv,
    .mac  = ls2k_gmac_class_mac,
    .poll = ls2k_gmac_class_poll,
};

static driver_t ls2k_gmac_driver = {
    .name       = "ls2k-gmac",
    .id_table   = NULL,
    .bus        = NULL,
    .probe      = ls2k_gmac_driver_probe,
    .remove     = ls2k_gmac_driver_remove,
    .class_ops  = &ls2k_gmac_net_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(ls2k_gmac_driver);
