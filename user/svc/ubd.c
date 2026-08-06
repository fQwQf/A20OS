/*
 * ubd — user-space virtio-blk driver (docs/hybrid-kernel/02-mainstream-plan.md
 * M4, mainstream hybrid form).
 *
 * Owns the reserved second virtio-mmio block device entirely from user
 * space.  The kernel block proxy (kernel/drivers/block/udisk.c) forwards
 * block requests over a one-page shared ring; each entry carries the
 * kernel buffer's physical address, and this driver performs the virtio
 * DMA directly into it — zero copy, no data byte crosses the ring or a
 * channel.  Completion is signalled back with device_block_complete.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "../svc/ubd_proto.h"

#define IRQ_TAG 0x55424449ULL /* "UBDI" */
#define CH_TAG  0x55424443ULL /* "UBDC" */

/* virtio-mmio 1.0 registers (offsets) */
#define VMMIO_MAGIC       0x000
#define VMMIO_VERSION     0x004
#define VMMIO_DEVID       0x008
#define VMMIO_DEVFEAT     0x010
#define VMMIO_DEVFEATSEL  0x014
#define VMMIO_DRVFEAT     0x020
#define VMMIO_DRVFEATSEL  0x024
#define VMMIO_QUEUESEL    0x030
#define VMMIO_QUEUENUMMAX 0x034
#define VMMIO_QUEUENUM    0x038
#define VMMIO_QUEUEREADY  0x044
#define VMMIO_QUEUENOTIFY 0x050
#define VMMIO_INTSTATUS   0x060
#define VMMIO_INTACK      0x064
#define VMMIO_STATUS      0x070
#define VMMIO_QUEUEDESCLO 0x080
#define VMMIO_QUEUEDESCHI 0x084
#define VMMIO_QUEUEAVAILLO 0x090
#define VMMIO_QUEUEAVAILHI 0x094
#define VMMIO_QUEUEUSEDLO 0x0A0
#define VMMIO_QUEUEUSEDHI 0x0A4
#define VMMIO_CFGCAPACITY 0x100

#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER      2u
#define VIRTIO_STATUS_DRIVER_OK   4u
#define VIRTIO_STATUS_FEATURES_OK 8u

#define VIRTQ_DESC_F_NEXT  1u
#define VIRTQ_DESC_F_WRITE 2u

#define VIRTIO_BLK_T_IN   0u
#define VIRTIO_BLK_T_OUT  1u

/* Shared ring protocol (must match kernel/drivers/block/udisk.c) */
#define UDISK_RING_REQS 16

typedef struct udisk_req {
    uint32_t      id;
    uint32_t      dir;          /* 0 = read, 1 = write */
    uint64_t      sector;
    uint32_t      sectors;
    _Atomic uint32_t done;
    _Atomic uint32_t result;
    uint64_t      data_pa;
} udisk_req_t;

typedef struct udisk_ring {
    _Atomic uint32_t head;
    uint32_t        cap;
    udisk_req_t     reqs[UDISK_RING_REQS];
} udisk_ring_t;

static volatile uint32_t *g_mmio;
static udisk_ring_t      *g_ring;
static uint32_t           g_last_done; /* highest id completed */

static inline uint32_t mmio_rd(uint32_t off) { return g_mmio[off / 4]; }
static inline void mmio_wr(uint32_t off, uint32_t v) { g_mmio[off / 4] = v; }

typedef struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[UBD_QUEUE_SIZE];
} virtq_avail_t;

typedef struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[UBD_QUEUE_SIZE];
} virtq_used_t;

typedef struct blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} blk_req_hdr_t;

static uint64_t g_hdr_va, g_hdr_pa;
static uint64_t g_status_va, g_status_pa;
static uint64_t g_desc_va, g_desc_pa;
static uint64_t g_avail_va, g_avail_pa;
static uint64_t g_used_va, g_used_pa;
static virtq_desc_t *g_desc;
static virtq_avail_t *g_avail;
static virtq_used_t  *g_used;
static volatile uint8_t *g_status;
static a20_handle_t g_out = A20_HANDLE_NULL;

static int alloc_page(uint64_t *out_va, uint64_t *out_pa)
{
    uint64_t h = (uint64_t)a20_vm_create_object(4096, 0);
    if ((int32_t)h < 0)
        return 0;
    a20_handle_t vmo = (a20_handle_t)h;
    uint64_t va = 0;
    if (a20_vm_map(vmo, 4096, 0, 1 | 2, &va) != A20_OK)
        return 0;
    for (uint32_t i = 0; i < 4096; i += 64)
        ((volatile uint8_t *)(uintptr_t)va)[i] = 0;
    uint64_t pa = 0;
    uint32_t cnt = 0;
    if (a20_device_vmo_phys(vmo, &pa, 1, &cnt) != A20_OK || cnt != 1 || !pa)
        return 0;
    *out_va = va;
    *out_pa = pa;
    return 1;
}

static int virtio_init(void)
{
    if (mmio_rd(VMMIO_MAGIC) != 0x74726976u)
        return -1;
    if (mmio_rd(VMMIO_VERSION) != 2u)
        return -1;
    if (mmio_rd(VMMIO_DEVID) != 2u)
        return -1;

    mmio_wr(VMMIO_STATUS, 0);
    mmio_wr(VMMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    mmio_wr(VMMIO_DRVFEATSEL, 0);
    mmio_wr(VMMIO_DRVFEAT, 0);
    mmio_wr(VMMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                          VIRTIO_STATUS_FEATURES_OK);

    mmio_wr(VMMIO_QUEUESEL, 0);
    uint32_t maxq = mmio_rd(VMMIO_QUEUENUMMAX);
    uint32_t qsz = UBD_QUEUE_SIZE < maxq ? UBD_QUEUE_SIZE : maxq;
    if (qsz == 0)
        return -1;
    mmio_wr(VMMIO_QUEUENUM, qsz);

    if (!alloc_page(&g_desc_va, &g_desc_pa)) return -1;
    if (!alloc_page(&g_avail_va, &g_avail_pa)) return -1;
    if (!alloc_page(&g_used_va, &g_used_pa)) return -1;
    if (!alloc_page(&g_hdr_va, &g_hdr_pa)) return -1;
    if (!alloc_page(&g_status_va, &g_status_pa)) return -1;
    g_desc = (virtq_desc_t *)g_desc_va;
    g_avail = (virtq_avail_t *)g_avail_va;
    g_used = (virtq_used_t *)g_used_va;
    g_status = (volatile uint8_t *)g_status_va;

    mmio_wr(VMMIO_QUEUEDESCLO, (uint32_t)g_desc_pa);
    mmio_wr(VMMIO_QUEUEDESCHI, (uint32_t)(g_desc_pa >> 32));
    mmio_wr(VMMIO_QUEUEAVAILLO, (uint32_t)g_avail_pa);
    mmio_wr(VMMIO_QUEUEAVAILHI, (uint32_t)(g_avail_pa >> 32));
    mmio_wr(VMMIO_QUEUEUSEDLO, (uint32_t)g_used_pa);
    mmio_wr(VMMIO_QUEUEUSEDHI, (uint32_t)(g_used_pa >> 32));
    mmio_wr(VMMIO_QUEUEREADY, 1);
    mmio_wr(VMMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                          VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    return 0;
}

/* DMA directly into/from @data_pa (the kernel's buffer), @sectors worth.
 * Builds a descriptor chain: header, one data descriptor per sector
 * (contiguous 512-byte buffers), status. */
static int blk_rw(uint64_t sector, int write, uint64_t data_pa, uint32_t sectors)
{
    if (sectors == 0 || sectors > 8)
        return -1;
    blk_req_hdr_t *hdr = (blk_req_hdr_t *)g_hdr_va;
    hdr->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = sector;
    g_status[0] = 0xff;

    uint16_t head = g_avail->idx & (UBD_QUEUE_SIZE - 1);
    uint16_t d = head;
    g_desc[d].addr = g_hdr_pa; g_desc[d].len = sizeof(*hdr);
    g_desc[d].flags = VIRTQ_DESC_F_NEXT;
    for (uint32_t i = 0; i < sectors; i++) {
        uint16_t nd = (uint16_t)((d + 1) & (UBD_QUEUE_SIZE - 1));
        g_desc[d].next = nd;
        d = nd;
        g_desc[d].addr = data_pa + (uint64_t)i * UBD_SECTOR_SIZE;
        g_desc[d].len = UBD_SECTOR_SIZE;
        g_desc[d].flags = (write ? 0 : VIRTQ_DESC_F_WRITE) | VIRTQ_DESC_F_NEXT;
    }
    uint16_t sd = (uint16_t)((d + 1) & (UBD_QUEUE_SIZE - 1));
    g_desc[d].next = sd;
    d = sd;
    g_desc[d].addr = g_status_pa; g_desc[d].len = 1;
    g_desc[d].flags = VIRTQ_DESC_F_WRITE; g_desc[d].next = 0;

    __atomic_store_n(&g_avail->ring[head], head, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_avail->idx = (uint16_t)(g_avail->idx + 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    mmio_wr(VMMIO_QUEUENOTIFY, 0);

    /* Wait for completion (poll the used ring; the irq also arrives). */
    uint16_t last = (uint16_t)(g_used->idx);
    for (uint64_t spins = 0; spins < 100000000; spins++) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (g_used->idx != last)
            return g_status[0] == 0 ? 0 : -1;
    }
    return -1;
}

/* Submit a possibly-large request in chunks that fit the virtqueue. */
static int blk_rw_chunked(uint64_t sector, int write, uint64_t data_pa,
                          uint32_t sectors)
{
    while (sectors > 0) {
        uint32_t n = sectors > 32 ? 32 : sectors;
        if (blk_rw(sector, write, data_pa, n) < 0)
            return -1;
        sector += n;
        data_pa += (uint64_t)n * UBD_SECTOR_SIZE;
        sectors -= n;
    }
    return 0;
}

/* Process all newly published ring requests. */
static int drain_ring(void)
{
    uint32_t head = __atomic_load_n(&g_ring->head, __ATOMIC_ACQUIRE);
    uint32_t processed = 0;
    for (uint32_t id = g_last_done; id < head; id++) {
        udisk_req_t *req = &g_ring->reqs[id % UDISK_RING_REQS];
        if (id != req->id)
            continue; /* slot reuse race; retried on the next drain */
        if (__atomic_load_n(&req->done, __ATOMIC_ACQUIRE))
            continue;
        int r = blk_rw_chunked(req->sector, req->dir != 0, req->data_pa,
                               req->sectors);
        __atomic_store_n(&req->result, r == 0 ? 0 : 1, __ATOMIC_RELEASE);
        __atomic_store_n(&req->done, 1, __ATOMIC_RELEASE);
        processed++;
    }
    g_last_done = head;
    if (processed)
        a20_device_block_complete(processed);
    return 0;
}

/* Drain doorbell channel messages (one per published request) so the
 * kernel-side channel never fills; then process the ring. */
static void drain_doorbell(a20_handle_t ep)
{
    for (;;) {
        uint8_t b;
        uint32_t blen = 1;
        uint32_t hcnt = 0;
        a20_status_t st = a20_channel_recv_flags(ep, &b, &blen, 0, &hcnt,
                                                 A20_MSG_NONBLOCK);
        if (st == -A20_ERR_WOULD_BLOCK)
            break;
        if (st < 0)
            break;
    }
    drain_ring();
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    a20_handle_t out = si ? si->stdout_handle : A20_HANDLE_NULL;
    g_out = out;
#define UBD_LOG(msg) a20_hdl_write_buf(out, msg, sizeof(msg) - 1, (void *)0)
    UBD_LOG("ubd: start\n");

    if (a20_device_claim(UBD_MMIO_BASE) != A20_OK) {
        UBD_LOG("ubd: claim failed\n");
        return 2;
    }
    uint64_t mmio_va = 0;
    if (a20_device_map_mmio(UBD_MMIO_BASE, UBD_MMIO_SIZE, 3, &mmio_va) != A20_OK) {
        UBD_LOG("ubd: mmio map failed\n");
        return 2;
    }
    g_mmio = (volatile uint32_t *)(uintptr_t)mmio_va;
    if (virtio_init() != 0) {
        UBD_LOG("ubd: virtio init failed\n");
        return 6;
    }
    uint64_t cap_lo = mmio_rd(VMMIO_CFGCAPACITY);
    uint64_t cap_hi = mmio_rd(VMMIO_CFGCAPACITY + 4);
    uint64_t capacity = cap_lo | (cap_hi << 32);

    /* Ring VMO (one page) + attach to the kernel proxy. */
    a20_handle_t ring_vmo = (a20_handle_t)a20_vm_create_object(4096, 0);
    if ((int32_t)ring_vmo < 0) {
        UBD_LOG("ubd: ring vmo failed\n");
        return 7;
    }
    uint64_t ring_va = 0;
    if (a20_vm_map(ring_vmo, 4096, 0, 1 | 2, &ring_va) != A20_OK)
        return 8;
    for (uint32_t i = 0; i < 4096; i += 64)
        ((volatile uint8_t *)(uintptr_t)ring_va)[i] = 0;
    g_ring = (udisk_ring_t *)(uintptr_t)ring_va;
    __atomic_store_n(&g_ring->cap, UDISK_RING_REQS, __ATOMIC_RELAXED);

    a20_handle_t ep = A20_HANDLE_NULL;
    if (a20_device_block_attach(ring_vmo, capacity, &ep) != A20_OK) {
        UBD_LOG("ubd: block attach failed\n");
        return 9;
    }

    a20_handle_t eq;
    if (a20_event_queue_create(&eq) != A20_OK)
        return 3;
    if (a20_event_watch(eq, ep, A20_EVENT_MASK(A20_EVENT_MESSAGE_READY),
                        CH_TAG) != A20_OK)
        return 4;
    if (a20_device_irq_listen(UBD_MMIO_IRQ, eq, IRQ_TAG) != A20_OK) {
        UBD_LOG("ubd: irq listen failed\n");
        return 5;
    }
    UBD_LOG("ubd: virtio-blk ring ready\n");

    for (;;) {
        /* Drain first: requests published before the watch exists must be
         * processed too (kernel never waits on a stale ring). */
        drain_doorbell(ep);
        a20_event_t ev;
        a20_time_t inf = { .secs = (uint64_t)-1, .nsecs = 0 };
        if (a20_event_wait(eq, inf, &ev) < 0)
            continue;
        if (ev.user_data == IRQ_TAG) {
            uint32_t is = mmio_rd(VMMIO_INTSTATUS);
            mmio_wr(VMMIO_INTACK, is);
            a20_device_irq_ack(UBD_MMIO_IRQ);
            drain_ring(); /* completions may arrive via irq without a doorbell */
        }
        /* CH_TAG handled by drain_doorbell at the loop top. */
    }
}
