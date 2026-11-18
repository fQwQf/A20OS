/*
 * A20OS Loongson-2K GMAC driver (DesignWare stmmac-class, "snps,dwmac-3.710").
 *
 * Developed for the Loongson LS2K1000 board, referencing RocketOS (MIT)
 * board/driver bring-up.  See docs/zh/ACKNOWLEDGMENTS.md and
 * docs/zh/platforms/physical-boards.md.
 *
 * LOCK_ORDER: control_lock serializes task-context PHY/control accesses;
 * lock protects short descriptor operations. Stop closes admission before
 * trying either lock, and never polls hardware with interrupts disabled.
 * A managed execution action pins the static DMA slot until checked stop,
 * class drain and remove have all succeeded. The polling data path uses the
 * LS2K1000 DWMAC 3.7 enhanced four-word descriptor format in chained mode.
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
#include "core/sync.h"
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

/* This controller is the older DWMAC revision, so MAC_MII_ADDR is
 * { GB[0], MW[1], CR[5:2], reserved, GR[10:6], PA[15:11] }: GB busy, MW
 * write-enable, CR a range selector for the MDC divider, GR the register index
 * and PA the PHY address.  There is no GOC opcode field on this variant at all
 * -- the newer EQOS part in kernel/drivers/net/starfive_gmac.c has one, at
 * different bit positions for every field here -- so a read returns its payload
 * in MAC_MII_DATA once GB clears, with no "post" form to wait for and no code
 * shared with the other driver. */
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
#define DMA_STATUS_AIS    (1U << 15)
#define DMA_STATUS_NIS    (1U << 16)
#define DMA_STATUS_PROCESS_MASK ((7U << 20) | (7U << 17))

/* DMA_BUS_MODE[0] SWR is a self-clearing reset: the DMA clears it once the
 * reset has completed, and every DMA register holds its reset value at that
 * point, which is why the descriptor base addresses programmed after
 * ls2k_gmac_dma_reset() are the ones that actually reach the hardware. */
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
#define GMAC_WAIT_LOOPS 1000000U

typedef struct {
    uint32_t status;
    uint32_t length;
    uint32_t buffer1;
    uint32_t buffer2;
    /* Separate CPU-owned and device-owned descriptors by a full LS2K cache
     * line. Chained buffer2 addresses, not array stride, advance the DMA. */
    uint32_t padding[12];
} gmac_desc_t;

_Static_assert(sizeof(gmac_desc_t) == 64, "GMAC descriptor cache isolation");

#define TX_DESC_OWN (1U << 31)
#define TX_DESC_IC  (1U << 30)
#define TX_DESC_LS  (1U << 29)
#define TX_DESC_FS  (1U << 28)
#define TX_DESC_TCH (1U << 20)
#define RX_DESC_OWN (1U << 31)
#define RX_DESC_ES  (1U << 15)
#define RX_DESC_RCH (1U << 14)
#define RX_DESC_FS  (1U << 9)
#define RX_DESC_LS  (1U << 8)
#define RX_DESC_FRAME_LEN(status) (((status) >> 16) & 0x3FFFU)

/* Link is re-read at 4 Hz rather than on every poll because each check costs
 * two MDIO transactions (the BMSR double read in ls2k_gmac_read_link) and MDIO
 * has no completion interrupt of its own; this divider is what keeps the shared
 * MDIO bus from becoming the reason the data path is late. */
#define GMAC_LINK_POLL_HZ 4U

#define GMAC_MAX_INSTANCES 4

typedef struct {
    uintptr_t base;
    int       valid;
    device_t *owner;
    int closing;
    int ready;
    int hardware_owned;
    int stopped;
    uint32_t version;
    mutex_t control_lock;
    spinlock_t lock;
    gmac_desc_t tx_desc[GMAC_DESC_NUM] ALIGNED(64);
    gmac_desc_t rx_desc[GMAC_DESC_NUM] ALIGNED(64);
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

static ls2k_gmac_priv_t *gmac_alloc_instance(device_t *owner)
{
    for (int i = 0; i < GMAC_MAX_INSTANCES; i++) {
        if (!g_ls2k_gmac_insts[i].valid) {
            ls2k_gmac_priv_t *inst = &g_ls2k_gmac_insts[i];
            memset(inst, 0, sizeof(*inst));
            inst->owner = owner;
            inst->phy_addr = -1;
            inst->valid = 1;
            spin_init(&inst->lock);
            mutex_init(&inst->control_lock);
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

/* All polling has both a timer deadline and a finite fallback for early boot
 * or a stalled clock. The control mutex, never the ring spinlock, owns MDIO. */
static int gmac_wait_clear(ls2k_gmac_priv_t *priv, uint32_t reg,
                           uint32_t mask, uint64_t timeout, int cleanup)
{
    uint64_t start = timer_get_ticks();
    for (unsigned i = 0; i < GMAC_WAIT_LOOPS; i++) {
        if (!cleanup && __atomic_load_n(&priv->closing, __ATOMIC_ACQUIRE))
            return -ESHUTDOWN;
        uint32_t value = gmac_read(priv->base, reg);
        if (value == 0xFFFFFFFFU)
            return -ENODEV;
        if (!(value & mask))
            return 0;
        if (timer_get_ticks() - start >= timeout)
            break;
    }
    return -ETIMEDOUT;
}

static int gmac_mdio_wait(ls2k_gmac_priv_t *priv)
{
    return gmac_wait_clear(priv, MAC_MII_ADDR, MII_ADDR_GB,
                           clock_ticks_per_sec() / 100 + 1, 0);
}

static uint16_t gmac_mdio_read(ls2k_gmac_priv_t *priv, int phy_addr, int reg)
{
    if (gmac_mdio_wait(priv) != 0)
        return 0xFFFF;
    uint32_t val = MII_ADDR_GB | (phy_addr << MII_ADDR_PA_SHIFT) |
                   (reg << MII_ADDR_GR_SHIFT) | (4 << MII_ADDR_CR_SHIFT);
    gmac_write(priv->base, MAC_MII_ADDR, val);
    if (gmac_mdio_wait(priv) != 0)
        return 0xFFFF;
    return (uint16_t)gmac_read(priv->base, MAC_MII_DATA);
}

static int gmac_mdio_write(ls2k_gmac_priv_t *priv, int phy_addr, int reg,
                            uint16_t data)
{
    int ret = gmac_mdio_wait(priv);
    if (ret)
        return ret;
    gmac_write(priv->base, MAC_MII_DATA, data);
    uint32_t val = MII_ADDR_GB | MII_ADDR_MW |
                   (phy_addr << MII_ADDR_PA_SHIFT) |
                   (reg << MII_ADDR_GR_SHIFT) | (4 << MII_ADDR_CR_SHIFT);
    gmac_write(priv->base, MAC_MII_ADDR, val);
    return gmac_mdio_wait(priv);
}

static int ls2k_gmac_read_link(ls2k_gmac_priv_t *priv)
{
    /* BMSR is latched low: the second read reports current carrier. A missing
     * PHY or failed MDIO returns all ones, never a live link. */
    (void)gmac_mdio_read(priv, priv->phy_addr, MII_BMSR);
    uint16_t bmsr = gmac_mdio_read(priv, priv->phy_addr, MII_BMSR);
    return bmsr != 0xFFFFU && (bmsr & BMSR_LINK_STATUS) != 0;
}

static void ls2k_gmac_init_desc(uintptr_t base, ls2k_gmac_priv_t *priv) {
    memset(priv->tx_desc, 0, sizeof(priv->tx_desc));
    memset(priv->rx_desc, 0, sizeof(priv->rx_desc));

    paddr_t tx_desc_pa = va_to_pa((const void *)priv->tx_desc);
    paddr_t rx_desc_pa = va_to_pa((const void *)priv->rx_desc);

    for (int i = 0; i < GMAC_DESC_NUM; i++) {
        int next = (i + 1) % GMAC_DESC_NUM;
        priv->tx_desc[i].status = TX_DESC_TCH;
        priv->tx_desc[i].buffer1 = (uint32_t)va_to_pa(priv->tx_buf[i]);
        priv->tx_desc[i].buffer2 = (uint32_t)va_to_pa(&priv->tx_desc[next]);
        priv->rx_desc[i].status = RX_DESC_OWN;
        priv->rx_desc[i].length = GMAC_BUF_SIZE | RX_DESC_RCH;
        priv->rx_desc[i].buffer1 = (uint32_t)va_to_pa(priv->rx_buf[i]);
        priv->rx_desc[i].buffer2 = (uint32_t)va_to_pa(&priv->rx_desc[next]);
    }
    dma_sync_for_device(priv->rx_buf, sizeof(priv->rx_buf));
    dma_sync_for_device(priv->tx_desc, sizeof(priv->tx_desc));
    dma_sync_for_device(priv->rx_desc, sizeof(priv->rx_desc));

    /* 32-bit physical addresses only.  This DMA revision has no descriptor-list
     * high-address register -- the EQOS part in starfive_gmac.c has a HADDR
     * pair for exactly this -- so a ring placed above 4 GiB cannot be
     * programmed at all. Probe validates every full DMA span before this point. */
    gmac_write(base, DMA_TX_BASE_ADDR, (uint32_t)tx_desc_pa);
    gmac_write(base, DMA_RX_BASE_ADDR, (uint32_t)rx_desc_pa);

    priv->tx_busy = 0;
    priv->rx_busy = 0;
}

static int ls2k_gmac_phy_init(ls2k_gmac_priv_t *priv) {
    int phy_addr = -1;
    uint16_t id1, id2;
    uint32_t phy_id;

    for (int i = 0; i < 32; i++) {
        if (__atomic_load_n(&priv->closing, __ATOMIC_ACQUIRE))
            return -ESHUTDOWN;
        id1 = gmac_mdio_read(priv, i, MII_PHYSID1);
        id2 = gmac_mdio_read(priv, i, MII_PHYSID2);
        if ((id1 == 0xFFFF && id2 == 0xFFFF) || (!id1 && !id2))
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

    if (gmac_mdio_write(priv, phy_addr, MII_BMCR, BMCR_RESET) != 0)
        return -1;
    uint64_t start = timer_get_ticks();
    for (unsigned i = 0; ; i++) {
        if (__atomic_load_n(&priv->closing, __ATOMIC_ACQUIRE))
            return -ESHUTDOWN;
        uint16_t val = gmac_mdio_read(priv, phy_addr, MII_BMCR);
        if (!(val & BMCR_RESET))
            break;
        if (i == GMAC_WAIT_LOOPS - 1 ||
            timer_get_ticks() - start >= clock_ticks_per_sec() * 2) {
            kinfo("[LS2K-GMAC] PHY reset timeout\n");
            return -1;
        }
    }

    if (gmac_mdio_write(priv, phy_addr, MII_BMCR,
                        BMCR_ANENABLE | BMCR_ANRESTART) != 0)
        return -1;
    mdelay(100);

    int link_up = ls2k_gmac_read_link(priv);
    __atomic_store_n(&priv->link_up, link_up, __ATOMIC_RELEASE);
    priv->last_link_check = timer_get_ticks();
    kinfo("[LS2K-GMAC] PHY carrier %s at addr %d%s\n",
          link_up ? "up" : "down", phy_addr,
          link_up ? "" : "; continuing without cable");
    return 0;
}

static int ls2k_gmac_dma_reset(ls2k_gmac_priv_t *priv, int cleanup)
{
    gmac_write(priv->base, DMA_BUS_MODE, DMA_BUS_MODE_SWR);
    return gmac_wait_clear(priv, DMA_BUS_MODE, DMA_BUS_MODE_SWR,
                           clock_ticks_per_sec() / 10 + 1, cleanup);
}

static int ls2k_gmac_hw_init(ls2k_gmac_priv_t *priv)
{
    uintptr_t base = priv->base;
    memcpy(priv->mac, (uint8_t[]){0x00, 0x55, 0x7B, 0xB5, 0x7D, 0xF7}, 6);
    priv->version = gmac_read(base, MAC_VERSION);
    /* The board contract is DWMAC 3.7, not EQOS or an absent register file. */
    if (priv->version == 0xFFFFFFFFU || (priv->version & 0xFFU) != 0x37U)
        return -ENODEV;
    priv->hardware_owned = 1;
    int ret = ls2k_gmac_dma_reset(priv, 0);
    if (ret)
        return ret;
    gmac_write(base, MAC_CONFIGURATION, 0);
    ls2k_gmac_init_desc(base, priv);

    /* Preserve the existing receive-all (bit 31) / promiscuous (bit 0)
     * bring-up policy. Address filtering is independent of DMA ownership. */
    gmac_write(base, MAC_FRAME_FILTER, 0x80000001);
    gmac_write(base, MAC_FLOW_CTRL, 0);
    /* MAC interrupt mask uses one to mask; DMA interrupt enable uses zero. */
    gmac_write(base, MAC_INTERRUPT_MASK, 0xFFFFFFFFU);

    uint32_t high = (priv->mac[5] << 8) | priv->mac[4] | (1U << 31);
    uint32_t low  = ((uint32_t)priv->mac[3] << 24) | ((uint32_t)priv->mac[2] << 16) |
                    ((uint32_t)priv->mac[1] << 8)  | priv->mac[0];
    gmac_write(base, MAC_ADDR0_HIGH, high);
    gmac_write(base, MAC_ADDR0_LOW, low);

    ret = ls2k_gmac_phy_init(priv);
    if (ret)
        return ret;
    if (__atomic_load_n(&priv->closing, __ATOMIC_ACQUIRE))
        return -ESHUTDOWN;

    gmac_write(base, MAC_CONFIGURATION,
               MAC_CONF_RE | MAC_CONF_TE | MAC_CONF_DM | MAC_CONF_IPC);

    /* Polling only. ST/SR start the engines; TSF selects TX store-and-forward. */
    gmac_write(base, DMA_INTR_ENABLE, 0);
    ret = gmac_wait_clear(priv, DMA_INTR_ENABLE, 0xFFFFFFFFU,
                           clock_ticks_per_sec() / 10 + 1, 0);
    if (ret)
        return ret;
    gmac_write(base, DMA_STATUS, 0x1FFFFU);
    gmac_write(base, DMA_CONTROL,
               DMA_CONTROL_TSF | DMA_CONTROL_ST | DMA_CONTROL_SR);
    uint32_t control = gmac_read(base, DMA_CONTROL);
    uint32_t mac = gmac_read(base, MAC_CONFIGURATION);
    if (control == 0xFFFFFFFFU || mac == 0xFFFFFFFFU)
        return -ENODEV;
    if ((control & (DMA_CONTROL_ST | DMA_CONTROL_SR)) !=
        (DMA_CONTROL_ST | DMA_CONTROL_SR) ||
        (mac & (MAC_CONF_RE | MAC_CONF_TE)) != (MAC_CONF_RE | MAC_CONF_TE))
        return -EIO;
    __atomic_store_n(&priv->ready, 1, __ATOMIC_RELEASE);

    kinfo("[LS2K-GMAC] Initialized at 0x%lx\n", (unsigned long)base);
    return 0;
}

int ls2k_gmac_send(uintptr_t base, const void *pkt, size_t len) {
    ls2k_gmac_priv_t *priv = gmac_instance(base);
    if (!priv || !pkt || len == 0 || len > GMAC_BUF_SIZE - 4)
        return -1;

    uint64_t flags = spin_lock_irqsave(&priv->lock);
    if (__atomic_load_n(&priv->closing, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&priv->ready, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&priv->link_up, __ATOMIC_ACQUIRE)) {
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
    /* 60 is the Ethernet minimum frame size (64 bytes) minus the 4-byte FCS the
     * MAC appends; a shorter frame on copper is dropped by the link partner and
     * shows up only as a transmit with no matching receive. */
    if (dma_len < 60) {
        memset(priv->tx_buf[idx] + dma_len, 0, 60 - dma_len);
        dma_len = 60;
    }
    dma_sync_for_device(priv->tx_buf[idx], dma_len);

    desc->length = (uint32_t)dma_len;
    __sync_synchronize();
    /* Enhanced four-word format: controls live in word 0. IC requests an
     * interrupt, not a checksum; polling needs no IC. Keep chained ownership. */
    desc->status = TX_DESC_OWN | TX_DESC_FS | TX_DESC_LS | TX_DESC_TCH;
    dma_sync_for_device(desc, sizeof(*desc));

    /* This DMA revision has no tail pointer: writing the poll-demand register
     * is what makes the DMA re-read the ring base and pick the descriptor up, so
     * this store is also the publish step for the descriptor writes above. */
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
    if (__atomic_load_n(&priv->closing, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&priv->ready, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&priv->link_up, __ATOMIC_ACQUIRE)) {
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

    uint32_t status = desc->status;
    uint32_t frame_len = RX_DESC_FRAME_LEN(status);
    int result = -EIO;
    /* One buffer must contain a complete frame including its four-byte FCS.
     * Reject bad device lengths before synchronizing or touching the buffer. */
    if (!(status & RX_DESC_ES) &&
        (status & (RX_DESC_FS | RX_DESC_LS)) == (RX_DESC_FS | RX_DESC_LS) &&
        frame_len >= 4 && frame_len <= GMAC_BUF_SIZE) {
        size_t len = frame_len - 4;
        if (len > maxlen)
            len = maxlen;
        if (len) {
            dma_sync_for_cpu(priv->rx_buf[idx], len);
            memcpy(buf, priv->rx_buf[idx], len);
        }
        result = (int)len;
    }
    dma_sync_for_device(priv->rx_buf[idx], GMAC_BUF_SIZE);
    desc->length = GMAC_BUF_SIZE | RX_DESC_RCH;
    desc->buffer1 = (uint32_t)va_to_pa(priv->rx_buf[idx]);
    desc->buffer2 = (uint32_t)va_to_pa(&priv->rx_desc[(idx + 1) % GMAC_DESC_NUM]);
    __sync_synchronize();
    desc->status = RX_DESC_OWN;
    dma_sync_for_device(desc, sizeof(*desc));
    gmac_write(base, DMA_RX_POLL_DEMAND, 1);
    priv->rx_busy = (idx + 1) % GMAC_DESC_NUM;
    spin_unlock_irqrestore(&priv->lock, flags);
    return result;
}

void ls2k_gmac_get_mac(uintptr_t base, uint8_t *mac) {
    ls2k_gmac_priv_t *priv = gmac_instance(base);
    if (priv && mac && !__atomic_load_n(&priv->closing, __ATOMIC_ACQUIRE))
        memcpy(mac, priv->mac, 6);
}

int ls2k_gmac_poll(uintptr_t base) {
    ls2k_gmac_priv_t *priv = gmac_instance(base);
    if (!priv)
        return -1;

    if (!mutex_trylock(&priv->control_lock))
        return -EBUSY;
    if (__atomic_load_n(&priv->closing, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&priv->ready, __ATOMIC_ACQUIRE)) {
        mutex_unlock(&priv->control_lock);
        return -ESHUTDOWN;
    }
    uint32_t status = gmac_read(base, DMA_STATUS);
    if (status == 0xFFFFFFFFU) {
        mutex_unlock(&priv->control_lock);
        return -ENODEV;
    }
    if (status & 0x1FFFFU)
        gmac_write(base, DMA_STATUS, status & 0x1FFFFU);
    uint64_t now = timer_get_ticks();
    uint64_t interval = clock_ticks_per_sec() / GMAC_LINK_POLL_HZ;
    if (priv->phy_addr >= 0 && now - priv->last_link_check >= interval) {
        int link_up = ls2k_gmac_read_link(priv);
        priv->last_link_check = now;
        __atomic_store_n(&priv->link_up, link_up, __ATOMIC_RELEASE);
    }
    mutex_unlock(&priv->control_lock);
    return 0;
}

/* ============================================================
 * driver_t integration
 * ============================================================ */

static int ls2k_gmac_stop(void *context)
{
    ls2k_gmac_priv_t *priv = context;
    __atomic_store_n(&priv->closing, 1, __ATOMIC_RELEASE);
    if (!mutex_trylock(&priv->control_lock))
        return -EBUSY;
    uint64_t flags;
    if (!spin_trylock_irqsave(&priv->lock, &flags)) {
        mutex_unlock(&priv->control_lock);
        return -EBUSY;
    }
    __atomic_store_n(&priv->ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&priv->link_up, 0, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&priv->lock, flags);

    int ret = 0;
    if (!priv->stopped && priv->hardware_owned) {
        uintptr_t base = priv->base;
        if (gmac_read(base, MAC_VERSION) != priv->version) {
            ret = -ENODEV;
            goto out;
        }
        /* Do not release on a posted store alone. A refused mask, live DMA
         * process, inaccessible register or stuck reset retains this slot. */
        gmac_write(base, DMA_INTR_ENABLE, 0);
        ret = gmac_wait_clear(priv, DMA_INTR_ENABLE, 0xFFFFFFFFU,
                               clock_ticks_per_sec() / 10 + 1, 1);
        if (ret)
            goto out;
        uint32_t control = gmac_read(base, DMA_CONTROL);
        uint32_t mac = gmac_read(base, MAC_CONFIGURATION);
        if (control == 0xFFFFFFFFU || mac == 0xFFFFFFFFU) {
            ret = -ENODEV;
            goto out;
        }
        gmac_write(base, DMA_CONTROL, control & ~(DMA_CONTROL_ST | DMA_CONTROL_SR));
        gmac_write(base, MAC_CONFIGURATION, mac & ~(MAC_CONF_TE | MAC_CONF_RE));
        ret = gmac_wait_clear(priv, DMA_CONTROL, DMA_CONTROL_ST | DMA_CONTROL_SR,
                               clock_ticks_per_sec() / 10 + 1, 1);
        if (!ret)
            ret = gmac_wait_clear(priv, MAC_CONFIGURATION, MAC_CONF_TE | MAC_CONF_RE,
                                   clock_ticks_per_sec() / 10 + 1, 1);
        if (!ret)
            ret = gmac_wait_clear(priv, DMA_STATUS, DMA_STATUS_PROCESS_MASK,
                                   clock_ticks_per_sec() / 10 + 1, 1);
        if (!ret)
            ret = ls2k_gmac_dma_reset(priv, 1);
        if (!ret)
            ret = gmac_wait_clear(priv, DMA_INTR_ENABLE, 0xFFFFFFFFU,
                                   clock_ticks_per_sec() / 10 + 1, 1);
        if (!ret)
            ret = gmac_wait_clear(priv, DMA_CONTROL, DMA_CONTROL_ST | DMA_CONTROL_SR,
                                   clock_ticks_per_sec() / 10 + 1, 1);
        if (!ret)
            ret = gmac_wait_clear(priv, MAC_CONFIGURATION, MAC_CONF_TE | MAC_CONF_RE,
                                   clock_ticks_per_sec() / 10 + 1, 1);
    }
    if (!ret)
        priv->stopped = 1;
out:
    mutex_unlock(&priv->control_lock);
    return ret;
}

static void ls2k_gmac_release(void *context)
{
    ls2k_gmac_priv_t *priv = context;
    /* Devres reaches here only after checked stop, active-call drain and
     * remove. Until then even a failed probe owns its exact static slot. */
    memset(priv, 0, sizeof(*priv));
}

static int ls2k_gmac_driver_probe(device_t *dev) {
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res || res->end < res->start || res->end - res->start < DMA_INTR_ENABLE + 3)
        return -ENODEV;
    int ret = device_probe_use_remove(dev);
    if (ret)
        return ret;
    ls2k_gmac_priv_t *priv = gmac_alloc_instance(dev);
    if (!priv)
        return -ENOSPC;
    dev->drv_priv = priv;
    ret = device_add_execution_action(dev, ls2k_gmac_stop, ls2k_gmac_release, priv);
    if (ret) {
        dev->drv_priv = NULL;
        ls2k_gmac_release(priv);
        return ret;
    }

    uintptr_t base = (uintptr_t)device_map_mmio(dev, 0);
    if (!base)
        return -ENODEV;
    priv->base = base;
    ret = dma_set_mask(dev, DMA_MASK_32BIT);
    if (ret)
        return ret;
    if (!dma_range_ok(dev, va_to_pa(priv->tx_desc), sizeof(priv->tx_desc)) ||
        !dma_range_ok(dev, va_to_pa(priv->rx_desc), sizeof(priv->rx_desc)) ||
        !dma_range_ok(dev, va_to_pa(priv->tx_buf), sizeof(priv->tx_buf)) ||
        !dma_range_ok(dev, va_to_pa(priv->rx_buf), sizeof(priv->rx_buf)))
        return -ERANGE;
    mutex_lock(&priv->control_lock);
    ret = ls2k_gmac_hw_init(priv);
    mutex_unlock(&priv->control_lock);
    if (ret)
        return ret;
    kinfo("[LS2K-GMAC] Probed '%s' at 0x%lx\n", dev->name, (unsigned long)base);
    return 0;
}

static int ls2k_gmac_driver_remove(device_t *dev) {
    ls2k_gmac_priv_t *priv = (ls2k_gmac_priv_t *)dev->drv_priv;
    if (priv && !priv->stopped)
        return -EBUSY;
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
    return priv && !__atomic_load_n(&priv->closing, __ATOMIC_ACQUIRE) ? priv->mac : NULL;
}

static int ls2k_gmac_class_link_up(struct device *dev) {
    ls2k_gmac_priv_t *priv = (ls2k_gmac_priv_t *)dev->drv_priv;
    return priv && !__atomic_load_n(&priv->closing, __ATOMIC_ACQUIRE) ?
        __atomic_load_n(&priv->link_up, __ATOMIC_ACQUIRE) : 0;
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


static const firmware_device_id_t ls2k_gmac_firmware_ids[] = {
    {.id = "ls2k1000-gmac"}, {0}
};
static driver_t ls2k_gmac_driver = {
    .name       = "ls2k-gmac",
    .firmware_ids = ls2k_gmac_firmware_ids,
    .irq_optional = 1,
    .bus        = &platform_bus,
    .probe      = ls2k_gmac_driver_probe,
    .remove     = ls2k_gmac_driver_remove,
    .class_ops  = &ls2k_gmac_net_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(ls2k_gmac_driver);
