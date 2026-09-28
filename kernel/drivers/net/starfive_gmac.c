/*
 * A20OS StarFive EQOS GMAC driver.
 *
 * Developed for the StarFive VisionFive 2 board, referencing RocketOS (MIT)
 * board/driver bring-up.  See docs/ACKNOWLEDGMENTS.md and
 * docs/platforms/physical-boards.md.
 *
 * LOCK_ORDER: each instance owns a private spinlock (g_gmac_insts[i].lock)
 * serializing descriptor-ring access in send/recv/poll.  The board enables
 * the GMAC1 clocks/reset in early_init() (SoC clock gating is a board-level
 * fact), so this driver only programs the EQOS interface.  The data path is
 * still poll-driven; IRQ-driven TX/RX can reuse the same lock but must be
 * added deliberately and documented in docs/drivers/lock-order.md.
 */

#include "drivers/net/starfive_gmac.h"
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

/* ============================================================
 * EQOS (Enhanced Quality of Service) GMAC Register Offsets
 * ============================================================ */

#define MAC_BASE  0x0000
#define MTL_BASE  0x0000
#define DMA_BASE  0x1000

/* MAC registers */
#define MAC_CONFIGURATION        (MAC_BASE + 0x0000)
#define MAC_FRAME_FILTER         (MAC_BASE + 0x0008)
#define MAC_HASH_TABLE_REG0      (MAC_BASE + 0x0008)
#define MAC_MII_ADDR             (MAC_BASE + 0x0200)
#define MAC_MII_DATA             (MAC_BASE + 0x0204)
#define MAC_FLOW_CTRL            (MAC_BASE + 0x0048)
#define MAC_DEBUG                (MAC_BASE + 0x005C)
#define MAC_HW_FEATURE0          (MAC_BASE + 0x011C)
#define MAC_HW_FEATURE1          (MAC_BASE + 0x0120)
#define MAC_HW_FEATURE2          (MAC_BASE + 0x0124)
#define MAC_MDIO_INTR_STATUS     (MAC_BASE + 0x0140)
#define MAC_ADDR0_HIGH           (MAC_BASE + 0x0300)
#define MAC_ADDR0_LOW            (MAC_BASE + 0x0304)

/* MTL registers */
#define MTL_OPERATION_MODE       0x0C00
#define MTL_TXQ0_OPERATION_MODE  0x0D00
#define MTL_RXQ0_OPERATION_MODE  0x0D30
#define MTL_RXQ0_MISSED_PKT      0x0D40

/* DMA registers */
#define DMA_MODE                 (DMA_BASE + 0x0000)
#define DMA_SYSBUS_MODE          (DMA_BASE + 0x0004)
#define DMA_STATUS                (DMA_BASE + 0x0008)
#define DMA_CH0_CONTROL          (DMA_BASE + 0x0100)
#define DMA_CH0_TX_CONTROL       (DMA_BASE + 0x0104)
#define DMA_CH0_RX_CONTROL       (DMA_BASE + 0x0108)
#define DMA_CH0_TXDESC_LIST_HADDR (DMA_BASE + 0x0110)
#define DMA_CH0_TXDESC_LIST_ADDR (DMA_BASE + 0x0114)
#define DMA_CH0_RXDESC_LIST_HADDR (DMA_BASE + 0x0118)
#define DMA_CH0_RXDESC_LIST_ADDR (DMA_BASE + 0x011C)
#define DMA_CH0_TXDESC_TAIL_PTR  (DMA_BASE + 0x0120)
#define DMA_CH0_RXDESC_TAIL_PTR  (DMA_BASE + 0x0128)
#define DMA_CH0_TXDESC_RING_LEN  (DMA_BASE + 0x012C)
#define DMA_CH0_RXDESC_RING_LEN  (DMA_BASE + 0x0130)
#define DMA_CH0_INTERRUPT_ENABLE (DMA_BASE + 0x0134)
#define DMA_CH0_STATUS           (DMA_BASE + 0x0160)

/* MAC Configuration bits */
#define MAC_CONF_RE       (1U << 0)
#define MAC_CONF_TE       (1U << 1)
#define MAC_CONF_DM       (1U << 13)
#define MAC_CONF_DO       (1U << 10)
#define MAC_CONF_IPC      (1U << 27)
#define MAC_CONF_PS       (1U << 15)
#define MAC_CONF_FES      (1U << 14)

/* MAC_MII_ADDR is { GB[0], MW[1], GOC[3:2], reserved, CR[12:8], GR[20:16],
 * PA[25:21] }: GB is the hardware busy flag, MW picks write over read, GOC is
 * the opcode (0 = write, 1 = write post, 2 = read, 3 = read post), CR is a
 * range selector for the MDC divider rather than a literal divisor, and PA/GR
 * are the PHY address and register index.  The two "post" opcodes leave GB
 * asserted until the PHY access retires, which is why every access here polls
 * GB instead of trusting a fixed delay. */
/* MII Address bits */
#define MII_ADDR_GB       (1U << 0)
#define MII_ADDR_GOC_READ (3U << 2)
#define MII_ADDR_GOC_WRITE (1U << 2)
#define MII_ADDR_MW       (1U << 1)
#define MII_ADDR_CR_SHIFT 8
#define MII_ADDR_CR_MASK  (0xFU << MII_ADDR_CR_SHIFT)
#define MII_ADDR_GR_SHIFT 16
#define MII_ADDR_PA_SHIFT 21

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
#define MTL_RXQ0_RQS_SHIFT 20
#define MTL_RXQ0_RXQEN    (1U << 0)

/* DMA_MODE[0] SWR is a self-clearing software reset: the DMA clears it once the
 * reset has completed, and every DMA register is back at its reset value at
 * that point.  Nothing else in the block may be programmed before that clear is
 * observed, or the write is wiped by the reset that follows it. */
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

/* Descriptor bits.  For EQOS ring descriptors the TX length is programmed in
 * des2 and des3 carries OWN/FD/LD plus the frame length, matching the
 * VisionFive 2 DWMAC4 ring format. */
#define DESC3_OWN     (1U << 31)
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

#define GMAC_MAX_INSTANCES 4

typedef struct {
    uintptr_t base;
    int       valid;
    spinlock_t lock;

    dma_desc_t tx_desc[GMAC_DESC_NUM] ALIGNED(16);
    dma_desc_t rx_desc[GMAC_DESC_NUM] ALIGNED(16);
    uint8_t    tx_buf[GMAC_DESC_NUM][GMAC_BUF_SIZE] ALIGNED(64);
    uint8_t    rx_buf[GMAC_DESC_NUM][GMAC_BUF_SIZE] ALIGNED(64);

    uint32_t   tx_busy;
    uint32_t   rx_busy;
    uint8_t    mac[6];
} starfive_gmac_priv_t;

static starfive_gmac_priv_t g_gmac_insts[GMAC_MAX_INSTANCES];

static starfive_gmac_priv_t *gmac_instance(uintptr_t base)
{
    for (int i = 0; i < GMAC_MAX_INSTANCES; i++) {
        if (g_gmac_insts[i].valid && g_gmac_insts[i].base == base)
            return &g_gmac_insts[i];
    }
    return NULL;
}

static starfive_gmac_priv_t *gmac_alloc_instance(uintptr_t base)
{
    starfive_gmac_priv_t *inst = gmac_instance(base);
    if (inst)
        return inst;
    for (int i = 0; i < GMAC_MAX_INSTANCES; i++) {
        if (!g_gmac_insts[i].valid) {
            inst = &g_gmac_insts[i];
            memset(inst, 0, sizeof(*inst));
            inst->base = base;
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

/* ============================================================
 * MII / PHY Access
 * ============================================================ */
/* GB is set by hardware for the whole duration of an MDIO transfer and clears
 * when it retires.  It has to be idle before a new access is started and is the
 * only completion signal the "post" opcodes give, so both the pre-wait and the
 * post-wait are mandatory.  The 10 ms ceiling is a hang guard, not a budget. */
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
    /* GOC = 3 is read-post: the payload appears in MAC_MII_DATA and is only
     * valid once GB clears, i.e. after the second gmac_mdio_wait() below. */
    uint32_t val = MII_ADDR_GB |
                   MII_ADDR_GOC_READ |
                   ((phy_addr << MII_ADDR_PA_SHIFT) & (0x1F << MII_ADDR_PA_SHIFT)) |
                   ((reg << MII_ADDR_GR_SHIFT) & (0x1F << MII_ADDR_GR_SHIFT)) |
                   (5 << MII_ADDR_CR_SHIFT);
    gmac_write(base, MAC_MII_ADDR, val);
    if (gmac_mdio_wait(base) != 0) return 0xFFFF;
    return (uint16_t)gmac_read(base, MAC_MII_DATA);
}

static void gmac_mdio_write(uintptr_t base, int phy_addr, int reg, uint16_t data) {
    if (gmac_mdio_wait(base) != 0) return;
    /* MII_DATA is loaded *before* MII_ADDR starts the transfer: a write-post
     * takes its payload from the register at the moment the opcode is issued,
     * so reversing the two stores pushes the register address out as data. */
    gmac_write(base, MAC_MII_DATA, data);
    uint32_t val = MII_ADDR_GB | MII_ADDR_MW | MII_ADDR_GOC_WRITE |
                   ((phy_addr << MII_ADDR_PA_SHIFT) & (0x1F << MII_ADDR_PA_SHIFT)) |
                   ((reg << MII_ADDR_GR_SHIFT) & (0x1F << MII_ADDR_GR_SHIFT)) |
                   (5 << MII_ADDR_CR_SHIFT);
    gmac_write(base, MAC_MII_ADDR, val);
    gmac_mdio_wait(base);
}

/* Registers above 0x1f are an indirect window on this class of PHY: 0x1e latches
 * the extended register address and 0x1f is the extended data port, so an
 * extended read is one MDIO write plus one MDIO read, not a wider access. */
static uint16_t gmac_phy_ext_read(uintptr_t base, int phy_addr, uint16_t reg)
{
    gmac_mdio_write(base, phy_addr, 0x1e, reg);
    return gmac_mdio_read(base, phy_addr, 0x1f);
}

static void gmac_phy_ext_write(uintptr_t base, int phy_addr,
                               uint16_t reg, uint16_t value)
{
    gmac_mdio_write(base, phy_addr, 0x1e, reg);
    gmac_mdio_write(base, phy_addr, 0x1f, value);
}

/* ============================================================
 * DMA Descriptor Management
 * ============================================================ */
static void gmac_init_desc(uintptr_t base, starfive_gmac_priv_t *priv) {
    memset(priv->tx_desc, 0, sizeof(priv->tx_desc));
    memset(priv->rx_desc, 0, sizeof(priv->rx_desc));

    paddr_t tx_desc_pa = va_to_pa((const void *)priv->tx_desc);
    paddr_t rx_desc_pa = va_to_pa((const void *)priv->rx_desc);

    for (int i = 0; i < GMAC_DESC_NUM; i++) {
        paddr_t tx_buf_pa = va_to_pa((const void *)priv->tx_buf[i]);
        paddr_t rx_buf_pa = va_to_pa((const void *)priv->rx_buf[i]);

        /* des0/des1 are the low and high halves of the buffer address.  A
         * transmit descriptor starts with OWN clear -- the CPU owns it and
         * nothing moves until OWN is set at submit time -- while FD (first) and
         * LD (last) mark it as a complete single-descriptor frame. */
        priv->tx_desc[i].des0 = (uint32_t)tx_buf_pa;
        priv->tx_desc[i].des1 = (uint32_t)((uint64_t)tx_buf_pa >> 32);
        priv->tx_desc[i].des3 = DESC3_FD | DESC3_LD;

        /* A receive descriptor is handed over the other way round: OWN set
         * means the buffer is available for the hardware to fill, and BUF1V
         * (buffer 1 valid) tells it to deposit the frame at the address in
         * des0/des1 rather than into the second buffer.  The DMA clearing OWN is
         * the only ownership notification the CPU ever gets. */
        priv->rx_desc[i].des0 = (uint32_t)rx_buf_pa;
        priv->rx_desc[i].des1 = (uint32_t)((uint64_t)rx_buf_pa >> 32);
        priv->rx_desc[i].des2 = 0;
        priv->rx_desc[i].des3 = DESC3_OWN | DESC3_BUF1V;
    }
    dma_sync_for_device(priv->tx_desc, sizeof(priv->tx_desc));
    dma_sync_for_device(priv->rx_desc, sizeof(priv->rx_desc));

    /* The descriptor-list base is a 64-bit *physical* address written as a
     * low/high pair, which is why it has to go through va_to_pa() and not the
     * kernel virtual address. */
    gmac_write(base, DMA_CH0_TXDESC_LIST_ADDR, (uint32_t)tx_desc_pa);
    gmac_write(base, DMA_CH0_TXDESC_LIST_HADDR,
               (uint32_t)((uint64_t)tx_desc_pa >> 32));
    gmac_write(base, DMA_CH0_RXDESC_LIST_ADDR, (uint32_t)rx_desc_pa);
    gmac_write(base, DMA_CH0_RXDESC_LIST_HADDR,
               (uint32_t)((uint64_t)rx_desc_pa >> 32));

    /* The ring-length register holds one less than the descriptor count, so a
     * 16-entry ring is 15: writing GMAC_DESC_NUM here leaves the last
     * descriptor outside the ring the DMA walks. */
    gmac_write(base, DMA_CH0_TXDESC_RING_LEN, GMAC_DESC_NUM - 1);
    gmac_write(base, DMA_CH0_RXDESC_RING_LEN, GMAC_DESC_NUM - 1);

    /* The tail pointer is the *physical address* of a descriptor, not its
     * index: the DMA dereferences it, so handing it an index makes it fetch
     * from the ring base and every descriptor after the first goes unused. */
    /* Point the ring tail at the last descriptor (EQOS ring mode). */
    gmac_write(base, DMA_CH0_RXDESC_TAIL_PTR, (uint32_t)(rx_desc_pa +
               sizeof(dma_desc_t) * (GMAC_DESC_NUM - 1)));

    priv->tx_busy = 0;
    priv->rx_busy = 0;
}

/* ============================================================
 * PHY Initialization (generic; the VisionFive 2 carries a Motorcomm
 * YT8531 PHY.  A scan across 0..31 mirrors the RocketOS reference instead of
 * trusting a fixed address.)
 * ============================================================ */
static int gmac_phy_init(uintptr_t base) {
    int phy_addr = -1;

    for (int i = 0; i < 32; i++) {
        uint16_t id1 = gmac_mdio_read(base, i, 2);
        uint16_t id2 = gmac_mdio_read(base, i, 3);
        uint32_t phy_id = ((uint32_t)id1 << 16) | id2;
        if (id1 == 0xFFFF && id2 == 0xFFFF)
            continue;
        if ((phy_id & 0x1FFFFFFF) == 0x1FFFFFFF)
            continue;
        phy_addr = i;
        kinfo("[StarFive-GMAC] PHY 0x%02x id 0x%08x\n", i, phy_id);
        break;
    }
    if (phy_addr < 0) {
        kinfo("[StarFive-GMAC] no PHY on MDIO\n");
        return -1;
    }

    /* Reset PHY */
    gmac_mdio_write(base, phy_addr, 0, 0x8000);
    uint64_t start = timer_get_ticks();
    while (1) {
        uint16_t val = gmac_mdio_read(base, phy_addr, 0);
        if (!(val & 0x8000)) break;
        if (timer_get_ticks() - start > clock_ticks_per_sec() * 2) {
            kinfo("[StarFive-GMAC] PHY reset timeout\n");
            return -1;
        }
    }

    /* The VF2 GMAC1 RGMII wiring uses the same Motorcomm YT8531 timing
     * values as the board DT: 0.30 ns RX internal delay and no TX delay. */
    uint16_t rgmii = gmac_phy_ext_read(base, phy_addr, 0xA003);
    if (rgmii != 0xFFFF) {
        /* YT8531 RC1R: RX delay is bits [3:0], GE TX delay [15:12]. */
        rgmii &= (uint16_t)~((0xFU << 12) | 0xFU);
        rgmii |= 0x2U;
        gmac_phy_ext_write(base, phy_addr, 0xA003, rgmii);
    }
    /* Start auto-negotiation only after the RGMII timing is programmed. */
    gmac_mdio_write(base, phy_addr, 0, 0x1200);
    mdelay(100);

    /* Wait for link up */
    start = timer_get_ticks();
    while (1) {
        uint16_t val = gmac_mdio_read(base, phy_addr, 1);
        if (val & 0x0004) break;
        if (timer_get_ticks() - start > clock_ticks_per_sec() * 5) {
            kinfo("[StarFive-GMAC] PHY link up timeout\n");
            return -1;
        }
        mdelay(10);
    }

    kinfo("[StarFive-GMAC] PHY link up at addr %d\n", phy_addr);
    return 0;
}

/* ============================================================
 * GMAC Initialization
 * ============================================================ */
int starfive_gmac_init(uintptr_t base) {
    starfive_gmac_priv_t *priv = gmac_alloc_instance(base);
    if (!priv)
        return -1;
    memcpy(priv->mac, (uint8_t[]){0x00, 0x55, 0x7B, 0xB5, 0x7D, 0xF7}, 6);

    /* DMA reset */
    gmac_write(base, DMA_MODE, DMA_MODE_SWR);
    uint64_t reset_start = timer_get_ticks();
    while (gmac_read(base, DMA_MODE) & DMA_MODE_SWR) {
        if (timer_get_ticks() - reset_start > clock_ticks_per_sec() / 10) {
            kinfo("[StarFive-GMAC] DMA reset timeout\n");
            return -1;
        }
    }
    mdelay(10);

    /* MAC reset - disable TX/RX */
    gmac_write(base, MAC_CONFIGURATION, 0);

    /* DMA system bus mode */
    /* BLEN[3:1] is the burst-length mask the AHB master may issue and EAME[11]
     * widens its arbitration window; both are bus hints, programmed only after
     * the SWR reset above has been observed cleared, because a write issued
     * while DMA_MODE_SWR is still set is erased by the reset that follows.  A
     * wrong value here costs throughput, never a transfer, so it is never the
     * explanation for a stalled ring. */
    gmac_write(base, DMA_SYSBUS_MODE,
               DMA_SYSBUS_MODE_EAME |
               DMA_SYSBUS_MODE_BLEN4 |
               DMA_SYSBUS_MODE_BLEN8 |
               DMA_SYSBUS_MODE_BLEN16);

    /* Initialize descriptors */
    gmac_init_desc(base, priv);

    /* MTL configuration */
    /* Written while MAC_CONFIGURATION is still 0: RE/TE are only set at the end
     * of this function, because re-programming a queue size or enabling a queue
     * under a running MAC leaves the DMA mid-transfer with a queue it no longer
     * owns. */
    gmac_write(base, MTL_OPERATION_MODE, MTL_OP_MODE_DTXSTS | MTL_OP_MODE_RAA_SP);
    gmac_write(base, MTL_TXQ0_OPERATION_MODE,
               MTL_TXQ0_TXQEN | MTL_TXQ0_TSF |
               (0x2 << MTL_TXQ0_TTC_SHIFT) |
               (0x7 << MTL_TXQ0_TQS_SHIFT));
    /* RQS at bit 20 is the receive-queue-size field: it states how many
     * descriptors the queue may hold, and it is saturated to its all-ones
     * encoding here so the hardware allocation can never come out smaller than
     * the GMAC_DESC_NUM = 16 descriptor ring programmed above.  Bit 0, RXQEN,
     * is defined in this file but is not part of this write -- a reader chasing
     * "the receive queue never hands up a descriptor" should read this register
     * back before looking anywhere else. */
    gmac_write(base, MTL_RXQ0_OPERATION_MODE,
               (0x1FFU << MTL_RXQ0_RQS_SHIFT));

    /* MAC configuration */
    gmac_write(base, MAC_FRAME_FILTER, 0x80000001); /* promiscuous for now */
    gmac_write(base, MAC_FLOW_CTRL, 0);

    /* Set MAC address */
    /* ADDR0_HIGH[31] AE is the address-enable bit that turns the pair on for
     * receive filtering, and the six octets are stored in reverse order --
     * MAC[0] in ADDR0_LOW[7:0], MAC[5] in ADDR0_HIGH[15:8] -- so writing the MAC
     * in natural order byte for byte produces a reversed address that no switch
     * will forward to this port. */
    uint32_t high = (priv->mac[5] << 8) | priv->mac[4] | (1U << 31);
    uint32_t low  = (priv->mac[3] << 24) | (priv->mac[2] << 16) |
                    (priv->mac[1] << 8)  | priv->mac[0];
    gmac_write(base, MAC_ADDR0_HIGH, high);
    gmac_write(base, MAC_ADDR0_LOW, low);

    /* PHY init */
    if (gmac_phy_init(base) != 0)
        return -1;

    /* Enable MAC TX/RX */
    gmac_write(base, MAC_CONFIGURATION,
               MAC_CONF_RE | MAC_CONF_TE | MAC_CONF_DM | MAC_CONF_IPC);

    /* DMA channel 0 configuration */
    /* The program burst length is encoded in units of 256 descriptors, so the
     * 16 written below asks for a 16 * 256 dword burst, and PBLX8 selects the
     * 8-beat AHB form of that burst. */
    gmac_write(base, DMA_CH0_CONTROL, DMA_CH0_CONTROL_PBLX8);
    /* ST is transmit store-and-forward, i.e. the DMA hands the MAC a whole
     * frame instead of dribbling it out.  OSP is the descriptor-list operation
     * mode: set selects ring mode over chain mode, and because this driver only
     * ever builds rings terminated by the last descriptor, OSP has to stay set
     * or the DMA walks the array as a chain and never wraps. */
    gmac_write(base, DMA_CH0_TX_CONTROL,
               DMA_CH0_TX_CONTROL_ST | DMA_CH0_TX_CONTROL_OSP |
               (16 << DMA_CH0_TX_CONTROL_TXPBL_SHIFT));
    /* SR is receive store-and-forward.  RBSZ[15:1] is a 15-bit count in
     * *bytes*, hence the shift by 1: a buffer of 32768 bytes or more would
     * overflow the field and the DMA would truncate every frame.  RXPBL is the
     * receive burst length in the same 256-descriptor units as TXPBL. */
    gmac_write(base, DMA_CH0_RX_CONTROL,
               DMA_CH0_RX_CONTROL_SR |
               ((GMAC_BUF_SIZE << DMA_CH0_RX_CONTROL_RBSZ_SHIFT) & DMA_CH0_RX_CONTROL_RBSZ_MASK) |
               (16 << DMA_CH0_RX_CONTROL_RXPBL_SHIFT));

    kinfo("[StarFive-GMAC] Initialized at 0x%lx\n", (unsigned long)base);
    return 0;
}

/* ============================================================
 * Packet TX/RX
 * ============================================================ */
int starfive_gmac_send(uintptr_t base, const void *pkt, size_t len) {
    starfive_gmac_priv_t *priv = gmac_instance(base);
    if (!priv || len > GMAC_BUF_SIZE - 4)
        return -1;

    uint64_t flags = spin_lock_irqsave(&priv->lock);
    uint32_t idx = priv->tx_busy;
    dma_desc_t *desc = &priv->tx_desc[idx];

    if (desc->des3 & DESC3_OWN) {   /* DMA still owns it */
        spin_unlock_irqrestore(&priv->lock, flags);
        return -1;
    }

    memcpy(priv->tx_buf[idx], pkt, len);
    /* 60 is the Ethernet minimum frame size (64 bytes) minus the 4-byte FCS the
     * MAC appends: a shorter frame on copper is dropped by the link partner and
     * shows up only as a transmit with no matching receive anywhere. */
    if (len < 60) {
        memset(priv->tx_buf[idx] + len, 0, 60 - len);
        len = 60;
    }
    dma_sync_for_device(priv->tx_buf[idx], len);

    /* des2 carries the frame length; des3 carries OWN, FD, LD and the same
     * length in its low 15 bits.  The full barrier between the two stores is
     * required: OWN is the handoff, and without it the DMA can observe the
     * handoff before the length store is visible and transmit a zero-length
     * frame.  OWN, FD and LD share one store, so the handoff is atomic. */
    desc->des2 = (uint32_t)len;
    __sync_synchronize();
    desc->des3 = DESC3_OWN | DESC3_FD | DESC3_LD | (uint32_t)len;
    dma_sync_for_device(desc, sizeof(*desc));

    /* The value is the physical address of the next descriptor and
     * DMA_CH0_TXDESC_TAIL_PTR is 32 bits wide, so the cast below silently
     * truncates if a ring ever lands above 4 GiB. */
    /* Wake DMA: writing the tail pointer is a write barrier for the ring. */
    paddr_t next_desc_pa = va_to_pa((const void *)&priv->tx_desc[(idx + 1) % GMAC_DESC_NUM]);
    gmac_write(base, DMA_CH0_TXDESC_TAIL_PTR, (uint32_t)next_desc_pa);

    priv->tx_busy = (idx + 1) % GMAC_DESC_NUM;
    spin_unlock_irqrestore(&priv->lock, flags);
    return 0;
}

int starfive_gmac_recv(uintptr_t base, void *buf, size_t maxlen) {
    starfive_gmac_priv_t *priv = gmac_instance(base);
    if (!priv)
        return -1;

    uint64_t flags = spin_lock_irqsave(&priv->lock);
    uint32_t idx = priv->rx_busy;
    dma_desc_t *desc = &priv->rx_desc[idx];

    if (desc->des3 & DESC3_OWN) {   /* No packet ready */
        spin_unlock_irqrestore(&priv->lock, flags);
        return -1;
    }

    dma_sync_for_cpu(desc, sizeof(*desc));
    dma_sync_for_cpu(priv->rx_buf[idx], GMAC_BUF_SIZE);

    /* The length the DMA wrote back lives in the low 15 bits of des3, the same
     * bits the transmit path programs, and the count includes the 4-byte FCS
     * that follows the frame, so the payload is four bytes shorter. */
    uint32_t frame_len = desc->des3 & 0x7FFF;
    if (frame_len < 4) {
        desc->des3 = DESC3_OWN | DESC3_BUF1V;
        dma_sync_for_device(desc, sizeof(*desc));
        gmac_write(base, DMA_CH0_RXDESC_TAIL_PTR,
                   (uint32_t)va_to_pa((const void *)desc));
        priv->rx_busy = (idx + 1) % GMAC_DESC_NUM;
        spin_unlock_irqrestore(&priv->lock, flags);
        return -1;
    }
    uint32_t len = frame_len - 4; /* strip FCS */
    if (len > maxlen) len = maxlen;
    if (len > 0) memcpy(buf, priv->rx_buf[idx], len);

    /* Return descriptor to DMA */
    desc->des3 = DESC3_OWN | DESC3_BUF1V;
    dma_sync_for_device(desc, sizeof(*desc));

    /* Handing one descriptor back is done by pointing the tail at that
     * descriptor's own address, which doubles as the re-arm for the DMA. */
    paddr_t tail_pa = va_to_pa((const void *)desc);
    gmac_write(base, DMA_CH0_RXDESC_TAIL_PTR, (uint32_t)tail_pa);

    priv->rx_busy = (idx + 1) % GMAC_DESC_NUM;
    spin_unlock_irqrestore(&priv->lock, flags);
    return (int)len;
}

void starfive_gmac_get_mac(uintptr_t base, uint8_t *mac) {
    starfive_gmac_priv_t *priv = gmac_instance(base);
    if (priv)
        memcpy(mac, priv->mac, 6);
}

int starfive_gmac_poll(uintptr_t base) {
    starfive_gmac_priv_t *priv = gmac_instance(base);
    if (!priv)
        return -1;

    uint64_t flags = spin_lock_irqsave(&priv->lock);
    /* DMA_CH0_STATUS is write-1-to-clear, so the value that was read has to go
     * back unmodified: the 1 bits are what clear, and inverting them -- the
     * reflex from the read-to-clear registers used elsewhere in the tree --
     * leaves every status bit latched forever.  A bit that gets set between the
     * read and the write simply survives to the next poll. */
    uint32_t status = gmac_read(base, DMA_CH0_STATUS);
    if (status)
        gmac_write(base, DMA_CH0_STATUS, status);
    spin_unlock_irqrestore(&priv->lock, flags);
    return 0;
}

/* ============================================================
 * driver_t integration
 * ============================================================ */

static int starfive_gmac_driver_probe(device_t *dev) {
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res)
        return -1;

    if (starfive_gmac_init(res->start) != 0) {
        kinfo("[StarFive-GMAC] Failed to init at 0x%lx\n", (unsigned long)res->start);
        return -1;
    }

    starfive_gmac_priv_t *priv = gmac_instance(res->start);
    dev->drv_priv = priv;
    kinfo("[StarFive-GMAC] Probed '%s' at 0x%lx\n", dev->name, (unsigned long)res->start);
    return 0;
}

static int starfive_gmac_driver_remove(device_t *dev) {
    starfive_gmac_priv_t *priv = (starfive_gmac_priv_t *)dev->drv_priv;
    if (priv)
        priv->valid = 0;
    dev->drv_priv = NULL;
    return 0;
}

static int starfive_gmac_class_send(struct device *dev, const void *pkt, size_t len) {
    starfive_gmac_priv_t *priv = (starfive_gmac_priv_t *)dev->drv_priv;
    if (!priv)
        return -1;
    return starfive_gmac_send(priv->base, pkt, len);
}

static int starfive_gmac_class_recv(struct device *dev, void *buf, size_t maxlen) {
    starfive_gmac_priv_t *priv = (starfive_gmac_priv_t *)dev->drv_priv;
    if (!priv)
        return -1;
    return starfive_gmac_recv(priv->base, buf, maxlen);
}

static const uint8_t *starfive_gmac_class_mac(struct device *dev) {
    starfive_gmac_priv_t *priv = (starfive_gmac_priv_t *)dev->drv_priv;
    return priv ? priv->mac : NULL;
}

static void starfive_gmac_class_poll(struct device *dev) {
    starfive_gmac_priv_t *priv = (starfive_gmac_priv_t *)dev->drv_priv;
    if (priv)
        starfive_gmac_poll(priv->base);
}

static net_dev_ops_t starfive_gmac_net_ops = {
    .send = starfive_gmac_class_send,
    .recv = starfive_gmac_class_recv,
    .mac  = starfive_gmac_class_mac,
    .poll = starfive_gmac_class_poll,
};

static const device_id_t starfive_gmac_ids[] = {
    { .vendor = STARFIVE_GMAC_PLATFORM_VENDOR, .device = STARFIVE_GMAC_PLATFORM_DEVICE,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t starfive_gmac_driver = {
    .name       = "starfive-gmac",
    .id_table   = starfive_gmac_ids,
    .bus        = &platform_bus,
    .probe      = starfive_gmac_driver_probe,
    .remove     = starfive_gmac_driver_remove,
    .class_ops  = &starfive_gmac_net_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(starfive_gmac_driver);
