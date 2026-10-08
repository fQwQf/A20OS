/*
 * A20OS RTL8139 PCI Fast Ethernet driver (PCI ID 10ec:8139).
 *
 * Register-level implementation of the classic ("non C+") RTL8139 data path,
 * structured exactly like kernel/drivers/net/e1000.c: a PCI id_table, one
 * static instance, and the net_dev_ops_t class interface consumed by
 * kernel/net/lwip_stack.c through device_find_by_class(DEV_CLASS_NET, n).
 *
 * Why this part is here at all
 * ----------------------------
 * The tree's only other emulated-PCI NIC (e1000) claims five Intel IDs, so a
 * QEMU machine configured with -nic user,model=rtl8139 -- which is what a lot
 * of older guest images and CI recipes ask for -- produced no NET class device
 * at all.  That is exactly the gap docs/platforms/x86_64-pc.md recorded as
 * "not implemented".
 *
 * The register map below was checked against the external Debian QEMU
 * 10.0.13+ds source tree (hw/net/rtl8139.c). Source provenance and the
 * external-unpack locator are in docs/history/2026-10-08/migration.md:
 *   - register file            hw/net/rtl8139.c:96-140
 *   - ISR/IMR bit names        hw/net/rtl8139.c:161-172
 *   - TX status bit names      hw/net/rtl8139.c:174-181
 *   - RX status bit names      hw/net/rtl8139.c:183-192
 *   - classic TX kick path     hw/net/rtl8139.c:1795-1836, 2406-2450
 *   - ring RX packet format    hw/net/rtl8139.c:1124-1170
 *   - CAPR/CBR off-by-16       hw/net/rtl8139.c:2540-2552
 * Two places where the QEMU model and the RTL8139C datasheet disagree are
 * called out at the code that depends on them (TXPOLL_VALUE, and the 64 KiB
 * receive ring chosen for RCR[12:11]).
 *
 * What is NOT claimed
 * -------------------
 *   - No silicon run.  Everything here has been compiled and reasoned about
 *     against the QEMU model; no RTL8139 in a real PC was exercised.
 *   - No checksum offload (see rtl8139_caps()).
 *   - No scatter-gather send (send() copies linearly; see below).
 *   - One static instance.
 *   - No carrier report: link_up is left NULL, which the net class documents as
 *     "always up" (device-classes.md, Network section).  The part does have a
 *     link bit in the Media Status Register, but its polarity differs across
 *     the 8139 / 8139B / 8139C variants this ID covers, and reporting a
 *     permanent "down" would silently take the netif offline -- a worse failure
 *     than the always-up default it replaces.
 */

#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "net/lwip_stack.h"
#include "core/bootargs.h"
#include "core/cpu.h"
#include "core/defs.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/perf.h"
#include "core/string.h"
#include "mm/mm.h"

#define RTL8139_VENDOR_REALTEK 0x10ECU
#define RTL8139_DEVICE_8139    0x8139U

/* ---------------------------------------------------------------- register
 * map.  Offsets are byte offsets; the widths in the comments are the widths
 * the QEMU model implements for each one, in its io_read8/16/32 and io_write8/
 * 16/32 dispatchers.  The file mixes 8-, 16- and 32-bit registers, so every
 * access below goes through a readw/writel helper with the width spelled out;
 * a single generic rtl8139_read() would silently issue the wrong width at half
 * the offsets. */
#define RTL8139_IDR0     0x00U  /* 8-bit x6: MAC address */
#define RTL8139_TSD(n)   (0x10U + 4U * (n)) /* 32-bit x4: transmit status */
#define RTL8139_TSAD(n)  (0x20U + 4U * (n)) /* 32-bit x4: transmit start addr */
#define RTL8139_RBSTART  0x30U  /* 32-bit: receive buffer start (physical) */
#define RTL8139_CR       0x37U  /* 8-bit: command register */
#define RTL8139_CAPR     0x38U  /* 16-bit: current address of packet read */
#define RTL8139_CBR      0x3AU  /* 16-bit: current buffer address (read-only) */
#define RTL8139_IMR      0x3CU  /* 16-bit: interrupt mask */
#define RTL8139_ISR      0x3EU  /* 16-bit: interrupt status, write-1-to-clear */
#define RTL8139_TCR      0x40U  /* 32-bit: transmit configuration */
#define RTL8139_RCR      0x44U  /* 32-bit: receive configuration */
#define RTL8139_RXMISSED 0x4CU  /* 32-bit: receive-missed counter, write clears */
#define RTL8139_TXPOLL   0xD9U  /* 8-bit: transmit poll (C+ mode) */

/* CR bits (hw/net/rtl8139.c:146-151). */
#define RTL8139_CR_RST   0x10U  /* reset; self-clearing in the model */
#define RTL8139_CR_TE    0x04U  /* transmitter enable */
#define RTL8139_CR_RE    0x08U  /* receiver enable */

/* TSD bits.  Named per the RTL8139C datasheet; the QEMU model names the same
 * bits differently (hw/net/rtl8139.c:174-181 calls 0x2000 TxHostOwns and
 * 0x8000 TxStatOK) but rtl8139_transmit_one() sets 0x2000 and 0x8000 together
 * on a successful transmit, so a TOK test against 0x2000 retires on both.
 * After reset the model leaves 0x2000 set in every descriptor, which is why the
 * busy mask starts clear instead of being derived from TSD.
 * The length occupies bits 12:0. */
#define RTL8139_TSD_LEN_MASK 0x1FFFU
#define RTL8139_TSD_TOK      0x2000U /* transmit OK */
#define RTL8139_TSD_TUN      0x4000U /* transmit FIFO underrun */
#define RTL8139_TSD_TOU      0x8000U /* transmit FIFO overflow / aborted */

/* ISR and IMR bits (hw/net/rtl8139.c:161-172). */
#define RTL8139_IRQ_ROK         0x0001U /* receive OK */
#define RTL8139_IRQ_RXE         0x0002U /* receive error */
#define RTL8139_IRQ_TOK         0x0004U /* transmit OK */
#define RTL8139_IRQ_TXE         0x0008U /* transmit error */
#define RTL8139_IRQ_BE          0x0010U /* buffer empty */
#define RTL8139_IRQ_RXFIFOOVER  0x0040U /* receive FIFO overflow */
#define RTL8139_IRQ_PCSTIMEOUT  0x0100U /* PCI system timeout */
/* Bits 9..12 of IMR are read-only in the model (hw/net/rtl8139.c:2600-2602),
 * and nothing in this set lands in that window, so the mask below programs
 * exactly the causes this driver services. */
#define RTL8139_IMR_USED (RTL8139_IRQ_ROK | RTL8139_IRQ_RXE | RTL8139_IRQ_TOK | \
                          RTL8139_IRQ_TXE | RTL8139_IRQ_BE |                 \
                          RTL8139_IRQ_RXFIFOOVER)

/* RCR bits (hw/net/rtl8139.c:194-201). */
#define RTL8139_RCR_ACCEPT_MY_PHYS   0x0002U
#define RTL8139_RCR_ACCEPT_MULTICAST 0x0004U
#define RTL8139_RCR_ACCEPT_BROADCAST 0x0008U
#define RTL8139_RCR_ACCEPT_ERR       0x0020U
/* RCR[12:11] selects the ring size as 8192 << (RxConfig >> 11) & 0x3.  0b11 is the only
 * setting whose QEMU behaviour needs no extra wrap bit: rtl8139_write_buffer()
 * takes its wrap branch unconditionally once RxBufferSize is 65536
 * (hw/net/rtl8139.c:745-777), while every smaller size is only taken when the
 * RXCFGWRAP bit is clear.  Real silicon documents 0b11 as the "8K + 16K" mode,
 * whose ring geometry this driver has NOT verified -- see risks in the commit
 * message and docs/drivers/meta/implementation-status.md. */
#define RTL8139_RCR_RING_64K  0x1800U

/*
 * Transmit poll value.  The RTL8139C datasheet's TxPoll "transmit poll" bit is
 * bit 5 and bit 7 selects the priority queue; Linux's rtl8139 driver writes
 * 0x20 (bit 5 only, normal priority).  QEMU's model triggers transmission on
 * bit 6 and treats bit 7 as "high priority" (hw/net/rtl8139.c:2743-2758).
 * Setting both bits keeps bit 7 clear, so the priority queue is the normal one
 * under either reading, and the frame goes out under either reading.  There is
 * no value that is both datasatically minimal and model-correct; this is the
 * honest compromise, and it is the one place where the two disagree.
 */
#define RTL8139_TXPOLL_VALUE 0x60U

/* RX header status bits (hw/net/rtl8139.c:183-192). */
#define RTL8139_RXST_OK      0x0001U
#define RTL8139_RXST_CRC_ERR 0x0004U
#define RTL8139_RXST_ALIGN   0x0002U
#define RTL8139_RXST_RUNT    0x0010U
#define RTL8139_RXST_LONG    0x0008U
/* A status word of zero means the slot has not been written by the device:
 * the ring is zeroed at probe and only the device ever writes a header.  Any
 * non-zero word is a real header, so an errored frame is retired rather than
 * mistaken for "empty" -- testing the OK bit instead would wedge the ring on
 * the first CRC error. */
#define RTL8139_RX_ERRORS \
    (RTL8139_RXST_CRC_ERR | RTL8139_RXST_ALIGN | \
     RTL8139_RXST_RUNT | RTL8139_RXST_LONG)

#define RTL8139_TX_DESC_COUNT 4U
/* Each TSAD buffer must be 1024-byte aligned on silicon, so the stride has to
 * be a multiple of 1024.  2048 also covers the widest frame the stack can hand
 * this driver (net_profile.h sizes NET_PROFILE_NETIF_FRAME_SIZE at 1600). */
#define RTL8139_TX_BUF_SIZE 2048U
#define RTL8139_TX_STRIDE   2048U
#define RTL8139_RX_BUF_SIZE 65536U
/* 1024 of slack for the alignment of the transmit region inside the block. */
#define RTL8139_BUF_TOTAL \
    (RTL8139_RX_BUF_SIZE + RTL8139_TX_DESC_COUNT * RTL8139_TX_STRIDE + 1024U)
/* Ethernet minimum frame: the payload plus header, before the FCS.  The device
 * appends the FCS itself but does NOT pad, so a short frame would leave the wire
 * at that length and the peer would count FCS errors. */
#define RTL8139_MIN_FRAME 60U

/*
 * Kernel parameter: a20.rtl8139.poll=1 keeps the driver on the polling path --
 * no IRQ is registered and IMR stays masked, so the class .poll hook plus
 * a20_lwip_poll_waiter() are the only things that move packets.  Default is
 * interrupt-driven.
 *
 * Parsed the same way as a20.wx (kernel/mm/wx.c:79-116) and the a20.ip family
 * (kernel/net/net_config.c:88): scan bootargs_get() for "key=value" tokens.
 * bootargs_init() runs before driver_core_init() in kernel/main.c, so the
 * command line is already cached by the time probe() runs.
 */
static int rtl8139_poll_mode = 0;

static const char *rtl8139_param_value(const char *tok, const char *tok_end,
                                       const char *key, char *val, size_t valsz)
{
    size_t klen = strlen(key);
    if ((size_t)(tok_end - tok) < klen + 1)
        return NULL;
    /* memcmp(), not strncmp(): this file is also #included verbatim by
     * kernel/drvmod/examples/rtl8139.c, and a loadable module may only call
     * symbols in the kernel's drv_export_table[] -- strncpy/memcmp/strcmp/
     * strlen are exported (kernel/drvmod/framework.c:313-318) but strncmp is
     * not, and the loader rejects the package outright on an unresolved
     * symbol rather than failing later at run time. */
    if (memcmp(tok, key, klen) != 0 || tok[klen] != '=')
        return NULL;

    const char *vstart = tok + klen + 1;
    size_t vlen = (size_t)(tok_end - vstart);
    if (vlen >= valsz)
        vlen = valsz - 1;
    memcpy(val, vstart, vlen);
    val[vlen] = '\0';
    return tok_end;
}

static void rtl8139_parse_cmdline(void)
{
    const char *cmdline = bootargs_get();
    char val[16];

    for (const char *p = cmdline; p && *p; ) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok_end = p;
        while (*tok_end && *tok_end != ' ' && *tok_end != '\t')
            tok_end++;

        if (rtl8139_param_value(p, tok_end, "a20.rtl8139.poll", val,
                                sizeof(val))) {
            if (strcmp(val, "1") == 0)
                rtl8139_poll_mode = 1;
            else if (strcmp(val, "0") == 0)
                rtl8139_poll_mode = 0;
            else
                kwarn("[RTL8139] unknown a20.rtl8139.poll='%s', keeping "
                      "interrupt-driven\n", val);
        }
        p = tok_end;
    }
}

typedef struct {
    uintptr_t regs;
    uint8_t mac[6];
    /* LOCK_ORDER: nic->lock is the innermost lock.  It protects the receive
     * ring pointer, the transmit descriptor busy mask, the TSAD/TSD programming
     * and the bounce buffers.  The IRQ handler takes only g_lwip_lock and then
     * calls into lwIP; send/recv/poll take only nic->lock and never call back
     * into lwIP.  The two are never held together -- see
     * docs/drivers/guide/lock-order.md. */
    spinlock_t lock;
    uint8_t *rx;                  /* RTL8139_RX_BUF_SIZE ring */
    uint8_t (*tx)[RTL8139_TX_BUF_SIZE];
    /* The kmalloc() pointer itself, because that is what kfree() takes. */
    void *buf_mem;
    uint32_t rx_read;             /* ring offset the next frame starts at */
    uint32_t tx_next;             /* next descriptor send() will try */
    uint32_t tx_busy;             /* bit i set: descriptor i still owned */
    /* Set last, after the IRQ is registered and before the causes are unmasked.
     * The handler checks it because request_irq() is a window in which an
     * interrupt can arrive while dev->drv_priv is still the previous value. */
    int ready;
    int irq;
    int irq_registered;
    uint64_t rx_packets;
    uint64_t rx_drops;
    uint64_t tx_packets;
    uint64_t tx_drops;
} rtl8139_device_t;

static rtl8139_device_t g_rtl8139;

static inline uint8_t rtl8139_readb(rtl8139_device_t *nic, uint32_t reg)
{
    return readb((const volatile void *)(nic->regs + reg));
}

static inline uint16_t rtl8139_readw(rtl8139_device_t *nic, uint32_t reg)
{
    return readw((const volatile void *)(nic->regs + reg));
}

static inline uint32_t rtl8139_readl(rtl8139_device_t *nic, uint32_t reg)
{
    return readl((const volatile void *)(nic->regs + reg));
}

static inline void rtl8139_writeb(rtl8139_device_t *nic, uint32_t reg,
                                  uint8_t value)
{
    writeb(value, (volatile void *)(nic->regs + reg));
}

static inline void rtl8139_writew(rtl8139_device_t *nic, uint32_t reg,
                                  uint16_t value)
{
    writew(value, (volatile void *)(nic->regs + reg));
}

static inline void rtl8139_writel(rtl8139_device_t *nic, uint32_t reg,
                                  uint32_t value)
{
    writel(value, (volatile void *)(nic->regs + reg));
}

#define RTL8139_ALIGN_UP(p, a) \
    ((p) = (void *)(((uintptr_t)(p) + ((a) - 1)) & ~(uintptr_t)((a) - 1)))

/* Defined below; alloc_buffers() calls it on the out-of-window path. */
static void rtl8139_free_buffers(rtl8139_device_t *nic);

static int rtl8139_alloc_buffers(rtl8139_device_t *nic, device_t *dev)
{
    void *raw = kmalloc(RTL8139_BUF_TOTAL);
    if (!raw) {
        kinfo("[RTL8139] no memory for the %u byte ring + %u transmit "
              "buffers (%u bytes total)\n",
              (unsigned)RTL8139_RX_BUF_SIZE,
              (unsigned)(RTL8139_TX_DESC_COUNT * RTL8139_TX_BUF_SIZE),
              (unsigned)RTL8139_BUF_TOTAL);
        return -ENOMEM;
    }
    memset(raw, 0, RTL8139_BUF_TOTAL);
    nic->buf_mem = raw;

    /* One block backs both the ring and every transmit buffer, and every
     * address this driver programs is the low 32 bits of it: RBSTART is a
     * 32-bit register and TSAD is four 32-bit registers.  A GPA above 4 GiB
     * would therefore be silently truncated to a different physical page --
     * the transmit path leaking kernel memory to the wire, the receive path
     * letting the device DMA over an unrelated page.  One range check over
     * the whole block covers every programmed address. */
    if (!dma_range_ok(dev, va_to_pa(raw), RTL8139_BUF_TOTAL)) {
        kinfo("[RTL8139] ring block 0x%lx..0x%lx is outside the 32-bit DMA "
              "window\n", (unsigned long)va_to_pa(raw),
              (unsigned long)(va_to_pa(raw) + RTL8139_BUF_TOTAL - 1U));
        rtl8139_free_buffers(nic);
        return -EOPNOTSUPP;
    }

    /* The block came from the buddy allocator (it is far above SLAB_MAX_OBJ),
     * so it is page aligned and both the ring base and every transmit buffer
     * that follows it inherit the alignment the hardware requires.  Aligning
     * anyway keeps that a property of this function rather than of kmalloc(). */
    RTL8139_ALIGN_UP(raw, RTL8139_TX_STRIDE);
    nic->rx = raw;
    raw += RTL8139_RX_BUF_SIZE;
    RTL8139_ALIGN_UP(raw, RTL8139_TX_STRIDE);
    nic->tx = (uint8_t (*)[RTL8139_TX_BUF_SIZE])raw;
    return 0;
}

static void rtl8139_free_buffers(rtl8139_device_t *nic)
{
    if (nic->buf_mem)
        kfree(nic->buf_mem);
    nic->buf_mem = NULL;
    nic->rx = NULL;
    nic->tx = NULL;
}

/* Retire transmit descriptors the device has finished.  Must be called with
 * nic->lock held. */
static void rtl8139_reclaim_tx_locked(rtl8139_device_t *nic)
{
    for (uint32_t i = 0; i < RTL8139_TX_DESC_COUNT; i++) {
        if (!(nic->tx_busy & (1U << i)))
            continue;
        uint32_t tsd = rtl8139_readl(nic, RTL8139_TSD(i));
        if (tsd & RTL8139_TSD_TOK) {
            nic->tx_busy &= ~(1U << i);
            nic->tx_packets++;
            a20_perf_count(A20_PERF_RTL8139_TX_RECLAIMED);
        } else if (tsd & (RTL8139_TSD_TUN | RTL8139_TSD_TOU)) {
            /* The device gave the descriptor back without sending it, so the
             * slot is reusable; counting it is what makes a silent loss show
             * up as tx_drops rather than as a hang. */
            nic->tx_busy &= ~(1U << i);
            nic->tx_drops++;
        }
        /* Otherwise the device still owns it.  Note the consequence: a
         * descriptor whose TSD never gets TOK or TUN/TOU stays busy forever and
         * costs one quarter of the transmit ring.  Nothing recovers it, because
         * writing TSD on a descriptor the device still owns would corrupt a
         * frame in flight; a device that retires nothing is a device that has
         * stopped, and the fix for that is a reset, not a re-arm. */
    }
}

/* Copy len bytes out of the receive ring starting at offset `off`, following
 * the ring's wrap.  Must be called with nic->lock held. */
static void rtl8139_ring_read(rtl8139_device_t *nic, uint32_t off, uint8_t *dst,
                              uint32_t len)
{
    uint32_t first = RTL8139_RX_BUF_SIZE - off;
    if (first > len)
        first = len;
    memcpy(dst, nic->rx + off, first);
    if (first < len)
        memcpy(dst + first, nic->rx, len - first);
}

/* Invalidate the CPU's view of the region just read.  Only the bytes actually
 * consumed are synced, not the whole 64 KiB ring: on an architecture where
 * arch_dma_sync_for_cpu() is a real cache operation, a per-frame 64 KiB sync
 * would cost more than the copy it guards. */
static void rtl8139_ring_sync_read(rtl8139_device_t *nic, uint32_t off,
                                   uint32_t len)
{
    if (off + len <= RTL8139_RX_BUF_SIZE) {
        arch_dma_sync_for_cpu(nic->rx + off, len);
        return;
    }
    uint32_t first = RTL8139_RX_BUF_SIZE - off;
    arch_dma_sync_for_cpu(nic->rx + off, first);
    arch_dma_sync_for_cpu(nic->rx, len - first);
}

/* Hand the ring back to the device.
 *
 * CAPR is the offset of the next unread packet MINUS 16: the model's
 * rtl8139_RxBufPtr_write() stores val + 0x10 and its read path subtracts 0x10
 * again (hw/net/rtl8139.c:2540-2552), which is the same 16-byte dead zone the
 * RTL8139C datasheet documents.  Writing the consumed offset verbatim would
 * leave the last 16 bytes of every packet unacknowledged and the ring would
 * eventually stop.
 *
 * CAPR is a 16-bit register, so the value has to be masked rather than left to
 * truncate at a non-multiple of 0x10000. */
static void rtl8139_release_ring_locked(rtl8139_device_t *nic)
{
    rtl8139_writew(nic, RTL8139_CAPR,
                   (uint16_t)((nic->rx_read - 16U) & 0xFFFFU));
}

/* RTL8139_IRQ_MODEL:
 * - The handler acknowledges by writing the ISR back (write-1-to-clear; a read
 *   does NOT clear it in this model, hw/net/rtl8139.c:2620-2637), then resolves
 *   the device's netif index and runs the same bounded RX drain the virtio-net
 *   IRQ path uses, under g_lwip_lock.  nic->lock is deliberately not taken
 *   here: the lwIP drain reaches recv(), which takes it.
 * - A shared line with no pending cause costs one register read.
 * - IMR is unmasked only after the handler is registered; with
 *   a20.rtl8139.poll=1 it is never unmasked at all and .poll is the only path. */
static int rtl8139_irq_handler(int irq, void *priv)
{
    (void)irq;
    device_t *dev = (device_t *)priv;
    rtl8139_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !nic->ready)
        return 0;

    uint16_t isr = rtl8139_readw(nic, RTL8139_ISR);
    if (!isr)
        return 0;
    rtl8139_writew(nic, RTL8139_ISR, isr);

    /* Same 8-slot walk e1000_irq_handler() does: A20_NET_MAX_DEVS is private to
     * kernel/net/lwip_stack.c, and the enumeration stops at the first NULL. */
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

    /* Counted here, after the drain, and only for causes this line can raise.
     * A gate that asserted "the network works" would pass with the handler
     * entirely dead, because .poll reclaims descriptors from the same lwIP
     * drain whether or not an interrupt arrives -- so the counters are what
     * make the interrupt path observable from outside.  IRQ_RX is ISR.TOK: only
     * the receive ring raises it.  IRQ_TX is the transmit half of the cause,
     * reported as a TSD TOK on any descriptor rather than as an ISR bit, because
     * the ISR carries no distinct per-descriptor completion. */
    a20_perf_count(A20_PERF_RTL8139_IRQ_CALLS);
    if (isr & RTL8139_IRQ_TOK)
        a20_perf_count(A20_PERF_RTL8139_IRQ_RX);
    for (uint32_t i = 0; i < RTL8139_TX_DESC_COUNT; i++) {
        if (rtl8139_readl(nic, RTL8139_TSD(i)) & RTL8139_TSD_TOK) {
            a20_perf_count(A20_PERF_RTL8139_IRQ_TX);
            break;
        }
    }
    return 0;
}

static void rtl8139_poll(device_t *dev)
{
    rtl8139_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !nic->rx)
        return;

    uint64_t flags = spin_lock_irqsave(&nic->lock);
    rtl8139_reclaim_tx_locked(nic);
    spin_unlock_irqrestore(&nic->lock, flags);
}

/*
 * Linear transmit.  The frame is copied into the descriptor's bounce buffer
 * before TSAD/TSD are programmed, so the device only ever reads memory this
 * driver owns.
 *
 * The class contract is "copies the pbuf chain into its staging buffer and
 * calls send()", which is why send_sg() stays NULL below: with the field
 * absent, lwip_stack.c hands this path byte-for-byte the frames it would have
 * handed any unconverted driver (driver_class.h:165-176).
 */
static int rtl8139_send(device_t *dev, const void *packet, size_t length)
{
    rtl8139_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !nic->tx || !packet)
        return -EINVAL;
    if (length == 0 || length > RTL8139_TX_BUF_SIZE)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&nic->lock);

    rtl8139_reclaim_tx_locked(nic);
    if (nic->tx_busy == (1U << RTL8139_TX_DESC_COUNT) - 1U) {
        spin_unlock_irqrestore(&nic->lock, flags);
        return -EAGAIN;
    }

    uint32_t slot = nic->tx_next;
    while (nic->tx_busy & (1U << slot))
        slot = (slot + 1U) % RTL8139_TX_DESC_COUNT;

    uint8_t *buf = nic->tx[slot];
    memcpy(buf, packet, length);
    if (length < RTL8139_MIN_FRAME)
        memset(buf + length, 0, RTL8139_MIN_FRAME - length);
    arch_dma_sync_for_device(buf, RTL8139_TX_BUF_SIZE);

    /* TSAD before TSD.  Writing TSD is what makes the device look at the
     * descriptor, so the address has to be visible first; the writel() barrier
     * in rtl8139_writel() is that ordering. */
    rtl8139_writel(nic, RTL8139_TSAD(slot), (uint32_t)va_to_pa(buf));
    rtl8139_writel(nic, RTL8139_TSD(slot),
                   (uint32_t)(length < RTL8139_MIN_FRAME ?
                              RTL8139_MIN_FRAME : length));
    rtl8139_writeb(nic, RTL8139_TXPOLL, RTL8139_TXPOLL_VALUE);

    nic->tx_busy |= 1U << slot;
    nic->tx_next = (slot + 1U) % RTL8139_TX_DESC_COUNT;
    spin_unlock_irqrestore(&nic->lock, flags);
    return (int)length;
}

/*
 * One frame out of the ring, or 0 when the ring is empty.
 *
 * Oversize policy: the frame is DROPPED, not truncated.  device-classes.md
 * ("Network" section) allows either dropping or -EMSGSIZE for a frame that does
 * not fit the caller's buffer, and explicitly forbids handing back a prefix of
 * a frame -- the net class contract is one frame per call.  A dropped frame
 * still advances rx_read and still releases CAPR, so the ring does not drift;
 * it is counted in rx_drops rather than vanishing.
 */
static int rtl8139_recv(device_t *dev, void *buffer, size_t max_length)
{
    rtl8139_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic || !nic->rx || !buffer)
        return -EINVAL;
    if (max_length == 0)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&nic->lock);

    uint32_t slot = nic->rx_read;
    rtl8139_ring_sync_read(nic, slot, 4);
    uint16_t hdr[2];
    memcpy(hdr, nic->rx + slot, sizeof(hdr));
    uint32_t status = hdr[0];
    uint32_t length = hdr[1];

    /* A zero status word is an unwritten slot: the ring was zeroed at probe and
     * only the device writes a header. */
    if (status == 0) {
        spin_unlock_irqrestore(&nic->lock, flags);
        return 0;
    }

    /* The length field counts the payload plus the 4-byte FCS the device
     * appends (hw/net/rtl8139.c:1140).  Anything below 5 is therefore not a
     * frame, and a length longer than the ring cannot be one either. */
    int dropped = 0;
    uint32_t payload = 0;
    if (length < 5U || length > RTL8139_RX_BUF_SIZE) {
        dropped = 1;
    } else if (status & RTL8139_RX_ERRORS) {
        /* Retire the frame without delivering it: handing the stack a prefix of
         * a frame the device flagged as bad is worse than handing it nothing. */
        dropped = 1;
    } else {
        payload = length - 4U;
        if (payload > max_length) {
            dropped = 1;
        } else {
            uint32_t data = (slot + 4U) & (RTL8139_RX_BUF_SIZE - 1U);
            rtl8139_ring_sync_read(nic, data, payload);
            rtl8139_ring_read(nic, data, (uint8_t *)buffer, payload);
        }
    }

    /* Advance past header + payload + FCS on every path, delivered or not --
     * otherwise the next call reads the same header again.  The device aligns
     * the next slot to 4 bytes (hw/net/rtl8139.c:1161); an Ethernet frame is
     * always at least 60 bytes, so 4 + (payload + 4) is already a multiple of 4
     * and the align is a no-op -- spelled out anyway so a future change cannot
     * silently desynchronise the two. */
    nic->rx_read = ((slot + 4U + length) & (RTL8139_RX_BUF_SIZE - 1U)) & ~3U;
    rtl8139_release_ring_locked(nic);
    a20_perf_count(A20_PERF_RTL8139_RX_DRAINED);

    if (dropped) {
        nic->rx_drops++;
        spin_unlock_irqrestore(&nic->lock, flags);
        return 0;
    }
    nic->rx_packets++;
    spin_unlock_irqrestore(&nic->lock, flags);
    return (int)payload;
}

static const uint8_t *rtl8139_mac(device_t *dev)
{
    rtl8139_device_t *nic = dev ? dev->drv_priv : NULL;
    return nic ? nic->mac : NULL;
}

/* Report what this device actually negotiated.  Zero, deliberately.
 *
 * NET_DEV_CAP_TX_CSUM_OFFLOAD and NET_DEV_CAP_RX_CSUM_OFFLOAD stay clear
 * because this tree deliberately does not enable checksum offload at all --
 * see the comment in kernel/drivers/core/driver_class.h:130-143: lwIP 2.2.2 as
 * vendored offers no way to tell a driver "this L4 checksum is a partial sum
 * you must finish", opt.h:2449-2450 defaults LWIP_CHECKSUM_ON_COPY to 0, and
 * netif.h:84-107 defines no flag a checksum handshake could ride on.  Setting
 * either bit under those semantics would leave lwIP verifying a checksum the
 * device never computed.  The RTL8139 does have the TX descriptor bits for it;
 * they are simply not driven.
 *
 * NET_DEV_CAP_TX_SG stays clear because send_sg() is NULL: this driver's
 * send() copies linearly.
 *
 * NET_DEV_CAP_MRG_RXBUF stays clear because the ring-receiver path hands back
 * exactly one packet per recv() call.
 */
static uint32_t rtl8139_caps(device_t *dev)
{
    (void)dev;
    return 0;
}

static int rtl8139_rx_irq_driven(device_t *dev)
{
    rtl8139_device_t *nic = dev ? dev->drv_priv : NULL;
    return nic && nic->irq_registered;
}

static int rtl8139_probe(device_t *dev)
{
    rtl8139_parse_cmdline();

    if (pci_enable_and_assign_bars(dev) < 0)
        return -ENODEV;
    /* BAR1, not BAR0.  This part decodes the same 0x100 register file through
     * two BARs: BAR0 is I/O space and BAR1 is a memory BAR aliasing it
     * (hw/net/rtl8139.c:3395-3396 registers bar_io as
     * PCI_BASE_ADDRESS_SPACE_IO and bar_mem, an alias of bar_io, as
     * PCI_BASE_ADDRESS_SPACE_MEMORY).  BAR0 therefore never becomes a
     * resource: pci_assign_bars() only publishes non-I/O BARs into dev->res
     * (kernel/drivers/bus/pci_bus.c:395), so bar_resource[0] stays -1 and
     * pci_get_bar_resource(dev, 0) returns NULL -- the probe used to fail
     * there, silently, because the only report is a kdebug the default log
     * level drops.  Linux's 8139cp maps the memory BAR for the same reason. */
    resource_t *bar = pci_get_bar_resource(dev, 1);
    /* 0x100 is the smallest window this driver can work in: the highest offset
     * it ever programs is TxPoll at 0xD9, and TXPCR at 0xEC is not used. */
    if (!bar || bar->end < bar->start ||
        bar->end - bar->start + 1U < 0x100U)
        return -ENODEV;

    rtl8139_device_t *nic = &g_rtl8139;
    memset(nic, 0, sizeof(*nic));
    nic->irq = -1;
    spin_init(&nic->lock);
    nic->regs = (uintptr_t)bar->start;

    /* Reset before reading anything else: the reset restores the MAC from the
     * EEPROM shadow, so a MAC read before it would report whatever the previous
     * boot left in IDR0..5. */
    rtl8139_writeb(nic, RTL8139_CR, RTL8139_CR_RST);
    /* Mask every cause and acknowledge whatever the device latched while the
     * line was not owned by anyone, before a handler can be installed. */
    rtl8139_writew(nic, RTL8139_IMR, 0);
    rtl8139_writew(nic, RTL8139_ISR, 0xFFFFU);

    for (unsigned i = 0; i < 6; i++)
        nic->mac[i] = rtl8139_readb(nic, RTL8139_IDR0 + i);
    if (nic->mac[0] == 0xFF || nic->mac[0] == 0x00)
        return -ENODEV;

    /* 32-bit, from what this driver itself programs: RBSTART is a 32-bit
     * register and TSAD is four 32-bit registers (see RTL8139_TSAD below), so
     * the register set this driver targets is itself the evidence that the part
     * cannot be given a wider address than 4 GiB.  Declared before the buffers
     * are allocated, which is the only point at which it can still change what
     * is allocated, and before any of them is programmed into the device. */
    int mask = dma_set_mask(dev, DMA_MASK_32BIT);
    if (mask < 0)
        return mask;

    /* A NIC whose ring cannot be allocated must fail the probe, not come up
     * pointing at memory this driver does not own.  The error is propagated
     * rather than folded into -ENOMEM so that a block outside the window above
     * stays distinguishable from an actual allocation failure. */
    int bufs = rtl8139_alloc_buffers(nic, dev);
    if (bufs < 0)
        return bufs;

    /* RCR before RBSTART and CAPR: writing RCR resets the ring's read and write
     * pointers (hw/net/rtl8139.c:1727-1729), so anything programmed before it
     * is discarded. */
    rtl8139_writel(nic, RTL8139_RCR, RTL8139_RCR_RING_64K |
                                        RTL8139_RCR_ACCEPT_MY_PHYS |
                                        RTL8139_RCR_ACCEPT_MULTICAST |
                                        RTL8139_RCR_ACCEPT_BROADCAST |
                                        RTL8139_RCR_ACCEPT_ERR);
    rtl8139_writel(nic, RTL8139_RXMISSED, 0xFFFFFFFFU);
    rtl8139_writel(nic, RTL8139_RBSTART, (uint32_t)va_to_pa(nic->rx));
    arch_dma_sync_for_device(nic->rx, RTL8139_RX_BUF_SIZE);
    nic->rx_read = 0;
    rtl8139_release_ring_locked(nic);

    /* TCR: interframe gap 9.6 us (TxIFG96, bits 26:24 = 3) and the FIFO
     * threshold.  Bits 16 (TxCRC) is left clear so the device appends the FCS. */
    rtl8139_writel(nic, RTL8139_TCR, (3U << 24) | (256U << 11));
    for (uint32_t i = 0; i < RTL8139_TX_DESC_COUNT; i++) {
        nic->tx_busy &= ~(1U << i);
        rtl8139_writel(nic, RTL8139_TSAD(i),
                       (uint32_t)va_to_pa(nic->tx[i]));
    }

    /* Transmitter before receiver: the device must never DMA into a ring this
     * driver is still filling.  CR is write-1-to-set for TE/RE and keeps the
     * other bits, so this is an OR rather than a replace. */
    rtl8139_writeb(nic, RTL8139_CR, rtl8139_readb(nic, RTL8139_CR) |
                                    RTL8139_CR_TE | RTL8139_CR_RE);

    /* Publish the instance before the line is armed: request_irq() opens a
     * window in which an interrupt can be delivered, and the handler resolves
     * the instance through dev->drv_priv.  ready stays 0 until the causes are
     * unmasked, so an interrupt that arrives in that window is acknowledged
     * and ignored rather than acting on a half-configured ring. */
    dev->drv_priv = nic;

    if (!rtl8139_poll_mode) {
        int irq = pci_intx_irq(dev);
        if (irq >= 0) {
            if (request_irq((uint32_t)irq, rtl8139_irq_handler, IRQF_SHARED,
                            dev) == 0) {
                nic->irq = irq;
                nic->irq_registered = 1;
            } else {
                kinfo("[RTL8139] IRQ %d registration failed; using "
                      "polling\n", irq);
            }
        }
    }
    /* Unmask only with a handler in place; with a20.rtl8139.poll=1 this is
     * never reached and .poll is the only progress path. */
    if (nic->irq_registered)
        rtl8139_writew(nic, RTL8139_IMR, RTL8139_IMR_USED);
    nic->ready = 1;

    kinfo("[RTL8139] ready: mac=%02x:%02x:%02x:%02x:%02x:%02x mode=%s "
          "irq=%d rxring=%u txdesc=%u\n",
          nic->mac[0], nic->mac[1], nic->mac[2], nic->mac[3], nic->mac[4],
          nic->mac[5], nic->irq_registered ? "irq" : "poll",
          nic->irq_registered ? nic->irq : -1,
          (unsigned)RTL8139_RX_BUF_SIZE, (unsigned)RTL8139_TX_DESC_COUNT);
    return 0;
}

static int rtl8139_remove(device_t *dev)
{
    rtl8139_device_t *nic = dev ? dev->drv_priv : NULL;
    if (!nic)
        return 0;

    /* Stop new work first: ready gates the IRQ handler, and the class layer has
     * already marked the class device offline, so no new send/recv can start. */
    nic->ready = 0;
    /* Mask device causes before releasing the handler, so a message in flight
     * has nowhere to land. */
    rtl8139_writew(nic, RTL8139_IMR, 0);
    rtl8139_writew(nic, RTL8139_ISR, 0xFFFFU);
    if (nic->irq_registered)
        free_irq((uint32_t)nic->irq, dev);

    /* Stop the data plane before the memory goes away: CR[1] and CR[0] are the
     * transmitter and receiver enables, and clearing them is what stops the
     * device DMAing into the buffers that kfree() is about to release. */
    rtl8139_writeb(nic, RTL8139_CR, 0);
    rtl8139_free_buffers(nic);

    dev->drv_priv = NULL;
    memset(nic, 0, sizeof(*nic));
    return 0;
}

static const net_dev_ops_t rtl8139_ops = {
    .send = rtl8139_send,
    .recv = rtl8139_recv,
    .mac = rtl8139_mac,
    .poll = rtl8139_poll,
    .rx_irq_driven = rtl8139_rx_irq_driven,
    .caps = rtl8139_caps,
};

static const device_id_t rtl8139_ids[] = {
    { .vendor = RTL8139_VENDOR_REALTEK, .device = RTL8139_DEVICE_8139,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

/* Second-stage narrowing after the bus ID match: the class has to be the PCI
 * ethernet controller class.  pci_class_code() only reads identity the bus
 * already enumerated (PCI spec class code at config offset 0x09), which is what
 * the two-phase match contract allows -- no register access, no BAR, no
 * hardware change.  A 10ec:8139 in any other class is not this device. */
static int rtl8139_match(device_t *dev)
{
    return pci_class_code(dev) == 0x020000U;
}

static driver_t rtl8139_driver = {
    .name = "rtl8139",
    .id_table = rtl8139_ids,
    .bus = &pci_bus,
    .match = rtl8139_match,
    .probe = rtl8139_probe,
    .remove = rtl8139_remove,
    .class_ops = &rtl8139_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(rtl8139_driver);