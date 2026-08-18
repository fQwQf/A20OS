/*
 * A20OS Loongson-2K GMAC driver (DesignWare stmmac-class, "snps,dwmac-3.710").
 *
 * Developed for the Loongson LS2K1000 board, referencing RocketOS (MIT)
 * board/driver bring-up.  See docs/ACKNOWLEDGMENTS.md and
 * docs/platforms/physical-boards.md.
 *
 * LOCK_ORDER: each instance owns a private spinlock (g_ls2k_gmac_insts[i].lock)
 * serializing descriptor-ring access in send/recv/poll.  The data path is
 * poll-driven; the MMIO base is the identity-mapped physical address the board
 * provides (0x40040000 on the LS2K1000).  The descriptor bit layout follows
 * the pre-existing A20OS port and must be confirmed against the 2K1000's
 * stmmac variant before any IRQ-driven rework.
 */

#include "drivers/net/ls2k_gmac.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "mm/mm.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/lock.h"
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

#define MII_BMCR         0
#define MII_BMSR         1
#define MII_PHYSID1      2
#define MII_PHYSID2      3
#define BMCR_RESET       (1U << 15)
#define BMCR_ANENABLE    (1U << 12)
#define BMCR_ANRESTART   (1U << 9)
#define BMSR_LINK_STATUS (1U << 2)

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
#define DMA_CONTROL_SR    (1U << 1)
#define DMA_CONTROL_ST    (1U << 13)
#define DMA_CONTROL_TSF   (1U << 21)

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

#define TX_DESC_OWN (1U << 31)
#define TX_DESC_IC  (1U << 30)
#define TX_DESC_LS  (1U << 29)
#define TX_DESC_FS  (1U << 28)
#define TX_DESC_TER (1U << 21)
#define RX_DESC_OWN (1U << 31)
#define RX_DESC_ES  (1U << 15)
#define RX_DESC_RER (1U << 15)
#define RX_DESC_FRAME_LEN(status) (((status) >> 16) & 0x3FFFU)

#define GMAC_LINK_POLL_HZ 4U

#define GMAC_MAX_INSTANCES 4

typedef struct {
    uintptr_t base;
    int       valid;
    spinlock_t lock;
    gmac_desc_t tx_desc[GMAC_DESC_NUM] ALIGNED(16);
    gmac_desc_t rx_desc[GMAC_DESC_NUM] ALIGNED(16);
    uint8_t     tx_buf[GMAC_DESC_NUM][GMAC_BUF_SIZE] ALIGNED(64);
    uint8_t     rx_buf[GMAC_DESC_NUM][GMAC_BUF_SIZE] ALIGNED(64);
    uint32_t    tx_busy;
    uint32_t    rx_busy;
    uint8_t     mac[6];
    int         phy_addr;
    uint32_t    phy_id;
    int         link_up;
    uint64_t    last_link_check;
} ls2k_gmac_priv_t;

static ls2k_gmac_priv_t g_ls2k_gmac_insts[GMAC_MAX_INSTANCES];

static ls2k_gmac_priv_t *gmac_instance(uintptr_t base)
{
    for (int i = 0; i < GMAC_MAX_INSTANCES; i++) {
        if (g_ls2k_gmac_insts[i].valid && g_ls2k_gmac_insts[i].base == base)
            return &g_ls2k_gmac_insts[i];
    }
    return NULL;
}

static ls2k_gmac_priv_t *gmac_alloc_instance(uintptr_t base)
{
    ls2k_gmac_priv_t *inst = gmac_instance(base);
    if (inst)
        return inst;
    for (int i = 0; i < GMAC_MAX_INSTANCES; i++) {
        if (!g_ls2k_gmac_insts[i].valid) {
            inst = &g_ls2k_gmac_insts[i];
            memset(inst, 0, sizeof(*inst));
            inst->base = base;
            inst->phy_addr = -1;
            inst->valid = 1;
            spin_init(&inst->lock);
            return inst;
        }
    }
    return NULL;
}

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

static int gmac_mdio_write(uintptr_t base, int phy_addr, int reg, uint16_t data) {
    if (gmac_mdio_wait(base) != 0)
        return -1;
    gmac_write(base, MAC_MII_DATA, data);
    uint32_t val = MII_ADDR_GB | MII_ADDR_MW |
                   (phy_addr << MII_ADDR_PA_SHIFT) |
                   (reg << MII_ADDR_GR_SHIFT) |
                   (4 << MII_ADDR_CR_SHIFT);
    gmac_write(base, MAC_MII_ADDR, val);
    return gmac_mdio_wait(base);
}

static int ls2k_gmac_read_link(uintptr_t base, int phy_addr)
{
    /* BMSR link is latched low, so the current value is the second read. */
    (void)gmac_mdio_read(base, phy_addr, MII_BMSR);
    uint16_t bmsr = gmac_mdio_read(base, phy_addr, MII_BMSR);
    return bmsr != 0xFFFFU && (bmsr & BMSR_LINK_STATUS) != 0;
}

static void ls2k_gmac_init_desc(uintptr_t base, ls2k_gmac_priv_t *priv) {
    memset(priv->tx_desc, 0, sizeof(priv->tx_desc));
    memset(priv->rx_desc, 0, sizeof(priv->rx_desc));

    paddr_t tx_desc_pa = va_to_pa((const void *)priv->tx_desc);
    paddr_t rx_desc_pa = va_to_pa((const void *)priv->rx_desc);

    for (int i = 0; i < GMAC_DESC_NUM; i++) {
        paddr_t tx_buf_pa = va_to_pa((const void *)priv->tx_buf[i]);
        paddr_t rx_buf_pa = va_to_pa((const void *)priv->rx_buf[i]);

        priv->tx_desc[i].status = i == GMAC_DESC_NUM - 1 ? TX_DESC_TER : 0;
        priv->tx_desc[i].buffer1 = (uint32_t)tx_buf_pa;

        priv->rx_desc[i].status = RX_DESC_OWN;
        priv->rx_desc[i].length = GMAC_BUF_SIZE |
                                  (i == GMAC_DESC_NUM - 1 ? RX_DESC_RER : 0);
        priv->rx_desc[i].buffer1 = (uint32_t)rx_buf_pa;
    }
    dma_sync_for_device(priv->rx_buf, sizeof(priv->rx_buf));
    dma_sync_for_device(priv->tx_desc, sizeof(priv->tx_desc));
    dma_sync_for_device(priv->rx_desc, sizeof(priv->rx_desc));

    gmac_write(base, DMA_TX_BASE_ADDR, (uint32_t)tx_desc_pa);
    gmac_write(base, DMA_RX_BASE_ADDR, (uint32_t)rx_desc_pa);

    priv->tx_busy = 0;
    priv->rx_busy = 0;
}

static int ls2k_gmac_phy_init(uintptr_t base, ls2k_gmac_priv_t *priv) {
    int phy_addr = -1;
    uint16_t id1, id2;
    uint32_t phy_id;

    for (int i = 0; i < 32; i++) {
        id1 = gmac_mdio_read(base, i, MII_PHYSID1);
        id2 = gmac_mdio_read(base, i, MII_PHYSID2);
        if (id1 == 0xFFFF && id2 == 0xFFFF)
            continue;
        phy_id = ((uint32_t)id1 << 16) | id2;
        if ((phy_id & 0x1FFFFFFF) == 0x1FFFFFFF)
            continue;
        phy_addr = i;
        priv->phy_addr = i;
        priv->phy_id = phy_id;
        kinfo("[LS2K-GMAC] PHY 0x%02x id 0x%08x\n", i, phy_id);
        break;
    }
    if (phy_addr < 0) {
        kinfo("[LS2K-GMAC] no PHY on MDIO\n");
        return -1;
    }

    if (gmac_mdio_write(base, phy_addr, MII_BMCR, BMCR_RESET) != 0)
        return -1;
    uint64_t start = timer_get_ticks();
    while (1) {
        uint16_t val = gmac_mdio_read(base, phy_addr, MII_BMCR);
        if (!(val & BMCR_RESET))
            break;
        if (timer_get_ticks() - start > clock_ticks_per_sec() * 2) {
            kinfo("[LS2K-GMAC] PHY reset timeout\n");
            return -1;
        }
    }

    if (gmac_mdio_write(base, phy_addr, MII_BMCR,
                        BMCR_ANENABLE | BMCR_ANRESTART) != 0)
        return -1;
    mdelay(100);

    int link_up = ls2k_gmac_read_link(base, phy_addr);
    __atomic_store_n(&priv->link_up, link_up, __ATOMIC_RELEASE);
    priv->last_link_check = timer_get_ticks();
    kinfo("[LS2K-GMAC] PHY carrier %s at addr %d%s\n",
          link_up ? "up" : "down", phy_addr,
          link_up ? "" : "; continuing without cable");
    return 0;
}

static int ls2k_gmac_dma_reset(uintptr_t base)
{
    gmac_write(base, DMA_BUS_MODE, DMA_BUS_MODE_SWR);
    uint64_t start = timer_get_ticks();
    while (gmac_read(base, DMA_BUS_MODE) & DMA_BUS_MODE_SWR) {
        if (timer_get_ticks() - start > clock_ticks_per_sec()) {
            kinfo("[LS2K-GMAC] DMA reset timeout\n");
            return -1;
        }
    }
    return 0;
}

int ls2k_gmac_init(uintptr_t base) {
    ls2k_gmac_priv_t *priv = gmac_alloc_instance(base);
    if (!priv)
        return -1;
    memcpy(priv->mac, (uint8_t[]){0x00, 0x55, 0x7B, 0xB5, 0x7D, 0xF7}, 6);

    uint32_t version = gmac_read(base, MAC_VERSION);
    kinfo("[LS2K-GMAC] MAC version 0x%08x\n", version);
    if (ls2k_gmac_dma_reset(base) != 0)
        return -1;
    mdelay(10);

    gmac_write(base, MAC_CONFIGURATION, 0);
    ls2k_gmac_init_desc(base, priv);

    gmac_write(base, MAC_FRAME_FILTER, 0x80000001);
    gmac_write(base, MAC_FLOW_CTRL, 0);
    gmac_write(base, MAC_INTERRUPT_MASK, 0xFFFFFFFFU);

    uint32_t high = (priv->mac[5] << 8) | priv->mac[4] | (1U << 31);
    uint32_t low  = (priv->mac[3] << 24) | (priv->mac[2] << 16) |
                    (priv->mac[1] << 8)  | priv->mac[0];
    gmac_write(base, MAC_ADDR0_HIGH, high);
    gmac_write(base, MAC_ADDR0_LOW, low);

    if (ls2k_gmac_phy_init(base, priv) != 0)
        return -1;

    gmac_write(base, MAC_CONFIGURATION,
               MAC_CONF_RE | MAC_CONF_TE | MAC_CONF_DM | MAC_CONF_IPC);

    /* Polling is the deliberate Phase 3 baseline. Device IRQ sources remain
     * masked in LIOINTC until the data path is validated with a cable. */
    gmac_write(base, DMA_INTR_ENABLE, 0);
    gmac_write(base, DMA_STATUS, 0xFFFFFFFFU);
    gmac_write(base, DMA_CONTROL,
               DMA_CONTROL_TSF | DMA_CONTROL_ST | DMA_CONTROL_SR);

    kinfo("[LS2K-GMAC] Initialized at 0x%lx\n", (unsigned long)base);
    return 0;
}

int ls2k_gmac_send(uintptr_t base, const void *pkt, size_t len) {
    ls2k_gmac_priv_t *priv = gmac_instance(base);
    if (!priv || !pkt || len == 0 || len > GMAC_BUF_SIZE - 4)
        return -1;

    uint64_t flags = spin_lock_irqsave(&priv->lock);
    if (!__atomic_load_n(&priv->link_up, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&priv->lock, flags);
        return -1;
    }
    uint32_t idx = priv->tx_busy;
    gmac_desc_t *desc = &priv->tx_desc[idx];

    dma_sync_for_cpu(desc, sizeof(*desc));
    if (desc->status & TX_DESC_OWN) {
        spin_unlock_irqrestore(&priv->lock, flags);
        return -1;
    }

    size_t packet_len = len;
    size_t dma_len = len;
    memcpy(priv->tx_buf[idx], pkt, dma_len);
    if (dma_len < 60) {
        memset(priv->tx_buf[idx] + dma_len, 0, 60 - dma_len);
        dma_len = 60;
    }
    dma_sync_for_device(priv->tx_buf[idx], dma_len);

    desc->length = (uint32_t)dma_len;
    __sync_synchronize();
    desc->status = TX_DESC_OWN | TX_DESC_IC | TX_DESC_FS | TX_DESC_LS |
                   (idx == GMAC_DESC_NUM - 1 ? TX_DESC_TER : 0);
    dma_sync_for_device(desc, sizeof(*desc));

    gmac_write(base, DMA_TX_POLL_DEMAND, 1);

    priv->tx_busy = (idx + 1) % GMAC_DESC_NUM;
    spin_unlock_irqrestore(&priv->lock, flags);
    return (int)packet_len;
}

int ls2k_gmac_recv(uintptr_t base, void *buf, size_t maxlen) {
    ls2k_gmac_priv_t *priv = gmac_instance(base);
    if (!priv || !buf || maxlen == 0)
        return -1;

    uint64_t flags = spin_lock_irqsave(&priv->lock);
    if (!__atomic_load_n(&priv->link_up, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&priv->lock, flags);
        return -1;
    }
    uint32_t idx = priv->rx_busy;
    gmac_desc_t *desc = &priv->rx_desc[idx];

    dma_sync_for_cpu(desc, sizeof(*desc));
    if (desc->status & RX_DESC_OWN) {
        spin_unlock_irqrestore(&priv->lock, flags);
        return -1;
    }

    dma_sync_for_cpu(priv->rx_buf[idx], GMAC_BUF_SIZE);

    if (desc->status & RX_DESC_ES) {
        dma_sync_for_device(priv->rx_buf[idx], GMAC_BUF_SIZE);
        desc->length = GMAC_BUF_SIZE |
                       (idx == GMAC_DESC_NUM - 1 ? RX_DESC_RER : 0);
        desc->status = RX_DESC_OWN;
        dma_sync_for_device(desc, sizeof(*desc));
        priv->rx_busy = (idx + 1) % GMAC_DESC_NUM;
        gmac_write(base, DMA_RX_POLL_DEMAND, 1);
        spin_unlock_irqrestore(&priv->lock, flags);
        return -1;
    }

    uint32_t frame_len = RX_DESC_FRAME_LEN(desc->status);
    uint32_t len = frame_len >= 4 ? frame_len - 4 : 0;
    if (len > maxlen) len = maxlen;
    if (len > 0) memcpy(buf, priv->rx_buf[idx], len);

    dma_sync_for_device(priv->rx_buf[idx], GMAC_BUF_SIZE);
    desc->length = GMAC_BUF_SIZE |
                   (idx == GMAC_DESC_NUM - 1 ? RX_DESC_RER : 0);
    desc->status = RX_DESC_OWN;
    dma_sync_for_device(desc, sizeof(*desc));
    gmac_write(base, DMA_RX_POLL_DEMAND, 1);

    priv->rx_busy = (idx + 1) % GMAC_DESC_NUM;
    spin_unlock_irqrestore(&priv->lock, flags);
    return (int)len;
}

void ls2k_gmac_get_mac(uintptr_t base, uint8_t *mac) {
    ls2k_gmac_priv_t *priv = gmac_instance(base);
    if (priv)
        memcpy(mac, priv->mac, 6);
}

int ls2k_gmac_poll(uintptr_t base) {
    ls2k_gmac_priv_t *priv = gmac_instance(base);
    if (!priv)
        return -1;

    uint64_t flags = spin_lock_irqsave(&priv->lock);
    uint32_t status = gmac_read(base, DMA_STATUS);
    if (status)
        gmac_write(base, DMA_STATUS, status);
    uint64_t now = timer_get_ticks();
    uint64_t interval = clock_ticks_per_sec() / GMAC_LINK_POLL_HZ;
    if (priv->phy_addr >= 0 && now - priv->last_link_check >= interval) {
        int link_up = ls2k_gmac_read_link(base, priv->phy_addr);
        priv->last_link_check = now;
        int old_link_up = __atomic_load_n(&priv->link_up, __ATOMIC_ACQUIRE);
        if (link_up != old_link_up) {
            __atomic_store_n(&priv->link_up, link_up, __ATOMIC_RELEASE);
            kinfo("[LS2K-GMAC] PHY carrier %s (rgmii=0x%08x)\n",
                  link_up ? "up" : "down",
                  gmac_read(base, MAC_RGMII_STATUS));
        }
    }
    spin_unlock_irqrestore(&priv->lock, flags);
    return 0;
}

/* ============================================================
 * driver_t integration
 * ============================================================ */

static int ls2k_gmac_driver_probe(device_t *dev) {
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res)
        return -1;

    if (ls2k_gmac_init(res->start) != 0) {
        kinfo("[LS2K-GMAC] Failed to init at 0x%lx\n", (unsigned long)res->start);
        return -1;
    }

    ls2k_gmac_priv_t *priv = gmac_instance(res->start);
    dev->drv_priv = priv;
    kinfo("[LS2K-GMAC] Probed '%s' at 0x%lx\n", dev->name, (unsigned long)res->start);
    return 0;
}

static int ls2k_gmac_driver_remove(device_t *dev) {
    ls2k_gmac_priv_t *priv = (ls2k_gmac_priv_t *)dev->drv_priv;
    if (priv)
        priv->valid = 0;
    dev->drv_priv = NULL;
    return 0;
}

static int ls2k_gmac_class_send(struct device *dev, const void *pkt, size_t len) {
    ls2k_gmac_priv_t *priv = (ls2k_gmac_priv_t *)dev->drv_priv;
    if (!priv)
        return -1;
    return ls2k_gmac_send(priv->base, pkt, len);
}

static int ls2k_gmac_class_recv(struct device *dev, void *buf, size_t maxlen) {
    ls2k_gmac_priv_t *priv = (ls2k_gmac_priv_t *)dev->drv_priv;
    if (!priv)
        return -1;
    return ls2k_gmac_recv(priv->base, buf, maxlen);
}

static const uint8_t *ls2k_gmac_class_mac(struct device *dev) {
    ls2k_gmac_priv_t *priv = (ls2k_gmac_priv_t *)dev->drv_priv;
    return priv ? priv->mac : NULL;
}

static int ls2k_gmac_class_link_up(struct device *dev) {
    ls2k_gmac_priv_t *priv = (ls2k_gmac_priv_t *)dev->drv_priv;
    return priv ? __atomic_load_n(&priv->link_up, __ATOMIC_ACQUIRE) : 0;
}

static void ls2k_gmac_class_poll(struct device *dev) {
    ls2k_gmac_priv_t *priv = (ls2k_gmac_priv_t *)dev->drv_priv;
    if (priv)
        ls2k_gmac_poll(priv->base);
}

static net_dev_ops_t ls2k_gmac_net_ops = {
    .send = ls2k_gmac_class_send,
    .recv = ls2k_gmac_class_recv,
    .mac  = ls2k_gmac_class_mac,
    .poll = ls2k_gmac_class_poll,
    .link_up = ls2k_gmac_class_link_up,
};

static const device_id_t ls2k_gmac_ids[] = {
    { .vendor = LS2K_GMAC_PLATFORM_VENDOR, .device = LS2K_GMAC_PLATFORM_DEVICE,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t ls2k_gmac_driver = {
    .name       = "ls2k-gmac",
    .id_table   = ls2k_gmac_ids,
    .bus        = &platform_bus,
    .probe      = ls2k_gmac_driver_probe,
    .remove     = ls2k_gmac_driver_remove,
    .class_ops  = &ls2k_gmac_net_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(ls2k_gmac_driver);
