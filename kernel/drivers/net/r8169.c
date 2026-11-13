/*
 * A20OS RTL8168 / RTL8111 PCI Gigabit Ethernet driver (r8169 family).
 *
 * Scope and provenance
 * --------------------
 * This is a register-level minimal driver for the parts Linux's r8169 driver
 * binds: RTL8168/8111/8411 and the older RTL8169/8110/810xE single-queue
 * cores.  It implements exactly one thing per net_dev_ops callback -- linear
 * transmit, one-frame receive, MAC readout, TX descriptor reclaim, and an INTx
 * handler that only acknowledges and wakes the stack -- and nothing else:
 *
 *   - no checksum/scatter-gather offload (caps() is absent, which the class
 *     documents as "linear send only");
 *   - no PHY link reporting: link_up stays NULL, which the class documents as
 *     "always up".  The PHYAR/PHYstatus indirect access is chip-generation
 *     specific, and reporting a wrong polarity would take the netif offline.
 *   - one static instance.
 *
 * Register offsets, descriptor format and the reset/start sequence follow the
 * Linux r8169 driver (drivers/net/ethernet/realtek/r8169_main.c and
 * r8169_common.h).  Where a value is a policy choice rather than a hardware
 * constant it is commented at the use site.
 *
 * What is NOT claimed
 * -------------------
 * No r8169-family silicon was exercised: QEMU has no r8168 model, so this file
 * is compile- and review-tested only.  The two paths that need a real PHY and
 * MDIO bench (link speed/duplex negotiation and the EEPROM-less MAC fallback)
 * are deliberately left to the hardware's post-reset defaults.
 */

#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "net/stack.h"
#include "core/bootargs.h"
#include "core/cpu.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/random.h"
#include "core/string.h"
#include "mm/frame.h"
#include "mm/mm.h"

#define R8169_VENDOR_REALTEK 0x10ECU

/* Device IDs Linux r8169 claims.  The wired part on the target laptop is
 * 10ec:8168 (RTL8111/8168/8411); the rest are the same register file with a
 * different PHY and are included so the driver binds instead of leaving the
 * port with no NET class device. */
#define R8169_DEV_8169 0x8169U /* RTL8169/8110 */
#define R8169_DEV_8168 0x8168U /* RTL8111/8168/8411 */
#define R8169_DEV_8161 0x8161U
#define R8169_DEV_8136 0x8136U /* RTL810xE */
#define R8169_DEV_8101 0x8101U

/* ---- register file (byte offsets, r8169_common.h) ------------------------ */
#define R8169_MAC0      0x00U /* 8-bit x6: IDR0..5 */
#define R8169_MAR0      0x08U /* 32-bit x8: multicast hash */
#define R8169_TNPDS_LO  0x20U /* Tx normal-priority descriptor start, 64-bit */
#define R8169_TNPDS_HI  0x24U
#define R8169_THPDS_LO  0x28U /* high-priority ring; unused, left disabled */
#define R8169_THPDS_HI  0x2CU
#define R8169_CHIPCMD   0x37U /* 8-bit */
#define R8169_TXPOLL    0x38U /* 8-bit: force a transmit descriptor poll */
#define R8169_INTRMASK  0x3CU /* 16-bit */
#define R8169_INTRSTAT  0x3EU /* 16-bit, write-1-to-clear */
#define R8169_TXCONFIG  0x40U
#define R8169_RXCONFIG  0x44U
#define R8169_CFG9346   0x50U /* 8-bit: config-register write unlock */
#define R8169_PHYSTATUS 0x6CU /* 8-bit: LinkStatus/FullDup/speed */
#define R8169_RXMAXSIZE 0xDAU /* 16-bit */
#define R8169_CPLUSCMD  0xE0U /* 16-bit: checksum/VLAN offload control */
#define R8169_INTRMIT   0xE2U /* 16-bit: interrupt coalescing */
#define R8169_RDSAR_LO  0xE4U /* Rx descriptor start, 64-bit */
#define R8169_RDSAR_HI  0xE8U
#define R8169_MAXTXPKT  0xECU /* 8-bit: unit 128 B on 8168, 32 B on 8169 */

/* ChipCmd bits. */
#define R8169_CMD_RESET 0x10U
#define R8169_CMD_RE    0x08U
#define R8169_CMD_TE    0x04U
#define R8169_CMD_RST_MASK 0x10U

/* TxPoll command bits: NPQ (0x40) polls the normal-priority ring that
 * TNPDS points at.  A descriptor ring is fetched by the device only after
 * this doorbell (r8169_main.c rtl8169_doorbell()). */
#define R8169_TXPOLL_NPQ 0x40U

/* Cfg9346 values. */
#define R8169_CFG_UNLOCK 0xC0U
#define R8169_CFG_LOCK   0x00U

/* IntrStatus / IntrMask bits (r8169_common.h "InterruptStatusBits"). */
#define R8169_IRQ_ROK        0x0001U /* Rx OK */
#define R8169_IRQ_RER        0x0002U /* Rx error */
#define R8169_IRQ_TOK        0x0004U /* Tx OK */
#define R8169_IRQ_TER        0x0008U /* Tx error */
#define R8169_IRQ_RXOVW      0x0010U /* Rx FIFO overflow */
#define R8169_IRQ_LINKCHG    0x0020U /* link change */
#define R8169_IRQ_RXFIFO     0x0040U /* Rx FIFO overrun */
#define R8169_IRQ_TXDESC     0x0080U /* Tx descriptor unavailable */
#define R8169_IRQ_SWINT      0x0100U
#define R8169_IRQ_RX_EVENTS  (R8169_IRQ_ROK | R8169_IRQ_RER | \
                              R8169_IRQ_RXOVW | R8169_IRQ_RXFIFO)
#define R8169_IRQ_USED       (R8169_IRQ_RX_EVENTS | R8169_IRQ_TOK | \
                              R8169_IRQ_TER | R8169_IRQ_TXDESC | \
                              R8169_IRQ_LINKCHG)

/* TxConfig: transmit DMA burst 7 (unlimited) at bits 10:8, interframe gap 3
 * at bits 25:24 (rtl_set_tx_config_registers()).  No TxCRC/TxIFG changes:
 * the device appends the FCS. */
#define R8169_TXCONFIG_VALUE ((7U << 8) | (3U << 24))

/* RxConfig, matching rtl_init_rxcfg() for the 8168 family (RX128_INT_EN |
 * RX_MULTI_EN | RX_DMA_BURST) plus the accept filters rtl_set_rx_mode()
 * installs.  RX_DMA_BURST is 7 (unlimited), not 6. */
#define R8169_RXCONFIG_RX128_INT_EN 0x00008000U
#define R8169_RXCONFIG_RX_MULTI_EN  0x00004000U
#define R8169_RXCONFIG_DMA_BURST    (7U << 8)
#define R8169_RXCONFIG_ACCEPT_MY_PHYS  0x00000002U
#define R8169_RXCONFIG_ACCEPT_MULTI    0x00000004U
#define R8169_RXCONFIG_ACCEPT_BCAST    0x00000008U
#define R8169_RXCONFIG_ACCEPT_MASK     0x0000003FU
#define R8169_RXCONFIG_VALUE \
    (R8169_RXCONFIG_RX128_INT_EN | R8169_RXCONFIG_RX_MULTI_EN | \
     R8169_RXCONFIG_DMA_BURST | R8169_RXCONFIG_ACCEPT_MY_PHYS | \
     R8169_RXCONFIG_ACCEPT_MULTI | R8169_RXCONFIG_ACCEPT_BCAST)

/* Interrupt causes rtl_set_irq_mask() enables for a single-vector device. */
#define R8169_IRQ_MASK (R8169_IRQ_ROK | R8169_IRQ_RER | R8169_IRQ_TOK | \
                        R8169_IRQ_TER | R8169_IRQ_LINKCHG)

/* PHYstatus bits (rtl8169_PHYstatus). */
#define R8169_PHY_LINK_STATUS 0x02U
#define R8169_PHY_FULL_DUP    0x01U
#define R8169_PHY_1000MBPS    0x10U
#define R8169_PHY_100MBPS     0x08U
#define R8169_PHY_10MBPS      0x04U

/* ---- descriptor format (r8169_common.h) ---------------------------------- */
/* DMA rings are aligned; expose that guarantee to both compilers so
 * 32-bit atomic descriptor accesses stay inline rather than using libatomic. */
typedef struct __attribute__((aligned(16))) r8169_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
} r8169_desc_t;
_Static_assert(sizeof(r8169_desc_t) == 16, "RTL8169 descriptor size");
_Static_assert(__builtin_offsetof(r8169_desc_t, addr) == 8, "RTL8169 DMA address offset");

#define R8169_DESC_OWN       0x80000000U /* set by the driver, cleared by NIC */
#define R8169_DESC_RING_END  0x40000000U /* last entry of the ring */
#define R8169_DESC_FIRST     0x20000000U /* first fragment of the frame */
#define R8169_DESC_LAST      0x10000000U /* last fragment of the frame */
#define R8169_DESC_LEN_MASK  0x00003FFFU /* Rx: frame length + FCS (14 bits) */
#define R8169_RX_RWT         0x00400000U /* over-length frame */
#define R8169_RX_RES         0x00200000U /* receive error summary */
#define R8169_RX_RUNT        0x00100000U /* runt frame */
#define R8169_RX_CRC         0x00080000U /* CRC error */
#define R8169_RX_ERR_MASK    (R8169_RX_RWT | R8169_RX_RES | \
                              R8169_RX_RUNT | R8169_RX_CRC)
#define R8169_ETH_HLEN       14U
#define R8169_ETH_FCS_LEN    4U

#define R8169_TX_DESC_COUNT 32U
#define R8169_RX_DESC_COUNT 64U
/* tx_busy is a 32-bit bitmap; keep the ring size and the map in step. */
_Static_assert(R8169_TX_DESC_COUNT == 32, "tx_busy bitmap is uint32_t");
#define R8169_BUF_SIZE      2048U
#define R8169_MIN_FRAME     60U
#define R8169_DESC_ALIGN    256U

/* Register-level model of the device, used only by the boot self-test.
 * R8169_SIM_REGS is the size of the register window this driver touches
 * (0x00..0xFF); the model is a word array plus a state machine in
 * r8169_sim_write(). */
#define R8169_SIM_REGS 0x100U

typedef struct r8169_sim {
    uint8_t regs[R8169_SIM_REGS];
    uint8_t cfg_locked;
    uint8_t next_tx;          /* descriptor index the model consumes next */
    uint8_t pending_tx_frame; /* one frame queued to the RX ring */
} r8169_sim_t;

typedef struct r8169_device {
    uintptr_t regs;
    r8169_sim_t *sim; /* non-NULL: accessors use the model, not MMIO */

    uint8_t mac[6];
    int irq;
    int irq_registered;
    int ready;

    r8169_desc_t *tx_ring;
    uint64_t tx_ring_dma;
    uint8_t *tx_buf;
    uint64_t tx_buf_dma;

    r8169_desc_t *rx_ring;
    uint64_t rx_ring_dma;
    uint8_t *rx_buf;
    uint64_t rx_buf_dma;

    uint32_t tx_next;
    uint32_t tx_busy;
    uint32_t rx_next;

    uint64_t tx_packets;
    uint64_t tx_dropped;
    uint64_t rx_packets;
    uint64_t rx_dropped;
    uint64_t irq_count;
    uint64_t error_irqs;
    unsigned error_logs;

    spinlock_t lock;
} r8169_device_t;


static uint8_t r8169_sim_read8(r8169_device_t *nic, uint32_t reg);
static void r8169_sim_write8(r8169_device_t *nic, uint32_t reg, uint8_t value);

static inline uint8_t r8169_read8(r8169_device_t *nic, uint32_t reg)
{
    if (nic->sim)
        return r8169_sim_read8(nic, reg);
    return readb((const volatile void *)(nic->regs + reg));
}

static inline void r8169_write8(r8169_device_t *nic, uint32_t reg,
                                uint8_t value)
{
    if (nic->sim) {
        r8169_sim_write8(nic, reg, value);
        return;
    }
    writeb(value, (volatile void *)(nic->regs + reg));
}

static inline uint16_t r8169_read16(r8169_device_t *nic, uint32_t reg)
{
    if (nic->sim) {
        const uint8_t *p = &nic->sim->regs[reg & (R8169_SIM_REGS - 1U)];
        return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    }
    return readw((const volatile void *)(nic->regs + reg));
}

static inline void r8169_write16(r8169_device_t *nic, uint32_t reg,
                                 uint16_t value)
{
    if (nic->sim) {
        /* Route every byte through the model: its protected-register and
         * doorbell state machine is per-byte, exactly like the register
         * file. */
        r8169_sim_write8(nic, reg, (uint8_t)(value & 0xffU));
        r8169_sim_write8(nic, reg + 1U, (uint8_t)(value >> 8));
        return;
    }
    writew(value, (volatile void *)(nic->regs + reg));
}

static inline uint32_t r8169_read32(r8169_device_t *nic, uint32_t reg)
{
    if (nic->sim) {
        const uint8_t *p = &nic->sim->regs[reg & (R8169_SIM_REGS - 1U)];
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    return readl((const volatile void *)(nic->regs + reg));
}

static inline void r8169_write32(r8169_device_t *nic, uint32_t reg,
                                 uint32_t value)
{
    if (nic->sim) {
        r8169_sim_write8(nic, reg, (uint8_t)(value & 0xffU));
        r8169_sim_write8(nic, reg + 1U, (uint8_t)((value >> 8) & 0xffU));
        r8169_sim_write8(nic, reg + 2U, (uint8_t)((value >> 16) & 0xffU));
        r8169_sim_write8(nic, reg + 3U, (uint8_t)((value >> 24) & 0xffU));
        return;
    }
    writel(value, (volatile void *)(nic->regs + reg));
}

/* A descriptor whose OWN bit is clear has been handed back by the device. */
static inline int r8169_desc_owned(const r8169_desc_t *d)
{
    return (__atomic_load_n(&d->opts1, __ATOMIC_ACQUIRE) &
            R8169_DESC_OWN) != 0;
}

/* Publish a descriptor write to the device.  The NIC fetches the ring with
 * DMA, so the payload/address stores must be visible before the OWN bit that
 * hands the descriptor over. */
static inline void r8169_desc_commit(r8169_desc_t *d, uint32_t opts1)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&d->opts1, opts1, __ATOMIC_RELEASE);
}

static void r8169_free_rings(r8169_device_t *nic)
{
    if (nic->tx_ring)
        dma_free_coherent_aligned(nic->tx_ring,
                                  R8169_TX_DESC_COUNT * sizeof(r8169_desc_t),
                                  nic->tx_ring_dma);
    if (nic->tx_buf)
        dma_free_coherent_aligned(nic->tx_buf,
                                  R8169_TX_DESC_COUNT * R8169_BUF_SIZE,
                                  nic->tx_buf_dma);
    if (nic->rx_ring)
        dma_free_coherent_aligned(nic->rx_ring,
                                  R8169_RX_DESC_COUNT * sizeof(r8169_desc_t),
                                  nic->rx_ring_dma);
    if (nic->rx_buf)
        dma_free_coherent_aligned(nic->rx_buf,
                                  R8169_RX_DESC_COUNT * R8169_BUF_SIZE,
                                  nic->rx_buf_dma);
    nic->tx_ring = NULL;
    nic->tx_buf = NULL;
    nic->rx_ring = NULL;
    nic->rx_buf = NULL;
}

static int r8169_alloc_rings(r8169_device_t *nic, device_t *dev)
{
    nic->tx_ring = dma_alloc_coherent_aligned(
        dev, R8169_TX_DESC_COUNT * sizeof(r8169_desc_t), R8169_DESC_ALIGN,
        &nic->tx_ring_dma);
    nic->tx_buf = dma_alloc_coherent_aligned(
        dev, R8169_TX_DESC_COUNT * R8169_BUF_SIZE, PAGE_SIZE,
        &nic->tx_buf_dma);
    nic->rx_ring = dma_alloc_coherent_aligned(
        dev, R8169_RX_DESC_COUNT * sizeof(r8169_desc_t), R8169_DESC_ALIGN,
        &nic->rx_ring_dma);
    nic->rx_buf = dma_alloc_coherent_aligned(
        dev, R8169_RX_DESC_COUNT * R8169_BUF_SIZE, PAGE_SIZE,
        &nic->rx_buf_dma);
    if (!nic->tx_ring || !nic->tx_buf || !nic->rx_ring || !nic->rx_buf) {
        r8169_free_rings(nic);
        return -ENOMEM;
    }
    return 0;
}

/* Hand every receive buffer to the device.  RingEnd marks the entry the NIC
 * wraps after, so the descriptor walk stays inside this allocation. */
static void r8169_rx_fill(r8169_device_t *nic)
{
    for (uint32_t i = 0; i < R8169_RX_DESC_COUNT; i++) {
        r8169_desc_t *d = &nic->rx_ring[i];
        d->addr = nic->rx_buf_dma + (uint64_t)i * R8169_BUF_SIZE;
        d->opts2 = 0;
        uint32_t opts1 = R8169_BUF_SIZE | R8169_DESC_OWN;
        if (i == R8169_RX_DESC_COUNT - 1)
            opts1 |= R8169_DESC_RING_END;
        r8169_desc_commit(d, opts1);
    }
    nic->rx_next = 0;
}

static unsigned r8169_reclaim_tx_locked(r8169_device_t *nic)
{
    unsigned released = 0;
    for (uint32_t i = 0; i < R8169_TX_DESC_COUNT; i++) {
        if (!(nic->tx_busy & (1U << i)))
            continue;
        if (r8169_desc_owned(&nic->tx_ring[i]))
            continue;
        nic->tx_busy &= ~(1U << i);
        released++;
    }
    return released;
}

static int r8169_irq_handler(int irq, void *priv)
{
    (void)irq;
    device_t *dev = (device_t *)priv;
    r8169_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !nic->ready)
        return 0;

    uint16_t status = r8169_read16(nic, R8169_INTRSTAT);
    if (!status)
        return 0;
    /* Write-1-to-clear the causes this handler services.  Clearing them is
     * what deasserts the level-triggered INTx line; the core masks the line
     * across this call, and the handler must leave nothing pending or the
     * unmask would immediately re-enter (see IRQ_TRIGGER_MODEL). */
    r8169_write16(nic, R8169_INTRSTAT, status);

    nic->irq_count++;
    if (status & R8169_IRQ_RX_EVENTS)
        net_stack_device_interrupt(dev);
    if (status & R8169_IRQ_LINKCHG) {
        uint8_t phy = r8169_read8(nic, R8169_PHYSTATUS);
        kinfo("[R8169] link %s phy=0x%02x\n",
              (phy & R8169_PHY_LINK_STATUS) ? "up" : "down", phy);
    }
    if (status & (R8169_IRQ_TER | R8169_IRQ_RXOVW | R8169_IRQ_RXFIFO)) {
        nic->error_irqs++;
        /* A stuck TX/RX error can fire forever.  Keep the first few causes
         * visible, then say once that the rest are in the counters; the
         * console rate limiter handles errors from other drivers. */
        if (nic->error_logs < 3) {
            nic->error_logs++;
            kerr("[R8169] status=0x%04x\n", status);
        } else if (nic->error_logs == 3) {
            nic->error_logs++;
            kerr("[R8169] further error interrupts suppressed; "
                 "see the remove() counters\n");
        }
    }
    return 0;
}

static void r8169_poll(device_t *dev)
{
    r8169_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !nic->tx_ring)
        return;
    uint64_t flags = spin_lock_irqsave(&nic->lock);
    unsigned released = r8169_reclaim_tx_locked(nic);
    /* 8168 doorbell-loss workaround from rtl_tx(): a TxPoll issued while
     * another transmit was in flight can be dropped, so re-kick whenever
     * work is still outstanding. */
    if (released && nic->tx_busy)
        r8169_write8(nic, R8169_TXPOLL, R8169_TXPOLL_NPQ);
    spin_unlock_irqrestore(&nic->lock, flags);
}

static int r8169_send(device_t *dev, const void *packet, size_t length)
{
    r8169_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !nic->tx_ring || !nic->tx_buf || !packet)
        return -EINVAL;
    if (length == 0 || length > R8169_BUF_SIZE)
        return -EINVAL;

    size_t wire_len = length < R8169_MIN_FRAME ? R8169_MIN_FRAME : length;

    uint64_t flags = spin_lock_irqsave(&nic->lock);
    r8169_reclaim_tx_locked(nic);
    if (nic->tx_busy == 0xFFFFFFFFU) {
        nic->tx_dropped++;
        spin_unlock_irqrestore(&nic->lock, flags);
        return -EAGAIN;
    }

    uint32_t slot = nic->tx_next;
    while (nic->tx_busy & (1U << slot))
        slot = (slot + 1U) % R8169_TX_DESC_COUNT;

    uint8_t *buf = nic->tx_buf + (size_t)slot * R8169_BUF_SIZE;
    memcpy(buf, packet, length);
    if (wire_len > length)
        memset(buf + length, 0, wire_len - length);
    arch_dma_sync_for_device(buf, wire_len);

    r8169_desc_t *d = &nic->tx_ring[slot];
    d->addr = nic->tx_buf_dma + (uint64_t)slot * R8169_BUF_SIZE;
    d->opts2 = 0;
    uint32_t opts1 = (uint32_t)wire_len | R8169_DESC_OWN | R8169_DESC_FIRST |
                     R8169_DESC_LAST;
    if (slot == R8169_TX_DESC_COUNT - 1)
        opts1 |= R8169_DESC_RING_END;
    r8169_desc_commit(d, opts1);

    /* Ring the normal-priority doorbell.  The device does not refresh the
     * transmit ring on its own: a descriptor becomes visible to it only after
     * this write (r8169_main.c rtl8169_doorbell()). */
    r8169_write8(nic, R8169_TXPOLL, R8169_TXPOLL_NPQ);

    nic->tx_busy |= 1U << slot;
    nic->tx_next = (slot + 1U) % R8169_TX_DESC_COUNT;
    nic->tx_packets++;
    spin_unlock_irqrestore(&nic->lock, flags);
    return (int)length;
}

/*
 * Decide whether a completed receive descriptor holds a frame this driver can
 * hand up, and return its payload length (FCS stripped).  The status layout is
 * rtl_rx()'s: 14 length bits including the FCS, RxRES/RxRUNT/RxCRC/RxRWT as
 * the error summary, and FirstFrag/LastFrag set on a single-descriptor frame.
 * Split frames are dropped rather than concatenated.
 */
static int r8169_rx_accept(uint32_t opts1, size_t *payload_len)
{
    if (opts1 & R8169_DESC_OWN)
        return 0;
    if (opts1 & R8169_RX_ERR_MASK)
        return 0;
    if ((opts1 & (R8169_DESC_FIRST | R8169_DESC_LAST)) !=
        (R8169_DESC_FIRST | R8169_DESC_LAST))
        return 0;
    uint32_t wire = opts1 & R8169_DESC_LEN_MASK;
    if (wire < R8169_ETH_HLEN + R8169_ETH_FCS_LEN || wire > R8169_BUF_SIZE)
        return 0;
    *payload_len = (size_t)(wire - R8169_ETH_FCS_LEN);
    return 1;
}

static int r8169_recv(device_t *dev, void *buffer, size_t max_length)
{
    r8169_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !nic->rx_ring || !nic->rx_buf || !buffer || !max_length)
        return -EINVAL;

    int result = -EAGAIN;
    uint64_t flags = spin_lock_irqsave(&nic->lock);
    r8169_desc_t *d = &nic->rx_ring[nic->rx_next];
    uint32_t opts1 = __atomic_load_n(&d->opts1, __ATOMIC_ACQUIRE);
    if (!(opts1 & R8169_DESC_OWN)) {
        size_t len = 0;
        if (r8169_rx_accept(opts1, &len)) {
            uint8_t *src = nic->rx_buf + (size_t)nic->rx_next * R8169_BUF_SIZE;
            arch_dma_sync_for_cpu(src, len);
            if (len <= max_length) {
                memcpy(buffer, src, len);
                result = (int)len;
                nic->rx_packets++;
            } else {
                nic->rx_dropped++;
                result = -EMSGSIZE;
            }
        } else {
            nic->rx_dropped++;
        }

        /* Re-arm before moving on: the device must never see a descriptor it
         * already owns with a stale buffer. */
        d->addr = nic->rx_buf_dma + (uint64_t)nic->rx_next * R8169_BUF_SIZE;
        d->opts2 = 0;
        uint32_t rearm = R8169_BUF_SIZE | R8169_DESC_OWN;
        if (nic->rx_next == R8169_RX_DESC_COUNT - 1)
            rearm |= R8169_DESC_RING_END;
        r8169_desc_commit(d, rearm);

        nic->rx_next = (nic->rx_next + 1U) % R8169_RX_DESC_COUNT;
    }
    spin_unlock_irqrestore(&nic->lock, flags);
    return result;
}

static const uint8_t *r8169_mac(device_t *dev)
{
    r8169_device_t *nic = dev ? dev->drv_priv : NULL;
    return nic ? nic->mac : NULL;
}

/* Carrier comes from PHYstatus bit 1 (rtl8169_PHYstatus / LinkStatus).  The
 * net stack re-reads this once a second through class_net_link_up(). */
static int r8169_link_up(device_t *dev)
{
    r8169_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !nic->regs)
        return 0;
    uint8_t phy = r8169_read8(nic, R8169_PHYSTATUS);
    /* 0xFF means the register window does not answer (device in a low-power
     * state).  The class contract treats "unknown" as up, not as down. */
    if (phy == 0xffU)
        return 1;
    return (phy & R8169_PHY_LINK_STATUS) != 0;
}

static int r8169_rx_irq_driven(device_t *dev)
{
    r8169_device_t *nic = dev ? dev->drv_priv : NULL;
    return nic && nic->irq_registered;
}

static int r8169_reset(r8169_device_t *nic)
{
    r8169_write8(nic, R8169_CHIPCMD, R8169_CMD_RESET);
    for (unsigned i = 0; i < 100; i++) {
        if (!(r8169_read8(nic, R8169_CHIPCMD) & R8169_CMD_RST_MASK))
            return 0;
        mdelay(1);
    }
    return -ETIMEDOUT;
}

/* Linux rtl_init_mac_address(): a board whose EEPROM has no usable address
 * gets a random locally-administered one and the RAR is reprogrammed, rather
 * than the interface failing to exist. */
static void r8169_random_mac(r8169_device_t *nic)
{
    uint8_t rnd[6];
    random_fill(rnd, sizeof(rnd));
    nic->mac[0] = (uint8_t)((rnd[0] & 0xfcU) | 0x02U); /* local, unicast */
    nic->mac[1] = rnd[1];
    nic->mac[2] = rnd[2];
    nic->mac[3] = rnd[3];
    nic->mac[4] = rnd[4];
    nic->mac[5] = rnd[5];
}

/* rtl_rar_set(): the RAR is written as MAC4 (16-bit) then MAC0 (32-bit), with
 * the config registers unlocked, so setting a random address takes effect
 * before the receiver is enabled. */
static void r8169_set_mac(r8169_device_t *nic)
{
    r8169_write8(nic, R8169_CFG9346, R8169_CFG_UNLOCK);
    uint16_t hi = (uint16_t)((uint16_t)nic->mac[4] |
                             (uint16_t)((uint16_t)nic->mac[5] << 8));
    uint32_t lo = (uint32_t)nic->mac[0] | ((uint32_t)nic->mac[1] << 8) |
                  ((uint32_t)nic->mac[2] << 16) |
                  ((uint32_t)nic->mac[3] << 24);
    r8169_write16(nic, R8169_MAC0 + 4U, hi);
    r8169_write32(nic, R8169_MAC0, lo);
    r8169_write8(nic, R8169_CFG9346, R8169_CFG_LOCK);
}

static int g_r8169_poll_mode;

static void r8169_parse_cmdline(void)
{
    const char *cmdline = bootargs_get();
    for (const char *p = cmdline; p && *p; ) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\t')
            end++;
        static const char key[] = "a20.r8169.poll=";
        if ((size_t)(end - p) > sizeof(key) - 1 && !memcmp(p, key, sizeof(key) - 1)) {
            const char *v = p + sizeof(key) - 1;
            if (end - v >= 1 && v[0] == '1')
                g_r8169_poll_mode = 1;
            else if (end - v >= 1 && v[0] == '0')
                g_r8169_poll_mode = 0;
            else
                kerr("[R8169] unknown a20.r8169.poll value, keeping default\n");
        }
        p = end;
    }
}

/*
 * Program the data path.  This is rtl_hw_start()'s sequence and it is shared
 * by probe and by the loopback self-test, so the simulation exercises exactly
 * the register writes the driver uses on hardware.
 */
static void r8169_hw_start(r8169_device_t *nic)
{
    /* Multicast hash: MAR0 (0x08) and MAR4 (0x0C) are the only two hash
     * dwords this core has (rtl_set_rx_mode()).  Writing past them would
     * reach the counter-address and transmit-descriptor registers.  All-ones
     * means "accept every multicast group", which is the only truthful
     * setting without a per-address filter. */
    r8169_write32(nic, R8169_MAR0 + 0x00U, 0xFFFFFFFFU);
    r8169_write32(nic, R8169_MAR0 + 0x04U, 0xFFFFFFFFU);

    r8169_rx_fill(nic);

    /* rtl_hw_start(): keep the config window open from here until the
     * receiver/transmitter are enabled.  The transmit/receive descriptor base
     * registers and the filter configuration are all in the Cfg9346-locked
     * range, so programming them after r8169_set_mac() re-locked the window
     * would be dropped by real hardware. */
    r8169_write8(nic, R8169_CFG9346, R8169_CFG_UNLOCK);

    /* Offloads off (CPlusCmd), no interrupt coalescing (IntrMitigate), and a
     * standard-MTU early-transmit threshold: 0x3F is NoEarlyTx on 8169 and
     * TxPacketMax (63 * 128 B) on 8168 (rtl_set_tx_config_registers /
     * rtl_hw_start). */
    r8169_write16(nic, R8169_CPLUSCMD, 0);
    r8169_write16(nic, R8169_INTRMIT, 0);
    r8169_write8(nic, R8169_MAXTXPKT, 63);

    /* Ring geometry and filter configuration, then the ring bases.  RxMaxSize
     * is the buffer size plus one (rtl_set_rx_max_size()); RDSAR is 64-bit, so
     * the high half is written first. */
    r8169_write16(nic, R8169_RXMAXSIZE, R8169_BUF_SIZE + 1U);
    r8169_write32(nic, R8169_TXCONFIG, R8169_TXCONFIG_VALUE);
    r8169_write32(nic, R8169_RXCONFIG, R8169_RXCONFIG_VALUE);

    r8169_write32(nic, R8169_TNPDS_HI, (uint32_t)(nic->tx_ring_dma >> 32));
    r8169_write32(nic, R8169_TNPDS_LO, (uint32_t)nic->tx_ring_dma);
    r8169_write32(nic, R8169_THPDS_HI, 0);
    r8169_write32(nic, R8169_THPDS_LO, 0);
    r8169_write32(nic, R8169_RDSAR_HI, (uint32_t)(nic->rx_ring_dma >> 32));
    r8169_write32(nic, R8169_RDSAR_LO, (uint32_t)nic->rx_ring_dma);

    /* Close the config window first, then enable the data plane, matching
     * rtl_hw_start(): the receiver must never DMA into a descriptor the
     * driver has not filled yet. */
    r8169_write8(nic, R8169_CFG9346, R8169_CFG_LOCK);
    r8169_write8(nic, R8169_CHIPCMD,
                 r8169_read8(nic, R8169_CHIPCMD) | R8169_CMD_TE |
                     R8169_CMD_RE);
    arch_dma_sync_for_device(nic->rx_buf,
                             R8169_RX_DESC_COUNT * R8169_BUF_SIZE);

}

static int r8169_probe(device_t *dev)
{
    r8169_parse_cmdline();

    if (pci_enable_and_assign_bars(dev) < 0)
        return -ENODEV;

    /* r8169 exposes the register file through BAR2; BAR0/1 are the I/O and
     * legacy windows.  A part that only published BAR0 is not this device. */
    resource_t *bar = pci_get_bar_resource(dev, 2);
    if (!bar || bar->end < bar->start ||
        bar->end - bar->start + 1U < 0x100U)
        return -ENODEV;

    r8169_device_t *nic = kmalloc(sizeof(*nic));
    if (!nic)
        return -ENOMEM;
    memset(nic, 0, sizeof(*nic));
    nic->irq = -1;
    spin_init(&nic->lock);
    nic->regs = (uintptr_t)pci_map_bar(dev, 2);
    if (!nic->regs) {
        kfree(nic);
        return -EIO;
    }

    /* Reset first: it restores the IDR shadow from the EEPROM and quiesces
     * any DMA the firmware left running.  Mask and acknowledge interrupts
     * before the reset so a stale cause cannot assert INTx during probe. */
    r8169_write16(nic, R8169_INTRMASK, 0);
    r8169_write16(nic, R8169_INTRSTAT, 0xFFFFU);
    if (r8169_reset(nic) < 0) {
        kerr("[R8169] reset timed out\n");
        kfree(nic);
        return -EIO;
    }
    r8169_write16(nic, R8169_INTRMASK, 0);
    r8169_write16(nic, R8169_INTRSTAT, 0xFFFFU);

    r8169_write8(nic, R8169_CFG9346, R8169_CFG_UNLOCK);
    for (unsigned i = 0; i < 6; i++)
        nic->mac[i] = r8169_read8(nic, R8169_MAC0 + i);

    /* A part whose IDR reads as a reset pattern has no EEPROM shadow this
     * driver can trust.  Use a random locally-administered address, as Linux
     * does, instead of refusing to bring the port up. */
    if ((nic->mac[0] == 0x00 && nic->mac[1] == 0x00) ||
        (nic->mac[0] == 0xFF && nic->mac[1] == 0xFF)) {
        r8169_random_mac(nic);
        kerr("[R8169] no usable MAC in the EEPROM; using random "
             "%02x:%02x:%02x:%02x:%02x:%02x\n",
             nic->mac[0], nic->mac[1], nic->mac[2], nic->mac[3],
             nic->mac[4], nic->mac[5]);
    }
    r8169_set_mac(nic);

    /* Descriptors carry a 64-bit address, so the device can decode the full
     * window; declare it before any DMA allocation so an out-of-window block
     * is rejected by the allocator instead of programmed into the ring. */
    int mask = dma_set_mask(dev, DMA_MASK_64BIT);
    if (mask < 0) {
        r8169_write8(nic, R8169_CFG9346, R8169_CFG_LOCK);
        kfree(nic);
        return mask;
    }
    int rc = r8169_alloc_rings(nic, dev);
    if (rc < 0) {
        r8169_write8(nic, R8169_CFG9346, R8169_CFG_LOCK);
        kfree(nic);
        return rc;
    }

    r8169_hw_start(nic);

    /* Publish the instance before the line is armed: request_irq() opens a
     * window in which the handler can run, and it resolves the instance
     * through dev->drv_priv.  ready stays 0 until the causes are unmasked, so
     * an interrupt in that window is acknowledged and ignored. */
    dev->drv_priv = nic;

    int irq = pci_intx_irq(dev);
    if (!g_r8169_poll_mode && irq >= 0) {
        /* The handler writes IntrStatus back (W1C), which deasserts the
         * level-triggered INTx line, so it uses the plain shared-line shape:
         * dispatch masks the line around the handler and unmasks it after. */
        if (request_irq((uint32_t)irq, r8169_irq_handler, IRQF_SHARED,
                        dev) == 0) {
            nic->irq = irq;
            nic->irq_registered = 1;
        } else {
            kinfo("[R8169] IRQ %d registration failed; using polling\n", irq);
        }
    }
    if (nic->irq_registered)
        r8169_write16(nic, R8169_INTRMASK, R8169_IRQ_MASK);
    nic->ready = 1;

    /* Provider-side link diagnostics.  link_up is deliberately left NULL
     * (always up): the stack consults it once, at netif registration, and
     * never re-polls carrier, so reporting a believable "down" at boot would
     * leave the interface down after the cable is plugged in. */
    uint8_t phy = r8169_read8(nic, R8169_PHYSTATUS);
    kinfo("[R8169] ready: mac=%02x:%02x:%02x:%02x:%02x:%02x mode=%s irq=%d "
          "rxdesc=%u txdesc=%u phy=0x%02x%s%s%s\n",
          nic->mac[0], nic->mac[1], nic->mac[2], nic->mac[3], nic->mac[4],
          nic->mac[5], nic->irq_registered ? "irq" : "poll",
          nic->irq_registered ? nic->irq : -1,
          (unsigned)R8169_RX_DESC_COUNT, (unsigned)R8169_TX_DESC_COUNT,
          phy, (phy & R8169_PHY_LINK_STATUS) ? " link-up" : " link-down",
          (phy & R8169_PHY_1000MBPS) ? " 1000m" :
              (phy & R8169_PHY_100MBPS) ? " 100m" :
              (phy & R8169_PHY_10MBPS) ? " 10m" : "",
          (phy & R8169_PHY_FULL_DUP) ? " full" : " half");
    return 0;
}

static int r8169_remove(device_t *dev)
{
    r8169_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic)
        return 0;

    /* Stop new work, mask the device causes, then release the handler; the
     * class layer has already taken the device offline so no new send/recv
     * can start. */
    nic->ready = 0;
    r8169_write16(nic, R8169_INTRMASK, 0);
    r8169_write16(nic, R8169_INTRSTAT, 0xFFFFU);
    if (nic->irq_registered) {
        free_irq((uint32_t)nic->irq, dev);
        nic->irq_registered = 0;
    }

    /* Clear TE/RE before the rings are freed, so the device stops DMAing into
     * memory the driver is about to hand back. */
    r8169_write8(nic, R8169_CHIPCMD, 0);
    unsigned wait;
    for (wait = 0; wait < 1000U; wait++) {
        if (!(r8169_read8(nic, R8169_CHIPCMD) &
              (R8169_CMD_TE | R8169_CMD_RE)))
            break;
        udelay(1);
    }
    if (wait == 1000U)
        return -ETIMEDOUT; /* Retain rings/context until a safe retry. */
    r8169_free_rings(nic);

    kinfo("[R8169] removed: irq=%lu err=%lu tx=%lu rx=%lu tx_drop=%lu "
          "rx_drop=%lu\n",
          (unsigned long)nic->irq_count, (unsigned long)nic->error_irqs,
          (unsigned long)nic->tx_packets, (unsigned long)nic->rx_packets,
          (unsigned long)nic->tx_dropped, (unsigned long)nic->rx_dropped);
    dev->drv_priv = NULL;
    kfree(nic);
    return 0;
}

/* ---- device model (boot self-test only) ---------------------------------- *
 *
 * Derived from the device behaviour r8169_main.c relies on: reset reloads the
 * IDR and self-clears ChipCmd.RST, the Cfg9346 window gates the MAC/filter/
 * descriptor-base registers, TxPoll.NPQ makes the core fetch the transmit
 * ring, the receive length is the frame plus FCS, and a completed receive
 * descriptor has DescOwn cleared.  It is not a cycle-accurate model; it is a
 * contract model that lets the real driver code run without silicon.
 */

static void *r8169_phys_to_va(uint64_t pa)
{
    pfn_t pfn = phys_to_pfn(pa);
    if (pfn == PFN_NONE)
        return NULL;
    return (void *)((uintptr_t)pfn_to_virt(pfn) + (pa & (PAGE_SIZE - 1U)));
}

static uint32_t r8169_sim_get32(r8169_sim_t *sim, uint32_t reg)
{
    const uint8_t *p = &sim->regs[reg & (R8169_SIM_REGS - 1U)];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t r8169_sim_read8(r8169_device_t *nic, uint32_t reg)
{
    return nic->sim->regs[reg & (R8169_SIM_REGS - 1U)];
}

/* Deliver one queued transmit descriptor into receive slot 0. */
static void r8169_sim_deliver(r8169_device_t *nic)
{
    r8169_sim_t *sim = nic->sim;
    uint64_t tx_base = (uint64_t)r8169_sim_get32(sim, R8169_TNPDS_LO) |
                       ((uint64_t)r8169_sim_get32(sim, R8169_TNPDS_HI) << 32);
    uint64_t rx_base = (uint64_t)r8169_sim_get32(sim, R8169_RDSAR_LO) |
                       ((uint64_t)r8169_sim_get32(sim, R8169_RDSAR_HI) << 32);
    if (!tx_base || !rx_base)
        return;

    r8169_desc_t *tx = (r8169_desc_t *)r8169_phys_to_va(tx_base);
    r8169_desc_t *rx = (r8169_desc_t *)r8169_phys_to_va(rx_base);
    if (!tx || !rx)
        return;

    for (uint32_t i = 0; i < R8169_TX_DESC_COUNT; i++) {
        uint32_t idx = (sim->next_tx + i) % R8169_TX_DESC_COUNT;
        r8169_desc_t *d = &tx[idx];
        uint32_t opts1 = __atomic_load_n(&d->opts1, __ATOMIC_ACQUIRE);
        if (!(opts1 & R8169_DESC_OWN))
            continue;
        if ((opts1 & (R8169_DESC_FIRST | R8169_DESC_LAST)) !=
            (R8169_DESC_FIRST | R8169_DESC_LAST))
            continue;
        uint32_t len = opts1 & 0xffffU;
        if (!len)
            continue;
        if (!(__atomic_load_n(&rx[0].opts1, __ATOMIC_ACQUIRE) &
              R8169_DESC_OWN)) {
            /* The driver has not re-armed the descriptor, so the device has
             * nowhere to put the frame and reports an RX overflow. */
            sim->regs[R8169_INTRSTAT] |=
                (uint8_t)(R8169_IRQ_RXOVW & 0xffU);
            return;
        }
        uint8_t *src = r8169_phys_to_va(d->addr);
        uint8_t *dst = r8169_phys_to_va(rx[0].addr);
        if (!src || !dst)
            return;
        memcpy(dst, src, len);
        /* Completed receive descriptor: 14-bit length + FCS, single fragment,
         * DescOwn cleared. */
        __atomic_store_n(&rx[0].opts1,
                         (len + R8169_ETH_FCS_LEN) | R8169_DESC_FIRST |
                             R8169_DESC_LAST,
                         __ATOMIC_RELEASE);
        /* Transmit completion: the core clears DescOwn. */
        __atomic_store_n(&d->opts1, opts1 & ~R8169_DESC_OWN, __ATOMIC_RELEASE);
        sim->regs[R8169_INTRSTAT] |= (uint8_t)((R8169_IRQ_ROK |
                                                R8169_IRQ_TOK) & 0xffU);
        sim->next_tx = (uint8_t)((idx + 1U) % R8169_TX_DESC_COUNT);
        return;
    }
}

static void r8169_sim_write8(r8169_device_t *nic, uint32_t reg, uint8_t value)
{
    r8169_sim_t *sim = nic->sim;
    reg &= R8169_SIM_REGS - 1U;

    if (reg == R8169_CHIPCMD) {
        if (value & R8169_CMD_RESET) {
            /* Reset: registers return to defaults, the IDR is reloaded from
             * the EEPROM shadow, the config window is locked and the RST bit
             * is self-clearing. */
            static const uint8_t factory_mac[6] = {0x52, 0x54, 0x00,
                                                   0x12, 0x34, 0x56};
            memset(sim->regs, 0, sizeof(sim->regs));
            memcpy(&sim->regs[R8169_MAC0], factory_mac, 6);
            sim->regs[R8169_PHYSTATUS] = R8169_PHY_LINK_STATUS |
                                         R8169_PHY_1000MBPS |
                                         R8169_PHY_FULL_DUP;
            sim->cfg_locked = 1;
            sim->next_tx = 0;
            return;
        }
        sim->regs[reg] = value;
        return;
    }
    if (reg == R8169_CFG9346) {
        sim->cfg_locked = (value & 0xC0U) != 0xC0U;
        sim->regs[reg] = value;
        return;
    }
    if (reg == R8169_TXPOLL) {
        sim->regs[reg] = value;
        if (value & R8169_TXPOLL_NPQ)
            r8169_sim_deliver(nic);
        return;
    }

    /* MAC/filter/descriptor-base writes are dropped while Cfg9346 is locked,
     * exactly like the hardware -- this is what makes the driver's unlock
     * window observable. */
    /* Byte ranges, because a 32-bit write is modelled as four byte writes:
     * MAC0..5, TNPDS/THPDS (0x20..0x2F), the Config0..5 window (0x50..0x56)
     * and RDSAR (0xE4..0xEB).  The multicast hash (MAR0..MAR7) is *not* in
     * this window: Linux's rtl_set_rx_mode() writes it without unlocking. */
    int protected_reg =
        reg <= 0x05U ||
        (reg >= 0x20U && reg <= 0x2FU) ||
        (reg >= 0x50U && reg <= 0x56U) ||
        (reg >= 0xE4U && reg <= 0xEBU);
    if (sim->cfg_locked && protected_reg)
        return;
    sim->regs[reg] = value;
}

/*
 * Descriptor self-test.  The ring words are the part of this driver that can
 * be checked without silicon: the bit positions and the length mask come from
 * r8169_main.c (DescOwn 31, RingEnd 30, FirstFrag 29, LastFrag 28; Rx length
 * GENMASK(13,0); RxRES bit 21, RxCRC bit 19).  An earlier version used a
 * 13-bit length mask and would have mis-sized every received frame.
 */
int a20_boot_selftest_r8169(void)
{
    size_t len = 0;
    int stage = 0;

    /* 64-byte frame (60 payload + 4 FCS), single descriptor, no errors. */
    uint32_t good = (64U & R8169_DESC_LEN_MASK) | R8169_DESC_FIRST |
                    R8169_DESC_LAST;
    stage = 1;
    if (!r8169_rx_accept(good, &len) || len != 60U)
        goto fail;
    /* The same descriptor with RingEnd set is still a valid frame. */
    stage = 2;
    if (!r8169_rx_accept(good | R8169_DESC_RING_END, &len) || len != 60U)
        goto fail;
    /* Still owned by the device: not a completion. */
    stage = 3;
    if (r8169_rx_accept(good | R8169_DESC_OWN, &len))
        goto fail;
    /* Error summary bits are drops. */
    stage = 4;
    if (r8169_rx_accept(good | R8169_RX_RES, &len))
        goto fail;
    stage = 5;
    if (r8169_rx_accept(good | R8169_RX_CRC, &len))
        goto fail;
    /* A 1518-byte frame must decode as 1514 payload bytes, which only works
     * with the 14-bit length field. */
    stage = 6;
    uint32_t big = (1518U & R8169_DESC_LEN_MASK) | R8169_DESC_FIRST |
                   R8169_DESC_LAST;
    if (!r8169_rx_accept(big, &len) || len != 1514U)
        goto fail;
    /* Fragment pair missing. */
    stage = 7;
    if (r8169_rx_accept(R8169_DESC_FIRST | 64U, &len))
        goto fail;
    /* Shorter than an Ethernet header plus FCS. */
    stage = 8;
    if (r8169_rx_accept(R8169_DESC_FIRST | R8169_DESC_LAST | 17U, &len))
        goto fail;
    /* TX descriptor flags for a single-fragment frame. */
    stage = 9;
    uint32_t tx = 60U | R8169_DESC_OWN | R8169_DESC_FIRST | R8169_DESC_LAST;
    if ((tx & R8169_DESC_OWN) == 0 || (tx & R8169_DESC_FIRST) == 0 ||
        (tx & R8169_DESC_LAST) == 0 || (tx & 0xffffU) != 60U)
        goto fail;
    stage = 10;
    if (R8169_TXPOLL_NPQ != 0x40U)
        goto fail;

    /*
     * Loopback through the register model: reset, read the MAC, program the
     * rings with the same r8169_hw_start() probe uses, transmit one frame and
     * receive it back.  This is the only way this driver's data path can be
     * executed without an r8168 part (QEMU has no model for one).
     */
    static r8169_sim_t sim;
    static r8169_device_t nic;
    static device_t dev;
    static uint8_t frame[64];
    static uint8_t out[128];

    memset(&sim, 0, sizeof(sim));
    memset(&nic, 0, sizeof(nic));
    memset(&dev, 0, sizeof(dev));
    nic.sim = &sim;
    nic.irq = -1;
    spin_init(&nic.lock);

    stage = 11;
    if (r8169_reset(&nic) < 0)
        goto fail;
    r8169_write8(&nic, R8169_CFG9346, R8169_CFG_UNLOCK);
    for (unsigned i = 0; i < 6; i++)
        nic.mac[i] = r8169_read8(&nic, R8169_MAC0 + i);
    if (nic.mac[0] != 0x52 || nic.mac[5] != 0x56)
        goto fail;
    stage = 12;
    r8169_set_mac(&nic);

    stage = 13;
    if (r8169_alloc_rings(&nic, NULL) < 0)
        goto fail;
    r8169_rx_fill(&nic);
    stage = 14;
    r8169_hw_start(&nic);

    /* With the config window closed, a ring-base write must be dropped. */
    stage = 15;
    uint32_t ring_saved = r8169_read32(&nic, R8169_TNPDS_LO);
    if (!ring_saved || ring_saved != (uint32_t)nic.tx_ring_dma)
        goto fail;
    r8169_write32(&nic, R8169_TNPDS_LO, 0xdeadbeefU);
    stage = 16;
    if (r8169_read32(&nic, R8169_TNPDS_LO) != ring_saved)
        goto fail;

    dev.drv_priv = &nic;
    for (unsigned i = 0; i < sizeof(frame); i++)
        frame[i] = (uint8_t)(0xa0U + i);
    stage = 17;
    if (r8169_send(&dev, frame, sizeof(frame)) != (int)sizeof(frame))
        goto fail;
    stage = 18;
    if (!(r8169_read16(&nic, R8169_INTRSTAT) & R8169_IRQ_ROK))
        goto fail;

    memset(out, 0, sizeof(out));
    stage = 19;
    int got = r8169_recv(&dev, out, sizeof(out));
    if (got != (int)sizeof(frame) || memcmp(out, frame, sizeof(frame)) != 0)
        goto fail;

    /* The transmit descriptor must now be reclaimable and the receive
     * descriptor re-armed for the next frame. */
    stage = 20;
    r8169_poll(&dev);
    if (nic.tx_busy != 0 || nic.tx_packets != 1 || nic.rx_packets != 1)
        goto fail;
    stage = 21;
    if (r8169_recv(&dev, out, sizeof(out)) != -EAGAIN)
        goto fail;

    r8169_free_rings(&nic);
    return 0;

fail:
    kerr("[R8169] loopback self-test failed at stage %d "
         "(locked=%u tnpds=%08x read=%08x)\n",
         stage, (unsigned)sim.cfg_locked,
         (unsigned)r8169_read32(&nic, R8169_TNPDS_LO),
         (unsigned)r8169_read32(&nic, R8169_TNPDS_LO));
    r8169_free_rings(&nic);
    return -1;
}

static const net_dev_ops_t r8169_ops = {
    .send = r8169_send,
    .recv = r8169_recv,
    .mac = r8169_mac,
    .poll = r8169_poll,
    .link_up = r8169_link_up,
    .rx_irq_driven = r8169_rx_irq_driven,
};

static const device_id_t r8169_ids[] = {
    { .vendor = R8169_VENDOR_REALTEK, .device = R8169_DEV_8169,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = R8169_VENDOR_REALTEK, .device = R8169_DEV_8168,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = R8169_VENDOR_REALTEK, .device = R8169_DEV_8161,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = R8169_VENDOR_REALTEK, .device = R8169_DEV_8136,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = R8169_VENDOR_REALTEK, .device = R8169_DEV_8101,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

/* Second-stage narrowing: the bus ID is not enough, the class has to be the
 * PCI Ethernet controller class (offset 0x09), which is what the two-phase
 * match contract allows a driver to look at. */
static int r8169_match(device_t *dev)
{
    return pci_class_code(dev) == 0x020000U;
}

static driver_t r8169_driver = {
    .name = "r8169",
    .id_table = r8169_ids,
    .bus = &pci_bus,
    .match = r8169_match,
    .probe = r8169_probe,
    .remove = r8169_remove,
    .class_ops = &r8169_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(r8169_driver);
