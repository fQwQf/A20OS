#include "drivers/net/virtio_net.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "net/lwip_stack.h"
#include "mm/mm.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/defs.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/timer.h"
#include "proc/proc.h"

#define VIRTIO_NET_QUEUE_RX        0
#define VIRTIO_NET_QUEUE_TX        1
/* One message-signalled vector per direction: a burst on one no longer delays
 * the other's completion behind a shared line. */
#define VIRTIO_NET_QUEUES          2
#define VIRTIO_NET_HDR_SIZE        12
#define VIRTIO_NET_MTU             1500
#define VIRTIO_NET_FRAME_MAX       1536
#define VIRTIO_NET_BUF_SIZE        (VIRTIO_NET_HDR_SIZE + VIRTIO_NET_FRAME_MAX)
#define VIRTIO_NET_F_MAC           5
#define VIRTIO_NET_F_STATUS        16
#define VIRTIO_NET_TX_TIMEOUT_TICKS (clock_ticks_per_sec() * 2)

typedef struct {
    virtq_desc_t  desc[VIRTIO_QUEUE_SIZE] ALIGNED(16);
    virtq_avail_t avail                   ALIGNED(2);
    virtq_used_t  used                    ALIGNED(4);
    ALIGNED(4096) uint8_t legacy_vq[4096 * 3];
    uint16_t last_used;
} virtio_net_queue_t;

typedef struct {
    virtio_transport_t vt;
    virtio_net_queue_t rxq;
    virtio_net_queue_t txq;
    uint8_t rx_buf[VIRTIO_QUEUE_SIZE][VIRTIO_NET_BUF_SIZE] ALIGNED(64);
    uint8_t tx_buf[VIRTIO_QUEUE_SIZE][VIRTIO_NET_BUF_SIZE] ALIGNED(64);
    uint8_t tx_busy[VIRTIO_QUEUE_SIZE];
    uint8_t mac[6];
    /* LOCK_ORDER: net->lock is innermost under g_lwip_lock.
     * Local order: g_lwip_lock -> net->lock.
     * Protects TX/RX descriptor rings, tx_busy[], rx_buf[], tx_buf[],
     * last_used, avail->idx, rx_packets, tx_packets, rx_drops, tx_drops. */
    spinlock_t lock;
    int valid;
    int legacy;
    int slot;
    int irq;
    int irq_registered;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_drops;
    uint32_t tx_drops;
} virtio_net_inst_t;

static virtio_net_inst_t g_net[VIRTIO_NET_MAX_DEVS];
static int g_nnet;

static void virtio_net_select_queue(virtio_net_inst_t *net, int qidx) {
    net->vt.write32(&net->vt, VIRTIO_MMIO_QUEUE_SEL, (uint32_t)qidx);
}

static int virtio_net_setup_queue(virtio_net_inst_t *net, virtio_net_queue_t *q, int qidx) {
    virtio_transport_t *vt = &net->vt;

    virtio_net_select_queue(net, qidx);
    uint32_t qmax = vt->read32(vt, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < VIRTIO_QUEUE_SIZE) {
        printf("[VIRTIO-NET%d] queue %d max too small: %u\n", net->slot, qidx, qmax);
        return -1;
    }
    vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_QUEUE_SIZE);

    memset(q, 0, sizeof(*q));
    if (net->legacy) {
        virtq_desc_t *l_desc = (virtq_desc_t *)(uintptr_t)q->legacy_vq;
        virtq_avail_t *l_avail =
            (virtq_avail_t *)(uintptr_t)(q->legacy_vq + VIRTIO_QUEUE_SIZE * sizeof(virtq_desc_t));
        virtq_used_t *l_used = (virtq_used_t *)(uintptr_t)(q->legacy_vq + 4096);

        q->last_used = 0;
        memcpy(l_desc, q->desc, sizeof(q->desc));
        memcpy(l_avail, &q->avail, sizeof(q->avail));
        memcpy(l_used, &q->used, sizeof(q->used));

        uint64_t vq_pa = va_to_pa(q->legacy_vq);
        vt->write32(vt, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
        mb();
        vt->write32(vt, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(vq_pa / 4096));
        mb();
    } else {
        uint64_t desc_pa = va_to_pa(q->desc);
        uint64_t avail_pa = va_to_pa(&q->avail);
        uint64_t used_pa = va_to_pa(&q->used);

        vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc_pa);
        vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_pa >> 32));
        vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)avail_pa);
        vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint32_t)(avail_pa >> 32));
        vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)used_pa);
        vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uint32_t)(used_pa >> 32));
        mb();
        vt->write32(vt, VIRTIO_MMIO_QUEUE_READY, 1);
        mb();
    }
    return 0;
}

static virtq_desc_t *queue_desc(virtio_net_inst_t *net, virtio_net_queue_t *q) {
    if (net->legacy)
        return (virtq_desc_t *)(uintptr_t)q->legacy_vq;
    return q->desc;
}

static virtq_avail_t *queue_avail(virtio_net_inst_t *net, virtio_net_queue_t *q) {
    if (net->legacy)
        return (virtq_avail_t *)(uintptr_t)(q->legacy_vq + VIRTIO_QUEUE_SIZE * sizeof(virtq_desc_t));
    return &q->avail;
}

static virtq_used_t *queue_used(virtio_net_inst_t *net, virtio_net_queue_t *q) {
    if (net->legacy)
        return (virtq_used_t *)(uintptr_t)(q->legacy_vq + 4096);
    return &q->used;
}

static void virtio_net_kick(virtio_net_inst_t *net, int qidx) {
    net->vt.write32(&net->vt, VIRTIO_MMIO_QUEUE_NOTIFY, (uint32_t)qidx);
    mb();
}

static void virtio_net_wait_for_tx_progress(void)
{
    task_t *cur = proc_current();
    if (cur)
        proc_yield();
    else
        cpu_relax();
}

static void virtio_net_submit_rx_locked(virtio_net_inst_t *net, uint16_t slot) {
    virtio_net_queue_t *q = &net->rxq;
    virtq_desc_t *desc = queue_desc(net, q);
    virtq_avail_t *avail = queue_avail(net, q);

    memset(net->rx_buf[slot], 0, VIRTIO_NET_HDR_SIZE);
    desc[slot].addr = va_to_pa(net->rx_buf[slot]);
    desc[slot].len = VIRTIO_NET_BUF_SIZE;
    desc[slot].flags = VIRTQ_DESC_F_WRITE;
    desc[slot].next = 0;

    /* Publish first, then clean.  See the DMA publication contract in
     * kernel/include/drivers/dual/virtq.h: arch_dma_sync_for_device() cleans a
     * cache line rather than ordering the store stream, so cleaning after the
     * store gives the device the same view as virtio_blk's clean-before.
     * virtio_net.c and virtio_blk.c differ in placement on purpose. */
    uint16_t avail_slot = avail->idx % VIRTIO_QUEUE_SIZE;
    avail->ring[avail_slot] = slot;
    wmb();
    avail->idx++;

    arch_dma_sync_for_device(net->rx_buf[slot], VIRTIO_NET_BUF_SIZE);
    arch_dma_sync_for_device(&desc[slot], sizeof(desc[slot]));
    arch_dma_sync_for_device(&avail->ring[avail_slot], sizeof(uint16_t));
    arch_dma_sync_for_device(&avail->idx, sizeof(uint16_t));
}

static void virtio_net_seed_rx_locked(virtio_net_inst_t *net) {
    for (uint16_t i = 0; i < VIRTIO_QUEUE_SIZE; i++)
        virtio_net_submit_rx_locked(net, i);
}

static void virtio_net_complete_tx_locked(virtio_net_inst_t *net) {
    virtio_net_queue_t *q = &net->txq;
    virtq_used_t *used = queue_used(net, q);

    arch_dma_sync_for_cpu(&used->idx, sizeof(uint16_t));
    uint16_t used_idx = ((volatile virtq_used_t *)used)->idx;
    while (q->last_used != used_idx) {
        uint16_t ring_idx = q->last_used % VIRTIO_QUEUE_SIZE;
        arch_dma_sync_for_cpu(&used->ring[ring_idx], sizeof(virtq_used_elem_t));
        uint16_t slot = (uint16_t)used->ring[ring_idx].id;
        if (slot < VIRTIO_QUEUE_SIZE)
            net->tx_busy[slot] = 0;
        q->last_used++;
    }
}

static int virtio_net_tx_free_locked(virtio_net_inst_t *net) {
    virtio_net_complete_tx_locked(net);
    for (int i = 0; i < VIRTIO_QUEUE_SIZE; i++) {
        if (!net->tx_busy[i])
            return i;
    }
    return -1;
}

static int virtio_net_init_instance(virtio_net_inst_t *net) {
    int idx = net->slot;
    virtio_transport_t *vt = &net->vt;

    vt->write32(vt, VIRTIO_MMIO_STATUS, 0);
    mb();

    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();

    vt->write32(vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint32_t features_lo = vt->read32(vt, VIRTIO_MMIO_DEVICE_FEATURES);
    uint32_t driver_lo = 0;
    if (features_lo & (1U << VIRTIO_NET_F_MAC))
        driver_lo |= (1U << VIRTIO_NET_F_MAC);
    if (features_lo & (1U << VIRTIO_NET_F_STATUS))
        driver_lo |= (1U << VIRTIO_NET_F_STATUS);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES, driver_lo);

    vt->write32(vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = vt->read32(vt, VIRTIO_MMIO_DEVICE_FEATURES);
    uint32_t driver_hi = 0;
    if (!net->legacy)
        driver_hi = features_hi & VIRTIO_F_VERSION_1_BIT;
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES, driver_hi);
    mb();

    if (!net->legacy) {
        status |= VIRTIO_STATUS_FEATURES_OK;
        vt->write32(vt, VIRTIO_MMIO_STATUS, status);
        mb();
        if (!(vt->read32(vt, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
            printf("[VIRTIO-NET%d] device rejected features\n", idx);
            return -1;
        }
    }

    if (driver_lo & (1U << VIRTIO_NET_F_MAC)) {
        uint32_t mac0 = vt->read32(vt, VIRTIO_MMIO_CONFIG + 0);
        uint32_t mac1 = vt->read32(vt, VIRTIO_MMIO_CONFIG + 4);
        net->mac[0] = (uint8_t)(mac0 & 0xff);
        net->mac[1] = (uint8_t)((mac0 >> 8) & 0xff);
        net->mac[2] = (uint8_t)((mac0 >> 16) & 0xff);
        net->mac[3] = (uint8_t)((mac0 >> 24) & 0xff);
        net->mac[4] = (uint8_t)(mac1 & 0xff);
        net->mac[5] = (uint8_t)((mac1 >> 8) & 0xff);
    } else {
        net->mac[0] = 0x02;
        net->mac[1] = 0x20;
        net->mac[2] = 0x25;
        net->mac[3] = 0xa2;
        net->mac[4] = 0x00;
        net->mac[5] = (uint8_t)idx;
    }

    if (virtio_net_setup_queue(net, &net->rxq, VIRTIO_NET_QUEUE_RX) < 0)
        return -1;
    if (virtio_net_setup_queue(net, &net->txq, VIRTIO_NET_QUEUE_TX) < 0)
        return -1;

    uint64_t flags = spin_lock_irqsave(&net->lock);
    virtio_net_seed_rx_locked(net);
    spin_unlock_irqrestore(&net->lock, flags);

    status |= VIRTIO_STATUS_DRIVER_OK;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    virtio_net_kick(net, VIRTIO_NET_QUEUE_RX);

    net->valid = 1;
    printf("[VIRTIO-NET%d] ready legacy=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           idx, net->legacy, net->mac[0], net->mac[1], net->mac[2],
           net->mac[3], net->mac[4], net->mac[5]);
    return 0;
}

static int virtio_net_irq_handler(int irq, void *priv);

/*
 * Publish a board-bound instance into the device model so the networking
 * stack can find it by class, the same way virtio_blk_init() publishes its
 * block devices.  Busless records never match a busless driver without a
 * match() callback, and virtio_net_driver declares none, so this cannot
 * re-run virtio_net_driver_probe() over an instance that is already up.
 */
static driver_t virtio_net_driver;
static device_t g_standalone_net_devs[VIRTIO_NET_MAX_DEVS];
static char g_standalone_net_names[VIRTIO_NET_MAX_DEVS][16];

static void virtio_net_publish_standalone(int idx, virtio_net_inst_t *net) {
    snprintf(g_standalone_net_names[idx],
             sizeof(g_standalone_net_names[idx]), "virtio-net%d", idx);
    device_t *dev = &g_standalone_net_devs[idx];
    memset(dev, 0, sizeof(*dev));
    dev->name     = g_standalone_net_names[idx];
    dev->drv      = &virtio_net_driver;
    dev->drv_priv = net;
    dev->state    = DEV_STATE_PROBED;
    if (device_register(dev) != 0) {
        kerr("[VIRTIO-NET] could not publish '%s' to the device model\n",
             g_standalone_net_names[idx]);
        dev->drv = NULL;
        dev->drv_priv = NULL;
    }
}

int virtio_net_init(void) {
    if (g_nnet >= VIRTIO_NET_MAX_DEVS)
        return -1;

    int idx = g_nnet;
    virtio_net_inst_t *net = &g_net[idx];
    memset(net, 0, sizeof(*net));
    net->slot = idx;
    spin_init(&net->lock);
    net->vt.irq = -1;

    if (arch_virtio_net_probe(idx, &net->vt) != 0)
        return -1;

    net->legacy = net->vt.legacy;

    if (virtio_net_init_instance(net) != 0)
        return -1;

    if (net->vt.irq >= 0) {
        if (request_irq((uint32_t)net->vt.irq, virtio_net_irq_handler, 0, net) != 0) {
            printf("[VIRTIO-NET%d] Failed to register IRQ %d\n", idx, net->vt.irq);
            net->vt.irq = -1;
        } else {
            /* Without this the class interface keeps reporting a polled
             * receive path for a device that is actually IRQ-driven. */
            net->irq_registered = 1;
        }
    }

    g_nnet++;
    virtio_net_publish_standalone(idx, net);
    return 0;
}

int virtio_net_ready(int idx) {
    return idx >= 0 && idx < g_nnet && g_net[idx].valid;
}

const uint8_t *virtio_net_mac(int idx) {
    if (!virtio_net_ready(idx))
        return NULL;
    return g_net[idx].mac;
}

/* lwIP linkoutput calls this to send a frame while holding g_lwip_lock.
 * Blocking here (waiting for a TX slot, or for send completion) deadlocks:
 *   g_lwip_lock -> virtio_net_send (blocks) -> IRQ -> lwIP cb -> needs g_lwip_lock
 * Fix: with nonblock=1, submit the descriptor and return without waiting.
 * The non-nonblock path keeps busy-waiting until done, for unlocked callers. */
int virtio_net_send(int idx, const void *packet, size_t len, int nonblock) {
    if (!packet || len == 0 || len > VIRTIO_NET_FRAME_MAX)
        return -1;
    if (!virtio_net_ready(idx))
        return -1;

    virtio_net_inst_t *net = &g_net[idx];
    int slot = -1;
    uint64_t deadline = timer_get_ticks() + VIRTIO_NET_TX_TIMEOUT_TICKS;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&net->lock);
        virtio_net_complete_tx_locked(net);
        slot = virtio_net_tx_free_locked(net);
        if (slot >= 0) {
            net->tx_busy[slot] = 1;
            spin_unlock_irqrestore(&net->lock, flags);
            break;
        }
        spin_unlock_irqrestore(&net->lock, flags);
        if (nonblock) {
            net->tx_drops++;
            return -1;
        }
        if (timer_get_ticks() >= deadline) {
            net->tx_drops++;
            return -1;
        }
        virtio_net_wait_for_tx_progress();
    }

    uint8_t *buf = net->tx_buf[slot];
    memset(buf, 0, VIRTIO_NET_HDR_SIZE);
    memcpy(buf + VIRTIO_NET_HDR_SIZE, packet, len);

    uint64_t flags = spin_lock_irqsave(&net->lock);
    virtio_net_queue_t *q = &net->txq;
    virtq_desc_t *desc = queue_desc(net, q);
    virtq_avail_t *avail = queue_avail(net, q);

    desc[slot].addr = va_to_pa(buf);
    desc[slot].len = (uint32_t)(VIRTIO_NET_HDR_SIZE + len);
    desc[slot].flags = 0;
    desc[slot].next = 0;

    uint16_t avail_slot = avail->idx % VIRTIO_QUEUE_SIZE;
    avail->ring[avail_slot] = (uint16_t)slot;
    wmb();
    avail->idx++;

    arch_dma_sync_for_device(buf, VIRTIO_NET_HDR_SIZE + len);
    arch_dma_sync_for_device(&desc[slot], sizeof(desc[slot]));
    arch_dma_sync_for_device(&avail->ring[avail_slot], sizeof(uint16_t));
    arch_dma_sync_for_device(&avail->idx, sizeof(uint16_t));
    virtio_net_kick(net, VIRTIO_NET_QUEUE_TX);
    spin_unlock_irqrestore(&net->lock, flags);

    /* nonblock mode: submit, then return without waiting for completion.
     * virtio_net_complete_tx_locked() clears tx_busy, and reaches it either
     * from the TX completion interrupt or from the device class poll hook. */
    if (nonblock) {
        net->tx_packets++;
        return (int)len;
    }

    for (;;) {
        flags = spin_lock_irqsave(&net->lock);
        virtio_net_complete_tx_locked(net);
        int done = !net->tx_busy[slot];
        spin_unlock_irqrestore(&net->lock, flags);
        if (done) {
            net->tx_packets++;
            return (int)len;
        }
        if (timer_get_ticks() >= deadline) {
            flags = spin_lock_irqsave(&net->lock);
            if (slot >= 0 && slot < VIRTIO_QUEUE_SIZE)
                net->tx_busy[slot] = 0;
            spin_unlock_irqrestore(&net->lock, flags);
            net->tx_drops++;
            return -1;
        }
        virtio_net_wait_for_tx_progress();
    }
}

int virtio_net_recv(int idx, void *packet, size_t maxlen) {
    if (!packet || maxlen == 0)
        return -1;
    if (!virtio_net_ready(idx))
        return -1;

    virtio_net_inst_t *net = &g_net[idx];
    uint64_t flags = spin_lock_irqsave(&net->lock);
    virtio_net_queue_t *q = &net->rxq;
    virtq_used_t *used = queue_used(net, q);

    arch_dma_sync_for_cpu(&used->idx, sizeof(uint16_t));
    uint16_t used_idx = ((volatile virtq_used_t *)used)->idx;
    if (q->last_used == used_idx) {
        spin_unlock_irqrestore(&net->lock, flags);
        return 0;
    }

    uint16_t ring_idx = q->last_used % VIRTIO_QUEUE_SIZE;
    arch_dma_sync_for_cpu(&used->ring[ring_idx], sizeof(virtq_used_elem_t));
    uint16_t slot = (uint16_t)used->ring[ring_idx].id;
    uint32_t used_len = used->ring[ring_idx].len;
    q->last_used++;

    int ret = 0;
    if (slot >= VIRTIO_QUEUE_SIZE || used_len <= VIRTIO_NET_HDR_SIZE) {
        net->rx_drops++;
    } else {
        if (used_len > VIRTIO_NET_BUF_SIZE)
            used_len = VIRTIO_NET_BUF_SIZE;
        size_t pkt_len = used_len - VIRTIO_NET_HDR_SIZE;
        if (pkt_len > maxlen) {
            pkt_len = maxlen;
            net->rx_drops++;
        }
        arch_dma_sync_for_cpu(net->rx_buf[slot], used_len);
        memcpy(packet, net->rx_buf[slot] + VIRTIO_NET_HDR_SIZE, pkt_len);
        net->rx_packets++;
        ret = (int)pkt_len;
    }

    if (slot < VIRTIO_QUEUE_SIZE) {
        virtio_net_submit_rx_locked(net, slot);
        virtio_net_kick(net, VIRTIO_NET_QUEUE_RX);
    }
    spin_unlock_irqrestore(&net->lock, flags);
    return ret;
}

/*
 * virtio_net_poll_rx_all:
 * Fallback RX progress path for transports that cannot raise a device IRQ
 * (e.g. PCI virtio on platforms without MSI/INTx routing), and an event-driven
 * bottom-half for IRQ-capable transports.
 *
 * - IRQ-capable: the IRQ top-half raises the lwIP RX pending flag, so the
 *   scheduler/idle hook only drains (acquiring g_lwip_lock) when a device
 *   actually signalled work.  With no network traffic the hot path acquires
 *   no lock at all.
 * - Poll-only: at least one live instance has no IRQ line, so every
 *   kernel_progress_poll() must keep draining unconditionally or its RX would
 *   never progress.
 */
void virtio_net_poll_rx_all_bounded(unsigned budget) {
    int poll_only = 0;
    for (int i = 0; i < g_nnet; i++) {
        if (g_net[i].valid && !g_net[i].irq_registered) {
            poll_only = 1;
            break;
        }
    }
    if (!poll_only && !a20_lwip_rx_pending_any())
        return;

    uint64_t flags = a20_lwip_lock();
    a20_lwip_poll_rx_locked(budget);
    a20_lwip_unlock(flags);
}

void virtio_net_poll_rx_all(void) {
    virtio_net_poll_rx_all_bounded(0);
}

/*
 * VIRTIO_NET_IRQ_MODEL:
 * - The device raises an IRQ on RX packet arrival and TX completion.
 * - The handler is a minimal top-half: it runs under g_lwip_lock, acks the
 *   device, drains the RX used ring into lwIP input (which copies pbuf data
 *   into the preallocated per-PCB bottom-half ring), schedules the bottom-half,
 *   and returns.  It never calls kmalloc or acquires g_net_lock.
 */
static int virtio_net_irq_handler(int irq, void *priv) {
    (void)irq;
    virtio_net_inst_t *net = (virtio_net_inst_t *)priv;
    if (!net || !net->valid)
        return 0;

    uint32_t isr = net->vt.read32(&net->vt, VIRTIO_MMIO_INTERRUPT_STATUS);
    /* Both legacy and modern virtio-mmio transports require the driver to
     * write the ISR value back to INTERRUPT_ACK; legacy devices keep the
     * interrupt line asserted otherwise.  Matches virtio_blk_irq_handler. */
    net->vt.write32(&net->vt, VIRTIO_MMIO_INTERRUPT_ACK, isr);

    uint64_t flags = a20_lwip_lock();
    a20_lwip_process_netif_irq_locked(net->slot);
    a20_lwip_unlock(flags);
    return 0;
}

static uint32_t virtio_net_mmio_read32(virtio_transport_t *t, uint32_t off) {
    return readl((const volatile void *)((uintptr_t)t->priv + off));
}

static void virtio_net_mmio_write32(virtio_transport_t *t, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)((uintptr_t)t->priv + off));
}

static int virtio_net_driver_probe(device_t *dev) {
    if (g_nnet >= VIRTIO_NET_MAX_DEVS) {
        kinfo("[VIRTIO-NET] Too many devices (max %d)\n", VIRTIO_NET_MAX_DEVS);
        return -1;
    }

    int idx = g_nnet;
    virtio_net_inst_t *net = &g_net[idx];
    memset(net, 0, sizeof(*net));
    net->vt.irq = -1;

    if (dev->bus == &pci_bus) {
        if (pci_virtio_transport_init(dev, 1, &net->vt) != 0)
            return -1;
    } else {
        resource_t *mmio_res = device_get_resource(dev, RES_MMIO, 0);
        if (!mmio_res) {
            kinfo("[VIRTIO-NET] No MMIO resource for device '%s'\n", dev->name);
            return -1;
        }
        net->vt.read32  = virtio_net_mmio_read32;
        net->vt.write32 = virtio_net_mmio_write32;
        net->vt.priv    = (void *)(uintptr_t)mmio_res->start;
        resource_t *mmio_irq = device_get_resource(dev, RES_IRQ, 0);
        if (mmio_irq)
            net->vt.irq = (int)mmio_irq->start;
    }
    net->slot       = idx;
    net->legacy     = (net->vt.read32(&net->vt, VIRTIO_MMIO_VERSION) == 1);
    spin_init(&net->lock);

    uint32_t magic = net->vt.read32(&net->vt, VIRTIO_MMIO_MAGIC);
    uint32_t dev_id = net->vt.read32(&net->vt, VIRTIO_MMIO_DEVICE_ID);
    if (magic != 0x74726976 || dev_id != 1) {
        kinfo("[VIRTIO-NET] Invalid transport for device '%s'\n", dev->name);
        return -1;
    }

    dev->drv_priv = net;

    if (virtio_net_init_instance(net) != 0) {
        kinfo("[VIRTIO-NET] Init failed for device '%s'\n", dev->name);
        return -1;
    }

    resource_t *irq_res = device_get_resource(dev, RES_IRQ, 0);

    /* Delivery, best first: one message-signalled vector per queue, then the
     * shared INTx line, then the gated polling path. */
    int msix_vectors = 0;
    if (net->vt.msix_prepare) {
        int r = net->vt.msix_prepare(&net->vt, VIRTIO_NET_QUEUES);
        if (r == 0) {
            msix_vectors = net->vt.msix_vectors;
        } else {
            kinfo("[VIRTIO-NET] %s: MSI-X unavailable (%d)\n", dev->name, r);
        }
    }

    if (msix_vectors > 0) {
        int registered = 0;
        for (int i = 0; i < msix_vectors; i++) {
            if (request_irq((uint32_t)(net->vt.msix_base + i),
                            virtio_net_irq_handler, 0, net) == 0)
                registered++;
        }
        if (registered == msix_vectors) {
            net->irq = net->vt.msix_base;
            net->irq_registered = 1;
            /* Leaving an INTx handler behind would give the device two
             * delivery paths for the same completions. */
            net->vt.irq = -1;
            net->vt.msix_arm(&net->vt);
            kinfo("[VIRTIO-NET] %s using MSI-X vectors %d..%d\n",
                  dev->name, net->vt.msix_base,
                  net->vt.msix_base + msix_vectors - 1);
        } else {
            for (int i = 0; i < registered; i++)
                free_irq((uint32_t)(net->vt.msix_base + i), net);
            net->vt.msix_teardown(&net->vt);
            kinfo("[VIRTIO-NET] %s MSI-X handler registration failed (%d/%d)\n",
                  dev->name, registered, msix_vectors);
        }
    } else if (net->vt.irq >= 0) {
        unsigned long irq_flags = net->vt.shared_irq ? IRQF_SHARED : 0;
        if (request_irq((uint32_t)net->vt.irq, virtio_net_irq_handler,
                        irq_flags, net) == 0) {
            net->irq = net->vt.irq;
            net->irq_registered = 1;
        } else {
            kinfo("[VIRTIO-NET] Failed to register transport IRQ for '%s'; using polling\n",
                  dev->name);
            net->vt.irq = -1;
        }
    } else if (dev->bus == &pci_bus) {
        /* PCI transports resolve their vector through arch_pci_intx_irq()
         * during transport init; irq < 0 means the platform has no INTx
         * routing, and MSI-X was refused above, so RX/TX stay on the gated
         * polling path.  The legacy IRQ Line register resource is NOT a
         * usable vector on PCI. */
        kinfo("[VIRTIO-NET] PCI transport using completion polling\n");
    } else if (irq_res) {
        if (request_irq((uint32_t)irq_res->start, virtio_net_irq_handler, 0, net) == 0) {
            net->irq = (int)irq_res->start;
            net->irq_registered = 1;
        } else
            kinfo("[VIRTIO-NET] Failed to register IRQ %lu for '%s'\n",
                  (unsigned long)irq_res->start, dev->name);
    } else {
        kinfo("[VIRTIO-NET] No IRQ resource for device '%s'\n", dev->name);
    }

    g_nnet++;
    kinfo("[VIRTIO-NET] Probed device '%s' (irq=%d msix=%d)\n",
          dev->name, net->irq_registered ? net->irq : -1, msix_vectors);
    return 0;
}

static int virtio_net_class_send(struct device *dev, const void *pkt, size_t len) {
    virtio_net_inst_t *net = (virtio_net_inst_t *)dev->drv_priv;
    return virtio_net_send(net->slot, pkt, len, 1);
}

static int virtio_net_class_recv(struct device *dev, void *buf, size_t maxlen) {
    virtio_net_inst_t *net = (virtio_net_inst_t *)dev->drv_priv;
    return virtio_net_recv(net->slot, buf, maxlen);
}

static const uint8_t *virtio_net_class_mac(struct device *dev) {
    virtio_net_inst_t *net = (virtio_net_inst_t *)dev->drv_priv;
    if (!net->valid)
        return NULL;
    return net->mac;
}

static void virtio_net_class_poll(struct device *dev) {
    virtio_net_inst_t *net = (virtio_net_inst_t *)dev->drv_priv;
    uint64_t flags = spin_lock_irqsave(&net->lock);
    virtio_net_complete_tx_locked(net);
    spin_unlock_irqrestore(&net->lock, flags);
}

static int virtio_net_class_rx_irq_driven(struct device *dev) {
    virtio_net_inst_t *net = (virtio_net_inst_t *)dev->drv_priv;
    return net && net->irq_registered;
}

/* Published to the stack as a weak symbol rather than through net_dev_ops_t.
 * These four counters were already maintained here but were unreachable from
 * anywhere, so a drop could not be attributed to the device or to lwIP. */
void virtio_net_dev_stats(struct device *dev, net_dev_stats_t *out) {
    if (!out)
        return;
    out->rx_packets = out->rx_drops = 0;
    out->tx_packets = out->tx_drops = 0;
    virtio_net_inst_t *net = (virtio_net_inst_t *)dev->drv_priv;
    if (!net)
        return;
    uint64_t flags = spin_lock_irqsave(&net->lock);
    out->rx_packets = net->rx_packets;
    out->rx_drops   = net->rx_drops;
    out->tx_packets = net->tx_packets;
    out->tx_drops   = net->tx_drops;
    spin_unlock_irqrestore(&net->lock, flags);
}

static net_dev_ops_t virtio_net_class_ops = {
    .send = virtio_net_class_send,
    .recv = virtio_net_class_recv,
    .mac  = virtio_net_class_mac,
    .poll = virtio_net_class_poll,
    .rx_irq_driven = virtio_net_class_rx_irq_driven,
};

static const device_id_t virtio_net_ids[] = {
    /* VirtIO-MMIO bus matching uses the transport device type as device ID. */
    { .vendor = 0, .device = 1,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    /* QEMU transitional virtio-net-pci presents the legacy net ID 0x1000
     * with modern capabilities; the transitional block ID is 0x1001. */
    { .vendor = 0x1AF4, .device = 0x1000,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1041,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static int virtio_net_driver_remove(device_t *dev) {
    virtio_net_inst_t *net = dev ? dev->drv_priv : NULL;
    if (!net)
        return 0;
    net->valid = 0;
    if (net->irq_registered) {
        if (net->vt.msix_vectors > 0) {
            for (int i = 0; i < net->vt.msix_vectors; i++)
                free_irq((uint32_t)(net->vt.msix_base + i), net);
            net->vt.msix_teardown(&net->vt);
        } else {
            free_irq((uint32_t)net->irq, net);
        }
    }
    net->irq_registered = 0;
    net->vt.write32(&net->vt, VIRTIO_MMIO_STATUS, 0);
    mb();
    dev->drv_priv = NULL;
    return 0;
}

static driver_t virtio_net_driver = {
    .name       = "virtio-net",
    .id_table   = virtio_net_ids,
    .bus        = NULL,
    .probe      = virtio_net_driver_probe,
    .remove     = virtio_net_driver_remove,
    .class_ops  = &virtio_net_class_ops,
    .class_type = DEV_CLASS_NET,
};

DRIVER_REGISTER(virtio_net_driver);
