/*
 * A20OS StarFive EQOS GMAC driver.
 *
 * LOCK_ORDER: This driver uses no private spinlock. send/recv are register-
 * polling paths against a single global gmac_priv_t instance (g_gmac).
 * Future IRQ-driven or SMP-safe versions must add a private lock and
 * document it in docs/driver-lock-order.md.
 */
#include "drivers/net/starfive_gmac.h"
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

/* ============================================================
 * EQOS (Enhanced Quality of Service) GMAC Register Offsets
 * ============================================================ */

#define MAC_BASE  0x0000
#define MTL_BASE  0x0C00
#define DMA_BASE  0x1000

/* MAC registers */
#define MAC_CONFIGURATION        (MAC_BASE + 0x0000)
#define MAC_FRAME_FILTER         (MAC_BASE + 0x0004)
#define MAC_HASH_TABLE_REG0      (MAC_BASE + 0x0008)
#define MAC_MII_ADDR             (MAC_BASE + 0x0020)
#define MAC_MII_DATA             (MAC_BASE + 0x0024)
#define MAC_FLOW_CTRL            (MAC_BASE + 0x0048)
#define MAC_VLAN_TAG             (MAC_BASE + 0x0050)
#define MAC_DEBUG                (MAC_BASE + 0x005C)
#define MAC_HW_FEATURE0          (MAC_BASE + 0x011C)
#define MAC_HW_FEATURE1          (MAC_BASE + 0x0120)
#define MAC_HW_FEATURE2          (MAC_BASE + 0x0124)
#define MAC_MDIO_INTR_STATUS     (MAC_BASE + 0x0140)
#define MAC_ADDR0_HIGH           (MAC_BASE + 0x0300)
#define MAC_ADDR0_LOW            (MAC_BASE + 0x0304)

/* MTL registers */
#define MTL_OPERATION_MODE       (MTL_BASE + 0x0000)
#define MTL_TXQ0_OPERATION_MODE  (MTL_BASE + 0x0D00)
#define MTL_RXQ0_OPERATION_MODE  (MTL_BASE + 0x0D30)
#define MTL_RXQ0_MISSED_PKT      (MTL_BASE + 0x0D40)

/* DMA registers */
#define DMA_MODE                 (DMA_BASE + 0x0000)
#define DMA_SYSBUS_MODE          (DMA_BASE + 0x0004)
#define DMA_STATUS               (DMA_BASE + 0x0008)
#define DMA_CH0_CONTROL          (DMA_BASE + 0x0100)
#define DMA_CH0_TX_CONTROL       (DMA_BASE + 0x0104)
#define DMA_CH0_RX_CONTROL       (DMA_BASE + 0x0108)
#define DMA_CH0_TXDESC_LIST_ADDR (DMA_BASE + 0x0114)
#define DMA_CH0_TXDESC_LIST_HADDR (DMA_BASE + 0x0118)
#define DMA_CH0_RXDESC_LIST_ADDR (DMA_BASE + 0x011C)
#define DMA_CH0_RXDESC_LIST_HADDR (DMA_BASE + 0x0120)
#define DMA_CH0_TXDESC_TAIL_PTR  (DMA_BASE + 0x0128)
#define DMA_CH0_RXDESC_TAIL_PTR  (DMA_BASE + 0x012C)
#define DMA_CH0_TXDESC_RING_LEN  (DMA_BASE + 0x0130)
#define DMA_CH0_RXDESC_RING_LEN  (DMA_BASE + 0x0134)
#define DMA_CH0_INTERRUPT_ENABLE (DMA_BASE + 0x0138)
#define DMA_CH0_STATUS           (DMA_BASE + 0x0160)

/* MAC Configuration bits */
#define MAC_CONF_RE       (1U << 0)
#define MAC_CONF_TE       (1U << 1)
#define MAC_CONF_DM       (1U << 13)
#define MAC_CONF_DO       (1U << 10)
#define MAC_CONF_IPC      (1U << 27)
#define MAC_CONF_PS       (1U << 15)
#define MAC_CONF_FES      (1U << 14)

/* MII Address bits */
#define MII_ADDR_GB       (1U << 0)
#define MII_ADDR_MW       (1U << 1)
#define MII_ADDR_CR_SHIFT 2
#define MII_ADDR_CR_MASK  (0xFU << MII_ADDR_CR_SHIFT)
#define MII_ADDR_GR_SHIFT 6
#define MII_ADDR_PA_SHIFT 11

/* MTL Operation Mode bits */
#define MTL_OP_MODE_DTXSTS  (1U << 1)
#define MTL_OP_MODE_RAA_SP  (1U << 2)

/* MTL Queue Operation Mode bits */
#define MTL_TXQ0_TSF      (1U << 1)
#define MTL_TXQ0_FTQ      (1U << 0)
#define MTL_TXQ0_TXQEN    (1U << 3)
#define MTL_TXQ0_TTC_SHIFT 4
#define MTL_TXQ0_TQS_SHIFT 16

#define MTL_RXQ0_RTC_SHIFT 3
#define MTL_RXQ0_RQS_SHIFT 16
#define MTL_RXQ0_RXQEN    (1U << 0)

/* DMA bits */
#define DMA_MODE_SWR      (1U << 0)

#define DMA_SYSBUS_MODE_EAME (1U << 11)
#define DMA_SYSBUS_MODE_BLEN4 (1U << 1)
#define DMA_SYSBUS_MODE_BLEN8 (1U << 2)
#define DMA_SYSBUS_MODE_BLEN16 (1U << 3)

#define DMA_CH0_CONTROL_PBLX8 (1U << 16)

#define DMA_CH0_TX_CONTROL_ST  (1U << 0)
#define DMA_CH0_TX_CONTROL_OSP (1U << 4)
#define DMA_CH0_TX_CONTROL_TXPBL_SHIFT 16
#define DMA_CH0_TX_CONTROL_TXPBL_MASK  (0x3FU << 16)

#define DMA_CH0_RX_CONTROL_SR  (1U << 0)
#define DMA_CH0_RX_CONTROL_RBSZ_SHIFT 1
#define DMA_CH0_RX_CONTROL_RBSZ_MASK  (0x7FFFU << 1)
#define DMA_CH0_RX_CONTROL_RXPBL_SHIFT 16
#define DMA_CH0_RX_CONTROL_RXPBL_MASK  (0x3FU << 16)

#define DMA_CH0_STATUS_TI  (1U << 0)
#define DMA_CH0_STATUS_RI  (1U << 6)
#define DMA_CH0_STATUS_AIS (1U << 14)
#define DMA_CH0_STATUS_NIS (1U << 15)

/* Descriptor bits */
#define DESC3_OWN     (1U << 31)
#define DESC3_IOC     (1U << 30)
#define DESC3_FD      (1U << 29)
#define DESC3_LD      (1U << 28)
#define DESC3_BUF1V   (1U << 24)
#define DESC2_IOC_BIT (1U << 31)

#define GMAC_DESC_NUM 16
#define GMAC_BUF_SIZE 1536

typedef struct {
    uint32_t des0;
    uint32_t des1;
    uint32_t des2;
    uint32_t des3;
} dma_desc_t;

typedef struct {
    uintptr_t base;
    int       valid;

    dma_desc_t tx_desc[GMAC_DESC_NUM] ALIGNED(16);
    dma_desc_t rx_desc[GMAC_DESC_NUM] ALIGNED(16);
    uint8_t    tx_buf[GMAC_DESC_NUM][GMAC_BUF_SIZE] ALIGNED(64);
    uint8_t    rx_buf[GMAC_DESC_NUM][GMAC_BUF_SIZE] ALIGNED(64);

    uint32_t   tx_busy;
    uint32_t   rx_busy;
    uint8_t    mac[6];
} gmac_priv_t;

static gmac_priv_t g_gmac;

static inline uint32_t gmac_read(uintptr_t base, uint32_t off) {
    return readl((volatile void *)(base + off));
}

static inline void gmac_write(uintptr_t base, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)(base + off));
}

/* ============================================================
 * MII / PHY Access
 * ============================================================ */
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
                   ((phy_addr << MII_ADDR_PA_SHIFT) & (0x1F << MII_ADDR_PA_SHIFT)) |
                   ((reg << MII_ADDR_GR_SHIFT) & (0x1F << MII_ADDR_GR_SHIFT)) |
                   (4 << MII_ADDR_CR_SHIFT);
    gmac_write(base, MAC_MII_ADDR, val);
    if (gmac_mdio_wait(base) != 0) return 0xFFFF;
    return (uint16_t)gmac_read(base, MAC_MII_DATA);
}

static void gmac_mdio_write(uintptr_t base, int phy_addr, int reg, uint16_t data) {
    if (gmac_mdio_wait(base) != 0) return;
    gmac_write(base, MAC_MII_DATA, data);
    uint32_t val = MII_ADDR_GB | MII_ADDR_MW |
                   ((phy_addr << MII_ADDR_PA_SHIFT) & (0x1F << MII_ADDR_PA_SHIFT)) |
                   ((reg << MII_ADDR_GR_SHIFT) & (0x1F << MII_ADDR_GR_SHIFT)) |
                   (4 << MII_ADDR_CR_SHIFT);
    gmac_write(base, MAC_MII_ADDR, val);
    gmac_mdio_wait(base);
}

/* ============================================================
 * DMA Descriptor Management
 * ============================================================ */
static void gmac_init_desc(uintptr_t base, gmac_priv_t *priv) {
    memset(priv->tx_desc, 0, sizeof(priv->tx_desc));
    memset(priv->rx_desc, 0, sizeof(priv->rx_desc));

    paddr_t tx_desc_pa = va_to_pa((const void *)priv->tx_desc);
    paddr_t rx_desc_pa = va_to_pa((const void *)priv->rx_desc);

    for (int i = 0; i < GMAC_DESC_NUM; i++) {
        paddr_t tx_buf_pa = va_to_pa((const void *)priv->tx_buf[i]);
        paddr_t rx_buf_pa = va_to_pa((const void *)priv->rx_buf[i]);

        priv->tx_desc[i].des0 = (uint32_t)tx_buf_pa;
        priv->tx_desc[i].des3 = DESC3_IOC | DESC3_FD | DESC3_LD;

        priv->rx_desc[i].des0 = (uint32_t)rx_buf_pa;
        priv->rx_desc[i].des1 = 0;
        priv->rx_desc[i].des2 = 0;
        priv->rx_desc[i].des3 = DESC3_OWN | DESC3_BUF1V | DESC3_IOC | DESC3_FD | DESC3_LD;
    }

    gmac_write(base, DMA_CH0_TXDESC_LIST_ADDR, (uint32_t)tx_desc_pa);
    gmac_write(base, DMA_CH0_TXDESC_LIST_HADDR, (uint32_t)(tx_desc_pa >> 32));
    gmac_write(base, DMA_CH0_RXDESC_LIST_ADDR, (uint32_t)rx_desc_pa);
    gmac_write(base, DMA_CH0_RXDESC_LIST_HADDR, (uint32_t)(rx_desc_pa >> 32));

    gmac_write(base, DMA_CH0_TXDESC_RING_LEN, GMAC_DESC_NUM - 1);
    gmac_write(base, DMA_CH0_RXDESC_RING_LEN, GMAC_DESC_NUM - 1);

    gmac_write(base, DMA_CH0_RXDESC_TAIL_PTR, (uint32_t)(rx_desc_pa +
               sizeof(dma_desc_t) * (GMAC_DESC_NUM - 1)));

    priv->tx_busy = 0;
    priv->rx_busy = 0;
}

/* ============================================================
 * PHY Initialization (Generic RGMII)
 * ============================================================ */
static int gmac_phy_init(uintptr_t base) {
    int phy_addr = 1; /* VisionFive2 PHY address */

    /* Reset PHY */
    gmac_mdio_write(base, phy_addr, 0, 0x8000);
    uint64_t start = timer_get_ticks();
    while (1) {
        uint16_t val = gmac_mdio_read(base, phy_addr, 0);
        if (!(val & 0x8000)) break;
        if (timer_get_ticks() - start > clock_ticks_per_sec() * 2) {
            kinfo("[GMAC] PHY reset timeout\n");
            return -1;
        }
    }

    /* Auto-negotiation */
    gmac_mdio_write(base, phy_addr, 0, 0x1200);
    mdelay(100);

    /* Wait for link up */
    start = timer_get_ticks();
    while (1) {
        uint16_t val = gmac_mdio_read(base, phy_addr, 1);
        if (val & 0x0004) break;
        if (timer_get_ticks() - start > clock_ticks_per_sec() * 5) {
            kinfo("[GMAC] PHY link up timeout\n");
            return -1;
        }
        mdelay(10);
    }

    kinfo("[GMAC] PHY link up\n");
    return 0;
}

/* ============================================================
 * GMAC Initialization
 * ============================================================ */
int starfive_gmac_init(uintptr_t base) {
    g_gmac.base = base;
    g_gmac.valid = 0;
    memcpy(g_gmac.mac, (uint8_t[]){0x00, 0x55, 0x7B, 0xB5, 0x7D, 0xF7}, 6);

    /* DMA reset */
    gmac_write(base, DMA_MODE, DMA_MODE_SWR);
    while (gmac_read(base, DMA_MODE) & DMA_MODE_SWR);
    mdelay(10);

    /* MAC reset - disable TX/RX */
    gmac_write(base, MAC_CONFIGURATION, 0);

    /* DMA system bus mode */
    gmac_write(base, DMA_SYSBUS_MODE,
               DMA_SYSBUS_MODE_EAME |
               DMA_SYSBUS_MODE_BLEN4 |
               DMA_SYSBUS_MODE_BLEN8 |
               DMA_SYSBUS_MODE_BLEN16);

    /* Initialize descriptors */
    gmac_init_desc(base, &g_gmac);

    /* MTL configuration */
    gmac_write(base, MTL_OPERATION_MODE, MTL_OP_MODE_DTXSTS | MTL_OP_MODE_RAA_SP);
    gmac_write(base, MTL_TXQ0_OPERATION_MODE,
               MTL_TXQ0_TXQEN | MTL_TXQ0_TSF |
               (0x2 << MTL_TXQ0_TTC_SHIFT) |
               (0x7 << MTL_TXQ0_TQS_SHIFT));
    gmac_write(base, MTL_RXQ0_OPERATION_MODE,
               MTL_RXQ0_RXQEN |
               (1536 << MTL_RXQ0_RQS_SHIFT));

    /* MAC configuration */
    gmac_write(base, MAC_FRAME_FILTER, 0x80000001); /* promiscuous for now */
    gmac_write(base, MAC_FLOW_CTRL, 0);

    /* Set MAC address */
    uint32_t high = (g_gmac.mac[5] << 8) | g_gmac.mac[4] | (1U << 31);
    uint32_t low  = (g_gmac.mac[3] << 24) | (g_gmac.mac[2] << 16) |
                    (g_gmac.mac[1] << 8)  | g_gmac.mac[0];
    gmac_write(base, MAC_ADDR0_HIGH, high);
    gmac_write(base, MAC_ADDR0_LOW, low);

    /* PHY init */
    if (gmac_phy_init(base) != 0)
        return -1;

    /* Enable MAC TX/RX */
    gmac_write(base, MAC_CONFIGURATION,
               MAC_CONF_RE | MAC_CONF_TE | MAC_CONF_DM | MAC_CONF_DO | MAC_CONF_IPC);

    /* DMA channel 0 configuration */
    gmac_write(base, DMA_CH0_CONTROL, DMA_CH0_CONTROL_PBLX8);
    gmac_write(base, DMA_CH0_TX_CONTROL,
               DMA_CH0_TX_CONTROL_ST | DMA_CH0_TX_CONTROL_OSP |
               (16 << DMA_CH0_TX_CONTROL_TXPBL_SHIFT));
    gmac_write(base, DMA_CH0_RX_CONTROL,
               DMA_CH0_RX_CONTROL_SR |
               ((GMAC_BUF_SIZE << DMA_CH0_RX_CONTROL_RBSZ_SHIFT) & DMA_CH0_RX_CONTROL_RBSZ_MASK) |
               (16 << DMA_CH0_RX_CONTROL_RXPBL_SHIFT));

    g_gmac.valid = 1;
    kinfo("[StarFive-GMAC] Initialized at 0x%lx\n", (unsigned long)base);
    return 0;
}

/* ============================================================
 * Packet TX/RX
 * ============================================================ */
int starfive_gmac_send(uintptr_t base, const void *pkt, size_t len) {
    if (!g_gmac.valid || len > GMAC_BUF_SIZE - 4) return -1;

    uint32_t idx = g_gmac.tx_busy;
    dma_desc_t *desc = &g_gmac.tx_desc[idx];

    if (desc->des3 & DESC3_OWN) return -1; /* DMA still owns it */

    memcpy(g_gmac.tx_buf[idx], pkt, len);
    if (len < 60) {
        memset(g_gmac.tx_buf[idx] + len, 0, 60 - len);
        len = 60;
    }

    desc->des2 = (uint32_t)len;
    desc->des3 = DESC3_OWN | DESC3_FD | DESC3_LD | DESC2_IOC_BIT | (uint32_t)len;

    /* Update tail pointer to wake DMA */
    paddr_t desc_pa = va_to_pa((const void *)desc);
    gmac_write(base, DMA_CH0_TXDESC_TAIL_PTR, (uint32_t)desc_pa);

    g_gmac.tx_busy = (idx + 1) % GMAC_DESC_NUM;
    return 0;
}

int starfive_gmac_recv(uintptr_t base, void *buf, size_t maxlen) {
    if (!g_gmac.valid) return -1;

    uint32_t idx = g_gmac.rx_busy;
    dma_desc_t *desc = &g_gmac.rx_desc[idx];

    if (desc->des3 & DESC3_OWN) return -1; /* No packet ready */

    uint32_t len = (desc->des3 & 0x7FFF) - 4; /* strip FCS */
    if (len > maxlen) len = maxlen;
    if (len > 0) memcpy(buf, g_gmac.rx_buf[idx], len);

    /* Return descriptor to DMA */
    desc->des3 = DESC3_OWN | DESC3_BUF1V | DESC3_IOC | DESC3_FD | DESC3_LD;

    paddr_t tail_pa = va_to_pa((const void *)&g_gmac.rx_desc[(idx + GMAC_DESC_NUM - 1) % GMAC_DESC_NUM]);
    gmac_write(base, DMA_CH0_RXDESC_TAIL_PTR, (uint32_t)tail_pa);

    g_gmac.rx_busy = (idx + 1) % GMAC_DESC_NUM;
    return (int)len;
}

void starfive_gmac_get_mac(uintptr_t base, uint8_t *mac) {
    (void)base;
    memcpy(mac, g_gmac.mac, 6);
}

int starfive_gmac_poll(uintptr_t base) {
    uint32_t status = gmac_read(base, DMA_CH0_STATUS);
    if (status) {
        gmac_write(base, DMA_CH0_STATUS, status);
    }
    return 0;
}

/* ============================================================
 * driver_t integration
 * ============================================================ */

typedef struct {
    gmac_priv_t    gmac;
    net_dev_ops_t  ops;
} starfive_gmac_drv_t;

static starfive_gmac_drv_t g_gmac_drv;

static int starfive_gmac_driver_probe(device_t *dev) {
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res) return -1;

    if (starfive_gmac_init(res->start) != 0) {
        kinfo("[StarFive-GMAC] Failed to init at 0x%lx\n", (unsigned long)res->start);
        return -1;
    }

    dev->drv_priv = &g_gmac_drv;
    kinfo("[StarFive-GMAC] Probed '%s' at 0x%lx\n", dev->name, (unsigned long)res->start);
    return 0;
}

static int starfive_gmac_driver_remove(device_t *dev) {
    (void)dev;
    return 0;
}

static int starfive_gmac_class_send(struct device *dev, const void *pkt, size_t len) {
    starfive_gmac_drv_t *priv = (starfive_gmac_drv_t *)dev->drv_priv;
    return starfive_gmac_send(priv->gmac.base, pkt, len);
}

static int starfive_gmac_class_recv(struct device *dev, void *buf, size_t maxlen) {
    starfive_gmac_drv_t *priv = (starfive_gmac_drv_t *)dev->drv_priv;
    return starfive_gmac_recv(priv->gmac.base, buf, maxlen);
}

static const uint8_t *starfive_gmac_class_mac(struct device *dev) {
    starfive_gmac_drv_t *priv = (starfive_gmac_drv_t *)dev->drv_priv;
    return priv->gmac.mac;
}

static void starfive_gmac_class_poll(struct device *dev) {
    starfive_gmac_drv_t *priv = (starfive_gmac_drv_t *)dev->drv_priv;
    starfive_gmac_poll(priv->gmac.base);
}

static net_dev_ops_t starfive_gmac_net_ops = {
    .send = starfive_gmac_class_send,
    .recv = starfive_gmac_class_recv,
    .mac  = starfive_gmac_class_mac,
    .poll = starfive_gmac_class_poll,
};

static driver_t starfive_gmac_driver = {
    .name       = "starfive-gmac",
    .id_table   = NULL,
    .bus        = NULL,
    .probe      = starfive_gmac_driver_probe,
    .remove     = starfive_gmac_driver_remove,
    .class_ops  = &starfive_gmac_net_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(starfive_gmac_driver);
