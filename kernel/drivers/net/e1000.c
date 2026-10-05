#include "drivers/bus/pci_msix.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "net/lwip_stack.h"
#include "core/cpu.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/mm.h"

#define E1000_VENDOR_INTEL 0x8086U
/* Common e1000 / e1000e PCI device IDs (Linux e1000/e1000e supported set). */
#define E1000_DEVICE_82540EM 0x100EU   /* 82540EM (QEMU default) */
#define E1000_DEVICE_82545EM 0x100FU   /* 82545EM / 82545GM */
#define E1000_DEVICE_82546EB 0x1010U   /* 82546EB */
#define E1000_DEVICE_82541PI 0x107CU   /* 82541PI / 82547GI */
#define E1000_DEVICE_82574L  0x10D3U   /* 82574L (e1000e) */

#define E1000_CTRL   0x0000U
#define E1000_STATUS 0x0008U
#define E1000_ICR    0x00C0U
#define E1000_IMS    0x00D0U
#define E1000_IMC    0x00D8U
/* ITR (interrupt throttle control) exists only on the e1000e-generation parts;
 * on the 82540EM / 82545EM / 82546EB / 82541PI it is a reserved hole in the
 * register file.  e1000_itr_init() gates every write to it on the device id, so
 * the define existing here is not the same as the register existing.  Bits 15:1
 * are the throttle interval in 1024 ns units, bit 0 is the throttle enable. */
#define E1000_ITR    0x000CU
#define E1000_RCTL   0x0100U
#define E1000_TCTL   0x0400U
#define E1000_TIPG   0x0410U
#define E1000_RDBAL  0x2800U
#define E1000_RDBAH  0x2804U
#define E1000_RDLEN  0x2808U
#define E1000_RDH    0x2810U
#define E1000_RDT    0x2818U
#define E1000_TDBAL  0x3800U
#define E1000_TDBAH  0x3804U
#define E1000_TDLEN  0x3808U
#define E1000_TDH    0x3810U
#define E1000_TDT    0x3818U
#define E1000_RAL0   0x5400U
#define E1000_RAH0   0x5404U
/* JUMBO[13:0] is the number of bytes of maximum frame beyond a plain 1514 B
 * Ethernet frame; the receiver drops anything longer when RCTL[5] LPE is clear
 * (hw/net/e1000x_common.c:141-161). */
#define E1000_JUMBO  0x3CA0U

/* Long-packet enable, and the ceiling this driver programs with it.
 *
 * Without LPE the device discards every frame past 1514 bytes
 * (e1000x_is_oversized(), hw/net/e1000x_common.c:141-152), and since one
 * receive descriptor here holds E1000_BUF_SIZE = 2048 bytes, no frame could
 * ever span two descriptors: the EOP walk in e1000_recv() would be unreachable
 * on any traffic.  LPE is what makes a cross-descriptor frame expressible at
 * all.
 *
 * The ceiling is deliberately two buffers and not the 16 KiB the part can
 * address, because that is the largest frame the walk above is claimed to
 * handle without a burst of descriptors per packet: 1514 + 2560 = 4074 bytes
 * of frame plus the 4-byte FCS lands inside 2 * 2048.  Real hardware honours
 * JUMBO; QEMU checks only LPE against its own 16 KiB limit
 * (hw/net/e1000x_common.c:147-149), so the register is written for the part's
 * sake and not because QEMU reads it.
 *
 * This is a receive-side capability only.  The link MTU the stack advertises is
 * NET_PROFILE_NETIF_MTU (kernel/net/net_profile.h), which stays 1500 on every
 * profile, so a frame between 1515 and 4074 bytes is reassembled correctly here
 * and then dropped by lwIP as over-long.  Raising the two together needs the
 * profile table and the pbuf sizing in the net stack, not a driver register. */
#define E1000_RCTL_LPE       (1U << 5)
#define E1000_JUMBO_BYTES    2560U

/* CTRL[6] SLU forces the PHY link up regardless of a cable, which the MAC
 * needs before it will pass traffic on some parts.  RCTL[1] EN is the receiver
 * enable, RCTL[15] BAM the broadcast accept mode, RCTL[26] SECRC the
 * secure-error report control (the receiver's policy for frames the MAC flagged
 * as errored).  TCTL[1] EN is the transmitter enable and TCTL[3] PSP selects
 * the switch-media packet format, which is inert on the copper 8254x/8257x
 * parts matched at the bottom of this file. */
#define E1000_CTRL_SLU       (1U << 6)
#define E1000_RCTL_EN        (1U << 1)
#define E1000_RCTL_BAM       (1U << 15)
#define E1000_RCTL_SECRC     (1U << 26)
#define E1000_TCTL_EN        (1U << 1)
#define E1000_TCTL_PSP       (1U << 3)
#define E1000_RXD_STAT_DD    (1U << 0)
#define E1000_RXD_STAT_EOP   (1U << 1)
#define E1000_TXD_CMD_EOP    (1U << 0)
#define E1000_TXD_CMD_IFCS   (1U << 1)
#define E1000_TXD_CMD_RS     (1U << 3)
#define E1000_TXD_STAT_DD    (1U << 0)
/* DD in a *transmit* descriptor is software-owned: the device never sets or
 * clears it, it only rewrites the rest of the descriptor when it retires the
 * frame.  e1000_send() refuses any slot whose DD is clear, so every TX
 * descriptor must be pre-armed with DD = 1 during probe; a zeroed ring
 * otherwise reads as permanently busy and every send fails with -1. */

/* Interrupt causes enabled in IMS when a line is registered: TX descriptor
 * write-back, link status change, RX overrun, and the RX timer.  ICR is
 * read-to-clear, which is also the top-half acknowledge. */
#define E1000_IMS_TXDW       (1U << 0)
#define E1000_IMS_LSC        (1U << 2)
#define E1000_IMS_RXO        (1U << 6)
#define E1000_IMS_RXT0       (1U << 7)
#define E1000_IMS_USED       (E1000_IMS_TXDW | E1000_IMS_LSC | \
                              E1000_IMS_RXO | E1000_IMS_RXT0)

/* 256 entries per direction, up from 64.  With a 2048-byte buffer that is
 * 512 KiB of payload per direction, which is why the rings are taken from
 * kmalloc() at probe time rather than declared here -- see
 * e1000_ring_bytes() for the .a20drv package-size reason.  RDLEN takes a
 * descriptor count, not a byte count, so the count is what is programmed. */
#define E1000_RING_SIZE 256U
#define E1000_BUF_SIZE  2048U

/* The legacy (non-EXT) descriptor is a 16-byte wire format, not a C layout:
 * receive is address[63:0] | length[15:0] | checksum[15:0] | status[7:0] |
 * errors[7:0] | special[15:0], and the device walks the ring with a fixed
 * 16-byte stride.  Both the packed attribute and the 16-byte ring alignment in
 * the device struct are therefore load bearing: dropping a field or letting the
 * compiler insert padding shifts every field the device reads.  The transmit
 * descriptor below is the same 16 bytes with the checksum/status pair replaced
 * by the cso/command/status/css quadruple. */
typedef struct {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t address;
    uint16_t length;
    uint8_t cso;
    uint8_t command;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    uintptr_t regs;
    uint8_t mac[6];
    uint32_t rx_next;
    uint32_t tx_next;
    /* LOCK_ORDER: per-NIC lock protects MMIO register sequencing and the
     * TX/RX descriptor rings plus their bounce buffers.  The IRQ handler
     * takes only g_lwip_lock (never this lock); send/recv take only this
     * lock (never g_lwip_lock).  No nesting between the two exists. */
    spinlock_t lock;
    int irq;
    int irq_registered;
    int msix_base;        /* first reserved vector, -1 when unused */
    int msix_vectors;     /* reserved table entries in use */
    e1000_rx_desc_t *rx;
    e1000_tx_desc_t *tx;
    uint8_t (*rx_buf)[E1000_BUF_SIZE];
    uint8_t (*tx_buf)[E1000_BUF_SIZE];
    /* Owns the four arrays above in one piece; this is the kmalloc() pointer
     * itself, because that is what kfree() takes. */
    void *ring_mem;
    /* Interrupt throttle interval programmed into ITR, in microseconds, or 0
     * when the part has no ITR.  Reported on the ready line so a diff of two
     * boots shows whether throttling is in force. */
    uint32_t itr_us;
    /* Frames the device spread over more than one receive descriptor, and
     * whether that has been reported once.  Same job as the virtio-net counter:
     * the EOP walk in e1000_recv() is only exercised by a frame that does not
     * fit one buffer, so a zero here says the traffic never produced one, not
     * that the walk works. */
    uint32_t rx_multi_desc;
    int rx_multi_reported;
} e1000_device_t;

static e1000_device_t g_e1000;

static inline uint32_t e1000_read(e1000_device_t *nic, uint32_t reg)
{
    return readl((const volatile void *)(nic->regs + reg));
}

static inline void e1000_write(e1000_device_t *nic, uint32_t reg, uint32_t value)
{
    writel(value, (volatile void *)(nic->regs + reg));
}

/*
 * Why the rings are kmalloc()ed rather than static arrays.
 *
 * Under the default DRIVER_DEPLOYMENT=generic profile this driver ships as
 * e1000.a20drv, and drvmod_load() rejects any package whose .text + .data +
 * .bss exceeds DRV_MOD_MAX_SIZE (kernel/include/drvmod/drvmod.h:31, 512 KiB,
 * enforced on "bad total_size" in kernel/drvmod/loader.c).  256 descriptors and
 * 2048-byte payload buffers on each of two directions is just over 1 MiB, so the
 * statically sized version cannot be loaded at all.
 *
 * va_to_pa() (kernel/include/mm/mm.h:26) is a flat subtraction of PAGE_OFFSET,
 * so any kernel virtual address converts; and a request above SLAB_MAX_OBJ
 * (kernel/mm/slab.c:12) goes straight to the buddy allocator and comes back as
 * whole pages, which is the 16-byte ring-base alignment RDBAL/RDBAL need.
 */
static size_t e1000_ring_bytes(void)
{
    size_t n = 2 * (size_t)E1000_RING_SIZE * sizeof(e1000_rx_desc_t);
    n += 2 * (size_t)E1000_RING_SIZE * E1000_BUF_SIZE;
    n += 64 * 4;   /* alignment slack for the four regions */
    return n;
}

#define E1000_ALIGN(p, a) \
    ((p) = (void *)(((uintptr_t)(p) + ((a) - 1)) & ~(uintptr_t)((a) - 1)))

static void e1000_free_rings(e1000_device_t *nic)
{
    if (nic->ring_mem)
        kfree(nic->ring_mem);
    nic->ring_mem = NULL;
    nic->rx = NULL;
    nic->tx = NULL;
    nic->rx_buf = NULL;
    nic->tx_buf = NULL;
}

static int e1000_alloc_rings(e1000_device_t *nic)
{
    size_t size = e1000_ring_bytes();
    void *raw = kmalloc(size);
    if (!raw) {
        kinfo("[E1000] no memory for %u-entry rings (%zu bytes)\n",
              (unsigned)E1000_RING_SIZE, size);
        return -1;
    }
    memset(raw, 0, size);
    nic->ring_mem = raw;

    void *p = raw;
    /* The rings go first and page aligned: RDBAL is the low 32 bits of a 64-bit
     * physical address, and a ring base that is not 16-byte aligned makes the
     * device walk the descriptors off the alignment it expects. */
    E1000_ALIGN(p, 64);
    nic->rx = p;
    p += (size_t)E1000_RING_SIZE * sizeof(e1000_rx_desc_t);
    nic->tx = p;
    p += (size_t)E1000_RING_SIZE * sizeof(e1000_tx_desc_t);

    E1000_ALIGN(p, 64);
    nic->rx_buf = p;
    p += (size_t)E1000_RING_SIZE * E1000_BUF_SIZE;
    nic->tx_buf = p;
    return 0;
}

/* E1000_IRQ_MODEL:
 * - The top-half acknowledges by reading ICR (read-to-clear), resolves the
 *   device's netif index, and runs the same bounded RX drain the virtio-net
 *   IRQ path uses, under g_lwip_lock.  A shared line with no pending cause
 *   costs one register read.
 * - IMS is unmasked only after the handler is registered; without a handler
 *   the device keeps its causes masked and the class .poll hook remains the
 *   only progress path. */
static int e1000_irq_handler(int irq, void *priv) {
    (void)irq;
    device_t *dev = (device_t *)priv;
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic)
        return 0;
    uint32_t icr = e1000_read(nic, E1000_ICR);
    if (!icr)
        return 0;
    int net_idx = -1;
    for (int i = 0; i < 8; i++) {
        device_t *cur = device_find_by_class(DEV_CLASS_NET, i);
        if (!cur)
            break;
        if (cur == dev) {
            net_idx = i;
            break;
        }
    }
    uint64_t flags = a20_lwip_lock();
    if (net_idx >= 0)
        a20_lwip_process_netif_irq_locked(net_idx);
    else
        a20_lwip_signal_rx_pending();
    a20_lwip_unlock(flags);
    return 0;
}

static void e1000_poll(device_t *dev)
{
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !nic->tx)
        return;
    (void)e1000_read(nic, E1000_ICR);
    arch_dma_sync_for_cpu(nic->tx, (size_t)E1000_RING_SIZE * sizeof(nic->tx[0]));
}

static int e1000_send(device_t *dev, const void *packet, size_t length)
{
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !packet || length == 0 || length > E1000_BUF_SIZE || !nic->tx)
        return -1;

    uint64_t flags = spin_lock_irqsave(&nic->lock);
    uint32_t slot = nic->tx_next;
    arch_dma_sync_for_cpu(&nic->tx[slot], sizeof(nic->tx[slot]));
    if (!(nic->tx[slot].status & E1000_TXD_STAT_DD)) {
        spin_unlock_irqrestore(&nic->lock, flags);
        return -1;
    }

    memcpy(nic->tx_buf[slot], packet, length);
    arch_dma_sync_for_device(nic->tx_buf[slot], length);
    nic->tx[slot].length = (uint16_t)length;
    nic->tx[slot].cso = 0;
    nic->tx[slot].command = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS |
                            E1000_TXD_CMD_RS;
    nic->tx[slot].status = 0;
    nic->tx[slot].css = 0;
    nic->tx[slot].special = 0;
    arch_dma_sync_for_device(&nic->tx[slot], sizeof(nic->tx[slot]));
    wmb();

    /* Publishing the descriptor is the TDT write below: TDT is the next
     * descriptor the device should transmit, so the post-increment index goes
     * out last.  The wmb() above plus this MMIO store are what order the
     * descriptor against the device -- see the DMA publication contract in
     * include/drivers/dual/virtq.h. */
    nic->tx_next = (slot + 1U) % E1000_RING_SIZE;
    e1000_write(nic, E1000_TDT, nic->tx_next);
    spin_unlock_irqrestore(&nic->lock, flags);
    return (int)length;
}

static int e1000_recv(device_t *dev, void *buffer, size_t max_length)
{
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !buffer || max_length == 0 || !nic->rx)
        return -1;

    uint64_t flags = spin_lock_irqsave(&nic->lock);
    uint32_t slot = nic->rx_next;
    arch_dma_sync_for_cpu(&nic->rx[slot], sizeof(nic->rx[slot]));
    if (!(nic->rx[slot].status & E1000_RXD_STAT_DD)) {
        spin_unlock_irqrestore(&nic->lock, flags);
        return 0;
    }

    /* DD (status[0]) says the device filled the descriptor, EOP (status[1])
     * says it is the last descriptor of the frame, and a non-zero errors word
     * retires the descriptor without delivering it.
     *
     * A frame longer than one 2048-byte buffer spans several descriptors.
     * They are walked here and concatenated into the caller's buffer rather
     * than dropped.  The old code zeroed the length whenever EOP was absent,
     * which threw away the first fragment of every jumbo frame -- and, because
     * it still returned the single descriptor to the device, left RDT pointing
     * into the middle of a frame so the next read started mid-packet.  Walking
     * to EOP retires the whole chain, so the two failures go together.
     *
     * The walk is bounded by the ring size: a device that never sets EOP costs
     * one full ring of descriptors and then resynchronises at a descriptor
     * boundary, instead of spinning on the same head forever. */
    size_t copied = 0;
    int bad = 0;
    int eop = 0;
    uint32_t last = nic->rx_next;
    uint32_t walked = 0;

    for (uint32_t n = 0; n < E1000_RING_SIZE && !eop; n++) {
        last = slot;
        walked++;
        arch_dma_sync_for_cpu(&nic->rx[slot], sizeof(nic->rx[slot]));
        /* Read every flag out of the descriptor word before clearing it: EOP
         * lives in the same byte that is about to be zeroed. */
        uint8_t status = nic->rx[slot].status;
        uint16_t chunk = nic->rx[slot].length;
        uint8_t errors = nic->rx[slot].errors;
        eop = (status & E1000_RXD_STAT_EOP) != 0;

        /* An error retires the whole frame, not just this descriptor: handing
         * the caller a prefix of a frame the device flagged as bad would be
         * worse than handing it nothing. */
        if (errors)
            bad = 1;
        if (!(status & E1000_RXD_STAT_DD))
            bad = 1;

        /* Truncation is not an error.  The stack gets what fits in max_length
         * and the rest of the frame is discarded, which is what a caller with a
         * short buffer means; copied stays short and EOP still ends the walk,
         * so the ring does not drift. */
        if (!bad && chunk > 0 && copied < max_length) {
            size_t take = chunk;
            if (take > max_length - copied)
                take = max_length - copied;
            arch_dma_sync_for_cpu(nic->rx_buf[slot], take);
            memcpy((uint8_t *)buffer + copied, nic->rx_buf[slot], take);
            copied += take;
        }

        /* Retire every descriptor the walk touches, delivered or not. */
        nic->rx[slot].status = 0;
        nic->rx[slot].errors = 0;
        arch_dma_sync_for_device(&nic->rx[slot], sizeof(nic->rx[slot]));

        if (bad)
            break;
        slot = (slot + 1U) % E1000_RING_SIZE;
    }

    /* RDT is the last index the device may fill, so the last descriptor this
     * walk retired is written back verbatim -- not `last + 1`, which is the
     * classic off-by-one and costs exactly one packet of stall on every ring
     * wrap.  `last` rather than `slot` also makes the broken-frame path
     * correct: the walk stops on the descriptor it gave up on, and that
     * descriptor has already been cleared, so it must be the one handed back. */
    e1000_write(nic, E1000_RDT, last);
    nic->rx_next = (last + 1U) % E1000_RING_SIZE;
    /* Reported once: the multi-descriptor frame is the only thing that
     * exercises the walk above, so its absence from a boot log is otherwise
     * indistinguishable from the walk being broken. */
    if (walked > 1) {
        nic->rx_multi_desc++;
        if (!nic->rx_multi_reported) {
            nic->rx_multi_reported = 1;
            kinfo("[E1000] first frame reassembled from %u descriptors (buf=%u)\n",
                  walked, (unsigned)E1000_BUF_SIZE);
        }
    }
    spin_unlock_irqrestore(&nic->lock, flags);
    return bad ? 0 : (int)copied;
}

static const uint8_t *e1000_mac(device_t *dev)
{
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    return nic ? nic->mac : NULL;
}

/* Message-signalled interrupts for the NIC's transmit and receive queues.
 *
 * Reserve two vectors: the first carries everything the ICR masks, the second
 * the receive-side causes.  Both start masked, and un-masking either is what
 * request_irq() does once a handler is really installed on the line.  Where the
 * table lives is the function's capability to say, so nothing here encodes a
 * BAR index: the 8254x family puts the table at the base of BAR3, but that is
 * this part's layout rather than a fact about MSI-X, and a driver that assumed
 * it would program the wrong window on any implementation that differs. */
static int e1000_msix_setup(device_t *dev, e1000_device_t *nic)
{
    pci_msix_info_t info;
    if (pci_msix_capability(dev, &info) < 0 || info.table_size < 2)
        return -ENODEV;

    int r = pci_msix_enable(dev, 2);
    if (r) {
        kinfo("[E1000] MSI-X unavailable (%d); using legacy interrupts\n", r);
        return r;
    }

    int base = irq_alloc_vectors(2);
    if (base < 0) {
        pci_msix_disable(dev);
        return base;
    }
    /* Vector 0 carries everything the ICR masks, vector 1 the receive-side
     * causes; both start masked, and un-masking either is what request_irq()
     * does once a handler is actually installed on the line. */
    for (unsigned i = 0; i < 2; i++) {
        r = pci_msix_program_vector(dev, i, (uint32_t)(base + (int)i));
        if (r) {
            irq_free_vectors((uint32_t)base, 2);
            pci_msix_disable(dev);
            return r;
        }
    }
    nic->msix_base = base;
    nic->msix_vectors = 2;
    return 0;
}

/* Unmask both table entries.  Called from request_irq()'s success path, which
 * is the earliest point at which a handler exists to receive the message. */
static void e1000_msix_commit(device_t *dev, e1000_device_t *nic)
{
    for (int i = 0; i < nic->msix_vectors; i++)
        pci_msix_set_vector_mask(dev, (unsigned)i, 0);
    (void)pci_msix_commit(dev);
    kinfo("[E1000] MSI-X enabled on vectors %d..%d\n",
          nic->msix_base, nic->msix_base + nic->msix_vectors - 1);
}

static void e1000_msix_teardown(device_t *dev, e1000_device_t *nic)
{
    if (nic->msix_vectors <= 0)
        return;
    pci_msix_disable(dev);
    irq_free_vectors((uint32_t)nic->msix_base, (unsigned)nic->msix_vectors);
    nic->msix_base = -1;
    nic->msix_vectors = 0;
}


/* Simple RX interrupt throttling, 82574L only.
 *
 * The register is not in the 8254x register file at all, so this is gated on
 * the device id rather than written unconditionally: on an 82540EM, 0x000C is
 * a reserved hole and writing it is at best ignored and at worst undefined.
 * On the 82574L it throttles interrupt delivery to at most one message per
 * interval, which bounds the per-packet interrupt cost of a bulk receive.
 *
 * Why a throttle is safe here rather than a throughput cut: the drain the
 * interrupt triggers is unbounded.  e1000_irq_handler() hands the netif index
 * to a20_lwip_process_netif_irq_locked(), which calls
 * a20_lwip_process_netif_rx_tx_locked(n, 0) with a zero budget -- the whole
 * ring is emptied per interrupt.  The longer interval therefore reduces how
 * often the ring is emptied, never how much of it is emptied, and with 256
 * descriptors the burst that arrives in one interval is bounded by the ring.
 *
 * 3.9 ms (3904 * 1024 ns).  Chosen small on purpose: the cost of throttling is
 * a receive packet waiting for the next interrupt, so the interval is a
 * latency budget, and 3.9 ms is invisible to every application this stack
 * serves while still cutting interrupt rate on a bulk receive by roughly an
 * order of magnitude against no throttling at all.  Bits 15:1 hold the count;
 * bit 0 enables. */
#define E1000_ITR_INTERVAL_US 3904U

static void e1000_itr_init(device_t *dev, e1000_device_t *nic)
{
    uint32_t id = pci_device_id(dev);
    if (id != E1000_DEVICE_82574L) {
        nic->itr_us = 0;
        return;
    }
    /* The field is 15 bits wide, so clamp rather than let a future edit write a
     * count that silently truncates into a shorter interval than intended. */
    uint32_t units = E1000_ITR_INTERVAL_US / 1024U;
    if (units > 0x7FFFU)
        units = 0x7FFFU;
    e1000_write(nic, E1000_ITR, (units << 1) | 1U);
    nic->itr_us = units * 1024U;
    kinfo("[E1000] 82574L: interrupt throttle %u us\n", (unsigned)nic->itr_us);
}

static int e1000_probe(device_t *dev)
{
    if (pci_enable_and_assign_bars(dev) < 0)
        return -1;
    resource_t *bar = pci_get_bar_resource(dev, 0);
    /* 0x6000 is the minimum window this driver accepts: the highest offset it
     * ever programs is RAH0 at 0x5404, so a smaller BAR would leave the
     * MAC-address read below outside the assigned mapping. */
    if (!bar || bar->end < bar->start || bar->end - bar->start + 1U < 0x6000U)
        return -1;

    e1000_device_t *nic = &g_e1000;
    memset(nic, 0, sizeof(*nic));
    nic->msix_base = -1;
    spin_init(&nic->lock);
    nic->regs = (uintptr_t)bar->start;

    uint32_t ral = e1000_read(nic, E1000_RAL0);
    uint32_t rah = e1000_read(nic, E1000_RAH0);
    nic->mac[0] = (uint8_t)ral;
    nic->mac[1] = (uint8_t)(ral >> 8);
    nic->mac[2] = (uint8_t)(ral >> 16);
    nic->mac[3] = (uint8_t)(ral >> 24);
    nic->mac[4] = (uint8_t)rah;
    nic->mac[5] = (uint8_t)(rah >> 8);
    /* RAL/RAH hold the address with the first octet in the LSB: RAL[7:0] is
     * MAC[0] and RAH[15:8] is MAC[5].  RAH[31] AV is the address-valid bit and
     * reads 0 on a part with no EEPROM-supplied MAC, which is how an
     * unprogrammed device is told apart from a live one. */
    if (!(rah & (1U << 31)))
        return -1;

    /* After the MAC read, before anything is programmed: a NIC whose rings
     * cannot be allocated must fail the probe, not come up with a device
     * pointing at memory this driver does not own. */
    if (e1000_alloc_rings(nic) < 0)
        return -1;

    /* IMC is write-1-to-clear against IMS, so writing all ones masks every
     * cause; the ICR read immediately after is the acknowledge of anything the
     * device had already latched, because ICR is read-to-clear.  SLU is forced
     * on only after that, so a link-up transition captured in ICR cannot
     * become a pending interrupt before the handler is registered below. */
    e1000_write(nic, E1000_IMC, 0xFFFFFFFFU);
    (void)e1000_read(nic, E1000_ICR);
    e1000_write(nic, E1000_CTRL, e1000_read(nic, E1000_CTRL) | E1000_CTRL_SLU);

    for (uint32_t i = 0; i < E1000_RING_SIZE; i++) {
        nic->rx[i].address = va_to_pa(nic->rx_buf[i]);
        nic->tx[i].address = va_to_pa(nic->tx_buf[i]);
        /* Pre-arm the TX DD bits while building the ring; see the DD note on
         * E1000_TXD_STAT_DD above. */
        nic->tx[i].status = E1000_TXD_STAT_DD;
    }
    arch_dma_sync_for_device(nic->rx_buf,
                             (size_t)E1000_RING_SIZE * E1000_BUF_SIZE);
    arch_dma_sync_for_device(nic->tx_buf,
                             (size_t)E1000_RING_SIZE * E1000_BUF_SIZE);
    arch_dma_sync_for_device(nic->rx, (size_t)E1000_RING_SIZE * sizeof(nic->rx[0]));
    arch_dma_sync_for_device(nic->tx, (size_t)E1000_RING_SIZE * sizeof(nic->tx[0]));

    uint64_t rx_pa = va_to_pa(nic->rx);
    /* The ring base is a 64-bit physical address split into the low and the
     * high 32-bit half, and RDLEN takes a *descriptor count*, not a byte count:
     * writing sizeof(nic->rx) (16 bytes per descriptor) would size the ring 16x
     * too large and let the device DMA past the arrays.  RDH is the first
     * descriptor the device may fill and RDT the last one it is allowed to
     * fill, so an empty ring is RDH = 0, RDT = size - 1. */
    e1000_write(nic, E1000_RDBAL, (uint32_t)rx_pa);
    e1000_write(nic, E1000_RDBAH, (uint32_t)(rx_pa >> 32));
    e1000_write(nic, E1000_RDLEN, E1000_RING_SIZE);
    e1000_write(nic, E1000_RDH, 0);
    e1000_write(nic, E1000_RDT, E1000_RING_SIZE - 1U);

    uint64_t tx_pa = va_to_pa(nic->tx);
    /* Same 64-bit split and same descriptor-count units on transmit.  TDH is
     * the descriptor the device is retiring and TDT the next one it should
     * send, so an empty ring is both 0. */
    e1000_write(nic, E1000_TDBAL, (uint32_t)tx_pa);
    e1000_write(nic, E1000_TDBAH, (uint32_t)(tx_pa >> 32));
    e1000_write(nic, E1000_TDLEN, E1000_RING_SIZE);
    e1000_write(nic, E1000_TDH, 0);
    e1000_write(nic, E1000_TDT, 0);

    /* TIPG is three 10-bit gap fields, low to high: IPG[9:0] = 0x0A before a
     * new transmit, IPGR1[19:10] = 0x08 after a deferred or retried transmit,
     * IPGR2[29:20] = 0x06 after carrier extension.  The 8254x keeps all three
     * separately, so programming only the low field shortens the gap on retried
     * frames and the link partner starts reporting FCS errors on frames this
     * driver never touched. */
    e1000_write(nic, E1000_TIPG, 10U | (8U << 10) | (6U << 20));
    /* TCTL[11:4] is IFCS, the inter-frame gap counted in 4-byte time
     * intervals, so 0x10 << 4 programs 16 of them; it changes the gap the link
     * partner sees and nothing else, which makes it a link-quality knob rather
     * than a correctness bit.  TCTL[21:12] is not a field in the 8254x/8257x
     * TCTL register set (only EN, PSP and IFCS are defined there), so the
     * 0x40 << 12 term lands on an unassigned bit: inert on copper, but it does
     * read back as set and will show up in any TCTL diff against a reference
     * dump. */
    e1000_write(nic, E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                (0x10U << 4) | (0x40U << 12));
    /* The receiver is enabled last, after both rings and the transmit side, so
     * the device can never DMA into a ring this driver is still filling.  RCTL
     * also has to be 0 before the ring base registers are programmed again on a
     * re-probe; e1000_remove() clears it.  JUMBO is written before RCTL.EN so
     * the ceiling is in place the instant the receiver can pass a long frame. */
    e1000_write(nic, E1000_JUMBO, E1000_JUMBO_BYTES);
    e1000_write(nic, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM |
                E1000_RCTL_SECRC | E1000_RCTL_LPE);

    dev->drv_priv = nic;
    /* Throttle before the causes are unmasked below, so the first interrupt
     * this device can raise is already governed by ITR. */
    e1000_itr_init(dev, nic);
    /* Prefer message-signalled interrupts: the table entries are programmed
     * and left masked, so nothing can arrive before the handlers below exist.
     * A device that cannot describe its table keeps the shared INTx line, and
     * one that does but whose vectors cannot be reserved also falls back. */
    int msix = e1000_msix_setup(dev, nic);
    if (msix == 0) {
        for (int i = 0; i < nic->msix_vectors; i++) {
            uint32_t line = (uint32_t)(nic->msix_base + i);
            if (request_irq(line, e1000_irq_handler, 0, dev) != 0) {
                kinfo("[E1000] vector %d not reservable; using legacy "
                      "interrupts\n", line);
                e1000_msix_teardown(dev, nic);
                msix = -1;
                break;
            }
        }
        if (msix == 0) {
            /* Both handlers are installed, so the entries may now be armed and
             * the function enabled. */
            e1000_msix_commit(dev, nic);
            nic->irq = nic->msix_base;
            nic->irq_registered = 1;
        }
    }
    if (msix != 0) {
        int irq = pci_intx_irq(dev);
        if (irq >= 0) {
            if (request_irq((uint32_t)irq, e1000_irq_handler, IRQF_SHARED,
                            dev) == 0) {
                nic->irq = irq;
                nic->irq_registered = 1;
            } else {
                kinfo("[E1000] IRQ %d registration failed; using polling\n",
                      irq);
            }
        }
    }
    /* Unmask device causes only with a handler in place. */
    if (nic->irq_registered)
        e1000_write(nic, E1000_IMS, E1000_IMS_USED);
    /* STATUS[1] is the read-only link status: 1 = link up. */
    kinfo("[E1000] ready: mac=%02x:%02x:%02x:%02x:%02x:%02x link=%s irq=%d%s ring=%u itr=%uus\n",
          nic->mac[0], nic->mac[1], nic->mac[2], nic->mac[3], nic->mac[4],
          nic->mac[5], (e1000_read(nic, E1000_STATUS) & 2U) ? "up" : "down",
          nic->irq_registered ? nic->irq : -1,
          nic->msix_vectors > 0 ? " (msix)" : "",
          (unsigned)E1000_RING_SIZE, (unsigned)nic->itr_us);
    return 0;
}

static int e1000_remove(device_t *dev)
{
    e1000_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic)
        return 0;
    /* Mask device causes before releasing the handler. */
    e1000_write(nic, E1000_IMC, 0xFFFFFFFFU);
    if (nic->msix_vectors > 0) {
        /* Both table entries have to go away while their handlers are still
         * installed, otherwise a message in flight lands on a line that has no
         * owner left. */
        for (int i = 0; i < nic->msix_vectors; i++)
            free_irq((uint32_t)(nic->msix_base + i), dev);
        e1000_msix_teardown(dev, nic);
    } else if (nic->irq_registered) {
        free_irq((uint32_t)nic->irq, dev);
    }
    e1000_write(nic, E1000_RCTL, 0);
    e1000_write(nic, E1000_TCTL, 0);
    /* Rings go last: they are what the device was DMAing into, and RCTL/TCTL
     * above are what stop it. */
    e1000_free_rings(nic);
    dev->drv_priv = NULL;
    memset(nic, 0, sizeof(*nic));
    return 0;
}

static const net_dev_ops_t e1000_ops = {
    .send = e1000_send,
    .recv = e1000_recv,
    .mac = e1000_mac,
    .poll = e1000_poll,
};

static const device_id_t e1000_ids[] = {
    { .vendor = E1000_VENDOR_INTEL, .device = E1000_DEVICE_82540EM,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = E1000_VENDOR_INTEL, .device = E1000_DEVICE_82545EM,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = E1000_VENDOR_INTEL, .device = E1000_DEVICE_82546EB,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = E1000_VENDOR_INTEL, .device = E1000_DEVICE_82541PI,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = E1000_VENDOR_INTEL, .device = E1000_DEVICE_82574L,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t e1000_driver = {
    .name = "e1000",
    .id_table = e1000_ids,
    .bus = &pci_bus,
    .probe = e1000_probe,
    .remove = e1000_remove,
    .class_ops = &e1000_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(e1000_driver);
