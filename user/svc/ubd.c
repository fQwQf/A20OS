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
#include "a20_services_idl.h"
#include "drivers/driver_descriptor.h"
#include "drivers/block/virtio_blk_core.h"

/* QEMU virt: virtio-mmio slot 3 = bus.3, IRQ = irq_base(1) + slot = 4
 * (driver-private placement, not service protocol). */
#define UBD_MMIO_BASE     0x10004000ULL
#define UBD_MMIO_SIZE     0x1000ULL
#define UBD_MMIO_IRQ      4u

#define UBD_SECTOR_SIZE   512u
#define UBD_QUEUE_SIZE    64u
#define UBD_EPROTO        (-71)
#define UBD_EIO           (-5)

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_USER_SERVICE,
                      A20_DRIVER_TYPE_BLOCK, "virtio-blk-user", A20_DRIVER_ABI, A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ | A20_DRIVER_RES_DMA,
                      0, 1,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_MMIO,
                                           0x10004000UL, 0));

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
static uint16_t           g_desc_head;
static uint16_t           g_queue_size;
static int                g_queue_ready;
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
static a20_virtio_blk_queue_state_t g_queue_state;
#endif
#ifdef CONFIG_UBD_QUEUE_SELFTEST
static int g_inject_used_mode;
#endif

static inline uint32_t mmio_rd(uint32_t off) { return g_mmio[off / 4]; }
static inline void mmio_wr(uint32_t off, uint32_t v) { g_mmio[off / 4] = v; }

static void zero_queue_page(uint64_t va)
{
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)va;
    for (uint32_t i = 0; i < 4096; i++)
        p[i] = 0;
}

typedef struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

_Static_assert(sizeof(virtq_desc_t) == sizeof(a20_virtio_blk_desc_t),
               "shared VirtIO descriptor size");
_Static_assert(__builtin_offsetof(virtq_desc_t, addr) ==
                   __builtin_offsetof(a20_virtio_blk_desc_t, addr) &&
               __builtin_offsetof(virtq_desc_t, len) ==
                   __builtin_offsetof(a20_virtio_blk_desc_t, len) &&
               __builtin_offsetof(virtq_desc_t, flags) ==
                   __builtin_offsetof(a20_virtio_blk_desc_t, flags) &&
               __builtin_offsetof(virtq_desc_t, next) ==
                   __builtin_offsetof(a20_virtio_blk_desc_t, next),
               "shared VirtIO descriptor offsets");

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
    /* Queue size must be a power of two. Select the largest supported size
     * no larger than our fixed backing arrays. */
    while (qsz && (qsz & (qsz - 1)) != 0)
        qsz &= qsz - 1;
    if (qsz < 4)
        return -1;
    g_queue_size = (uint16_t)qsz;
    g_desc_head = 0;
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    if (a20_virtio_blk_queue_init(&g_queue_state, g_queue_size, 1) != 0)
        return -1;
#endif
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
    g_queue_ready = 1;
    return 0;
}

#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
static int virtio_reinitialize_queue(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    mmio_wr(VMMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    mmio_wr(VMMIO_DRVFEATSEL, 0);
    mmio_wr(VMMIO_DRVFEAT, 0);
    mmio_wr(VMMIO_DRVFEATSEL, 1);
    mmio_wr(VMMIO_DRVFEAT, 0);
    mmio_wr(VMMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                          VIRTIO_STATUS_FEATURES_OK);
    mmio_wr(VMMIO_QUEUESEL, 0);
    mmio_wr(VMMIO_QUEUENUM, g_queue_size);
    zero_queue_page(g_desc_va);
    zero_queue_page(g_avail_va);
    zero_queue_page(g_used_va);
    mmio_wr(VMMIO_QUEUEDESCLO, (uint32_t)g_desc_pa);
    mmio_wr(VMMIO_QUEUEDESCHI, (uint32_t)(g_desc_pa >> 32));
    mmio_wr(VMMIO_QUEUEAVAILLO, (uint32_t)g_avail_pa);
    mmio_wr(VMMIO_QUEUEAVAILHI, (uint32_t)(g_avail_pa >> 32));
    mmio_wr(VMMIO_QUEUEUSEDLO, (uint32_t)g_used_pa);
    mmio_wr(VMMIO_QUEUEUSEDHI, (uint32_t)(g_used_pa >> 32));
    mmio_wr(VMMIO_QUEUEREADY, 1);
    mmio_wr(VMMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                          VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

static int virtio_recover_queue(uint16_t current_slot)
{
    uint16_t slots[1];
    int failed = a20_virtio_blk_queue_fail_all(&g_queue_state, slots, 1);
    if (failed < 0)
        return failed;
    g_queue_ready = 0;
    mmio_wr(VMMIO_STATUS, 0);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t spins = 1000000;
    while (mmio_rd(VMMIO_STATUS) != 0 && --spins)
        __asm__ volatile("nop");
    if (!spins)
        return UBD_EIO;
    int reset = a20_virtio_blk_queue_reset(&g_queue_state);
    if (reset != 0)
        return reset;
    int ret = virtio_reinitialize_queue();
    for (int i = 0; i < failed; i++)
        (void)a20_virtio_blk_queue_release(&g_queue_state, slots[i]);
    g_queue_ready = ret == 0;
    (void)current_slot;
    return ret;
}
#endif

/* DMA directly into/from @data_pa (the kernel's buffer), @sectors worth.
 * Builds a descriptor chain: header, one data descriptor per sector
 * (contiguous 512-byte buffers), status. */
static int blk_rw(uint64_t sector, int write, uint64_t data_pa, uint32_t sectors)
{
    if (!g_queue_ready)
        return UBD_EIO;
    if (g_queue_size < 4 || sectors == 0 || sectors > g_queue_size - 2)
        return -1;
    blk_req_hdr_t *hdr = (blk_req_hdr_t *)g_hdr_va;
    hdr->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = sector;
    uint16_t head;
    int desc_count;
    uint16_t queue_slot = 0;
    uint16_t ring_slot = 0;
    int queue_result = 0;
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    uint16_t expected_head = 0;
    queue_result = a20_virtio_blk_queue_reserve(
        &g_queue_state, (uint16_t)(sectors + 2),
        write ? 1U : sectors * UBD_SECTOR_SIZE + 1U,
        write ? 1U : 0U, &queue_slot, &expected_head);
    if (queue_result != 0)
        return queue_result;
    head = expected_head;
#endif

#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    a20_virtio_blk_segment_t segments[UBD_QUEUE_SIZE - 2];
    for (uint32_t i = 0; i < sectors; i++) {
        segments[i].addr = data_pa + (uint64_t)i * UBD_SECTOR_SIZE;
        segments[i].len = UBD_SECTOR_SIZE;
    }
    desc_count = a20_virtio_blk_build_chain(
        (a20_virtio_blk_desc_t *)g_desc, g_queue_size, head,
        g_hdr_pa, sizeof(*hdr), segments, (uint16_t)sectors,
        g_status_pa + head,
        write ? 0 : 1);
    if (desc_count < 0) {
        (void)a20_virtio_blk_queue_cancel(&g_queue_state, queue_slot);
        return desc_count;
    }
#else
    head = g_avail->idx & (g_queue_size - 1);
    uint16_t d = head;
    g_desc[d].addr = g_hdr_pa; g_desc[d].len = sizeof(*hdr);
    g_desc[d].flags = VIRTQ_DESC_F_NEXT;
    for (uint32_t i = 0; i < sectors; i++) {
        uint16_t nd = (uint16_t)((d + 1) & (g_queue_size - 1));
        g_desc[d].next = nd;
        d = nd;
        g_desc[d].addr = data_pa + (uint64_t)i * UBD_SECTOR_SIZE;
        g_desc[d].len = UBD_SECTOR_SIZE;
        g_desc[d].flags = (write ? 0 : VIRTQ_DESC_F_WRITE) | VIRTQ_DESC_F_NEXT;
    }
    uint16_t sd = (uint16_t)((d + 1) & (g_queue_size - 1));
    g_desc[d].next = sd;
    d = sd;
    g_desc[d].addr = g_status_pa + head; g_desc[d].len = 1;
    g_desc[d].flags = VIRTQ_DESC_F_WRITE; g_desc[d].next = 0;
    desc_count = (int)sectors + 2;
#endif
    g_status[head] = 0xff;
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    queue_result = a20_virtio_blk_queue_prepare_publish(
        &g_queue_state, queue_slot, &ring_slot);
    if (queue_result != 0) {
        (void)a20_virtio_blk_queue_cancel(&g_queue_state, queue_slot);
        return queue_result;
    }
#endif
    uint16_t prior_used = g_used->idx;
    uint16_t avail_slot =
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
        ring_slot;
#else
        g_avail->idx & (g_queue_size - 1);
#endif
    __atomic_store_n(&g_avail->ring[avail_slot], head, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    uint16_t new_avail = 0;
    queue_result = a20_virtio_blk_queue_commit_publish(
        &g_queue_state, queue_slot, &new_avail);
    if (queue_result != 0) {
        (void)a20_virtio_blk_queue_cancel(&g_queue_state, queue_slot);
        return queue_result;
    }
    g_avail->idx = new_avail;
#else
    g_avail->idx = (uint16_t)(g_avail->idx + 1);
#endif
    __atomic_thread_fence(__ATOMIC_RELEASE);
#ifdef CONFIG_UBD_QUEUE_SELFTEST
    if (g_inject_used_mode != 0) {
        int inject_mode = g_inject_used_mode;
        g_inject_used_mode = 0;
        uint16_t corrupt_slot = prior_used & (g_queue_size - 1);
        if (inject_mode == 1) {
            /* This aliases a valid 16-bit head if narrowed before checking. */
            g_used->ring[corrupt_slot].id = (uint32_t)head + 65536U;
            g_used->ring[corrupt_slot].len = 1;
        } else {
            g_status[head] = 1;
            g_used->ring[corrupt_slot].id = head;
            g_used->ring[corrupt_slot].len =
                write ? 1U : sectors * UBD_SECTOR_SIZE + 1U;
        }
        __atomic_store_n(&g_used->idx, (uint16_t)(prior_used + 1),
                         __ATOMIC_RELEASE);
    } else
#endif
    mmio_wr(VMMIO_QUEUENOTIFY, 0);

    /* Wait for completion (poll the used ring; the irq also arrives). */
    for (uint64_t spins = 0; spins < 100000000; spins++) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (g_used->idx != prior_used) {
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
            uint16_t used_slot = 0, completed_slot = 0;
            uint16_t device_idx = g_used->idx;
            uint16_t used_head = 0;
            uint32_t used_len = 0;
            if (a20_virtio_blk_queue_used_ring_slot(&g_queue_state, device_idx,
                                                     &used_slot) != 1) {
                queue_result = UBD_EPROTO;
            } else {
                uint32_t raw_head = g_used->ring[used_slot].id;
                used_len = g_used->ring[used_slot].len;
                if (raw_head > UINT16_MAX || raw_head >= g_queue_size) {
                    queue_result = UBD_EPROTO;
                } else {
                    used_head = (uint16_t)raw_head;
                    if (a20_virtio_blk_queue_validate_head(
                            &g_queue_state, used_head, &completed_slot) != 0 ||
                        completed_slot != queue_slot) {
                        queue_result = UBD_EPROTO;
                    } else {
                        int result = -1;
                        int retired = a20_virtio_blk_queue_complete_used(
                            &g_queue_state, device_idx, used_head, used_len,
                            g_status[used_head], &completed_slot, &result);
                        if (retired == 1)
                            queue_result = result;
                        else
                            queue_result = UBD_EPROTO;
                    }
                }
            }
            if (queue_result == UBD_EPROTO)
                (void)virtio_recover_queue(queue_slot);
            else
                (void)a20_virtio_blk_queue_release(&g_queue_state, queue_slot);
            return queue_result;
#else
            return g_status[head] == 0 ? 0 : -1;
#endif
        }
    }
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    (void)virtio_recover_queue(queue_slot);
#endif
    return -1;
}

/* Submit a possibly-large request in chunks that fit the virtqueue. */
static int blk_rw_chunked(uint64_t sector, int write, uint64_t data_pa,
                          uint32_t sectors)
{
    while (sectors > 0) {
        /* Each sector occupies one descriptor, in addition to the request
         * header and status descriptors. Keep every chain within the ring. */
        uint32_t max_sectors = g_queue_size - 2;
        uint32_t n = sectors > max_sectors ? max_sectors : sectors;
        if (blk_rw(sector, write, data_pa, n) < 0)
            return -1;
        sector += n;
        data_pa += (uint64_t)n * UBD_SECTOR_SIZE;
        sectors -= n;
    }
    return 0;
}

#if defined(CONFIG_UBD_QUEUE_SELFTEST) && defined(CONFIG_VIRTIO_BLK_CORE_RUST)
static void ubd_queue_test_log(const char *message, uint32_t length)
{
    (void)a20_hdl_write_buf(g_out, message, length, (void *)0);
}

static int ubd_queue_selftest(void)
{
    uint64_t test_va = 0, test_pa = 0;
    if (!alloc_page(&test_va, &test_pa))
        return 1;

    /* Publish a request but suppress the device kick, then present a
     * deliberately out-of-range used head. Recovery must reset the hardware
     * queue and leave the same request slot reusable for the next real read. */
    g_inject_used_mode = 1;
    int ret = blk_rw(0, 0, test_pa, 1);
    if (ret != UBD_EPROTO) {
        ubd_queue_test_log("UBD_QUEUE_TEST: malformed-head result mismatch\n", 47);
        return 2;
    }
    ret = blk_rw(0, 0, test_pa, 1);
    if (ret != 0) {
        ubd_queue_test_log("UBD_QUEUE_TEST: read after reset failed\n", 40);
        return 3;
    }

    /* A readonly backend gives a genuine device-completed IOERR and lets
     * us verify that a subsequent request reuses the queue without reset. */
#ifdef CONFIG_UBD_QUEUE_REAL_IOERR_TEST
    ret = blk_rw(1, 1, test_pa, 1);
    if (ret != UBD_EIO) {
        ubd_queue_test_log("UBD_QUEUE_TEST: real IOERR result mismatch\n", 43);
        return 4;
    }
    ret = blk_rw(0, 0, test_pa, 1);
    if (ret != 0) {
        ubd_queue_test_log("UBD_QUEUE_TEST: read after real IOERR failed\n", 45);
        return 5;
    }
#else
    /* Synthetic completion intentionally suppresses the device kick, leaving
     * its avail entry unseen; reset before testing slot reuse afterward. */
    g_inject_used_mode = 2;
    ret = blk_rw(1, 1, test_pa, 1);
    if (ret != UBD_EIO) {
        ubd_queue_test_log("UBD_QUEUE_TEST: injected IOERR result mismatch\n", 47);
        return 4;
    }
    ret = virtio_recover_queue(0);
    if (ret != 0) {
        ubd_queue_test_log("UBD_QUEUE_TEST: reset after IOERR failed\n", 41);
        return 5;
    }
    ret = blk_rw(0, 0, test_pa, 1);
    if (ret != 0) {
        ubd_queue_test_log("UBD_QUEUE_TEST: read after IOERR reset failed\n", 46);
        return 6;
    }
#endif
    return 0;
}
#endif

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
#if defined(CONFIG_UBD_QUEUE_SELFTEST) && defined(CONFIG_VIRTIO_BLK_CORE_RUST)
    int queue_test = ubd_queue_selftest();
    if (queue_test != 0) {
        UBD_LOG("UBD_QUEUE_TEST: FAIL\n");
        return 10 + queue_test;
    }
#ifdef CONFIG_UBD_QUEUE_REAL_IOERR_TEST
    UBD_LOG("UBD_QUEUE_TEST: PASS malformed-reset/real-ioerr/reuse\n");
#else
    UBD_LOG("UBD_QUEUE_TEST: PASS malformed-reset/synthetic-ioerr/reset\n");
#endif
#endif
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
