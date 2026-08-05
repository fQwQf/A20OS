/*
 * udisk — kernel block proxy for a user-space virtio-blk driver
 * (docs/hybrid-kernel/02-mainstream-plan.md M4, mainstream hybrid form).
 *
 * The page cache and filesystems stay in the kernel (they consume this
 * device through the normal block_dev_t / bcache interface).  Block
 * requests are forwarded over a one-page shared-memory ring to the
 * user-space driver, which performs the actual virtio DMA into the
 * kernel-provided data physical address (zero copy: no data bytes ever
 * cross the ring or a channel).  The doorbell is a channel message; the
 * completion is a syscall that wakes the parked kernel waiter.
 */
#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/refcount.h"
#include "mm/slab.h"
#include "mm/mm.h"
#include "mm/vmo.h"
#include "fs/block_cache.h"
#include "drivers/block/block_dev.h"
#include "fs/mount_setup.h"
#include "abi/native/types.h"
#include "abi/native/ipc_internal.h"
#include "proc/proc.h"
#include "ipc/objstats.h"

#define UDISK_RING_REQS  16

typedef struct udisk_req {
    uint32_t      id;
    uint32_t      dir;          /* 0 = read (device->data_pa), 1 = write */
    uint64_t      sector;
    uint32_t      sectors;
    _Atomic uint32_t done;
    _Atomic uint32_t result;
    uint64_t      data_pa;
} udisk_req_t;

typedef struct udisk_ring {
    _Atomic uint32_t head;      /* published request count */
    uint32_t        cap;
    udisk_req_t     reqs[UDISK_RING_REQS];
} udisk_ring_t;

#define UDISK_MAX_INST 4

typedef struct udisk_inst {
    block_dev_t      bdev;
    struct vmo      *ring_vmo;
    udisk_ring_t    *ring;      /* kernel alias (pfn_to_virt) */
    a20_channel_ep_t *doorbell; /* kernel endpoint to the driver */
    spinlock_t       lock;
    wait_queue_t     waiters;
    uint32_t         id_counter;
    uint32_t         in_flight;
    uint64_t         capacity;
    uint8_t          active;
} udisk_inst_t;

static udisk_inst_t g_insts[UDISK_MAX_INST];
static int          g_udisk_mounted;
static udisk_inst_t *g_mount_target;

static int udisk_read_sector(block_dev_t *dev, uint64_t lba, void *buf,
                             size_t count);

static void udisk_mount_kthread(void)
{
    udisk_inst_t *u = g_mount_target;
    /* Probe + mount.  Block reads park until the user driver services the
     * ring, so no delay is needed here. */
    int r = try_mount(&u->bdev, "/ubd", "fat32");
    if (r == 0)
        g_udisk_mounted = 1;
    g_mount_target = NULL;
    proc_exit(0);
}

static int udisk_rw(udisk_inst_t *u, uint64_t lba, void *buf, size_t count, int dir)
{
    udisk_ring_t *r = u->ring;
    if (!r)
        return -1;

    uint32_t id;
    udisk_req_t *req;
    uint64_t flags = spin_lock_irqsave(&u->lock);
    /* Flow control: at most UDISK_RING_REQS-1 requests in flight so a slot
     * is never reused while its previous owner is outstanding. */
    while (u->in_flight >= UDISK_RING_REQS - 1) {
        spin_unlock_irqrestore(&u->lock, flags);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
        uint64_t f2 = spin_lock_irqsave(&u->lock);
        bool linked = token.task &&
            wait_queue_link(&u->waiters, &(wait_queue_entry_t){0}, token, 0);
        spin_unlock_irqrestore(&u->lock, f2);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&u->waiters, &(wait_queue_entry_t){0});
        proc_park_finish(token);
        (void)reason;
        flags = spin_lock_irqsave(&u->lock);
    }
    id = u->id_counter++;
    u->in_flight++;
    uint32_t slot = id % UDISK_RING_REQS;
    req = &r->reqs[slot];
    req->id = id;
    req->dir = (uint32_t)dir;
    req->sector = lba;
    req->sectors = (uint32_t)count;
    req->data_pa = (uint64_t)va_to_pa(buf);
    __atomic_store_n(&req->done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&req->result, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&r->head, id + 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&u->lock, flags);

    /* Doorbell: one byte through the channel to the driver. */
    uint8_t b = 1;
    a20_channel_send_dwc(u->doorbell, &b, 1, 0, 0, NULL, 0, 0);

    /* Wait for this request's completion. */
    for (;;) {
        if (__atomic_load_n(&req->done, __ATOMIC_ACQUIRE))
            return __atomic_load_n(&req->result, __ATOMIC_ACQUIRE) == 0 ? 0 : -1;

        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
        if (!token.task)
            return -1;
        uint64_t f2 = spin_lock_irqsave(&u->lock);
        bool linked = token.task && wait_queue_link(&u->waiters,
                        &(wait_queue_entry_t){0}, token, 0);
        spin_unlock_irqrestore(&u->lock, f2);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&u->waiters, &(wait_queue_entry_t){0});
        proc_park_finish(token);
        (void)reason;
        if (__atomic_load_n(&req->done, __ATOMIC_ACQUIRE))
            return __atomic_load_n(&req->result, __ATOMIC_ACQUIRE) == 0 ? 0 : -1;
    }
}

static int udisk_read_sector(block_dev_t *dev, uint64_t lba, void *buf, size_t count)
{
    udisk_inst_t *u = (udisk_inst_t *)dev->priv;
    return udisk_rw(u, lba, buf, count, 0);
}

static int udisk_write_sector(block_dev_t *dev, uint64_t lba, const void *buf, size_t count)
{
    udisk_inst_t *u = (udisk_inst_t *)dev->priv;
    return udisk_rw(u, lba, (void *)buf, count, 1);
}

block_dev_t *udisk_get_dev(void)
{
    for (int i = 0; i < UDISK_MAX_INST; i++)
        if (g_insts[i].active)
            return &g_insts[i].bdev;
    return NULL;
}

static void udisk_wake(udisk_inst_t *u)
{
    wait_queue_wake_all(&u->waiters, 0, PROC_WAKE_EVENT);
}

/* Attach a driver's ring VMO: create the doorbell pair, bind the device. */
int udisk_attach(struct vmo *vmo, uint64_t capacity,
                 a20_channel_ep_t **out_doorbell, int owner_pid)
{
    udisk_inst_t *u = NULL;
    for (int i = 0; i < UDISK_MAX_INST; i++) {
        if (!g_insts[i].active) { u = &g_insts[i]; break; }
    }
    if (!u)
        return -1;
    memset(u, 0, sizeof(*u));

    udisk_ring_t *ring = (udisk_ring_t *)pfn_to_virt(vmo_peek_page(vmo, 0));
    if (!ring)
        return -1;

    a20_channel_ep_t *ep0 = a20_channel_create(0, NULL);
    if (!ep0)
        return -1;
    a20_channel_ep_t *ubd_ep = ep0->peer;
    /* The ubd_ep reference moves to the caller via the returned handle. */

    spin_init(&u->lock);
    wait_queue_init(&u->waiters);
    u->ring_vmo = vmo;
    vmo_ref(vmo);
    u->ring = ring;
    u->doorbell = ep0;
    u->capacity = capacity;
    u->id_counter = 0;
    u->active = 1;
    u->bdev.read_sector = udisk_read_sector;
    u->bdev.write_sector = udisk_write_sector;
    u->bdev.capacity = capacity;
    u->bdev.sector_size = 512;
    u->bdev.priv = u;

    a20_objstat_add(&g_a20_objstats.channel_eps, 1);
    klog(KLOG_INFO, "udisk: attached pid=%d capacity=%lu\n", owner_pid,
         (unsigned long)capacity);

    /* Auto-mount in a kernel thread so attach returns to the driver
     * immediately (mount probing blocks on reads the driver must serve). */
    if (!g_udisk_mounted && !g_mount_target) {
        g_mount_target = u;
        if (proc_alloc(udisk_mount_kthread) < 0)
            g_mount_target = NULL;
    }

    *out_doorbell = ubd_ep;
    return 0;
}

int udisk_complete(int pid, uint32_t n_done)
{
    for (int i = 0; i < UDISK_MAX_INST; i++) {
        if (g_insts[i].active && g_insts[i].doorbell) {
            if (n_done) {
                uint64_t flags = spin_lock_irqsave(&g_insts[i].lock);
                if (g_insts[i].in_flight >= n_done)
                    g_insts[i].in_flight -= n_done;
                else
                    g_insts[i].in_flight = 0;
                spin_unlock_irqrestore(&g_insts[i].lock, flags);
            }
            udisk_wake(&g_insts[i]);
            return 0;
        }
    }
    return -1;
}