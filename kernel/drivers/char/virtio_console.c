/*
 * virtio-console (virtio-serial) — port 0 of one PCI function, published as a
 * CHAR class device.  drvmod package `virtio-console.a20drv`; the only
 * difference from a built-in driver is the registration backend in
 * kernel/drvmod/examples/virtio_console.c.
 *
 * Queue layout of a non-multiport virtio-serial device.  QEMU's
 * virtio_serial_device_realize (hw/char/virtio-serial-bus.c) adds, in order,
 * the port-0 receive and transmit queues and then the control pair, before any
 * per-port pair:
 *
 *   q0 receive   host -> guest, port 0     (128 slots offered)
 *   q1 transmit  guest -> host, port 0     (128 slots offered)
 *   q2 control receive, q3 control transmit (deliberately not set up)
 *
 * VIRTIO_CONSOLE_F_MULTIPORT is not negotiated, so the device keeps exactly
 * one port at id 0, serves it from q0/q1 with no control-virtq handshake, and
 * QEMU marks it guest_connected as soon as the driver sets DRIVER_OK.  That is
 * what lets this driver carry a console port on two virtqueues.  The limits
 * that follow are recorded in docs/drivers/meta/implementation-status.md: no
 * port above 0, no multiport control protocol.
 *
 * Receive is interrupt driven: the ISR drains the used ring into a byte ring
 * and mirrors the bytes into the console input path while console mirroring is
 * on.  Transmit is synchronous — one descriptor in flight, bounded wait for its
 * completion, never a hardware wait under the instance lock (see
 * docs/drivers/guide/lock-order.md).
 */
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/char/uart.h"
#include "drivers/char/virtio_console.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drvmod/drvmod.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/* VIRTIO_ID_CONSOLE.  virtio-console and virtio-guest-agent share the id;
 * only virtio-serial implements the port queues used here. */
#define VPORT_DEVICE_ID          3
/* modern (virtio-1.0 only) 1af4:1043 = 0x1040 + VIRTIO_ID_CONSOLE, and the
 * transitional 1af4:1003 that QEMU presents when it also offers the legacy
 * interface (pci_config_set_device_id(config, proxy->trans_devid)). */
#define VPORT_PCI_ID_MODERN      0x1043
#define VPORT_PCI_ID_TRANS       0x1003

/* VirtIO MMIO-style offsets accepted by virtio_transport_t; the PCI transport
 * translates them into the common/notify/ISR capability windows. */
#define VPORT_MMIO_DEV_FEATURES      0x010u
#define VPORT_MMIO_DEV_FEATURES_SEL  0x014u
#define VPORT_MMIO_DRV_FEATURES      0x020u
#define VPORT_MMIO_DRV_FEATURES_SEL  0x024u
#define VPORT_MMIO_QUEUE_SEL         0x030u
#define VPORT_MMIO_QUEUE_NUM_MAX     0x034u
#define VPORT_MMIO_QUEUE_NUM         0x038u
#define VPORT_MMIO_QUEUE_DESC_LOW    0x080u
#define VPORT_MMIO_QUEUE_DESC_HIGH   0x084u
#define VPORT_MMIO_QUEUE_DRIVER_LOW  0x090u
#define VPORT_MMIO_QUEUE_DRIVER_HIGH 0x094u
#define VPORT_MMIO_QUEUE_DEVICE_LOW  0x0a0u
#define VPORT_MMIO_QUEUE_DEVICE_HIGH 0x0a4u
#define VPORT_MMIO_QUEUE_READY       0x044u
#define VPORT_MMIO_QUEUE_NOTIFY      0x050u
#define VPORT_MMIO_INTR_STATUS       0x060u
#define VPORT_MMIO_INTR_ACK          0x064u
#define VPORT_MMIO_STATUS            0x070u

#define VPORT_F_VERSION_1        0x1u
#define VPORT_STATUS_ACK         1u
#define VPORT_STATUS_DRIVER      2u
#define VPORT_STATUS_DRIVER_OK   4u
#define VPORT_STATUS_FEATURES_OK 8u
#define VPORT_STATUS_FAILED      128u

#define VPORT_Q_RX               0U
#define VPORT_Q_TX               1U
#define VPORT_RX_SLOTS           8U
#define VPORT_TX_SLOTS           1U
#define VPORT_BUF_SIZE           4096U
#define VPORT_RING_SIZE          4096U
#define VPORT_TX_TIMEOUT_MS      2000U
/* Console bytes are copied out in batches this size: the copy has to happen
 * under the instance lock (uart_receive_char must not be called with it held,
 * so the bytes cannot simply be emitted there) and an IRQ top-half cannot use
 * a 4 KiB stack buffer. */
#define VPORT_ECHO_BATCH         64U

#define VPORT_QD_F_WRITE         2u

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vport_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VPORT_RX_SLOTS];
    uint16_t used_event;
} vport_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} vport_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vport_used_elem_t ring[VPORT_RX_SLOTS];
    uint16_t avail_event;
} vport_used_t;

/* One coherent allocation per queue: rings first, then the data area, so every
 * descriptor address is dma_handle + a compile-time offset (never va_to_pa). */
typedef struct {
    vport_desc_t  desc[VPORT_RX_SLOTS] ALIGNED(64);
    vport_avail_t avail ALIGNED(64);
    vport_used_t  used ALIGNED(64);
    uint8_t       buf[VPORT_RX_SLOTS][VPORT_BUF_SIZE] ALIGNED(64);
} vport_rx_mem_t;

typedef struct {
    vport_desc_t  desc[VPORT_TX_SLOTS] ALIGNED(64);
    vport_avail_t avail ALIGNED(64);
    vport_used_t  used ALIGNED(64);
    uint8_t       buf[VPORT_TX_SLOTS][VPORT_BUF_SIZE] ALIGNED(64);
} vport_tx_mem_t;

typedef struct {
    virtio_transport_t vt;
    vport_rx_mem_t    *rx_mem;
    uint64_t           rx_dma;
    vport_tx_mem_t    *tx_mem;
    uint64_t           tx_dma;
    /* Guards the byte ring, the console-mirror flag and state, both
     * last_used cursors and the counters.  Innermost lock: uart_receive_char()
     * takes the UART rx lock and flushes wake queues, so it is never called
     * with this held. */
    spinlock_t lock ALIGNED(64);
    uint8_t    ring[VPORT_RING_SIZE];
    uint32_t   head;
    uint32_t   tail;
    uint8_t    echo[VPORT_BUF_SIZE];
    uint32_t   echo_len;
    uint32_t   echo_pos;
    uint32_t   rx_bytes;
    uint32_t   rx_dropped;
    uint32_t   tx_bytes;
    uint32_t   tx_timeouts;
    /* One descriptor and one buffer are shared by every writer, so this says
     * whether the previous write's frame is still owned by the device.  Without
     * it a second writer -- or a retry after a timeout -- overwrites
     * tx_mem->buf[0] and desc[0] while the device is still reading them, and
     * the frame on the wire becomes a splice of two writes.  Guarded by
     * p->lock, and not cleared on the timeout path: the descriptor was already
     * handed to the device, so only the device's own used.idx can retire it. */
    uint8_t    tx_busy;
    /* used.idx as it stood when tx_busy was raised: the frame in flight is
     * retired exactly when used.idx stops equalling this.  Kept across a
     * timeout so a later write can reap a completion that arrived after the
     * deadline instead of writing over a frame the device may still be
     * reading. */
    uint16_t   tx_submitted_used;
    uint16_t   rx_last_used;
    uint16_t   tx_last_used;
    int        console_mirror;
    int        irq;
    int        irq_registered;
    int        valid;
} vport_inst_t;

/* One port, one instance: a second virtio-serial function stays unbound
 * rather than getting a second port nothing multiplexes. */
static vport_inst_t g_vport;

/* ------------------------------------------------------------------ */
/* Ring helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint64_t vport_rx_buf_pa(const vport_inst_t *p, unsigned slot)
{
    return p->rx_dma + offsetof(vport_rx_mem_t, buf) +
           (uint64_t)slot * VPORT_BUF_SIZE;
}

static inline uint64_t vport_tx_buf_pa(const vport_inst_t *p)
{
    return p->tx_dma + offsetof(vport_tx_mem_t, buf);
}

static void vport_rx_post_all(vport_inst_t *p)
{
    for (unsigned i = 0; i < VPORT_RX_SLOTS; i++) {
        p->rx_mem->desc[i].addr = vport_rx_buf_pa(p, i);
        p->rx_mem->desc[i].len = VPORT_BUF_SIZE;
        p->rx_mem->desc[i].flags = VPORT_QD_F_WRITE;
        p->rx_mem->desc[i].next = 0;
    }
    dma_sync_for_device(p->rx_mem, sizeof(*p->rx_mem));
    for (unsigned i = 0; i < VPORT_RX_SLOTS; i++) {
        uint16_t idx = p->rx_mem->avail.idx;
        p->rx_mem->avail.ring[idx % VPORT_RX_SLOTS] = (uint16_t)i;
        p->rx_mem->avail.idx = (uint16_t)(idx + 1);
    }
    wmb();
    dma_sync_for_device(&p->rx_mem->avail, sizeof(p->rx_mem->avail));
}

/*
 * Deliver the console-mirror backlog to the kernel console input path.
 *
 * Called with inst->lock dropped.  The bytes are copied out in batches under
 * the lock and only then handed to uart_receive_char(), which takes the UART
 * rx lock itself: nesting one driver lock inside another is not a documented
 * local order, so this is where the copy has to happen.
 */
static void vport_console_flush(vport_inst_t *p)
{
    uint8_t batch[VPORT_ECHO_BATCH];

    for (;;) {
        uint32_t n;
        uint64_t flags = spin_lock_irqsave(&p->lock);
        if (p->echo_pos >= p->echo_len) {
            p->echo_len = 0;
            p->echo_pos = 0;
            spin_unlock_irqrestore(&p->lock, flags);
            return;
        }
        n = p->echo_len - p->echo_pos;
        if (n > VPORT_ECHO_BATCH)
            n = VPORT_ECHO_BATCH;
        memcpy(batch, p->echo + p->echo_pos, n);
        p->echo_pos += n;
        spin_unlock_irqrestore(&p->lock, flags);

        for (uint32_t i = 0; i < n; i++)
            uart_receive_char((char)batch[i]);
    }
}

/*
 * Move everything the device completed into the byte ring and repost the slots.
 * Runs from the IRQ top-half and from read()/poll(), so it must not block.
 */
static void vport_rx_pump(vport_inst_t *p)
{
    int repost = 0;

    uint64_t flags = spin_lock_irqsave(&p->lock);
    if (!p->valid || !p->rx_mem) {
        spin_unlock_irqrestore(&p->lock, flags);
        return;
    }

    dma_sync_for_cpu(&p->rx_mem->used, sizeof(p->rx_mem->used));
    while (p->rx_last_used != p->rx_mem->used.idx) {
        uint16_t ring_idx = (uint16_t)(p->rx_last_used % VPORT_RX_SLOTS);
        uint32_t id = p->rx_mem->used.ring[ring_idx].id;
        uint32_t len = p->rx_mem->used.ring[ring_idx].len;
        if (id >= VPORT_RX_SLOTS)
            break; /* protocol violation: stop consuming, keep the rest */
        if (len > VPORT_BUF_SIZE)
            len = VPORT_BUF_SIZE;

        uint8_t *chunk = p->rx_mem->buf[id];
        dma_sync_for_cpu(chunk, len);

        uint32_t kept = 0;
        for (uint32_t i = 0; i < len; i++) {
            uint32_t next = (p->head + 1U) % VPORT_RING_SIZE;
            if (next == p->tail) {
                p->rx_dropped++;
                break;
            }
            p->ring[p->head] = chunk[i];
            p->head = next;
            kept++;
            p->rx_bytes++;
        }

        /* Post back exactly what was copied out, so the device never
         * overwrites bytes this CPU has not read yet. */
        if (kept) {
            dma_sync_for_device(chunk, kept);
            if (p->console_mirror) {
                uint32_t room = sizeof(p->echo) - p->echo_len;
                uint32_t n = kept < room ? kept : room;
                if (n) {
                    memcpy(p->echo + p->echo_len, chunk, n);
                    p->echo_len += n;
                }
                if (n < kept)
                    p->rx_dropped += kept - n;
            }
        }

        uint16_t idx = p->rx_mem->avail.idx;
        p->rx_mem->avail.ring[idx % VPORT_RX_SLOTS] = (uint16_t)id;
        p->rx_mem->avail.idx = (uint16_t)(idx + 1);
        p->rx_last_used++;
        repost = 1;
    }

    if (repost) {
        wmb();
        dma_sync_for_device(&p->rx_mem->avail, sizeof(p->rx_mem->avail));
    }
    spin_unlock_irqrestore(&p->lock, flags);

    if (repost)
        p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_NOTIFY, VPORT_Q_RX);
    vport_console_flush(p);
}

static int vport_irq_handler(int irq, void *priv)
{
    (void)irq;
    vport_inst_t *p = priv;
    if (!p || !p->valid)
        return 0;

    uint32_t status = p->vt.read32(&p->vt, VPORT_MMIO_INTR_STATUS);
    if (!p->vt.legacy && status)
        p->vt.write32(&p->vt, VPORT_MMIO_INTR_ACK, status);

    vport_rx_pump(p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Queue setup                                                         */
/* ------------------------------------------------------------------ */

static int vport_setup_queue(vport_inst_t *p, uint32_t index, uint32_t slots)
{
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_SEL, index);
    if (p->vt.read32(&p->vt, VPORT_MMIO_QUEUE_NUM_MAX) < slots)
        return -ENODEV;
    if (p->vt.read32(&p->vt, VPORT_MMIO_QUEUE_READY) != 0)
        return -EBUSY;
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_NUM, slots);
    return 0;
}

static void vport_program_queue(vport_inst_t *p, uint32_t index,
                                uint64_t desc, uint64_t avail, uint64_t used)
{
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_SEL, index);
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_DESC_LOW, (uint32_t)desc);
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc >> 32));
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_DRIVER_LOW, (uint32_t)avail);
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_DRIVER_HIGH,
                  (uint32_t)(avail >> 32));
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_DEVICE_LOW, (uint32_t)used);
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_DEVICE_HIGH,
                  (uint32_t)(used >> 32));
    mb();
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_READY, 1);
    mb();
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void vport_free_dma(vport_inst_t *p)
{
    if (p->tx_mem) {
        dma_free_coherent_aligned(p->tx_mem, sizeof(*p->tx_mem), p->tx_dma);
        p->tx_mem = NULL;
    }
    if (p->rx_mem) {
        dma_free_coherent_aligned(p->rx_mem, sizeof(*p->rx_mem), p->rx_dma);
        p->rx_mem = NULL;
    }
}

/* Full rollback for every probe failure: the core only clears pointers, so
 * IRQ, DMA and the device state have to be undone here. */
static void vport_fail(vport_inst_t *p, int irq_registered, int irq)
{
    if (irq_registered && irq >= 0)
        free_irq((uint32_t)irq, p);
    p->vt.write32(&p->vt, VPORT_MMIO_STATUS,
                  p->vt.read32(&p->vt, VPORT_MMIO_STATUS) |
                  VPORT_STATUS_FAILED);
    vport_free_dma(p);
    memset(p, 0, sizeof(*p));
}

static int vport_probe(device_t *dev)
{
    if (g_vport.valid)
        return -EBUSY; /* single instance, documented limit */

    virtio_transport_t vt;
    if (pci_virtio_transport_init(dev, VPORT_DEVICE_ID, &vt) < 0)
        return -ENODEV;

    vport_inst_t *p = &g_vport;
    memset(p, 0, sizeof(*p));
    spin_init(&p->lock);
    p->vt = vt;
    p->console_mirror = 1;
    p->irq = vt.irq;

    /* Feature negotiation: VIRTIO_F_VERSION_1 only, so the queues are the
     * modern split rings described at the top of this file. */
    p->vt.write32(&p->vt, VPORT_MMIO_STATUS, 0);
    mb();
    uint32_t status = VPORT_STATUS_ACK | VPORT_STATUS_DRIVER;
    p->vt.write32(&p->vt, VPORT_MMIO_STATUS, status);
    mb();

    p->vt.write32(&p->vt, VPORT_MMIO_DEV_FEATURES_SEL, 1);
    uint32_t features_hi = p->vt.read32(&p->vt, VPORT_MMIO_DEV_FEATURES);
    if (!(features_hi & VPORT_F_VERSION_1)) {
        vport_fail(p, 0, -1);
        return -EOPNOTSUPP;
    }
    p->vt.write32(&p->vt, VPORT_MMIO_DRV_FEATURES_SEL, 1);
    p->vt.write32(&p->vt, VPORT_MMIO_DRV_FEATURES, VPORT_F_VERSION_1);
    p->vt.write32(&p->vt, VPORT_MMIO_DRV_FEATURES_SEL, 0);
    p->vt.write32(&p->vt, VPORT_MMIO_DRV_FEATURES, 0);
    mb();

    status |= VPORT_STATUS_FEATURES_OK;
    p->vt.write32(&p->vt, VPORT_MMIO_STATUS, status);
    mb();
    if (!(p->vt.read32(&p->vt, VPORT_MMIO_STATUS) & VPORT_STATUS_FEATURES_OK)) {
        vport_fail(p, 0, -1);
        return -ENODEV;
    }

    p->rx_mem = dma_alloc_coherent_aligned(dev, sizeof(*p->rx_mem),
                                           PAGE_SIZE, &p->rx_dma);
    if (!p->rx_mem) {
        vport_fail(p, 0, -1);
        return -ENOMEM;
    }
    memset(p->rx_mem, 0, sizeof(*p->rx_mem));
    p->tx_mem = dma_alloc_coherent_aligned(dev, sizeof(*p->tx_mem),
                                           PAGE_SIZE, &p->tx_dma);
    if (!p->tx_mem) {
        vport_fail(p, 0, -1);
        return -ENOMEM;
    }
    memset(p->tx_mem, 0, sizeof(*p->tx_mem));
    dma_sync_for_device(p->rx_mem, sizeof(*p->rx_mem));
    dma_sync_for_device(p->tx_mem, sizeof(*p->tx_mem));

    if (vport_setup_queue(p, VPORT_Q_RX, VPORT_RX_SLOTS) < 0 ||
        vport_setup_queue(p, VPORT_Q_TX, VPORT_TX_SLOTS) < 0) {
        vport_fail(p, 0, -1);
        return -ENODEV;
    }
    vport_program_queue(p, VPORT_Q_RX,
                        p->rx_dma + offsetof(vport_rx_mem_t, desc),
                        p->rx_dma + offsetof(vport_rx_mem_t, avail),
                        p->rx_dma + offsetof(vport_rx_mem_t, used));
    vport_program_queue(p, VPORT_Q_TX,
                        p->tx_dma + offsetof(vport_tx_mem_t, desc),
                        p->tx_dma + offsetof(vport_tx_mem_t, avail),
                        p->tx_dma + offsetof(vport_tx_mem_t, used));

    int irq_registered = 0;
    if (p->vt.irq >= 0) {
        unsigned long irq_flags = p->vt.shared_irq ? IRQF_SHARED : 0;
        if (request_irq((uint32_t)p->vt.irq, vport_irq_handler, irq_flags,
                        p) == 0) {
            p->irq_registered = 1;
            irq_registered = 1;
        } else {
            kinfo("[VPORT] INTx %d unavailable, receive falls back to "
                  "read/poll\n", p->vt.irq);
            p->vt.irq = -1;
        }
    }

    p->valid = 1;
    dev->drv_priv = p;
    /* The core publishes the class device right after this returns 0; asking
     * for the node name here is the only place a driver may choose one. */
    int ret = drv_device_set_devfs_name(dev, "vport0");
    if (ret < 0) {
        p->valid = 0;
        vport_fail(p, irq_registered, p->vt.irq);
        dev->drv_priv = NULL;
        return ret;
    }

    status |= VPORT_STATUS_DRIVER_OK;
    p->vt.write32(&p->vt, VPORT_MMIO_STATUS, status);
    mb();

    vport_rx_post_all(p);
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_NOTIFY, VPORT_Q_RX);

    kinfo("[VPORT] virtio-console port 0 ready (irq=%d, /dev/vport0)\n",
          p->vt.irq);
    return 0;
}

static int vport_remove(device_t *dev)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    if (!p)
        return 0;

    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->valid = 0;
    spin_unlock_irqrestore(&p->lock, flags);

    if (p->irq_registered && p->irq >= 0)
        free_irq((uint32_t)p->irq, p);

    p->vt.write32(&p->vt, VPORT_MMIO_STATUS, 0);
    mb();
    vport_free_dma(p);
    dev->drv_priv = NULL;
    memset(p, 0, sizeof(*p));
    return 0;
}

/* ------------------------------------------------------------------ */
/* CHAR class operations                                               */
/* ------------------------------------------------------------------ */

static int vport_class_read(device_t *dev, void *buf, size_t count)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    if (!p || !p->valid || !buf || !count)
        return -EINVAL;

    /* Drain first: a client that only reads must still see data that arrived
     * before the last interrupt was acknowledged (and this is the whole
     * receive path when INTx registration failed). */
    vport_rx_pump(p);

    uint64_t flags = spin_lock_irqsave(&p->lock);
    size_t avail = (p->head + VPORT_RING_SIZE - p->tail) % VPORT_RING_SIZE;
    size_t n = count < avail ? count : avail;
    for (size_t i = 0; i < n; i++)
        ((char *)buf)[i] = p->ring[(p->tail + i) % VPORT_RING_SIZE];
    p->tail = (uint32_t)((p->tail + n) % VPORT_RING_SIZE);
    spin_unlock_irqrestore(&p->lock, flags);

    return n ? (int)n : -EAGAIN;
}

static int vport_class_write(device_t *dev, const void *buf, size_t count)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    if (!p || !p->valid || !buf || !count)
        return -EINVAL;

    size_t len = count > VPORT_BUF_SIZE ? VPORT_BUF_SIZE : count;
    uint16_t before;

    {
        uint64_t flags = spin_lock_irqsave(&p->lock);
        if (!p->valid) {
            spin_unlock_irqrestore(&p->lock, flags);
            return -ENODEV;
        }

        /* A frame left in flight by a write that timed out is still owned by
         * the device.  Retire it here if it has since completed, so this write
         * does not splice onto it; if it has not, refuse rather than overwrite
         * a buffer the device may still be reading.  The used ring is
         * authoritative either way -- it, and not the timeout, is what actually
         * frees the descriptor. */
        if (p->tx_busy) {
            dma_sync_for_cpu(&p->tx_mem->used, sizeof(p->tx_mem->used));
            if (p->tx_mem->used.idx == p->tx_submitted_used) {
                spin_unlock_irqrestore(&p->lock, flags);
                return -EBUSY;
            }
            p->tx_busy = 0;
        }

        p->tx_busy = 1;
        memcpy(p->tx_mem->buf[0], buf, len);
        dma_sync_for_device(p->tx_mem->buf[0], len);

        p->tx_mem->desc[0].addr = vport_tx_buf_pa(p);
        p->tx_mem->desc[0].len = (uint32_t)len;
        p->tx_mem->desc[0].flags = 0;
        p->tx_mem->desc[0].next = 0;
        dma_sync_for_device(p->tx_mem->desc, sizeof(p->tx_mem->desc));

        uint16_t idx = p->tx_mem->avail.idx;
        p->tx_mem->avail.ring[idx % VPORT_TX_SLOTS] = 0;
        p->tx_mem->avail.idx = (uint16_t)(idx + 1);
        before = p->tx_mem->used.idx;
        p->tx_submitted_used = before;
        wmb();
        dma_sync_for_device(&p->tx_mem->avail, sizeof(p->tx_mem->avail));
        spin_unlock_irqrestore(&p->lock, flags);
    }
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_NOTIFY, VPORT_Q_TX);

    /* Bounded wait for the single in-flight descriptor.  The instance lock is
     * deliberately dropped first: lock-order.md forbids waiting for hardware
     * under a spinlock. */
    uint64_t deadline = clock_get_ticks() + MS_TO_TICKS(VPORT_TX_TIMEOUT_MS);
    for (;;) {
        dma_sync_for_cpu(&p->tx_mem->used, sizeof(p->tx_mem->used));
        uint16_t used_idx = p->tx_mem->used.idx;
        if (used_idx != before) {
            /* The used element retires the descriptor; its length field says
             * nothing about this queue.  do_flush_queued_data() in QEMU's
             * hw/char/virtio-serial-bus.c pushes every completed guest->host
             * buffer with virtqueue_push(vq, elem, 0), so the reported length
             * is always 0 while the bytes really did go to the host (the
             * receive direction is different: write_to_port() pushes the
             * length it copied, which is why vport_rx_pump() can use it).
             * Clamping len to it turned every write() into a zero-length
             * write.  The queue holds descriptor 0 and nothing else, so the
             * bytes delivered are exactly the len published above. */
            uint64_t flags = spin_lock_irqsave(&p->lock);
            p->tx_bytes += (uint32_t)len;
            p->tx_last_used = used_idx;
            /* The device has consumed the frame: the shared descriptor and
             * buffer are free for the next writer. */
            p->tx_busy = 0;
            spin_unlock_irqrestore(&p->lock, flags);
            return (int)len;
        }
        if (clock_get_ticks() >= deadline)
            break;
        proc_yield();
    }

    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->tx_timeouts++;
    /* tx_busy deliberately stays set.  The descriptor was already published in
     * avail, so clearing it here is what let the next write overwrite buf[0]
     * and desc[0] while the device was still reading them -- the frame on the
     * wire came out spliced.  Leaving it set makes this frame in flight until
     * the used ring retires it, which the next write reaps. */
    spin_unlock_irqrestore(&p->lock, flags);
    return -ETIMEDOUT;
}

static int vport_class_poll(device_t *dev, short events)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    (void)events;
    if (!p || !p->valid)
        return 0;
    vport_rx_pump(p);
    uint64_t flags = spin_lock_irqsave(&p->lock);
    int ready = p->head != p->tail;
    spin_unlock_irqrestore(&p->lock, flags);
    return ready;
}

static int vport_class_ioctl(device_t *dev, unsigned long req, void *arg)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    if (!p || !p->valid)
        return -ENODEV;

    /* The generic char adapter hands ioctl() the raw user pointer (see
     * docs/drivers/guide/device-classes.md), so this driver does its own
     * usercopy rather than dereferencing arg. */
    if (req == VPORT_IOCTL_SET_CONSOLE) {
        int32_t attach = 0;
        if (!arg || copy_from_user(&attach, arg, sizeof(attach)) < 0)
            return -EFAULT;
        uint64_t flags = spin_lock_irqsave(&p->lock);
        p->console_mirror = attach ? 1 : 0;
        spin_unlock_irqrestore(&p->lock, flags);
        return 0;
    }

    if (req == VPORT_IOCTL_GET_STATS) {
        struct vport_stats st;
        memset(&st, 0, sizeof(st));
        st.version = VPORT_IOCTL_VERSION;
        uint64_t flags = spin_lock_irqsave(&p->lock);
        st.rx_bytes = p->rx_bytes;
        st.rx_dropped = p->rx_dropped;
        st.tx_bytes = p->tx_bytes;
        st.tx_timeouts = p->tx_timeouts;
        st.console_mirror = (uint32_t)p->console_mirror;
        spin_unlock_irqrestore(&p->lock, flags);
        if (!arg || copy_to_user(arg, &st, sizeof(st)) < 0)
            return -EFAULT;
        return 0;
    }

    return -ENOTTY;
}

static const char_dev_ops_t vport_class_ops = {
    .read = vport_class_read,
    .write = vport_class_write,
    .ioctl = vport_class_ioctl,
    .poll = vport_class_poll,
};

static const device_id_t vport_ids[] = {
    { .vendor = 0x1AF4, .device = VPORT_PCI_ID_MODERN,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = VPORT_PCI_ID_TRANS,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t vport_driver = {
    .name       = "virtio-console",
    .id_table   = vport_ids,
    .bus        = &pci_bus,
    .probe      = vport_probe,
    .remove     = vport_remove,
    .class_ops  = &vport_class_ops,
    .class_type = DEV_CLASS_CHAR,
};

DRIVER_REGISTER(vport_driver);