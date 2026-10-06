/*
 * virtio-rng — VIRTIO_ID_RNG (4) entropy source, published as a CHAR class
 * device named /dev/hwrng.  drvmod package `virtio-rng.a20drv`; the only
 * difference from a built-in driver is the registration backend in
 * kernel/drvmod/examples/virtio_rng.c.
 *
 * Two transports, one data plane.  The driver binds `dev->bus == &pci_bus`
 * through pci_virtio_transport_init() (x86_64, loongarch64) and everything
 * else as a raw virtio-mmio v2 window (riscv64, aarch64), which is the same
 * split virtio-blk and virtio-input already use.  Both go through
 * virtio_transport_t, so everything below the probe is transport-agnostic and
 * is driven exclusively by MMIO-style register offsets.
 *
 * Queue 0 is the request queue, and it is the only queue the device has
 * (hw/virtio/virtio-rng.c: virtio_add_queue(vdev, 8, handle_input)).  The
 * device does no unsolicited writes: it only acts on a descriptor the driver
 * posted, so this driver never keeps the queue permanently loaded.  It posts
 * the free slots when a read needs entropy and lets them come back through the
 * used ring.  That matters for more than CPU time — QEMU asks its backend for
 * exactly the number of bytes the avail ring advertises
 * (virtqueue_get_avail_bytes) and then stops, so a permanently-posted queue
 * would turn every probe of the driver into an unbounded entropy draw on the
 * host, with nothing on the guest reading it.
 *
 * Interrupt driven where the platform delivers one: the ISR acks the
 * virtio-mmio interrupt status and drains the used ring into a byte ring.
 * Where request_irq() fails, read() and poll() still drain the ring, so the
 * device degrades to a polled source rather than disappearing.
 *
 * Read is bounded: with an empty ring the driver posts the free slots, rings
 * the device and waits up to VRNG_READ_TIMEOUT_MS, yielding between drains.
 * The instance lock is never held across that wait (see
 * docs/drivers/guide/lock-order.md), and the wait gives up rather than
 * parking, so a device that stops answering cannot wedge a reader.
 *
 * Every byte handed to a reader is also folded into the kernel entropy pool in
 * kernel/core/random.c through random_reseed(), which is what sys_getrandom()
 * and the ASLR/stack-canary seed draw from.  The pool is fed from the read
 * path only — random_reseed() reaches proc_current() through
 * arch_entropy_sample(), so calling it from the ISR would sample scheduler
 * state from interrupt context.  A guest that never opens /dev/hwrng
 * therefore does not harvest the device at all; that boundary is recorded in
 * docs/drivers/meta/implementation-status.md.
 */
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drvmod/drvmod.h"
#include "core/consts.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/random.h"
#include "core/string.h"
#include "core/timer.h"
#include "proc/proc.h"

/* VIRTIO_ID_RNG. */
#define VRNG_DEVICE_ID          4
/* modern (virtio-1.0 only) 1af4:1044 = 0x1040 + VIRTIO_ID_RNG, and the
 * transitional 1af4:1005 QEMU presents when it also offers the legacy
 * interface (pci_config_set_device_id(config, proxy->trans_devid)). */
#define VRNG_PCI_ID_MODERN      0x1044
#define VRNG_PCI_ID_TRANS       0x1005

/* VirtIO MMIO-style offsets accepted by virtio_transport_t; the PCI transport
 * translates them into the common/notify/ISR capability windows. */
#define VRNG_MMIO_MAGIC            0x000u
#define VRNG_MMIO_VERSION          0x004u
#define VRNG_MMIO_DEVICE_ID        0x008u
#define VRNG_MMIO_DEV_FEATURES     0x010u
#define VRNG_MMIO_DEV_FEATURES_SEL 0x014u
#define VRNG_MMIO_DRV_FEATURES     0x020u
#define VRNG_MMIO_DRV_FEATURES_SEL 0x024u
#define VRNG_MMIO_QUEUE_SEL        0x030u
#define VRNG_MMIO_QUEUE_NUM_MAX    0x034u
#define VRNG_MMIO_QUEUE_NUM        0x038u
#define VRNG_MMIO_QUEUE_READY      0x044u
#define VRNG_MMIO_QUEUE_NOTIFY     0x050u
#define VRNG_MMIO_INTR_STATUS      0x060u
#define VRNG_MMIO_INTR_ACK         0x064u
#define VRNG_MMIO_STATUS           0x070u
#define VRNG_MMIO_QUEUE_DESC_LOW   0x080u
#define VRNG_MMIO_QUEUE_DESC_HIGH  0x084u
#define VRNG_MMIO_QUEUE_DRIVER_LOW  0x090u
#define VRNG_MMIO_QUEUE_DRIVER_HIGH 0x094u
#define VRNG_MMIO_QUEUE_DEVICE_LOW  0x0a0u
#define VRNG_MMIO_QUEUE_DEVICE_HIGH 0x0a4u

#define VRNG_MMIO_MAGIC_VALUE      0x74726976u
#define VRNG_F_VERSION_1           0x1u
#define VRNG_STATUS_ACK            1u
#define VRNG_STATUS_DRIVER         2u
#define VRNG_STATUS_DRIVER_OK      4u
#define VRNG_STATUS_FEATURES_OK    8u
#define VRNG_STATUS_FAILED         128u

/* QEMU builds the request queue with 8 slots (virtio_add_queue(vdev, 8, ...));
 * asking for more than the device offers would fail the setup, so the driver
 * takes exactly that. */
#define VRNG_Q_REQ            0U
#define VRNG_Q_SLOTS          8U
#define VRNG_BUF_SIZE         256U
/* 8 x 256 bytes of device buffer plus the byte ring handed to readers.  Sized
 * so one full drain of the request queue fits with room to spare. */
#define VRNG_RING_SIZE        4096U
#define VRNG_READ_TIMEOUT_MS  500U
/* random_reseed() per read, capped: a reader asking for 4 KiB should not turn
 * into 512 pool writes.  The remainder is dropped rather than compressed. */
#define VRNG_POOL_MAX_CHUNKS  64U

#define VRNG_QD_F_WRITE       2u

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vrng_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VRNG_Q_SLOTS];
    uint16_t used_event;
} vrng_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} vrng_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vrng_used_elem_t ring[VRNG_Q_SLOTS];
    uint16_t avail_event;
} vrng_used_t;

/* One coherent allocation: rings first, then the data area, so every descriptor
 * address is dma_handle + a compile-time offset (never va_to_pa). */
typedef struct {
    vrng_desc_t  desc[VRNG_Q_SLOTS] ALIGNED(64);
    vrng_avail_t avail ALIGNED(64);
    vrng_used_t  used ALIGNED(64);
    uint8_t      buf[VRNG_Q_SLOTS][VRNG_BUF_SIZE] ALIGNED(64);
} vrng_mem_t;

typedef struct {
    virtio_transport_t vt;
    vrng_mem_t        *mem;
    uint64_t           dma;
    /* Guards the byte ring, the slot bookkeeping, the counters and the valid
     * flag.  Innermost lock: nothing below ever takes another kernel lock with
     * it held — the device is only touched from the ISR and from read/poll,
     * both of which already run with interrupts masked. */
    spinlock_t lock ALIGNED(64);
    uint8_t    ring[VRNG_RING_SIZE];
    uint32_t   head;
    uint32_t   tail;
    uint8_t    slot_posted[VRNG_Q_SLOTS];
    uint32_t   entropy_bytes;
    uint32_t   pool_chunks;
    uint32_t   read_timeouts;
    uint16_t   last_used;
    int        mmio;          /* 1 on the raw virtio-mmio path */
    int        irq;
    int        irq_registered;
    int        valid;
} vrng_inst_t;

/* One device, one instance: a second virtio-rng function stays unbound rather
 * than fighting the first one for the single /dev/hwrng node. */
static vrng_inst_t g_vrng;

/* ------------------------------------------------------------------ */
/* Ring helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint64_t vrng_buf_pa(const vrng_inst_t *p, unsigned slot)
{
    return p->dma + offsetof(vrng_mem_t, buf) + (uint64_t)slot * VRNG_BUF_SIZE;
}

/*
 * Move everything the device completed into the byte ring.
 *
 * Runs from the IRQ top-half and from read()/poll(), so it must not block and
 * must not allocate.  A malformed used element stops the drain rather than
 * walking off the ring: the bytes already banked stay valid, and the next read
 * re-runs this.
 */
static void vrng_pump(vrng_inst_t *p)
{
    uint64_t flags = spin_lock_irqsave(&p->lock);
    if (!p->valid || !p->mem) {
        spin_unlock_irqrestore(&p->lock, flags);
        return;
    }

    dma_sync_for_cpu(&p->mem->used, sizeof(p->mem->used));
    while (p->last_used != p->mem->used.idx) {
        uint16_t slot_idx = (uint16_t)(p->last_used % VRNG_Q_SLOTS);
        uint32_t id = p->mem->used.ring[slot_idx].id;
        uint32_t len = p->mem->used.ring[slot_idx].len;
        if (id >= VRNG_Q_SLOTS)
            break; /* protocol violation: stop consuming, keep the rest */
        if (len > VRNG_BUF_SIZE)
            len = VRNG_BUF_SIZE;
        if (!p->slot_posted[id])
            break; /* not ours: same stop, same reason */

        uint8_t *chunk = p->mem->buf[id];
        dma_sync_for_cpu(chunk, len);

        for (uint32_t i = 0; i < len; i++) {
            uint32_t next = (p->head + 1U) % VRNG_RING_SIZE;
            if (next == p->tail)
                break; /* ring full: the rest of this buffer is discarded */
            p->ring[p->head] = chunk[i];
            p->head = next;
            p->entropy_bytes++;
        }
        p->slot_posted[id] = 0;
        p->last_used++;
    }
    spin_unlock_irqrestore(&p->lock, flags);
}

/* Copy out of the byte ring.  Returns 0 when it is empty. */
static size_t vrng_take(vrng_inst_t *p, void *buf, size_t count)
{
    uint64_t flags = spin_lock_irqsave(&p->lock);
    size_t avail = (p->head + VRNG_RING_SIZE - p->tail) % VRNG_RING_SIZE;
    size_t n = count < avail ? count : avail;
    for (size_t i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = p->ring[(p->tail + i) % VRNG_RING_SIZE];
    p->tail = (uint32_t)((p->tail + n) % VRNG_RING_SIZE);
    spin_unlock_irqrestore(&p->lock, flags);
    return n;
}

/*
 * Hand the free slots to the device and ring it.
 *
 * Ordered descriptor -> wmb -> avail entry -> wmb -> notify, with the notify
 * issued after the instance lock is dropped: that is the sequence the spec
 * requires, and the lock is not held across a bus write.  Returns how many
 * slots were posted, so a caller can tell "nothing to ask for" from "asked".
 */
static uint32_t vrng_post_free(vrng_inst_t *p)
{
    uint8_t newly[VRNG_Q_SLOTS];
    uint32_t posted = 0;

    uint64_t flags = spin_lock_irqsave(&p->lock);
    if (!p->valid || !p->mem) {
        spin_unlock_irqrestore(&p->lock, flags);
        return 0;
    }
    memset(newly, 0, sizeof(newly));
    for (unsigned i = 0; i < VRNG_Q_SLOTS; i++) {
        if (p->slot_posted[i])
            continue;
        p->mem->desc[i].addr = vrng_buf_pa(p, i);
        p->mem->desc[i].len = VRNG_BUF_SIZE;
        p->mem->desc[i].flags = VRNG_QD_F_WRITE;
        p->mem->desc[i].next = 0;
        newly[i] = 1;
        p->slot_posted[i] = 1;
        posted++;
    }
    if (posted) {
        dma_sync_for_device(p->mem->desc, sizeof(p->mem->desc));
        for (unsigned i = 0; i < VRNG_Q_SLOTS; i++) {
            if (!newly[i])
                continue;
            uint16_t idx = p->mem->avail.idx;
            p->mem->avail.ring[idx % VRNG_Q_SLOTS] = (uint16_t)i;
            p->mem->avail.idx = (uint16_t)(idx + 1);
        }
        wmb();
        dma_sync_for_device(&p->mem->avail, sizeof(p->mem->avail));
    }
    spin_unlock_irqrestore(&p->lock, flags);

    if (posted)
        p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_NOTIFY, VRNG_Q_REQ);
    return posted;
}

static uint32_t vrng_inflight(vrng_inst_t *p)
{
    uint32_t n = 0;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    for (unsigned i = 0; i < VRNG_Q_SLOTS; i++)
        n += p->slot_posted[i] ? 1 : 0;
    spin_unlock_irqrestore(&p->lock, flags);
    return n;
}

/*
 * Fold the bytes just handed to a reader into the kernel entropy pool.
 *
 * random_reseed() mixes one 64-bit word per call (kernel/core/random.c), so a
 * 256-byte read becomes four pool reseeds rather than one.
 */
static void vrng_feed_pool(vrng_inst_t *p, const void *buf, size_t len)
{
    const uint8_t *bytes = buf;
    size_t chunks = len / sizeof(uint64_t);
    if (chunks > VRNG_POOL_MAX_CHUNKS)
        chunks = VRNG_POOL_MAX_CHUNKS;

    for (size_t i = 0; i < chunks; i++) {
        uint64_t word;
        memcpy(&word, bytes + i * sizeof(uint64_t), sizeof(word));
        random_reseed(word);
    }
    /* A short read that carried no full word still carries entropy. */
    if (!chunks && len) {
        uint64_t word = 0;
        memcpy(&word, bytes, len);
        random_reseed(word);
    }

    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->pool_chunks += (uint32_t)chunks;
    spin_unlock_irqrestore(&p->lock, flags);
}

static int vrng_irq_handler(int irq, void *priv)
{
    (void)irq;
    vrng_inst_t *p = priv;
    if (!p || !p->valid)
        return 0;

    /* On the PCI transport the read below already reaches the ISR window and
     * deasserts; only the raw virtio-mmio path needs the explicit ack. */
    uint32_t status = p->vt.read32(&p->vt, VRNG_MMIO_INTR_STATUS);
    if (p->mmio && status)
        p->vt.write32(&p->vt, VRNG_MMIO_INTR_ACK, status);

    vrng_pump(p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Queue setup                                                         */
/* ------------------------------------------------------------------ */

static int vrng_setup_queue(vrng_inst_t *p, uint32_t index, uint32_t slots)
{
    p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_SEL, index);
    if (p->vt.read32(&p->vt, VRNG_MMIO_QUEUE_NUM_MAX) < slots)
        return -ENODEV;
    if (p->vt.read32(&p->vt, VRNG_MMIO_QUEUE_READY) != 0)
        return -EBUSY;
    p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_NUM, slots);
    return 0;
}

static void vrng_program_queue(vrng_inst_t *p, uint32_t index,
                               uint64_t desc, uint64_t avail, uint64_t used)
{
    p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_SEL, index);
    p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_DESC_LOW, (uint32_t)desc);
    p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc >> 32));
    p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_DRIVER_LOW, (uint32_t)avail);
    p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_DRIVER_HIGH,
                  (uint32_t)(avail >> 32));
    p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_DEVICE_LOW, (uint32_t)used);
    p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_DEVICE_HIGH, (uint32_t)(used >> 32));
    mb();
    p->vt.write32(&p->vt, VRNG_MMIO_QUEUE_READY, 1);
    mb();
}

static void vrng_free_dma(vrng_inst_t *p)
{
    if (p->mem) {
        dma_free_coherent_aligned(p->mem, sizeof(*p->mem), p->dma);
        p->mem = NULL;
    }
}

/* Full rollback for every probe failure: the core only clears pointers, so
 * IRQ, DMA and the device state have to be undone here. */
static void vrng_fail(vrng_inst_t *p, int irq_registered, int irq)
{
    if (irq_registered && irq >= 0)
        free_irq((uint32_t)irq, p);
    p->vt.write32(&p->vt, VRNG_MMIO_STATUS,
                  p->vt.read32(&p->vt, VRNG_MMIO_STATUS) | VRNG_STATUS_FAILED);
    vrng_free_dma(p);
    memset(p, 0, sizeof(*p));
}

/* ------------------------------------------------------------------ */
/* Transport bring-up                                                 */
/* ------------------------------------------------------------------ */

static uint32_t vrng_mmio_read32(virtio_transport_t *t, uint32_t off)
{
    return readl((const volatile void *)((uintptr_t)t->priv + off));
}

static void vrng_mmio_write32(virtio_transport_t *t, uint32_t off, uint32_t val)
{
    writel(val, (volatile void *)((uintptr_t)t->priv + off));
}

static int vrng_init_transport(device_t *dev, const virtio_transport_t *vt_in,
                               int is_mmio)
{
    vrng_inst_t *p = &g_vrng;

    memset(p, 0, sizeof(*p));
    spin_init(&p->lock);
    p->vt = *vt_in;
    p->irq = p->vt.irq;
    p->mmio = is_mmio;

    /* The raw virtio-mmio window carries the magic/version/device-id probe the
     * PCI capability path does not need; on PCI the transport answers the same
     * three offsets, so the check runs identically on both. */
    if (p->vt.read32(&p->vt, VRNG_MMIO_MAGIC) != VRNG_MMIO_MAGIC_VALUE ||
        p->vt.read32(&p->vt, VRNG_MMIO_VERSION) != 2U ||
        p->vt.read32(&p->vt, VRNG_MMIO_DEVICE_ID) != VRNG_DEVICE_ID)
        return -ENODEV;

    /* Feature negotiation: VIRTIO_F_VERSION_1 only, so the queue is the modern
     * split ring programmed below.  virtio-rng offers no other feature bits
     * (get_features in hw/virtio/virtio-rng.c returns its argument unchanged),
     * so the low half is explicitly offered as zero. */
    p->vt.write32(&p->vt, VRNG_MMIO_STATUS, 0);
    mb();
    uint32_t status = VRNG_STATUS_ACK | VRNG_STATUS_DRIVER;
    p->vt.write32(&p->vt, VRNG_MMIO_STATUS, status);
    mb();

    p->vt.write32(&p->vt, VRNG_MMIO_DEV_FEATURES_SEL, 1);
    uint32_t features_hi = p->vt.read32(&p->vt, VRNG_MMIO_DEV_FEATURES);
    if (!(features_hi & VRNG_F_VERSION_1))
        return -EOPNOTSUPP;
    p->vt.write32(&p->vt, VRNG_MMIO_DRV_FEATURES_SEL, 1);
    p->vt.write32(&p->vt, VRNG_MMIO_DRV_FEATURES, VRNG_F_VERSION_1);
    p->vt.write32(&p->vt, VRNG_MMIO_DRV_FEATURES_SEL, 0);
    p->vt.write32(&p->vt, VRNG_MMIO_DRV_FEATURES, 0);
    mb();

    status |= VRNG_STATUS_FEATURES_OK;
    p->vt.write32(&p->vt, VRNG_MMIO_STATUS, status);
    mb();
    if (!(p->vt.read32(&p->vt, VRNG_MMIO_STATUS) & VRNG_STATUS_FEATURES_OK))
        return -ENODEV;

    p->mem = dma_alloc_coherent_aligned(dev, sizeof(*p->mem), PAGE_SIZE,
                                        &p->dma);
    if (!p->mem)
        return -ENOMEM;
    memset(p->mem, 0, sizeof(*p->mem));
    dma_sync_for_device(p->mem, sizeof(*p->mem));

    if (vrng_setup_queue(p, VRNG_Q_REQ, VRNG_Q_SLOTS) < 0)
        return -ENODEV;
    vrng_program_queue(p, VRNG_Q_REQ,
                       p->dma + offsetof(vrng_mem_t, desc),
                       p->dma + offsetof(vrng_mem_t, avail),
                       p->dma + offsetof(vrng_mem_t, used));

    int irq_registered = 0;
    if (p->vt.irq >= 0) {
        unsigned long irq_flags = p->vt.shared_irq ? IRQF_SHARED : 0;
        if (request_irq((uint32_t)p->vt.irq, vrng_irq_handler, irq_flags,
                        p) == 0) {
            p->irq_registered = 1;
            irq_registered = 1;
        } else {
            kinfo("[VRNG] INTx %d unavailable, entropy falls back to "
                  "read/poll\n", p->vt.irq);
            p->vt.irq = -1;
        }
    }

    p->valid = 1;
    dev->drv_priv = p;

    /* The core publishes the class device right after this returns 0; asking
     * for the node name here is the only place a driver may choose one. */
    int ret = drv_device_set_devfs_name(dev, "hwrng");
    if (ret < 0) {
        p->valid = 0;
        vrng_fail(p, irq_registered, p->vt.irq);
        dev->drv_priv = NULL;
        return ret;
    }

    status |= VRNG_STATUS_DRIVER_OK;
    p->vt.write32(&p->vt, VRNG_MMIO_STATUS, status);
    mb();

    kinfo("[VRNG] virtio-rng ready (irq=%d, /dev/hwrng)\n", p->vt.irq);
    return 0;
}

static int vrng_probe(device_t *dev)
{
    if (g_vrng.valid)
        return -EBUSY; /* single instance, documented limit */

    virtio_transport_t vt;
    int is_mmio = 1;

    if (dev->bus == &pci_bus) {
        if (pci_virtio_transport_init(dev, VRNG_DEVICE_ID, &vt) != 0)
            return -ENODEV;
        is_mmio = 0;
    } else {
        resource_t *mmio_res = drv_device_get_resource(dev, RES_MMIO, 0);
        if (!mmio_res || !mmio_res->start)
            return -ENODEV;
        /* An MMIO-backed device with no interrupt resource still works: the
         * read path drains the ring itself, so the IRQ is optional here. */
        resource_t *irq_res = drv_device_get_resource(dev, RES_IRQ, 0);
        vt.read32 = vrng_mmio_read32;
        vt.write32 = vrng_mmio_write32;
        vt.priv = (void *)(uintptr_t)mmio_res->start;
        vt.legacy = 0;
        vt.irq = irq_res ? (int)irq_res->start : -1;
    }

    vrng_inst_t *p = &g_vrng;
    int ret = vrng_init_transport(dev, &vt, is_mmio);
    if (ret != 0) {
        /* vrng_init_transport() leaves the instance partly built on every
         * failure path, so roll it back here rather than at each site. */
        if (p->valid || p->mem || p->irq_registered)
            vrng_fail(p, p->irq_registered, p->vt.irq);
        else
            memset(p, 0, sizeof(*p));
        return ret;
    }
    return 0;
}

static int vrng_remove(device_t *dev)
{
    vrng_inst_t *p = dev ? dev->drv_priv : NULL;
    if (!p)
        return 0;

    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->valid = 0;
    spin_unlock_irqrestore(&p->lock, flags);

    if (p->irq_registered && p->irq >= 0)
        free_irq((uint32_t)p->irq, p);

    p->vt.write32(&p->vt, VRNG_MMIO_STATUS, 0);
    mb();
    vrng_free_dma(p);
    dev->drv_priv = NULL;
    memset(p, 0, sizeof(*p));
    return 0;
}

/* ------------------------------------------------------------------ */
/* CHAR class operations                                               */
/* ------------------------------------------------------------------ */

static int vrng_class_read(device_t *dev, void *buf, size_t count)
{
    vrng_inst_t *p = dev ? dev->drv_priv : NULL;
    if (!p || !p->valid || !buf || !count)
        return -EINVAL;

    /* Drain first: data that arrived before the last interrupt was handled is
     * still readable, and this is the whole path when request_irq() failed. */
    vrng_pump(p);
    size_t n = vrng_take(p, buf, count);

    if (!n) {
        /* Empty: ask the device for more.  Nothing is posted at probe, so the
         * very first read is what starts the device. */
        uint32_t posted = vrng_post_free(p);
        uint64_t deadline =
            clock_get_ticks() + MS_TO_TICKS(VRNG_READ_TIMEOUT_MS);
        for (;;) {
            proc_yield();
            vrng_pump(p);
            n = vrng_take(p, buf, count);
            if (n)
                break;
            /* Nothing was posted and nothing is still in flight: the device
             * is never going to answer, so stop waiting for it. */
            if (!posted && !vrng_inflight(p))
                break;
            if (clock_get_ticks() >= deadline)
                break;
        }
    }

    if (!n) {
        uint64_t flags = spin_lock_irqsave(&p->lock);
        p->read_timeouts++;
        spin_unlock_irqrestore(&p->lock, flags);
        return -ETIMEDOUT;
    }

    vrng_feed_pool(p, buf, n);
    return (int)n;
}

static int vrng_class_poll(device_t *dev, short events)
{
    vrng_inst_t *p = dev ? dev->drv_priv : NULL;
    (void)events;
    if (!p || !p->valid)
        return 0;
    vrng_pump(p);
    uint64_t flags = spin_lock_irqsave(&p->lock);
    int ready = p->head != p->tail;
    spin_unlock_irqrestore(&p->lock, flags);
    return ready;
}

static const char_dev_ops_t vrng_class_ops = {
    .read = vrng_class_read,
    .poll = vrng_class_poll,
};

static const device_id_t vrng_ids[] = {
    /* virtio-mmio: the device-id register carries the protocol type, there is
     * no PCI vendor to match on. */
    { .vendor = 0, .device = VRNG_DEVICE_ID,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = VRNG_PCI_ID_MODERN,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = VRNG_PCI_ID_TRANS,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t vrng_driver = {
    .name       = "virtio-rng",
    .id_table   = vrng_ids,
    .bus        = NULL,
    .probe      = vrng_probe,
    .remove     = vrng_remove,
    .class_ops  = &vrng_class_ops,
    .class_type = DEV_CLASS_CHAR,
};

DRIVER_REGISTER(vrng_driver);