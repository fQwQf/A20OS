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
 * The raw /dev/vport0 personality keeps the non-MULTIPORT port-0 contract.
 * With a20.console=vport0, MULTIPORT control queues perform DEVICE_READY /
 * PORT_READY / PORT_OPEN for physical port 1 (data queues 4/5), still exposed
 * as logical vport0. QEMU's flow-controlled virtserialport reserves port zero
 * for the legacy console, whose backend may discard bytes on EAGAIN.
 *
 * Receive is interrupt driven and feeds the existing console input discipline.
 * Transmit has configurable, separately owned DMA slots (default sixteen).
 * The selected bulk TTY accepts copied frames, waits for space without a
 * spinlock, and exposes an
 * explicit drain. Raw port clients retain synchronous write completion.
 */
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/char/uart.h"
#include "drivers/char/virtio_console.h"
#include "drivers/char/virtio_console_config.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drvmod/drvmod.h"
#include "core/errno.h"
#include "core/bootargs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "proc/signal.h"
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
#define VPORT_Q_CTRL_RX          2U
#define VPORT_Q_CTRL_TX          3U
#define VPORT_RX_SLOTS           CONFIG_VIRTIO_CONSOLE_RX_SLOTS
#define VPORT_TX_SLOTS           CONFIG_VIRTIO_CONSOLE_TX_SLOTS
#define VPORT_RING_SLOTS         ((VPORT_RX_SLOTS > VPORT_TX_SLOTS) ? VPORT_RX_SLOTS : VPORT_TX_SLOTS)
#define VPORT_BUF_SIZE           CONFIG_VIRTIO_CONSOLE_BUFFER_SIZE
#define VPORT_RING_SIZE          CONFIG_VIRTIO_CONSOLE_RING_SIZE
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
    uint16_t ring[VPORT_RING_SLOTS];
    uint16_t used_event;
} vport_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} vport_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vport_used_elem_t ring[VPORT_RING_SLOTS];
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
    vport_desc_t desc[VPORT_RX_SLOTS] ALIGNED(64);
    vport_avail_t avail ALIGNED(64);
    vport_used_t used ALIGNED(64);
    uint8_t buf[VPORT_RX_SLOTS][64] ALIGNED(64);
} vport_ctrl_mem_t;

typedef struct {
    virtio_transport_t vt;
    vport_rx_mem_t    *rx_mem;
    uint64_t           rx_dma;
    vport_tx_mem_t    *tx_mem;
    uint64_t           tx_dma;
    vport_ctrl_mem_t  *ctrl_rx, *ctrl_tx;
    uint64_t           ctrl_rx_dma, ctrl_tx_dma;
    uint16_t           ctrl_last_used;
    /* Guards the byte ring, the console-mirror flag and state, both
     * last_used cursors and the counters.  Innermost lock: uart_receive_char()
     * takes the UART rx lock and flushes wake queues, so it is never called
     * with this held. */
    spinlock_t lock ALIGNED(64);
    /* Serialize submit -> completion, including descriptor retirement. The
     * instance spinlock must never cover a hardware wait. */
    mutex_t tx_lock;
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
    /* tx_lock owns submission/retirement. Bits remain set after timeout or
     * interruption until the device returns the corresponding descriptor. */
    uint32_t   tx_busy;
    uint8_t    console_output;
    uint16_t   rx_queue, tx_queue;
    uint8_t    port_id;
    uint16_t   rx_last_used;
    uint16_t   tx_last_used;
    int        console_mirror;
    int        irq;
    int        irq_registered;
    int        valid;
    int        closing;
    int        reset_required;
    int        hardware_stopped;
    unsigned   active_calls; /* includes lock-free RX notify/console delivery */
    device_t  *owner;
} vport_inst_t;

/* One port, one instance: a second virtio-serial function stays unbound
 * rather than getting a second port nothing multiplexes. */
static vport_inst_t g_vport;
static int vport_stop(void *context);

static int vport_call_begin(vport_inst_t *p)
{
    if (!p) return -ENODEV;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    int ret = -ENODEV;
    if (__atomic_load_n(&p->valid, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&p->closing, __ATOMIC_ACQUIRE)) {
        p->active_calls++;
        ret = 0;
    }
    spin_unlock_irqrestore(&p->lock, flags);
    return ret;
}

static void vport_call_end(vport_inst_t *p)
{
    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->active_calls--;
    spin_unlock_irqrestore(&p->lock, flags);
}


/* ------------------------------------------------------------------ */
/* Ring helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint64_t vport_rx_buf_pa(const vport_inst_t *p, unsigned slot)
{
    return p->rx_dma + offsetof(vport_rx_mem_t, buf) +
           (uint64_t)slot * VPORT_BUF_SIZE;
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
        if (__atomic_load_n(&p->closing, __ATOMIC_ACQUIRE) ||
            p->echo_pos >= p->echo_len) {
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
    if (__atomic_load_n(&p->closing, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&p->valid, __ATOMIC_ACQUIRE) || !p->rx_mem) {
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
        p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_NOTIFY, p->rx_queue);
    vport_console_flush(p);
}

static int vport_irq_handler(int irq, void *priv)
{
    (void)irq;
    vport_inst_t *p = priv;
    if (!p)
        return 0;

    uint32_t status = p->vt.read32(&p->vt, VPORT_MMIO_INTR_STATUS);
    if (status && status != 0xffffffffU)
        p->vt.write32(&p->vt, VPORT_MMIO_INTR_ACK, status);

    if (!vport_call_begin(p)) {
        vport_rx_pump(p);
        vport_call_end(p);
    }
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
    if (p->ctrl_rx) {
        dma_free_coherent_aligned(p->ctrl_rx, sizeof(*p->ctrl_rx), p->ctrl_rx_dma);
        p->ctrl_rx = NULL;
    }
    if (p->ctrl_tx) {
        dma_free_coherent_aligned(p->ctrl_tx, sizeof(*p->ctrl_tx), p->ctrl_tx_dma);
        p->ctrl_tx = NULL;
    }
    if (p->tx_mem) {
        dma_free_coherent_aligned(p->tx_mem, sizeof(*p->tx_mem), p->tx_dma);
        p->tx_mem = NULL;
    }
    if (p->rx_mem) {
        dma_free_coherent_aligned(p->rx_mem, sizeof(*p->rx_mem), p->rx_dma);
        p->rx_mem = NULL;
    }
}

/* Managed stop owns fallible reset and release, including partial probe. */
static int vport_fail(vport_inst_t *p, int error)
{
    uint32_t status = p->vt.read32(&p->vt, VPORT_MMIO_STATUS);
    if (status != 0xffffffffU)
        p->vt.write32(&p->vt, VPORT_MMIO_STATUS, status | VPORT_STATUS_FAILED);
    return error;
}

static int vport_boot_option(const char *option)
{
    const char *p = bootargs_get();
    size_t length = strlen(option);
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if ((size_t)(p - start) == length && !memcmp(start, option, length)) return 1;
    }
    return 0;
}
static int vport_is_management(void)
{
    return vport_boot_option("a20.vmd=1");
}

/* A flow-controlled virtserialport needs the MULTIPORT control handshake
 * even when only one port is exposed. QEMU's virtconsole intentionally drops
 * bytes on a short backend write, so it cannot be the reliable bulk terminal. */
static int vport_control_setup(vport_inst_t *p, device_t *dev)
{
    p->ctrl_rx = dma_alloc_coherent_aligned(dev, sizeof(*p->ctrl_rx), PAGE_SIZE, &p->ctrl_rx_dma);
    p->ctrl_tx = dma_alloc_coherent_aligned(dev, sizeof(*p->ctrl_tx), PAGE_SIZE, &p->ctrl_tx_dma);
    if (!p->ctrl_rx || !p->ctrl_tx) return -ENOMEM;
    memset(p->ctrl_rx, 0, sizeof(*p->ctrl_rx));
    memset(p->ctrl_tx, 0, sizeof(*p->ctrl_tx));
    p->ctrl_rx->avail.flags = p->ctrl_tx->avail.flags = 1;
    if (vport_setup_queue(p, VPORT_Q_CTRL_RX, VPORT_RX_SLOTS) ||
        vport_setup_queue(p, VPORT_Q_CTRL_TX, 1)) return -ENODEV;
    vport_program_queue(p, VPORT_Q_CTRL_RX,
        p->ctrl_rx_dma + offsetof(vport_ctrl_mem_t, desc),
        p->ctrl_rx_dma + offsetof(vport_ctrl_mem_t, avail),
        p->ctrl_rx_dma + offsetof(vport_ctrl_mem_t, used));
    vport_program_queue(p, VPORT_Q_CTRL_TX,
        p->ctrl_tx_dma + offsetof(vport_ctrl_mem_t, desc),
        p->ctrl_tx_dma + offsetof(vport_ctrl_mem_t, avail),
        p->ctrl_tx_dma + offsetof(vport_ctrl_mem_t, used));
    for (unsigned i = 0; i < VPORT_RX_SLOTS; i++) {
        p->ctrl_rx->desc[i] = (vport_desc_t){
            .addr=p->ctrl_rx_dma + offsetof(vport_ctrl_mem_t, buf) + i * 64,
            .len=64, .flags=VPORT_QD_F_WRITE};
        p->ctrl_rx->avail.ring[i] = (uint16_t)i;
    }
    wmb();
    p->ctrl_rx->avail.idx = VPORT_RX_SLOTS;
    dma_sync_for_device(p->ctrl_rx, sizeof(*p->ctrl_rx));
    dma_sync_for_device(p->ctrl_tx, sizeof(*p->ctrl_tx));
    return 0;
}

static int vport_control_send(vport_inst_t *p, uint16_t event)
{
    /* Fixed physical port id, value=1; encode the wire packet bytewise. */
    uint8_t packet[8] = {p->port_id,0,0,0,(uint8_t)event,(uint8_t)(event >> 8),1,0};
    memcpy(p->ctrl_tx->buf[0], packet, sizeof(packet));
    p->ctrl_tx->desc[0] = (vport_desc_t){
        .addr=p->ctrl_tx_dma + offsetof(vport_ctrl_mem_t, buf), .len=8};
    uint16_t before = p->ctrl_tx->used.idx;
    p->ctrl_tx->avail.ring[0] = 0;
    wmb();
    p->ctrl_tx->avail.idx++;
    dma_sync_for_device(p->ctrl_tx, sizeof(*p->ctrl_tx));
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_NOTIFY, VPORT_Q_CTRL_TX);
    uint64_t deadline = clock_get_ticks() + MS_TO_TICKS(VPORT_TX_TIMEOUT_MS);
    do {
        if (__atomic_load_n(&p->closing, __ATOMIC_ACQUIRE)) return -ENODEV;
        dma_sync_for_cpu(&p->ctrl_tx->used, sizeof(p->ctrl_tx->used));
        if (p->ctrl_tx->used.idx != before) return 0;
        proc_yield();
    } while (clock_get_ticks() < deadline);
    return -ETIMEDOUT;
}

static int vport_control_wait(vport_inst_t *p, uint16_t event)
{
    uint64_t deadline = clock_get_ticks() + MS_TO_TICKS(VPORT_TX_TIMEOUT_MS);
    do {
        if (__atomic_load_n(&p->closing, __ATOMIC_ACQUIRE)) return -ENODEV;
        dma_sync_for_cpu(p->ctrl_rx, sizeof(*p->ctrl_rx));
        uint16_t used = p->ctrl_rx->used.idx;
        if ((uint16_t)(used - p->ctrl_last_used) > VPORT_RX_SLOTS) return -EIO;
        rmb();
        int found = 0;
        while (p->ctrl_last_used != used) {
            vport_used_elem_t elem = p->ctrl_rx->used.ring[p->ctrl_last_used % VPORT_RX_SLOTS];
            if (elem.id >= VPORT_RX_SLOTS || elem.len < 8 || elem.len > 64) return -EIO;
            const uint8_t *packet = p->ctrl_rx->buf[elem.id];
            unsigned type = packet[4] | (unsigned)packet[5] << 8;
            if (packet[0] == p->port_id && !packet[1] && !packet[2] && !packet[3] &&
                type == event && packet[6] == 1 && packet[7] == 0) found = 1;
            p->ctrl_rx->avail.ring[p->ctrl_rx->avail.idx % VPORT_RX_SLOTS] = (uint16_t)elem.id;
            wmb();
            p->ctrl_rx->avail.idx++;
            p->ctrl_last_used++;
        }
        dma_sync_for_device(&p->ctrl_rx->avail, sizeof(p->ctrl_rx->avail));
        p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_NOTIFY, VPORT_Q_CTRL_RX);
        if (found) return 0;
        proc_yield();
    } while (clock_get_ticks() < deadline);
    return -ETIMEDOUT;
}

static int vport_probe(device_t *dev)
{
    if (g_vport.owner)
        return -EBUSY; /* single instance, including failed-stop bindings */
    int managed = device_probe_use_remove(dev);
    if (managed) return managed;
    vport_inst_t *p = &g_vport;
    memset(p, 0, sizeof(*p));
    spin_init(&p->lock);
    mutex_init(&p->tx_lock);
    p->owner = dev;
    p->irq = -1;
    dev->drv_priv = p;
    int execution = device_add_execution_action(dev, vport_stop, NULL, p);
    if (execution) return execution;
    virtio_transport_t vt = {0};
    if (pci_virtio_transport_init(dev, VPORT_DEVICE_ID, &vt) < 0 || vt.legacy)
        return -ENODEV;

    p->vt = vt;
    p->console_mirror = !vport_is_management();
    p->console_output = p->console_mirror && vport_boot_option("a20.console=vport0");
    p->port_id = p->console_output ? 1 : 0;
    p->rx_queue = p->console_output ? 4 : VPORT_Q_RX;
    p->tx_queue = p->console_output ? 5 : VPORT_Q_TX;
    p->irq = vt.irq;

    /* Negotiate modern split rings and, for the bulk terminal only, the
     * control protocol needed by a flow-controlled host serial port. */
    p->reset_required = 1;
    if (p->vt.read32(&p->vt, VPORT_MMIO_STATUS) == 0xffffffffU) return -ENODEV;
    int reset = virtio_transport_reset(&p->vt, 100);
    if (reset) return reset;
    uint32_t status = VPORT_STATUS_ACK | VPORT_STATUS_DRIVER;
    p->vt.write32(&p->vt, VPORT_MMIO_STATUS, status);
    mb();

    p->vt.write32(&p->vt, VPORT_MMIO_DEV_FEATURES_SEL, 1);
    uint32_t features_hi = p->vt.read32(&p->vt, VPORT_MMIO_DEV_FEATURES);
    if (!(features_hi & VPORT_F_VERSION_1)) {
        return vport_fail(p, -EOPNOTSUPP);
    }
    p->vt.write32(&p->vt, VPORT_MMIO_DRV_FEATURES_SEL, 1);
    p->vt.write32(&p->vt, VPORT_MMIO_DRV_FEATURES, VPORT_F_VERSION_1);
    p->vt.write32(&p->vt, VPORT_MMIO_DRV_FEATURES_SEL, 0);
    p->vt.write32(&p->vt, VPORT_MMIO_DEV_FEATURES_SEL, 0);
    uint32_t features_lo = p->vt.read32(&p->vt, VPORT_MMIO_DEV_FEATURES);
    if (p->console_output && !(features_lo & 2U)) {
        return vport_fail(p, -EOPNOTSUPP);
    }
    p->vt.write32(&p->vt, VPORT_MMIO_DRV_FEATURES, p->console_output ? 2U : 0U);
    mb();

    status |= VPORT_STATUS_FEATURES_OK;
    p->vt.write32(&p->vt, VPORT_MMIO_STATUS, status);
    mb();
    if (!(p->vt.read32(&p->vt, VPORT_MMIO_STATUS) & VPORT_STATUS_FEATURES_OK)) {
        return vport_fail(p, -ENODEV);
    }

    p->rx_mem = dma_alloc_coherent_aligned(dev, sizeof(*p->rx_mem),
                                           PAGE_SIZE, &p->rx_dma);
    if (!p->rx_mem) {
        return vport_fail(p, -ENOMEM);
    }
    memset(p->rx_mem, 0, sizeof(*p->rx_mem));
    p->tx_mem = dma_alloc_coherent_aligned(dev, sizeof(*p->tx_mem),
                                           PAGE_SIZE, &p->tx_dma);
    if (!p->tx_mem) {
        return vport_fail(p, -ENOMEM);
    }
    memset(p->tx_mem, 0, sizeof(*p->tx_mem));
    /* TX is retired by writers/drain. Completion IRQs for every short TTY
     * write add no information, while RX still needs prompt IRQ delivery. */
    p->tx_mem->avail.flags = 1; /* VIRTQ_AVAIL_F_NO_INTERRUPT */
    dma_sync_for_device(p->rx_mem, sizeof(*p->rx_mem));
    dma_sync_for_device(p->tx_mem, sizeof(*p->tx_mem));

    if (vport_setup_queue(p, p->rx_queue, VPORT_RX_SLOTS) < 0 ||
        vport_setup_queue(p, p->tx_queue, VPORT_TX_SLOTS) < 0) {
        return vport_fail(p, -ENODEV);
    }
    vport_program_queue(p, p->rx_queue,
                        p->rx_dma + offsetof(vport_rx_mem_t, desc),
                        p->rx_dma + offsetof(vport_rx_mem_t, avail),
                        p->rx_dma + offsetof(vport_rx_mem_t, used));
    vport_program_queue(p, p->tx_queue,
                        p->tx_dma + offsetof(vport_tx_mem_t, desc),
                        p->tx_dma + offsetof(vport_tx_mem_t, avail),
                        p->tx_dma + offsetof(vport_tx_mem_t, used));

    if (p->console_output) {
        int error = vport_control_setup(p, dev);
        if (error) return vport_fail(p, error);
    }

    if (p->vt.irq >= 0) {
        unsigned long irq_flags = p->vt.shared_irq ? IRQF_SHARED : 0;
        if (request_irq((uint32_t)p->vt.irq, vport_irq_handler, irq_flags,
                        p) == 0) {
            p->irq_registered = 1;
        } else {
            kinfo("[VPORT] INTx %d unavailable, receive falls back to "
                  "read/poll\n", p->vt.irq);
            p->vt.irq = -1;
        }
    }

    /* The core publishes the class device right after this returns 0; asking
     * for the node name here is the only place a driver may choose one. */
    int ret = drv_device_set_devfs_name(dev, "vport0");
    if (ret < 0) return vport_fail(p, ret);

    status |= VPORT_STATUS_DRIVER_OK;
    p->vt.write32(&p->vt, VPORT_MMIO_STATUS, status);
    mb();

    __atomic_store_n(&p->valid, 1, __ATOMIC_RELEASE);
    vport_rx_post_all(p);
    p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_NOTIFY, p->rx_queue);

    if (p->console_output) {
        p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_NOTIFY, VPORT_Q_CTRL_RX);
        int error = vport_control_send(p, 0); /* DEVICE_READY */
        if (!error) error = vport_control_wait(p, 1); /* PORT_ADD */
        if (!error) error = vport_control_send(p, 3); /* PORT_READY */
        if (!error) error = vport_control_wait(p, 6); /* host PORT_OPEN */
        if (!error) error = vport_control_send(p, 6); /* guest PORT_OPEN */
        if (error) {
            __atomic_store_n(&p->valid, 0, __ATOMIC_RELEASE);
            return vport_fail(p, error);
        }
        kinfo("[VPORT] bulk TTY ready (flow-controlled port 1, %u TX slots)\n",
              VPORT_TX_SLOTS);
    }

    kinfo("[VPORT] virtio-console port 0 ready (irq=%d, /dev/vport0)\n",
          p->vt.irq);
    return 0;
}

static int vport_stop(void *context)
{
    vport_inst_t *p = context;
    __atomic_store_n(&p->closing, 1, __ATOMIC_RELEASE);
    uint64_t flags;
    if (!spin_trylock_irqsave(&p->lock, &flags)) return -EBUSY;
    __atomic_store_n(&p->valid, 0, __ATOMIC_RELEASE);
    if (p->active_calls) {
        spin_unlock_irqrestore(&p->lock, flags);
        return -EBUSY;
    }
    spin_unlock_irqrestore(&p->lock, flags);
    if (p->reset_required && !p->hardware_stopped) {
        if (p->vt.read32(&p->vt, VPORT_MMIO_STATUS) == 0xffffffffU) return -ENODEV;
        int ret = virtio_transport_reset(&p->vt, 100);
        if (ret) return ret;
    }
    p->hardware_stopped = 1;
    if (p->irq_registered && p->irq >= 0) free_irq((uint32_t)p->irq, p);
    __atomic_store_n(&p->irq_registered, 0, __ATOMIC_RELEASE);
    return 0;
}

static int vport_remove(device_t *dev)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    if (!p) return 0;
    int ret = vport_stop(p);
    if (ret) return ret;
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
    if (!buf || !count) return -EINVAL;
    int result = vport_call_begin(p);
    if (result) return result;

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

    vport_call_end(p);
    return n ? (int)n : -EAGAIN;
}

/* Called with the instance lock and tx_lock held. Device-returned ids are
 * validated before indexing the DMA allocation. Completion, rather than a
 * timeout or syscall return, is the only transfer of buffer ownership. */
static int vport_tx_reap_locked(vport_inst_t *p)
{
    dma_sync_for_cpu(&p->tx_mem->used, sizeof(p->tx_mem->used));
    uint16_t used = p->tx_mem->used.idx;
    if ((uint16_t)(used - p->tx_last_used) > VPORT_TX_SLOTS) return -EIO;
    rmb();
    while (p->tx_last_used != used) {
        uint32_t id = p->tx_mem->used.ring[p->tx_last_used % VPORT_TX_SLOTS].id;
        if (id >= VPORT_TX_SLOTS || !(p->tx_busy & (1U << id))) return -EIO;
        p->tx_bytes += p->tx_mem->desc[id].len;
        p->tx_busy &= ~(1U << id);
        p->tx_last_used++;
    }
    return 0;
}

static int vport_tx_wait(vport_inst_t *p, int all, int interruptible)
{
    uint64_t deadline = clock_get_ticks() + MS_TO_TICKS(VPORT_TX_TIMEOUT_MS);
    unsigned spins = 0;
    for (;;) {
        if (__atomic_load_n(&p->closing, __ATOMIC_ACQUIRE)) return -ENODEV;
        uint64_t flags = spin_lock_irqsave(&p->lock);
        int error = vport_tx_reap_locked(p);
        int ready = all ? !p->tx_busy : p->tx_busy != (1U << VPORT_TX_SLOTS) - 1;
        spin_unlock_irqrestore(&p->lock, flags);
        if (error || ready) return error;
        if (++spins < 256) {
            __asm__ volatile("" ::: "memory");
            continue;
        }
        spins = 0;
        if (interruptible && signal_task_has_unblocked(proc_current()))
            return -ERESTARTSYS;
        if (clock_get_ticks() >= deadline) break;
        proc_yield();
    }
    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->tx_timeouts++;
    spin_unlock_irqrestore(&p->lock, flags);
    return -ETIMEDOUT;
}

static int vport_write_locked(device_t *dev, const void *buf, size_t count)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    if (!buf || !count) return -EINVAL;
    if (__atomic_load_n(&p->closing, __ATOMIC_ACQUIRE)) return -ENODEV;
    int error = vport_tx_wait(p, 0, 1);
    if (error) return error;
    size_t len = count > VPORT_BUF_SIZE ? VPORT_BUF_SIZE : count;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    unsigned slot = (unsigned)__builtin_ctz(~p->tx_busy & ((1U << VPORT_TX_SLOTS) - 1));
    p->tx_busy |= 1U << slot;
    memcpy(p->tx_mem->buf[slot], buf, len);
    dma_sync_for_device(p->tx_mem->buf[slot], len);
    p->tx_mem->desc[slot].addr = p->tx_dma + offsetof(vport_tx_mem_t, buf) +
                               (uint64_t)slot * VPORT_BUF_SIZE;
    p->tx_mem->desc[slot].len = (uint32_t)len;
    p->tx_mem->desc[slot].flags = 0;
    p->tx_mem->desc[slot].next = 0;
    dma_sync_for_device(&p->tx_mem->desc[slot], sizeof(p->tx_mem->desc[slot]));
    uint16_t idx = p->tx_mem->avail.idx;
    p->tx_mem->avail.ring[idx % VPORT_TX_SLOTS] = (uint16_t)slot;
    wmb();
    p->tx_mem->avail.idx = (uint16_t)(idx + 1);
    dma_sync_for_device(&p->tx_mem->avail, sizeof(p->tx_mem->avail));
    spin_unlock_irqrestore(&p->lock, flags);
    /* The device may already be draining this queue. Respect its split-ring
     * notification suppression after publishing the new index. The full
     * barrier pairs with the device's enable-and-recheck protocol. */
    mb();
    dma_sync_for_cpu(&p->tx_mem->used, sizeof(p->tx_mem->used));
    if (!(p->tx_mem->used.flags & 1U)) /* VIRTQ_USED_F_NO_NOTIFY */
        p->vt.write32(&p->vt, VPORT_MMIO_QUEUE_NOTIFY, p->tx_queue);
    /* Console writes accept copied frames and apply backpressure only when
     * all slots are device-owned. Raw vport clients remain synchronous. */
    if (p->console_output) return (int)len;
    error = vport_tx_wait(p, 1, 0);
    return error ? error : (int)len;
}

static int vport_class_write(device_t *dev, const void *buf, size_t count)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    int n = vport_call_begin(p);
    if (n) return n;
    mutex_lock(&p->tx_lock);
    n = vport_write_locked(dev, buf, count);
    mutex_unlock(&p->tx_lock);
    vport_call_end(p);
    return n;
}

static int vport_class_poll(device_t *dev, short events)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    (void)events;
    if (vport_call_begin(p)) return 0;
    vport_rx_pump(p);
    uint64_t flags = spin_lock_irqsave(&p->lock);
    int ready = p->head != p->tail;
    spin_unlock_irqrestore(&p->lock, flags);
    vport_call_end(p);
    return ready;
}

static int vport_ioctl_impl(device_t *dev, unsigned long req, void *arg)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    if (__atomic_load_n(&p->closing, __ATOMIC_ACQUIRE)) return -ENODEV;

    if (req == VPORT_IOCTL_CONSOLE_READY)
        return p->console_output && p->port_id == 1 ? 0 : -ENODEV;

    if (req == VPORT_IOCTL_DRAIN) {
        mutex_lock(&p->tx_lock);
        int result = vport_tx_wait(p, 1, 1);
        mutex_unlock(&p->tx_lock);
        return result;
    }

    /* The generic char adapter hands ioctl() the raw user pointer (see
     * docs/drivers/guide/device-classes.md), so this driver does its own
     * usercopy rather than dereferencing arg. */
    if (req == VPORT_IOCTL_SET_CONSOLE) {
        int32_t attach = 0;
        if (!arg || copy_from_user(&attach, arg, sizeof(attach)) < 0)
            return -EFAULT;
        if (attach && vport_is_management())
            return -EPERM;
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

static int vport_class_ioctl(device_t *dev, unsigned long req, void *arg)
{
    vport_inst_t *p = dev ? dev->drv_priv : NULL;
    int ret = vport_call_begin(p);
    if (ret) return ret;
    ret = vport_ioctl_impl(dev, req, arg);
    vport_call_end(p);
    return ret;
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
