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

/* Ring depth.  The generic VIRTIO_QUEUE_SIZE in drivers/block/virtio_blk.h
 * stays at 32 because it also sizes virtio_blk/gpu/snd ring structures; the
 * network rings are sized here instead, which is why this driver carries its
 * own avail/used types below rather than the block ones. */
#define VIRTIO_NET_QUEUE_SIZE      256

/* Descriptors consumed per posted receive buffer: one, holding the
 * virtio_net_hdr_mrg_rxbuf (12 B) followed by that buffer's share of the frame
 * data.  No NEXT chain, no trailing context descriptor.
 *
 * That shape is a measured result, not a preference.  Three were built and run
 * against QEMU 10.0 with MRG_RXBUF acknowledged:
 *
 *   [hdr 12B][data 1536B][ctx 4B]  ping 0/4 replies -- the device returns the
 *                                 right used length and num_buffers, and every
 *                                 frame after the first reaches the stack as
 *                                 zeros, so ethernet_input() drops it;
 *   [data 1548B][ctx 4B]           ping 0/4 replies, same symptom;
 *   [data 1548B]                   ping 4/4 replies.
 *
 * So the payload has to share a descriptor with the header, and the context
 * descriptor is left off.  Everything below keeps that shape and changes only
 * how many payload bytes each buffer carries.
 *
 * That payload size is what decides whether MRG_RXBUF does anything at all.
 * QEMU consumes a second receive buffer only when the frame did not fit in the
 * first one: it pops one element per iteration
 * (hw/net/virtio-net.c:1971), copies at most the remaining `size - offset`
 * bytes of the frame into it (hw/net/virtio-net.c:2023-2025), and loops
 * `while (offset < size)` (hw/net/virtio-net.c:1958); the iteration count --
 * the number of elements it popped -- is written back into the header's
 * num_buffers field (hw/net/virtio-net.c:2043).  A
 * 1536 B payload swallows a 1514 B frame whole, so num_buffers was
 * structurally pinned at 1 and the reassembly in virtio_net_recv() -- correct or
 * not -- was unreachable.  Posting small buffers inverts that: the device now
 * has to span the frame, and the loop that reads num_buffers has real work.
 *
 * 512 B is the compromise.  The first element of a frame carries a 12-byte
 * header in front of its payload, so it yields 512 of the 1504 bytes left after
 * the 10-byte host header, while every continuation element starts at
 * guest_offset 0 and yields the whole 524.  A full-MTU frame therefore spans
 * 1 + ceil((1504 - 512) / 524) = 3 buffers, which puts the merge path on
 * ordinary traffic, while a 60 B minimum frame still fits in one, so a ping
 * reply costs a single buffer.  Slots per frame is the other half of the trade,
 * because the device reserves its elements one at a time from the available ring
 * (hw/net/virtio-net.c:1971-1987): a burst of MTU frames needs 3 buffers posted
 * each, and 256 slots is 85 frames of headroom.
 *
 * Descriptors per buffer stays one either way; VIRTIO_NET_RX_DESC_MAX is the
 * size of the RX descriptor table, which is the ring depth when every buffer is
 * posted as its own descriptor. */
#define VIRTIO_NET_RX_DESC_MAX     VIRTIO_NET_QUEUE_SIZE
#define VIRTIO_NET_TX_DESC_MAX     VIRTIO_NET_QUEUE_SIZE

/* Payload bytes one posted receive buffer carries when MRG_RXBUF is
 * acknowledged.  Has to be smaller than a maximum frame or the device never
 * writes num_buffers > 1 and the merge path is dead code again. */
#define VIRTIO_NET_RX_PAYLOAD      512

/* virtio_net_hdr_mrg_rxbuf is 12 bytes; plain virtio_net_hdr is 10.  Every
 * buffer is allocated with the larger header in front so one array serves
 * both, and net->hdr_len says which one this device actually writes. */
#define VIRTIO_NET_HDR_BASE        10
#define VIRTIO_NET_HDR_MRG         12
/* Transmit buffer: header plus room for a whole frame.  TX never merges, so
 * this is fixed; only the receive side is sized from the negotiated features. */
#define VIRTIO_NET_TX_BUF_SIZE     (VIRTIO_NET_HDR_MRG + VIRTIO_NET_FRAME_MAX)
/* Offset of num_buffers inside the header (virtio spec 5.1.6). */
#define VIRTIO_NET_HDR_NUMBUF_OFF  10

#define VIRTIO_NET_MTU             1500
#define VIRTIO_NET_FRAME_MAX       1536

/* Legacy (v1) transports lay the three rings out by hand: descriptor table
 * first, then the available ring and the used ring each on a 4096-byte
 * boundary, because the 0.9.5 spec requires the available ring not to cross a
 * page and the used ring to be page aligned.  Three pages is exactly enough
 * for the 256-descriptor table plus both rings (4096 + 518 + 2054). */
#define VIRTIO_NET_LEGACY_PAGES    3
#define VIRTIO_NET_LEGACY_BYTES    (4096 * VIRTIO_NET_LEGACY_PAGES)

#define VIRTIO_NET_F_CSUM          0
#define VIRTIO_NET_F_GUEST_CSUM    1
#define VIRTIO_NET_F_MAC           5
#define VIRTIO_NET_F_MRG_RXBUF     15
#define VIRTIO_NET_F_STATUS        16
#define VIRTIO_NET_F_MQ            22
#define VIRTIO_NET_TX_TIMEOUT_TICKS (clock_ticks_per_sec() * 2)

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_NET_QUEUE_SIZE];
    uint16_t used_event;
} virtio_net_avail_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTIO_NET_QUEUE_SIZE];
    uint16_t avail_event;
} virtio_net_used_t;

typedef struct {
    /* Every array below lives inside the instance's single kmalloc() block
     * (see virtio_net_ring_bytes) rather than in .bss.  A modern transport
     * addresses desc/avail/used separately; a legacy one addresses the whole
     * ring area by page frame number, so legacy_vq -- when non-NULL -- is the
     * page-aligned base and the three offsets place the parts within it. */
    virtq_desc_t      *desc;
    virtio_net_avail_t *avail;
    virtio_net_used_t  *used;
    uint8_t           *legacy_vq;
    /* Legacy ring placement, computed by virtio_net_setup_queue() from the
     * descriptor count actually in use. */
    uint32_t legacy_desc_off;
    uint32_t legacy_avail_off;
    uint32_t legacy_used_off;
    /* used->ring[].id is an index into the available ring (virtio 1.0 §2.7.8),
     * not a descriptor head.  This maps it back to the buffer slot, which is
     * what the driver owns.  The old code read id as the slot directly, which
     * only worked because every queue kept avail->ring[i] == i. */
    uint16_t *slot_of_avail;
    uint16_t last_used;
} virtio_net_queue_t;

typedef struct {
    virtio_transport_t vt;
    virtio_net_queue_t rxq;
    virtio_net_queue_t txq;
    /* Owns every ring, buffer and side table below, in one piece.  Must be the
     * kmalloc() return value, not a pointer derived from it, because that is
     * what kfree() takes. */
    void   *ring_mem;
    size_t  ring_mem_size;
    /* Flat arrays with a per-instance stride rather than fixed-stride 2D
     * arrays: the receive stride is negotiated (small buffers when MRG_RXBUF
     * is acknowledged, a whole frame when it is not), and it is decided after
     * the feature exchange but before the memory is taken.  Buffer n starts at
     * rx_buf + n * rx_buf_size. */
    uint8_t  *rx_buf;
    unsigned rx_buf_size;
    uint8_t (*tx_buf)[VIRTIO_NET_TX_BUF_SIZE];
    uint8_t  *tx_busy;
    /* Buffers consumed by the frame being reassembled, in available-ring order,
     * so every one of them goes back on the ring.  A mergeable receive buffer
     * lets the device span one frame over however many posted buffers it needs
     * -- at VIRTIO_NET_RX_PAYLOAD a full-MTU frame reaches three -- so this has
     * to be as long as the ring, not as long as one posted buffer.
     *
     * Per instance rather than per call on purpose: virtio_net_recv() holds
     * net->lock for its whole body and never nests, so a scratch array there is
     * exclusive, and it keeps a 512-byte array off the interrupt stack. */
    uint16_t *rx_recycle;
    uint8_t mac[6];
    /* LOCK_ORDER: net->lock is innermost under g_lwip_lock.
     * Local order: g_lwip_lock -> net->lock.
     * Protects TX/RX descriptor rings, tx_busy[], rx_buf[], tx_buf[],
     * slot_of_avail[], last_used, avail->idx, rx_packets,
     * tx_packets, rx_drops, tx_drops, rx_mrg_frames, rx_mrg_reported. */
    spinlock_t lock;
    int valid;
    int legacy;
    int slot;
    int irq;
    int irq_registered;
    /* Ring depth this device can actually host: min(256, QueueNumMax).
     * A device offering fewer than 256 must not fail the probe, so every ring
     * index is taken modulo this rather than the compile-time constant. */
    unsigned qsize;
    /* Bytes of virtio_net_hdr this device writes in front of the frame. */
    unsigned hdr_len;
    int mrg_rxbuf;
    /* Feature bits the device offered but this driver deliberately does not
     * acknowledge.  Kept only so the ready line can report them. */
    int have_mq;
    int have_csum;
    int have_guest_csum;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_drops;
    uint32_t tx_drops;
    /* Frames the device spread over more than one posted buffer, and whether
     * that has been reported once.  Purely observational: MRG_RXBUF is only
     * real if this counter ever moves, so the first occurrence is logged
     * rather than left for someone to infer from a working ping. */
    uint32_t rx_mrg_frames;
    int rx_mrg_reported;
} virtio_net_inst_t;

static virtio_net_inst_t g_net[VIRTIO_NET_MAX_DEVS];
static int g_nnet;

static void virtio_net_select_queue(virtio_net_inst_t *net, int qidx) {
    net->vt.write32(&net->vt, VIRTIO_MMIO_QUEUE_SEL, (uint32_t)qidx);
}

/* Page-aligning helper for the legacy layout; see VIRTIO_NET_LEGACY_PAGES. */
static uint32_t virtio_net_page_align(uint32_t off)
{
    return (off + 4095u) & ~4095u;
}

/*
 * Why the rings are kmalloc()ed instead of declared as static arrays.
 *
 * Under the default DRIVER_DEPLOYMENT=generic profile this driver is not
 * linked into the kernel image: tools/driver-modules.mk packages it as
 * virtio-net.a20drv and drvmod_load() rejects any package whose .text + .data
 * + .bss exceeds DRV_MOD_MAX_SIZE (kernel/include/drvmod/drvmod.h:31,
 * 512 KiB, enforced in kernel/drvmod/loader.c on "bad total_size").  256 slots
 * of header plus payload, twice, is about 800 KiB of .bss on its own -- the
 * statically sized version produced a .bss of 0x1a60e8 bytes and the probe
 * never ran, because the loader refused the module with -ENOEXEC before any
 * code in it was reached.
 *
 * Taking the memory at probe time keeps the package small and sizes the rings
 * to the device.  Two properties make it safe to hand these addresses to the
 * device:
 *
 *   - va_to_pa() (kernel/include/mm/mm.h:26) is a flat subtraction of
 *     PAGE_OFFSET, so any kernel virtual address converts, and
 *   - sizes above SLAB_MAX_OBJ (kernel/mm/slab.c:12) go straight to the buddy
 *     allocator and are returned as whole pages, which is what the legacy
 *     PFN-addressed ring layout needs.
 *
 * The size is asked for after the feature exchange, so the receive stride
 * (net->rx_buf_size) is already known; nothing here touches the device.
 */
static size_t virtio_net_ring_bytes(const virtio_net_inst_t *net)
{
    size_t n = (size_t)VIRTIO_NET_QUEUE_SIZE * net->rx_buf_size +
               (size_t)VIRTIO_NET_QUEUE_SIZE * VIRTIO_NET_TX_BUF_SIZE;
    /* Both ring descriptor tables are sized for VIRTIO_NET_RX_DESC_MAX. */
    n += (size_t)VIRTIO_NET_RX_DESC_MAX * sizeof(virtq_desc_t);
    n += (size_t)VIRTIO_NET_TX_DESC_MAX * sizeof(virtq_desc_t);
    n += 2 * sizeof(virtio_net_avail_t);
    n += 2 * sizeof(virtio_net_used_t);
    n += 2 * (size_t)VIRTIO_NET_QUEUE_SIZE * sizeof(uint16_t);  /* slot_of_avail */
    n += (size_t)VIRTIO_NET_QUEUE_SIZE * sizeof(uint16_t);      /* rx_recycle */
    n += (size_t)VIRTIO_NET_QUEUE_SIZE;                         /* tx_busy */
    /* Alignment slack: every region below is 64-byte aligned so the payload
     * buffers keep the cache-line granularity arch_dma_sync_for_device()
     * expects, and so the descriptor tables satisfy their 16-byte rule.  Twelve
     * regions, less than one page of headroom. */
    n += 64 * 12;
    if (net->legacy)
        n += 2 * ((size_t)VIRTIO_NET_LEGACY_BYTES + 4096);
    return n;
}

#define VIRTIO_NET_ALIGN(p, a) \
    ((p) = (void *)(((uintptr_t)(p) + ((a) - 1)) & ~(uintptr_t)((a) - 1)))

static void virtio_net_free_ring(virtio_net_inst_t *net)
{
    /* Only ring_mem is passed to kfree(); everything else points inside it and
     * must be cleared rather than freed. */
    if (net->ring_mem)
        kfree(net->ring_mem);
    net->ring_mem = NULL;
    net->ring_mem_size = 0;
    net->rx_buf = NULL;
    net->rx_buf_size = 0;
    net->tx_buf = NULL;
    net->tx_busy = NULL;
    net->rx_recycle = NULL;
    memset(&net->rxq, 0, sizeof(net->rxq));
    memset(&net->txq, 0, sizeof(net->txq));
}

static int virtio_net_alloc_ring(virtio_net_inst_t *net)
{
    size_t size = virtio_net_ring_bytes(net);
    void *raw = kmalloc(size);
    if (!raw) {
        kinfo("[VIRTIO-NET%d] no memory for %u-slot rings (%zu bytes)\n",
              net->slot, VIRTIO_NET_QUEUE_SIZE, size);
        return -1;
    }
    memset(raw, 0, size);
    net->ring_mem = raw;
    net->ring_mem_size = size;

    void *p = raw;

    if (net->legacy) {
        /* virtio 0.9.5 addresses this ring area by PFN, so its base has to be
         * page aligned.  The returned pointer is not (the slab allocator puts
         * a header in front of objects), hence the reserved page of slack. */
        VIRTIO_NET_ALIGN(p, 4096);
        net->rxq.legacy_vq = p;
        p += VIRTIO_NET_LEGACY_BYTES;
        net->txq.legacy_vq = p;
        p += VIRTIO_NET_LEGACY_BYTES;
    }

    VIRTIO_NET_ALIGN(p, 64);
    net->rxq.desc = p;
    p += (size_t)VIRTIO_NET_RX_DESC_MAX * sizeof(virtq_desc_t);
    net->txq.desc = p;
    p += (size_t)VIRTIO_NET_TX_DESC_MAX * sizeof(virtq_desc_t);

    VIRTIO_NET_ALIGN(p, 2);
    net->rxq.avail = p;
    p += sizeof(virtio_net_avail_t);
    net->txq.avail = p;
    p += sizeof(virtio_net_avail_t);

    VIRTIO_NET_ALIGN(p, 4);
    net->rxq.used = p;
    p += sizeof(virtio_net_used_t);
    net->txq.used = p;
    p += sizeof(virtio_net_used_t);

    VIRTIO_NET_ALIGN(p, 2);
    net->rxq.slot_of_avail = p;
    p += (size_t)VIRTIO_NET_QUEUE_SIZE * sizeof(uint16_t);
    net->txq.slot_of_avail = p;
    p += (size_t)VIRTIO_NET_QUEUE_SIZE * sizeof(uint16_t);

    VIRTIO_NET_ALIGN(p, 2);
    net->rx_recycle = p;
    p += (size_t)VIRTIO_NET_QUEUE_SIZE * sizeof(uint16_t);

    net->tx_busy = p;
    p += (size_t)VIRTIO_NET_QUEUE_SIZE;

    VIRTIO_NET_ALIGN(p, 64);
    net->rx_buf = p;
    p += (size_t)VIRTIO_NET_QUEUE_SIZE * net->rx_buf_size;
    net->tx_buf = p;

    return 0;
}

static int virtio_net_setup_queue(virtio_net_inst_t *net, virtio_net_queue_t *q,
                                  int qidx, unsigned ndesc)
{
    virtio_transport_t *vt = &net->vt;

    virtio_net_select_queue(net, qidx);
    uint32_t qmax = vt->read32(vt, VIRTIO_MMIO_QUEUE_NUM_MAX);
    /* Take the device's own maximum rather than refusing the probe: a NIC that
     * offers 64 slots is still a NIC, and the ring arrays are statically sized
     * for the 256-slot case, so a smaller qsize simply leaves the tail unused. */
    if (qmax == 0) {
        printf("[VIRTIO-NET%d] queue %d max is zero\n", net->slot, qidx);
        return -1;
    }
    unsigned want = VIRTIO_NET_QUEUE_SIZE;
    if (qmax < want) {
        /* Largest power of two not exceeding qmax.  Spelled as a loop rather
         * than __builtin_clz: a .a20drv package is linked without libgcc, and
         * the 64-bit lowering of __builtin_clz becomes a call to __clzdi2,
         * which the module loader reports as an unresolved symbol
         * ("[DRVMOD] unresolved symbol '__clzdi2'") and refuses the module. */
        want = 1;
        while ((want << 1) != 0 && (want << 1) <= qmax)
            want <<= 1;
    }
    if (want == 0 || want > VIRTIO_NET_QUEUE_SIZE) {
        printf("[VIRTIO-NET%d] queue %d max unusable: %u\n", net->slot, qidx, qmax);
        return -1;
    }
    if (ndesc > VIRTIO_NET_RX_DESC_MAX) {
        printf("[VIRTIO-NET%d] queue %d wants %u descriptors, table holds %u\n",
               net->slot, qidx, ndesc, VIRTIO_NET_RX_DESC_MAX);
        return -1;
    }
    net->qsize = want;
    vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, want);

    /* virtio_net_alloc_ring() already zeroed the whole block; re-zeroing the
     * queue's own state here is enough -- the descriptor and ring arrays start
     * empty because the device has not seen them yet. */
    q->last_used = 0;
    q->legacy_desc_off = q->legacy_avail_off = q->legacy_used_off = 0;

    if (net->legacy) {
        q->legacy_desc_off = 0;
        q->legacy_avail_off = virtio_net_page_align(ndesc * sizeof(virtq_desc_t));
        q->legacy_used_off  = virtio_net_page_align(q->legacy_avail_off +
                                                    (uint32_t)sizeof(virtio_net_avail_t));
        if (q->legacy_used_off + (uint32_t)sizeof(virtio_net_used_t) >
            (uint32_t)VIRTIO_NET_LEGACY_BYTES) {
            printf("[VIRTIO-NET%d] queue %d legacy layout %u+%u does not fit\n",
                   net->slot, qidx, q->legacy_used_off,
                   (unsigned)sizeof(virtio_net_used_t));
            return -1;
        }

        uint64_t vq_pa = va_to_pa(q->legacy_vq);
        vt->write32(vt, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
        mb();
        vt->write32(vt, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(vq_pa / 4096));
        mb();
    } else {
        uint64_t desc_pa = va_to_pa(q->desc);
        uint64_t avail_pa = va_to_pa(q->avail);
        uint64_t used_pa = va_to_pa(q->used);

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
        return (virtq_desc_t *)(uintptr_t)(q->legacy_vq + q->legacy_desc_off);
    return q->desc;
}

static virtio_net_avail_t *queue_avail(virtio_net_inst_t *net, virtio_net_queue_t *q) {
    if (net->legacy)
        return (virtio_net_avail_t *)(uintptr_t)(q->legacy_vq + q->legacy_avail_off);
    return q->avail;
}

static virtio_net_used_t *queue_used(virtio_net_inst_t *net, virtio_net_queue_t *q) {
    if (net->legacy)
        return (virtio_net_used_t *)(uintptr_t)(q->legacy_vq + q->legacy_used_off);
    return q->used;
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

/*
 * Publish one device-writable receive buffer.
 *
 * One descriptor, holding the virtio_net_hdr_mrg_rxbuf and this buffer's slice
 * of the frame data together, and no NEXT.  The two split layouts that were
 * measured against QEMU 10.0 -- [hdr 12B][data 1536B][ctx 4B] and
 * [data 1548B][ctx 4B] -- both hand the stack zeroed frames even though the
 * device reports the right used length, so the payload has to share a
 * descriptor with the header; see the note on VIRTIO_NET_RX_DESC_MAX above.
 *
 * The descriptor length is net->rx_buf_size, which is small on purpose when
 * MRG_RXBUF is acknowledged: every buffer posted here is one element the device
 * may consume, so it is also the granularity at which it decides a frame spans
 * more than one of them.
 */
static void virtio_net_submit_rx_locked(virtio_net_inst_t *net, unsigned slot) {
    virtio_net_queue_t *q = &net->rxq;
    virtq_desc_t *desc = queue_desc(net, q);
    virtio_net_avail_t *avail = queue_avail(net, q);
    uint8_t *buf = net->rx_buf + (size_t)slot * net->rx_buf_size;

    memset(buf, 0, VIRTIO_NET_HDR_MRG);

    desc[slot].addr = va_to_pa(buf);
    desc[slot].len = net->rx_buf_size;
    desc[slot].flags = VIRTQ_DESC_F_WRITE;
    desc[slot].next = 0;

    /* Publish first, then clean.  See the DMA publication contract in
     * kernel/include/drivers/dual/virtq.h: arch_dma_sync_for_device() cleans a
     * cache line rather than ordering the store stream, so cleaning after the
     * store gives the device the same view as virtio_blk's clean-before.
     * virtio_net.c and virtio_blk.c differ in placement on purpose. */
    uint16_t avail_slot = avail->idx % net->qsize;
    q->slot_of_avail[avail_slot] = (uint16_t)slot;
    avail->ring[avail_slot] = (uint16_t)slot;
    wmb();
    avail->idx++;

    arch_dma_sync_for_device(buf, net->rx_buf_size);
    arch_dma_sync_for_device(&desc[slot], sizeof(virtq_desc_t));
    arch_dma_sync_for_device(&avail->ring[avail_slot], sizeof(uint16_t));
    arch_dma_sync_for_device(&avail->idx, sizeof(uint16_t));
}

static void virtio_net_seed_rx_locked(virtio_net_inst_t *net) {
    for (unsigned i = 0; i < net->qsize; i++)
        virtio_net_submit_rx_locked(net, i);
}

static void virtio_net_complete_tx_locked(virtio_net_inst_t *net) {
    virtio_net_queue_t *q = &net->txq;
    virtio_net_used_t *used = queue_used(net, q);

    arch_dma_sync_for_cpu(&used->idx, sizeof(uint16_t));
    uint16_t used_idx = ((volatile virtio_net_used_t *)used)->idx;
    while (q->last_used != used_idx) {
        uint16_t ring_idx = q->last_used % net->qsize;
        arch_dma_sync_for_cpu(&used->ring[ring_idx], sizeof(virtq_used_elem_t));
        uint16_t avail_idx = (uint16_t)used->ring[ring_idx].id;
        uint16_t slot = q->slot_of_avail[avail_idx % net->qsize];
        if (slot < VIRTIO_NET_QUEUE_SIZE)
            net->tx_busy[slot] = 0;
        q->last_used++;
    }
}

static int virtio_net_tx_free_locked(virtio_net_inst_t *net) {
    virtio_net_complete_tx_locked(net);
    for (unsigned i = 0; i < net->qsize; i++) {
        if (!net->tx_busy[i])
            return (int)i;
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
    /*
     * MRG_RXBUF is acknowledged only on a modern (version 1) transport.
     *
     * It is offered on legacy devices too, but the two differ in a way that
     * decides the whole receive path: with VERSION_1 the spec fixes the guest
     * header at virtio_net_hdr_mrg_rxbuf (12 bytes) regardless of the feature,
     * while without it the header is virtio_net_hdr (10 bytes) unless MRG_RXBUF
     * is acknowledged.  This driver has one buffer layout and one header size
     * per instance, and net->hdr_len is computed from the negotiated set, so
     * the legacy path keeps the layout that provably fits its three-page ring
     * area and costs only a receive buffer of 1536 rather than 1548 bytes.
     *
     * Acknowledging the feature does not buy a frame spread over several posted
     * buffers: virtio_net_submit_rx_locked() posts one descriptor per buffer
     * because that is the only shape QEMU 10.0 delivers (see
     * VIRTIO_NET_RX_DESC_MAX).  It does buy the 12-byte header and the
     * num_buffers field, both of which virtio_net_recv() reads, and it is what
     * makes net->hdr_len come out at 12 rather than 10.
     */
    if (!net->legacy && (features_lo & (1U << VIRTIO_NET_F_MRG_RXBUF)))
        driver_lo |= (1U << VIRTIO_NET_F_MRG_RXBUF);
    /*
     * VIRTIO_NET_F_CSUM / VIRTIO_NET_F_GUEST_CSUM are deliberately NOT
     * acknowledged.  Acknowledging either means the device stops computing the
     * L4 checksum and expects the partial sum in the vnet header instead, and
     * lwIP 2.2.2 as vendored cannot be told that is the case: LWIP_CHECKSUM_ON_COPY
     * defaults to 0 (opt.h:2449-2450) and netif.h:84-107 defines no flag that
     * carries the handshake.  Accepting the feature anyway would leave lwIP
     * verifying a checksum the device never produced -- a silent corruption of
     * every packet, not a missing feature.  The offer is recorded and printed
     * so the gap is visible rather than forgotten.
     */
    net->have_csum       = (features_lo & (1U << VIRTIO_NET_F_CSUM)) != 0;
    net->have_guest_csum = (features_lo & (1U << VIRTIO_NET_F_GUEST_CSUM)) != 0;
    /* VIRTIO_NET_F_MQ is detected, not negotiated: the multiqueue path is not
     * implemented, and acknowledging a control-queue layout this driver never
     * builds would leave the device posting into queues that do not exist. */
    net->have_mq = (features_lo & (1U << VIRTIO_NET_F_MQ)) != 0;

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

    net->mrg_rxbuf = (driver_lo & (1U << VIRTIO_NET_F_MRG_RXBUF)) != 0;
    /* VERSION_1 fixes a 12-byte header; a legacy device writes 10 unless
     * MRG_RXBUF was acknowledged, and this driver never acknowledges it there. */
    net->hdr_len = (!net->legacy || net->mrg_rxbuf) ? VIRTIO_NET_HDR_MRG
                                                     : VIRTIO_NET_HDR_BASE;
    /* How much payload each posted receive buffer carries.
     *
     * With MRG_RXBUF the device may span a frame over consecutive buffers, so
     * the payload is cut down to VIRTIO_NET_RX_PAYLOAD and the merge path in
     * virtio_net_recv() runs on every full-MTU frame.  Without it the feature
     * is unavailable to a driver that did not acknowledge it, and the device
     * drops a frame that does not fit one buffer
     * (hw/net/virtio-net.c:2030-2036), so those buffers have to hold a whole
     * frame -- there is no second buffer to spill into. */
    net->rx_buf_size = net->hdr_len +
        (net->mrg_rxbuf ? VIRTIO_NET_RX_PAYLOAD : VIRTIO_NET_FRAME_MAX);

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
        net->mac[3] = 0xa3;
        net->mac[4] = 0x00;
        net->mac[5] = (uint8_t)idx;
    }

    /* One posted buffer per descriptor, mergeable or not: with MRG_RXBUF the
     * device spreads a frame over several buffers and hands one used-ring entry
     * back per consumed buffer, which is the case the recycle list exists for.
     * Without it the count is the ring depth and no frame ever spans two. */
    unsigned rx_desc = net->mrg_rxbuf ? VIRTIO_NET_RX_DESC_MAX : VIRTIO_NET_QUEUE_SIZE;

    /* Allocated before either queue is programmed, and freed on every path out
     * of this function below: a probe that fails after the rings exist must not
     * leak the ring block, and the instance is memset() by its next probe
     * attempt. */
    if (virtio_net_alloc_ring(net) < 0)
        return -1;

    if (virtio_net_setup_queue(net, &net->rxq, VIRTIO_NET_QUEUE_RX, rx_desc) < 0) {
        virtio_net_free_ring(net);
        return -1;
    }
    if (virtio_net_setup_queue(net, &net->txq, VIRTIO_NET_QUEUE_TX,
                               VIRTIO_NET_TX_DESC_MAX) < 0) {
        virtio_net_free_ring(net);
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&net->lock);
    virtio_net_seed_rx_locked(net);
    spin_unlock_irqrestore(&net->lock, flags);

    status |= VIRTIO_STATUS_DRIVER_OK;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    virtio_net_kick(net, VIRTIO_NET_QUEUE_RX);

    net->valid = 1;
    printf("[VIRTIO-NET%d] ready legacy=%d mac=%02x:%02x:%02x:%02x:%02x:%02x "
           "qsize=%u hdr=%u mrg_rxbuf=%d rxbuf=%u offered(mq=%d csum=%d guest_csum=%d)\n",
           idx, net->legacy, net->mac[0], net->mac[1], net->mac[2],
           net->mac[3], net->mac[4], net->mac[5], net->qsize, net->hdr_len,
           net->mrg_rxbuf, net->rx_buf_size,
           net->have_mq, net->have_csum, net->have_guest_csum);
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

/* Shared submit body for the linear and scatter-gather entry points. */
static int virtio_net_send_iov(virtio_net_inst_t *net, const uint8_t *const *segs,
                               const size_t *lens, unsigned nr, size_t total,
                               int nonblock)
{
    uint64_t deadline = timer_get_ticks() + VIRTIO_NET_TX_TIMEOUT_TICKS;
    int slot = -1;

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
    memset(buf, 0, net->hdr_len);
    size_t off = net->hdr_len;
    for (unsigned i = 0; i < nr; i++) {
        memcpy(buf + off, segs[i], lens[i]);
        off += lens[i];
    }

    uint64_t flags = spin_lock_irqsave(&net->lock);
    virtio_net_queue_t *q = &net->txq;
    virtq_desc_t *desc = queue_desc(net, q);
    virtio_net_avail_t *avail = queue_avail(net, q);

    desc[slot].addr = va_to_pa(buf);
    desc[slot].len = (uint32_t)off;
    desc[slot].flags = 0;
    desc[slot].next = 0;

    uint16_t avail_slot = avail->idx % net->qsize;
    q->slot_of_avail[avail_slot] = (uint16_t)slot;
    avail->ring[avail_slot] = (uint16_t)slot;
    wmb();
    avail->idx++;

    arch_dma_sync_for_device(buf, off);
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
        return (int)total;
    }

    for (;;) {
        flags = spin_lock_irqsave(&net->lock);
        virtio_net_complete_tx_locked(net);
        int done = !net->tx_busy[slot];
        spin_unlock_irqrestore(&net->lock, flags);
        if (done) {
            net->tx_packets++;
            return (int)total;
        }
        if (timer_get_ticks() >= deadline) {
            flags = spin_lock_irqsave(&net->lock);
            if (slot >= 0 && slot < VIRTIO_NET_QUEUE_SIZE)
                net->tx_busy[slot] = 0;
            spin_unlock_irqrestore(&net->lock, flags);
            net->tx_drops++;
            return -1;
        }
        virtio_net_wait_for_tx_progress();
    }
}

/* lwIP linkoutput calls this to send a frame while holding g_lwip_lock.
 * Blocking here (waiting for a TX slot, or for send completion) deadlocks:
 *   g_lwip_lock -> virtio_net_send (blocks) -> IRQ -> lwIP cb -> needs g_lwip_lock
 * Fix: submit the descriptor and return without waiting.
 * The non-nonblock path keeps busy-waiting until done, for unlocked callers. */
int virtio_net_send(int idx, const void *packet, size_t len, int nonblock) {
    if (!packet || len == 0 || len > VIRTIO_NET_FRAME_MAX)
        return -1;
    if (!virtio_net_ready(idx))
        return -1;

    virtio_net_inst_t *net = &g_net[idx];
    const uint8_t *segs[1];
    size_t lens[1];
    segs[0] = (const uint8_t *)packet;
    lens[0] = len;
    return virtio_net_send_iov(net, segs, lens, 1, len, nonblock);
}

int virtio_net_recv(int idx, void *packet, size_t maxlen) {
    if (!packet || maxlen == 0)
        return -1;
    if (!virtio_net_ready(idx))
        return -1;

    virtio_net_inst_t *net = &g_net[idx];
    uint64_t flags = spin_lock_irqsave(&net->lock);
    virtio_net_queue_t *q = &net->rxq;
    virtio_net_used_t *used = queue_used(net, q);

    arch_dma_sync_for_cpu(&used->idx, sizeof(uint16_t));
    uint16_t used_idx = ((volatile virtio_net_used_t *)used)->idx;
    if (q->last_used == used_idx) {
        spin_unlock_irqrestore(&net->lock, flags);
        return 0;
    }

    unsigned hdr_len = net->hdr_len;
    /* Used entries the device has published but this call has not consumed. */
    unsigned pending = (unsigned)(uint16_t)(used_idx - q->last_used);
    uint16_t ring_idx = q->last_used % net->qsize;
    arch_dma_sync_for_cpu(&used->ring[ring_idx], sizeof(virtq_used_elem_t));
    uint16_t avail_idx = (uint16_t)used->ring[ring_idx].id;
    uint32_t used_len = used->ring[ring_idx].len;
    unsigned slot = q->slot_of_avail[avail_idx % net->qsize];
    q->last_used++;

    if (slot >= net->qsize) {
        net->rx_drops++;
        spin_unlock_irqrestore(&net->lock, flags);
        return 0;
    }

    /*
     * num_buffers says how many posted buffers this one frame consumed.  The
     * device writes it into the header of the FIRST buffer of the chain
     * (virtio 1.1 5.1.6) at offset 10, and each consumed buffer gets its own
     * used-ring entry carrying only the bytes written into it.  Without
     * MRG_RXBUF there is no such field and a frame is exactly one buffer.
     *
     * It is clamped to the entries actually published.  A device claiming more
     * buffers than it wrote used entries for is describing a frame this driver
     * cannot finish; consuming only what exists keeps the used ring and the
     * posted buffers in step, so the cost is one dropped frame rather than a
     * permanently desynchronised ring.
     */
    unsigned nbuf = 1;
    if (net->mrg_rxbuf) {
        uint8_t *head = net->rx_buf + (size_t)slot * net->rx_buf_size;
        arch_dma_sync_for_cpu(head, VIRTIO_NET_HDR_MRG);
        nbuf = (unsigned)head[VIRTIO_NET_HDR_NUMBUF_OFF] |
               ((unsigned)head[VIRTIO_NET_HDR_NUMBUF_OFF + 1] << 8);
        if (nbuf == 0)
            nbuf = 1;
        if (nbuf > pending)
            nbuf = pending;
        if (nbuf > net->qsize)
            nbuf = net->qsize;
        if (nbuf > 1) {
            /* The one line that makes MRG_RXBUF observable.  A device that
             * never spans buffers leaves this at zero, which is a fact about
             * the posting above rather than about the stack. */
            net->rx_mrg_frames++;
            if (!net->rx_mrg_reported) {
                net->rx_mrg_reported = 1;
                kinfo("[VIRTIO-NET%d] first frame reassembled from %u buffers "
                      "(rxbuf=%u)\n", net->slot, nbuf, net->rx_buf_size);
            }
        }
    }
    int truncated = 0;
    size_t copied = 0;

    for (unsigned k = 0; k < nbuf; k++) {
        unsigned buf_slot;
        uint32_t buf_len;

        if (k == 0) {
            buf_slot = slot;
            buf_len = used_len;
        } else {
            ring_idx = q->last_used % net->qsize;
            arch_dma_sync_for_cpu(&used->ring[ring_idx], sizeof(virtq_used_elem_t));
            avail_idx = (uint16_t)used->ring[ring_idx].id;
            buf_len = used->ring[ring_idx].len;
            q->last_used++;
            buf_slot = q->slot_of_avail[avail_idx % net->qsize];
            if (buf_slot >= net->qsize) {
                /* Nothing sane left to do with this entry: keep the buffers
                 * accounted for so far and drop the frame. */
                truncated = 1;
                nbuf = k;
                break;
            }
        }
        net->rx_recycle[k] = (uint16_t)buf_slot;

        /* Only the FIRST buffer of a merged frame carries a virtio_net_hdr.
         * The device writes one header in front of the frame and then raw
         * payload into every continuation buffer, and its used length counts
         * exactly what it wrote: `total` starts at 0 for each element and only
         * the i == 0 iteration adds guest_hdr_len
         * (hw/net/virtio-net.c:1996-2025: `if (i == 0)` at :1996,
         * `total += n->guest_hdr_len` at :2016, the payload copy at :2023).
         * Subtracting a header from a
         * continuation buffer's length -- as this did before, when no device
         * ever produced a continuation -- would drop the first 12 payload
         * bytes of every merged frame and hand a checksum-invalid frame to
         * the stack. */
        size_t skip = (k == 0) ? hdr_len : 0;
        if (buf_len <= skip)
            continue;
        size_t n = buf_len - skip;
        if (n > (size_t)VIRTIO_NET_FRAME_MAX)
            n = VIRTIO_NET_FRAME_MAX;
        uint8_t *base = net->rx_buf + (size_t)buf_slot * net->rx_buf_size;
        arch_dma_sync_for_cpu(base, skip + n);
        if (copied < maxlen) {
            size_t room = maxlen - copied;
            size_t take = n < room ? n : room;
            memcpy((uint8_t *)packet + copied, base + skip, take);
            copied += take;
        } else {
            truncated = 1;
        }
    }
    /* Whatever was taken off the used ring goes straight back, including a
     * frame that turned out to be unusable: a buffer that is not re-posted is
     * a permanently lost receive slot -- and with small buffers one lost slot
     * is one third of a frame's capacity, so the ring would drain rather than
     * degrade.  All nbuf of them, in the order the device consumed them. */
    for (unsigned k = 0; k < nbuf; k++)
        virtio_net_submit_rx_locked(net, net->rx_recycle[k]);
    virtio_net_kick(net, VIRTIO_NET_QUEUE_RX);


    int ret = 0;
    if (truncated || copied == 0) {
        net->rx_drops++;
    } else {
        net->rx_packets++;
        ret = (int)copied;
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

    uint64_t flags = a20_lwip_ingress_lock();
    a20_lwip_poll_rx_locked(budget);
    a20_lwip_ingress_unlock(flags);
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
 *   and returns.  It never calls kmalloc or acquires a socket-table bucket
 *   lock.
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

    uint64_t flags = a20_lwip_ingress_lock();
    a20_lwip_process_netif_irq_locked(net->slot);
    a20_lwip_ingress_unlock(flags);
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

static uint32_t virtio_net_class_caps(struct device *dev) {
    virtio_net_inst_t *net = (virtio_net_inst_t *)dev->drv_priv;
    uint32_t caps = 0;
    /* Only bits whose driver-side path exists are reported.  MRG_RXBUF is
     * implemented; the two checksum bits are not, because lwIP cannot be told
     * a checksum was offloaded (see virtio_net_init_instance()). */
    if (net && net->mrg_rxbuf)
        caps |= NET_DEV_CAP_MRG_RXBUF;
    return caps;
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
    .caps = virtio_net_class_caps,
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
    /* After the device is stopped and the IRQ freed: the rings are what it was
     * writing into, and virtio_net_recv() must not reach them once they are
     * gone. */
    virtio_net_free_ring(net);
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
