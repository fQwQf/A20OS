#include "drivers/gpu/virtio_gpu.h"
#include "drivers/gpu/gpu_core.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_hwapi.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/errno.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"

#include "mm/mm.h"
#include "mm/frame.h"
#include "drivers/block/virtio_blk.h"
#include "sys/usercopy.h"

#define VIRTIO_GPU_QUEUE_SIZE VIRTIO_QUEUE_SIZE
#define VIRTIO_GPU_COMMAND_BYTES 128U

typedef struct {
    virtio_transport_t vt;
    virtq_desc_t       desc[VIRTIO_GPU_QUEUE_SIZE] ALIGNED(64);
    virtq_avail_t      avail ALIGNED(64);
    virtq_used_t       used ALIGNED(64);
    
    mutex_t            command_lock ALIGNED(64);
    /* Parked controlq issuers woken by the completion IRQ; internally
     * locked so the IRQ top-half may collect without command_lock. */
    wait_queue_t       waiters;
    int                irq_registered;
    int                slot;
    
    uint32_t           width;
    uint32_t           height;
    uint32_t           bpp;
    uintptr_t          fb_phys;
    size_t             fb_size;
    int                fb_order;
    
    uint16_t           desc_idx;
    uint16_t           last_used;
    int                valid;
    int                virgl;   /* VIRTIO_GPU_F_VIRGL negotiated */
    int                context_init; /* VIRTIO_GPU_F_CONTEXT_INIT negotiated */
    int                has_edid;   /* VIRTIO_GPU_F_EDID negotiated */
    int                edid_valid;
    uint8_t            edid[128];
    /* Device-owned staging.  Lifecycle and ioctl callers often provide stack
     * objects, which must never be exposed directly to DMA: after a timeout
     * the device may still access them after the caller returns. */
    uint8_t            command_req[VIRTIO_GPU_COMMAND_BYTES] ALIGNED(64);
    uint8_t            command_resp[VIRTIO_GPU_COMMAND_BYTES] ALIGNED(64);
    /* Large 3D command staging: SUBMIT_3D and capset blobs exceed the
     * fixed 128-byte command slot.  Guarded by command_lock; only one
     * in-flight large command at a time. */
    uint8_t           *big_req;
    size_t             big_req_cap;
    uint8_t           *big_resp;
    size_t             big_resp_cap;
    /* SUBMIT_3D command header staging (DMA-safe, instance-owned).
     *
     * This must be exactly struct virtio_gpu_cmd_submit and nothing more.  The
     * host copies the command with iov_to_buf(sg, n, sizeof(struct
     * virtio_gpu_cmd_submit), buf, size), so it skips a hardcoded 32 bytes of
     * this descriptor and reads the stream from whatever follows.  Appending a
     * mem_entry here -- as this once did -- does not just add a field: it makes
     * the descriptor longer than the offset the host skips, so the host reads
     * the padding as the first dwords of the command and submits a stream
     * truncated by the same amount.  Nothing reports it: SUBMIT_3D answers OK
     * because the transport worked, and the renderer rejects the garbage in
     * silence. */
    struct virtio_gpu_cmd_submit submit_hdr ALIGNED(64);
    struct virtio_gpu_ctrl_hdr submit_resp ALIGNED(64);
} virtio_gpu_inst_t;

static virtio_gpu_inst_t g_gpu_inst;

/* The host copies a SUBMIT_3D payload from a fixed offset into the request
 * scatter-gather list, so the header descriptor has to be exactly the spec
 * structure and no larger.  Assert it rather than trust the comment: growing
 * this type is a one-line change that would otherwise ship a silently truncated
 * command stream to every guest, with the host still answering OK. */
_Static_assert(sizeof(struct virtio_gpu_cmd_submit) == 32,
               "host skips sizeof(virtio_gpu_cmd_submit) bytes before the command");
_Static_assert(sizeof(((virtio_gpu_inst_t *)0)->submit_hdr) == 32,
               "submit_hdr must be the bare cmd_submit; a trailing field shifts the payload");


static void virtio_gpu_mmio_write32(virtio_transport_t *t, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)((uintptr_t)t->priv + off));
}

static uint32_t virtio_gpu_mmio_read32(virtio_transport_t *t, uint32_t off) {
    return readl((const volatile void *)((uintptr_t)t->priv + off));
}

/* VIRTIO_GPU_IRQ_MODEL:
 * - The top-half acknowledges the device interrupt and wakes the parked
 *   controlq issuer; it never touches queue state — the issuer owns
 *   last_used and the single in-flight command chain.
 * - A spurious or shared-line invocation degrades to one no-op wake. */
static int virtio_gpu_irq_handler(int irq, void *priv) {
    (void)irq;
    virtio_gpu_inst_t *inst = (virtio_gpu_inst_t *)priv;
    if (!inst)
        return 0;
    uint32_t isr = inst->vt.read32(&inst->vt, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!inst->vt.legacy && isr)
        inst->vt.write32(&inst->vt, VIRTIO_MMIO_INTERRUPT_ACK, isr);
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    (void)wait_queue_collect_all(&inst->waiters, 0, PROC_WAKE_EVENT,
                                 &wake_q, NULL);
    (void)proc_wake_q_flush(&wake_q);
    return 0;
}

static int virtio_gpu_send_cmd(virtio_gpu_inst_t *inst, void *req, size_t req_len, void *resp, size_t resp_len) {
    if (!inst || !req || !resp || req_len > sizeof(inst->command_req) ||
        resp_len > sizeof(inst->command_resp))
        return -EINVAL;

    mutex_lock(&inst->command_lock);
    int completed = 0;
    memcpy(inst->command_req, req, req_len);
    memset(inst->command_resp, 0, resp_len);
    
    uint16_t head = inst->desc_idx % VIRTIO_GPU_QUEUE_SIZE;
    uint16_t slot = head;
    uint16_t resp_slot = (slot + 1) % VIRTIO_GPU_QUEUE_SIZE;
    
    // Descriptor for request (device-read)
    inst->desc[slot].addr  = va_to_pa(inst->command_req);
    inst->desc[slot].len   = (uint32_t)req_len;
    inst->desc[slot].flags = VIRTQ_DESC_F_NEXT;
    inst->desc[slot].next  = resp_slot;
    
    inst->desc[resp_slot].addr  = va_to_pa(inst->command_resp);
    inst->desc[resp_slot].len   = (uint32_t)resp_len;
    inst->desc[resp_slot].flags = VIRTQ_DESC_F_WRITE;
    inst->desc[resp_slot].next  = 0;
    
    arch_dma_sync_for_device(&inst->desc[slot], sizeof(virtq_desc_t));
    arch_dma_sync_for_device(&inst->desc[resp_slot], sizeof(virtq_desc_t));
    arch_dma_sync_for_device(inst->command_req, req_len);
    arch_dma_sync_for_device(inst->command_resp, resp_len);
    
    /* Snapshot completion state before publishing the new avail entry.  The
     * device may consume an entry as soon as avail.idx becomes visible, even
     * before the notification write.  Taking this snapshot after publishing
     * races a fast QEMU device and then waits forever for a second completion. */
    arch_dma_sync_for_cpu(&inst->used, sizeof(inst->used));
    uint16_t used_before = ((volatile virtq_used_t *)&inst->used)->idx;

    uint16_t avail_slot = inst->avail.idx % VIRTIO_GPU_QUEUE_SIZE;
    inst->avail.ring[avail_slot] = slot;
    wmb();
    inst->avail.idx++;
    wmb();
    
    arch_dma_sync_for_device(&inst->avail, sizeof(inst->avail));
    
    // Notify device (queue 0 = controlq)
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();
    
    // Poll until used->idx advances beyond what it was before submit
    volatile virtq_used_t *used = &inst->used;
    uint64_t start = clock_get_ticks();
    uint64_t frequency = clock_ticks_per_sec();
    uint64_t deadline = (start && frequency) ? start + frequency : 0;
    uint32_t spins = 100000000U;
    /* The device writes used->idx behind our back, so every observation of it
     * must be preceded by an invalidate-for-CPU: on a coherent port that helper
     * is a no-op, on aarch64 it is a dsb sy and a dc ivac per line, and a load
     * that hits the line cached by the previous observation would never see the
     * completion.  That per-observation sync cannot be hoisted out.
     *
     * What can go is the 65536-iteration chunk that paid for it before the
     * first sleep -- 131072 dsb sy on aarch64, twice per page flip and twice
     * more per 30Hz flush.  Once a completion IRQ is registered the wait queue
     * is the cheap way to observe the index, so park on the first iteration and
     * let the handler schedule each re-check.  Without an IRQ there is nothing
     * to park on and the chunked poll stands. */
    uint32_t park_mask = (inst->irq_registered && proc_current()) ? 0U : 0xffffU;
    while (spins--) {
        arch_dma_sync_for_cpu((void *)used, sizeof(*used));
        if (used->idx != used_before) {
            completed = 1;
            break;
        }
        if (deadline && clock_get_ticks() >= deadline)
            break;
        arch_cpu_relax();
        /* Boot-time probe has no current task and must remain a bounded poll.
         * Runtime flushes yield periodically so the host backend and kernel
         * progress paths are not starved by a full-vCPU busy loop; with a
         * registered IRQ the flush instead parks until the completion
         * interrupt (bounded chunks guard against a missed wake). */
        if ((spins & park_mask) == 0 && proc_current()) {
            if (inst->irq_registered && deadline) {
                uint64_t now = clock_get_ticks();
                if (now < deadline) {
                    uint64_t chunk = now + MS_TO_TICKS(20);
                    if (chunk > deadline)
                        chunk = deadline;
                    proc_wait_token_t token =
                        proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, chunk);
                    wait_queue_entry_t entry = {0};
                    wait_queue_link(&inst->waiters, &entry, token, 0);
                    arch_dma_sync_for_cpu((void *)used, sizeof(*used));
                    if (used->idx != used_before) {
                        wait_queue_unlink(&inst->waiters, &entry);
                        (void)proc_park_cancel(token);
                        proc_park_finish(token);
                        completed = 1;
                        break;
                    }
                    (void)proc_park_commit(token);
                    wait_queue_unlink(&inst->waiters, &entry);
                    proc_park_finish(token);
                }
            } else {
                proc_yield();
            }
        }
    }
    
    if (!completed) {
        static unsigned timeout_logs;
        if (timeout_logs++ < 4)
            kinfo("[GPU] send_cmd TIMEOUT cmd=%x used=%u before=%u avail=%u desc=%u status=%x ready=%u\n",
                  ((struct virtio_gpu_ctrl_hdr *)inst->command_req)->type,
                  used->idx, used_before, inst->avail.idx, slot,
                  inst->vt.read32(&inst->vt, VIRTIO_MMIO_STATUS),
                  inst->vt.read32(&inst->vt, VIRTIO_MMIO_QUEUE_READY));
        mutex_unlock(&inst->command_lock);
        return -1;
    }
    
    uint16_t ring_idx = (uint16_t)(used_before % VIRTIO_GPU_QUEUE_SIZE);
    arch_dma_sync_for_cpu(inst->command_resp, resp_len);
    if (used->ring[ring_idx].id != slot) {
        mutex_unlock(&inst->command_lock);
        return -1;
    }
    memcpy(resp, inst->command_resp, resp_len);

    /* Polling still has to deassert the transport interrupt.  On PCI, reading
     * ISR clears the level; on MMIO, the observed bits must be acknowledged.
     * Leaving the first boot-time completion asserted turns into an interrupt
     * storm as soon as the scheduler enables IRQs, starving later GUI flushes. */
    uint32_t isr = inst->vt.read32(&inst->vt, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!inst->vt.legacy && isr)
        inst->vt.write32(&inst->vt, VIRTIO_MMIO_INTERRUPT_ACK, isr);
    
    inst->last_used = used->idx;
    /* This synchronous queue has one in-flight chain. Reuse descriptor 0/1
     * only after the device has returned it; cycling through the whole table
     * exposed a runtime-only failure in QEMU's virtio-gpu controlq path. */
    inst->desc_idx = 0;
    
    mutex_unlock(&inst->command_lock);
    return 0;
}

/*
 * Large-command variant of virtio_gpu_send_cmd for 3D paths whose payloads
 * (GET_CAPSET, SUBMIT_3D) exceed the fixed 128-byte command slot.  Uses the
 * driver-owned big_req/big_resp buffers (grown on demand, guarded by
 * command_lock) so DMA never references caller stack objects.
 */
static int virtio_gpu_send_cmd_big(virtio_gpu_inst_t *inst, const void *req,
                                   size_t req_len, void *resp, size_t resp_len)
{
    if (!inst || !req || !resp || req_len == 0)
        return -EINVAL;

    mutex_lock(&inst->command_lock);
    if (req_len > inst->big_req_cap) {
        uint8_t *nr = kmalloc(req_len);
        if (!nr) { mutex_unlock(&inst->command_lock); return -ENOMEM; }
        if (inst->big_req) kfree(inst->big_req);
        inst->big_req = nr;
        inst->big_req_cap = req_len;
    }
    if (resp_len > inst->big_resp_cap) {
        uint8_t *nr = kmalloc(resp_len);
        if (!nr) { mutex_unlock(&inst->command_lock); return -ENOMEM; }
        if (inst->big_resp) kfree(inst->big_resp);
        inst->big_resp = nr;
        inst->big_resp_cap = resp_len;
    }
    memcpy(inst->big_req, req, req_len);
    memset(inst->big_resp, 0, resp_len);

    uint16_t slot = 0;
    uint16_t resp_slot = 1;
    inst->desc[slot].addr  = va_to_pa(inst->big_req);
    inst->desc[slot].len   = (uint32_t)req_len;
    inst->desc[slot].flags = VIRTQ_DESC_F_NEXT;
    inst->desc[slot].next  = resp_slot;
    inst->desc[resp_slot].addr  = va_to_pa(inst->big_resp);
    inst->desc[resp_slot].len   = (uint32_t)resp_len;
    inst->desc[resp_slot].flags = VIRTQ_DESC_F_WRITE;
    inst->desc[resp_slot].next  = 0;
    arch_dma_sync_for_device(inst->desc, sizeof(virtq_desc_t));
    arch_dma_sync_for_device(inst->big_req, req_len);
    arch_dma_sync_for_device(inst->big_resp, resp_len);

    arch_dma_sync_for_cpu(&inst->used, sizeof(inst->used));
    uint16_t used_before = ((volatile virtq_used_t *)&inst->used)->idx;

    uint16_t avail_slot = inst->avail.idx % VIRTIO_GPU_QUEUE_SIZE;
    inst->avail.ring[avail_slot] = slot;
    wmb();
    inst->avail.idx++;
    wmb();
    arch_dma_sync_for_device(&inst->avail, sizeof(inst->avail));
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();

    volatile virtq_used_t *used = &inst->used;
    uint64_t start = clock_get_ticks();
    uint64_t frequency = clock_ticks_per_sec();
    uint64_t deadline = (start && frequency) ? start + frequency : 0;
    uint32_t spins = 100000000U;
    int completed = 0;
    while (spins--) {
        arch_dma_sync_for_cpu((void *)used, sizeof(*used));
        if (used->idx != used_before) { completed = 1; break; }
        if (deadline && clock_get_ticks() >= deadline)
            break;
        arch_cpu_relax();
        if ((spins & 0xffffU) == 0 && proc_current()) {
            if (inst->irq_registered && deadline) {
                uint64_t now = clock_get_ticks();
                if (now < deadline) {
                    uint64_t chunk = now + MS_TO_TICKS(20);
                    if (chunk > deadline) chunk = deadline;
                    proc_wait_token_t token =
                        proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, chunk);
                    wait_queue_entry_t entry = {0};
                    wait_queue_link(&inst->waiters, &entry, token, 0);
                    arch_dma_sync_for_cpu((void *)used, sizeof(*used));
                    if (used->idx != used_before) {
                        wait_queue_unlink(&inst->waiters, &entry);
                        (void)proc_park_cancel(token);
                        proc_park_finish(token);
                        completed = 1;
                        break;
                    }
                    (void)proc_park_commit(token);
                    wait_queue_unlink(&inst->waiters, &entry);
                    proc_park_finish(token);
                }
            } else {
                proc_yield();
            }
        }
    }
    if (!completed) {
        mutex_unlock(&inst->command_lock);
        return -1;
    }
    uint16_t ring_idx = (uint16_t)(used_before % VIRTIO_GPU_QUEUE_SIZE);
    arch_dma_sync_for_cpu(inst->big_resp, resp_len);
    if (used->ring[ring_idx].id != slot) {
        mutex_unlock(&inst->command_lock);
        return -1;
    }
    memcpy(resp, inst->big_resp, resp_len);
    uint32_t isr = inst->vt.read32(&inst->vt, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!inst->vt.legacy && isr)
        inst->vt.write32(&inst->vt, VIRTIO_MMIO_INTERRUPT_ACK, isr);
    inst->last_used = used->idx;
    inst->desc_idx = 0;
    mutex_unlock(&inst->command_lock);
    return 0;
}

/* ---- virtio-gpu 3D helpers ------------------------------------------ */

/* Query capset info by index (VIRTIO_GPU_CMD_GET_CAPSET_INFO).  Returns the
 * capset id / version / size, or -1 if unavailable. */
static int virtio_gpu_get_capset_info(virtio_gpu_inst_t *inst, uint32_t index,
                                      uint32_t *id, uint32_t *max_version,
                                      uint32_t *max_size)
{
    struct virtio_gpu_get_capset_info req ALIGNED(64);
    struct virtio_gpu_resp_capset_info resp ALIGNED(64);
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    req.capset_index = index;
    if (virtio_gpu_send_cmd(inst, &req, sizeof(req), &resp, sizeof(resp)) < 0 ||
        resp.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO)
        return -1;
    *id = resp.capset_id;
    *max_version = resp.capset_max_version;
    *max_size = resp.capset_max_size;
    return 0;
}

/* Fetch the raw capset blob (VIRTIO_GPU_CMD_GET_CAPSET) into buf.  Backs
 * DRM_IOCTL_VIRTGPU_GET_CAPS, which is how the capset reaches userspace.
 *
 * hdr.ctx_id must name a live context: the host answers
 * VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER (0x1205) when it is 0, which is what
 * a zeroed request struct would otherwise send.
 *
 * *out_len is the number of bytes actually written, which may be below bufsz
 * because the host, not the caller, decides how big a capset is.  It cannot
 * be left implicit: callers copy the blob straight to userspace, and the
 * previous fixed 4 KiB staging buffer reported success after filling only its
 * own 4 KiB, so a 1 MiB caller buffer was copied out with a megabyte of
 * untouched heap behind the capset. */
static int virtio_gpu_get_capset(virtio_gpu_inst_t *inst, uint32_t ctx_id,
                                 uint32_t capset_id, uint32_t version,
                                 void *buf, size_t bufsz, size_t *out_len)
{
    if (!buf || bufsz == 0)
        return -EINVAL;

    /* GET_CAPSET_INFO is indexed while GET_CAPSET is not, so the id has to be
     * looked up in the info table before it can be sent -- both to learn how big
     * the blob is (a constant here is what truncated every capset over 4 KiB)
     * and to reject an id the host does not advertise, which otherwise comes
     * back as 0x1205 and reads like a renderer that cannot make a context. */
    uint32_t info_ver = 0, info_size = 0;
    int info_rc = -1;
    for (uint32_t idx = 0; idx < 16; idx++) {
        uint32_t id = 0, ver = 0, sz = 0;
        if (virtio_gpu_get_capset_info(inst, idx, &id, &ver, &sz) < 0)
            break;
        if (id == capset_id) {
            info_ver = ver;
            info_size = sz;
            info_rc = 0;
            break;
        }
    }
    if (info_rc < 0)
        return -ENOENT;

    /* The host fills the version actually asked for, and rejects one it does
     * not have.  A client that says 0 means "whatever you have". */
    if (version == 0 || version > info_ver)
        version = info_ver;

    size_t want = bufsz;
    if (info_size > 0 && info_size < want)
        want = info_size;
    if (want > VIRTIO_GPU_MAX_CAPSET_BYTES)
        want = VIRTIO_GPU_MAX_CAPSET_BYTES;

    struct virtio_gpu_get_capset req ALIGNED(64);
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    req.hdr.ctx_id = ctx_id;
    req.capset_id = capset_id;
    req.capset_version = version;

    size_t rsz = sizeof(struct virtio_gpu_ctrl_hdr) + want;
    uint8_t *resp = kmalloc(rsz);
    if (!resp)
        return -ENOMEM;

    int rc = virtio_gpu_send_cmd_big(inst, &req, sizeof(req), resp, rsz);
    if (rc < 0) {
        kinfo("[GPU] get_capset: send_cmd_big failed rc=%d\n", rc);
        kfree(resp);
        return rc;
    }
    struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)resp;
    if (hdr->type != VIRTIO_GPU_RESP_OK_CAPSET) {
        kinfo("[GPU] get_capset: resp=0x%x want=0x%x | sent ctx=%u capset_id=%u"
              " ver=%u | host id=%u max_ver=%u size=%u\n",
              hdr->type, VIRTIO_GPU_RESP_OK_CAPSET, ctx_id, capset_id, version,
              capset_id, info_ver, info_size);
        kfree(resp);
        return -1;
    }
    memcpy(buf, resp + sizeof(struct virtio_gpu_ctrl_hdr), want);
    kfree(resp);
    if (out_len)
        *out_len = want;
    return 0;
}

static int gpu_get_info(struct device *dev, uint32_t *width, uint32_t *height, uint32_t *bpp) {
    virtio_gpu_inst_t *inst = dev->drv_priv;
    *width = inst->width;
    *height = inst->height;
    *bpp = inst->bpp;
    return 0;
}

static int gpu_get_fb(struct device *dev, uintptr_t *fb_paddr, size_t *fb_size) {
    virtio_gpu_inst_t *inst = dev->drv_priv;
    *fb_paddr = inst->fb_phys;
    *fb_size = inst->fb_size;
    return 0;
}

static int gpu_get_edid(struct device *dev, uint8_t *buf, size_t cap) {
    virtio_gpu_inst_t *inst = dev->drv_priv;
    if (!inst || !inst->valid || !inst->edid_valid)
        return -ENODEV;
    if (cap < sizeof(inst->edid))
        return -EINVAL;
    memcpy(buf, inst->edid, sizeof(inst->edid));
    return (int)sizeof(inst->edid);
}

/* Fetch the scanout-0 EDID block from the device (VIRTIO_GPU_F_EDID).  The
 * response exceeds the fixed 128-byte command slot, so it goes through the
 * large-command path.  Caches the 128-byte base block on success. */
static void virtio_gpu_fetch_edid(virtio_gpu_inst_t *inst) {
    if (!inst->has_edid)
        return;
    struct virtio_gpu_resp_edid *resp = kmalloc(sizeof(*resp));
    if (!resp)
        return;
    struct virtio_gpu_get_edid req;
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_GET_EDID;
    req.scanout = 0;
    memset(resp, 0, sizeof(*resp));
    if (virtio_gpu_send_cmd_big(inst, &req, sizeof(req), resp, sizeof(*resp)) == 0 &&
        resp->hdr.type == VIRTIO_GPU_RESP_OK_EDID && resp->size >= 128) {
        memcpy(inst->edid, resp->edid, sizeof(inst->edid));
        inst->edid_valid = 1;
    }
    kfree(resp);
}

static int gpu_flush(struct device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    virtio_gpu_inst_t *inst = dev->drv_priv;
    
    if (w == 0 || h == 0) {
        x = 0; y = 0;
        w = inst->width; h = inst->height;
    }

    if (x >= inst->width || y >= inst->height)
        return -1;
    if (w > inst->width - x)
        w = inst->width - x;
    if (h > inst->height - y)
        h = inst->height - y;

    size_t offset = ((size_t)y * inst->width + x) * (inst->bpp / 8);
    size_t bytes = ((size_t)(h - 1) * inst->width + w) * (inst->bpp / 8);
    pfn_t fb_pfn = phys_to_pfn(inst->fb_phys);
    void *fb_virt = pfn_to_virt(fb_pfn);
    if (!fb_virt)
        return -1;
    arch_dma_sync_for_device((uint8_t *)fb_virt + offset, bytes);
    
    struct virtio_gpu_transfer_to_host_2d t2h ALIGNED(64);
    memset(&t2h, 0, sizeof(t2h));
    t2h.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    t2h.r.x = x;
    t2h.r.y = y;
    t2h.r.width = w;
    t2h.r.height = h;
    t2h.offset = offset;
    t2h.resource_id = 1;
    
    struct virtio_gpu_ctrl_hdr resp ALIGNED(64);
    memset(&resp, 0, sizeof(resp));
    
    if (virtio_gpu_send_cmd(inst, &t2h, sizeof(t2h), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;
    
    struct virtio_gpu_resource_flush flush ALIGNED(64);
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.x = x;
    flush.r.y = y;
    flush.r.width = w;
    flush.r.height = h;
    flush.resource_id = 1;
    
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &flush, sizeof(flush), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;
    
    return 0;
}

static int virtio_gpu_ctx_create(virtio_gpu_inst_t *inst, uint32_t ctx_id,
                                 uint32_t context_init,
                                 const char *name, size_t nlen);
static int virtio_gpu_ctx_destroy(virtio_gpu_inst_t *inst, uint32_t ctx_id);
static int virtio_gpu_resource_create_3d(virtio_gpu_inst_t *inst,
                                         uint32_t ctx_id,
                                         uint32_t resource_id,
                                         uint32_t target, uint32_t format,
                                         uint32_t bind, uint32_t width,
                                         uint32_t height, uint32_t depth,
                                         uint32_t array_size,
                                         uint32_t last_level,
                                         uint32_t nr_samples, uint32_t flags);
static int virtio_gpu_submit_3d(virtio_gpu_inst_t *inst, uint32_t ctx_id,
                                const void *cmdbuf, size_t len);
static int virtio_gpu_resource_unref(virtio_gpu_inst_t *inst, uint32_t resource_id);
static int virtio_gpu_ctx_attach_resource(virtio_gpu_inst_t *inst, uint32_t ctx_id,
                                          uint32_t resource_id);

/* VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D: pull a rendered region back into the
 * guest's own pages.
 *
 * The host renders into its own GL object, so a submit alone leaves the guest's
 * buffer holding whatever it held before.  Reading it directly is not a race and
 * not a coherency problem -- the bytes were never written.  The mem entries are
 * mandatory here: this command is what tells the host which guest frames to
 * deposit into, so a copy without them has no destination. */
static int virtio_gpu_transfer_from_host_3d(virtio_gpu_inst_t *inst, uint32_t ctx_id,
                                            uint32_t resource_id,
                                            const struct virtio_gpu_box *box,
                                            uint32_t level, uint32_t stride,
                                            uint32_t layer_stride, uint64_t offset,
                                            const struct virtio_gpu_mem_entry *entries,
                                            uint32_t nr_entries)
{
    if (!inst->virgl)
        return -ENXIO;
    if (!box || !entries || nr_entries == 0)
        return -EINVAL;

    /* The trailing entry list makes the request variable-length, which is what
     * the big sender is for; a fixed descriptor would advertise a body shorter
     * than the one the device is about to parse. */
    size_t body = sizeof(struct virtio_gpu_transfer_from_host_3d) +
                  (size_t)nr_entries * sizeof(struct virtio_gpu_mem_entry);
    uint8_t *req = kmalloc(body);
    if (!req)
        return -ENOMEM;

    struct virtio_gpu_transfer_from_host_3d *hdr =
        (struct virtio_gpu_transfer_from_host_3d *)req;
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.type = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    hdr->hdr.ctx_id = ctx_id;
    hdr->box = *box;
    hdr->offset = offset;
    hdr->resource_id = resource_id;
    hdr->level = level;
    hdr->stride = stride;
    hdr->layer_stride = layer_stride;
    memcpy(req + sizeof(*hdr), entries,
           (size_t)nr_entries * sizeof(*entries));

    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    int rc = virtio_gpu_send_cmd_big(inst, req, body, &resp, sizeof(resp));
    kfree(req);
    if (rc < 0)
        return rc;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA ? 0 : -EIO;
}

/* ---- virtio-gpu 3D command wrappers (virgl passthrough) ---------------- */

/* VIRTIO_GPU_CMD_CTX_CREATE: create a virgl rendering context. */
static int virtio_gpu_ctx_create(virtio_gpu_inst_t *inst, uint32_t ctx_id,
                                 uint32_t context_init,
                                 const char *name, size_t nlen)
{
    if (!inst->virgl)
        return -ENXIO;
    struct virtio_gpu_ctx_create req ALIGNED(64);
    struct virtio_gpu_ctrl_hdr resp ALIGNED(64);
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    req.hdr.ctx_id = ctx_id;
    req.nlen = (uint32_t)nlen;
    req.context_init = context_init;
    if (name && nlen) {
        size_t c = nlen < sizeof(req.debug_name) ? nlen : sizeof(req.debug_name);
        memcpy(req.debug_name, name, c);
    }
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &req, sizeof(req), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -EIO;
    return 0;
}

/* VIRTIO_GPU_CMD_CTX_DESTROY */
static int virtio_gpu_ctx_destroy(virtio_gpu_inst_t *inst, uint32_t ctx_id)
{
    struct virtio_gpu_ctx_destroy req ALIGNED(64);
    struct virtio_gpu_ctrl_hdr resp ALIGNED(64);
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    req.hdr.ctx_id = ctx_id;
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &req, sizeof(req), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -EIO;
    return 0;
}

/* VIRTIO_GPU_CMD_RESOURCE_CREATE_3D */
static int virtio_gpu_resource_create_3d(virtio_gpu_inst_t *inst,
                                         uint32_t ctx_id,
                                         uint32_t resource_id,
                                         uint32_t target, uint32_t format,
                                         uint32_t bind, uint32_t width,
                                         uint32_t height, uint32_t depth,
                                         uint32_t array_size,
                                         uint32_t last_level,
                                         uint32_t nr_samples, uint32_t flags)
{
    if (!inst->virgl)
        return -ENXIO;
    struct virtio_gpu_resource_create_3d req ALIGNED(64);
    struct virtio_gpu_ctrl_hdr resp ALIGNED(64);
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    req.hdr.ctx_id = ctx_id;
    req.resource_id = resource_id;
    req.target = target;
    req.format = format;
    req.bind = bind;
    req.width = width;
    req.height = height;
    req.depth = depth;
    req.array_size = array_size;
    req.last_level = last_level;
    req.nr_samples = nr_samples;
    req.flags = flags;
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &req, sizeof(req), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -EIO;
    return 0;
}

/* VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE: make a resource reachable from a context.
 *
 * RESOURCE_CREATE_3D only allocates the host-side object.  virglrenderer looks a
 * resource up by walking the context's own list, so until this command is sent
 * every command naming that resource fails as an illegal resource -- and the
 * failure is silent from the guest's side, because the host still answers
 * SUBMIT_3D with OK. */
static int virtio_gpu_ctx_attach_resource(virtio_gpu_inst_t *inst, uint32_t ctx_id,
                                          uint32_t resource_id)
{
    if (!inst->virgl)
        return -ENXIO;
    struct virtio_gpu_ctx_resource req ALIGNED(64);
    struct virtio_gpu_ctrl_hdr resp ALIGNED(64);
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    req.hdr.ctx_id = ctx_id;
    req.resource_id = resource_id;
    req.padding = 0;
    memset(&resp, 0, sizeof(resp));
    int dbg_rc = virtio_gpu_send_cmd(inst, &req, sizeof(req), &resp, sizeof(resp));
    if (dbg_rc < 0 || resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -EIO;
    return 0;
}

/* VIRTIO_GPU_CMD_SUBMIT_3D: forward a virgl command stream blob. */
static int virtio_gpu_submit_3d(virtio_gpu_inst_t *inst, uint32_t ctx_id,
                                const void *cmdbuf, size_t len)
{
    if (!inst->virgl)
        return -ENXIO;
    if (len > VIRTIO_GPU_3D_MAX_CMD_BYTES)
        return -EINVAL;

    /* The command blob must be DMA-visible; stage it in the driver buffer
     * and attach it as backing.  A single mem_entry references the staging
     * buffer. */
    mutex_lock(&inst->command_lock);
    if (len > inst->big_req_cap) {
        uint8_t *nr = kmalloc(len);
        if (!nr) { mutex_unlock(&inst->command_lock); return -ENOMEM; }
        if (inst->big_req) kfree(inst->big_req);
        inst->big_req = nr;
        inst->big_req_cap = len;
    }
    memcpy(inst->big_req, cmdbuf, len);
    arch_dma_sync_for_device(inst->big_req, len);

    memset(&inst->submit_hdr, 0, sizeof(inst->submit_hdr));
    inst->submit_hdr.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    inst->submit_hdr.hdr.ctx_id = ctx_id;
    inst->submit_hdr.size = (uint32_t)len;
    arch_dma_sync_for_device(&inst->submit_hdr, sizeof(inst->submit_hdr));
    memset(&inst->submit_resp, 0, sizeof(inst->submit_resp));

    /* Three-descriptor chain: header (read), command blob (read), response
     * (write).  The host skips sizeof(struct virtio_gpu_cmd_submit) bytes into
     * this chain before reading the command, so the header descriptor must be
     * exactly that structure -- see the submit_hdr declaration. */
    uint16_t s0 = 0, s1 = 1, s2 = 2;
    inst->desc[s0].addr  = va_to_pa(&inst->submit_hdr);
    inst->desc[s0].len   = sizeof(inst->submit_hdr);
    inst->desc[s0].flags = VIRTQ_DESC_F_NEXT;
    inst->desc[s0].next  = s1;
    inst->desc[s1].addr  = va_to_pa(inst->big_req);
    inst->desc[s1].len   = (uint32_t)len;
    inst->desc[s1].flags = VIRTQ_DESC_F_NEXT;
    inst->desc[s1].next  = s2;
    inst->desc[s2].addr  = va_to_pa(&inst->submit_resp);
    inst->desc[s2].len   = sizeof(inst->submit_resp);
    inst->desc[s2].flags = VIRTQ_DESC_F_WRITE;
    inst->desc[s2].next  = 0;
    arch_dma_sync_for_device(&inst->desc[0], sizeof(virtq_desc_t));
    arch_dma_sync_for_device(&inst->desc[1], sizeof(virtq_desc_t));
    arch_dma_sync_for_device(&inst->desc[2], sizeof(virtq_desc_t));

    arch_dma_sync_for_cpu(&inst->used, sizeof(inst->used));
    uint16_t used_before = ((volatile virtq_used_t *)&inst->used)->idx;
    uint16_t avail_slot = inst->avail.idx % VIRTIO_GPU_QUEUE_SIZE;
    inst->avail.ring[avail_slot] = s0;
    wmb();
    inst->avail.idx++;
    wmb();
    arch_dma_sync_for_device(&inst->avail, sizeof(inst->avail));
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();

    volatile virtq_used_t *used = &inst->used;
    uint64_t start = clock_get_ticks();
    uint64_t frequency = clock_ticks_per_sec();
    uint64_t deadline = (start && frequency) ? start + frequency : 0;
    uint32_t spins = 100000000U;
    int completed = 0;
    while (spins--) {
        arch_dma_sync_for_cpu((void *)used, sizeof(*used));
        if (used->idx != used_before) { completed = 1; break; }
        if (deadline && clock_get_ticks() >= deadline)
            break;
        arch_cpu_relax();
        /* Parked on inst->waiters rather than spinning: a virgl submit that
         * takes longer than one 20 ms chunk would otherwise keep a vCPU busy
         * while holding command_lock, starving the host backend thread that
         * has to consume this very descriptor.  Bounded chunks keep a lost
         * completion interrupt from parking forever. */
        if ((spins & 0xffffU) == 0 && proc_current()) {
            if (inst->irq_registered && deadline) {
                uint64_t now = clock_get_ticks();
                if (now < deadline) {
                    uint64_t chunk = now + MS_TO_TICKS(20);
                    if (chunk > deadline) chunk = deadline;
                    proc_wait_token_t token =
                        proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, chunk);
                    wait_queue_entry_t entry = {0};
                    wait_queue_link(&inst->waiters, &entry, token, 0);
                    arch_dma_sync_for_cpu((void *)used, sizeof(*used));
                    if (used->idx != used_before) {
                        wait_queue_unlink(&inst->waiters, &entry);
                        (void)proc_park_cancel(token);
                        proc_park_finish(token);
                        completed = 1;
                        break;
                    }
                    (void)proc_park_commit(token);
                    wait_queue_unlink(&inst->waiters, &entry);
                    proc_park_finish(token);
                }
            } else {
                proc_yield();
            }
        }
    }
    if (!completed) {
        mutex_unlock(&inst->command_lock);
        return -EIO;
    }
    uint16_t ring_idx = (uint16_t)(used_before % VIRTIO_GPU_QUEUE_SIZE);
    arch_dma_sync_for_cpu(&inst->submit_resp, sizeof(inst->submit_resp));
    if (used->ring[ring_idx].id != s0) {
        mutex_unlock(&inst->command_lock);
        return -EIO;
    }
    if (inst->submit_resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        mutex_unlock(&inst->command_lock);
        return -EIO;
    }
    uint32_t isr = inst->vt.read32(&inst->vt, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!inst->vt.legacy && isr)
        inst->vt.write32(&inst->vt, VIRTIO_MMIO_INTERRUPT_ACK, isr);
    inst->last_used = used->idx;
    inst->desc_idx = 0;
    mutex_unlock(&inst->command_lock);
    return 0;
}

/* VIRTIO_GPU_CMD_RESOURCE_UNREF */
static int virtio_gpu_resource_unref(virtio_gpu_inst_t *inst, uint32_t resource_id)
{
    struct {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t resource_id;
        uint32_t padding;
    } req ALIGNED(64);
    struct virtio_gpu_ctrl_hdr resp ALIGNED(64);
    memset(&req, 0, sizeof(req));
    req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    req.resource_id = resource_id;
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &req, sizeof(req), &resp, sizeof(resp)) < 0)
        return -EIO;
    return 0;
}

/* VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: hand the host the physical pages
 * backing a resource.  Without this a 3D resource has no memory on the host
 * side, so a submitted command stream would operate on nothing.  The command
 * is variable-length (one mem_entry per page), hence the big send path. */
static int virtio_gpu_resource_attach_backing(virtio_gpu_inst_t *inst,
                                              uint32_t resource_id,
                                              const struct virtio_gpu_mem_entry *entries,
                                              uint32_t nr_entries)
{
    if (!inst->virgl)
        return -ENXIO;
    if (!entries || nr_entries == 0)
        return -EINVAL;

    size_t body = sizeof(struct virtio_gpu_resource_attach_backing) +
                  (size_t)nr_entries * sizeof(struct virtio_gpu_mem_entry);

    /* command_lock has to cover every access to the shared big_req staging
     * buffer, not only the descriptor/avail work below.  big_req is a single
     * buffer for the whole instance and virtio_gpu_send_cmd_big() and
     * virtio_gpu_submit_3d() grow, free and refill it under this same lock;
     * ATTACH_BACKING arrives once per DRM_IOCTL_VIRTGPU_EXECBUFFER buffer, so
     * two clients reach this point concurrently.  Growing and filling the body
     * before the lock let a second caller kfree() the buffer underneath the
     * first caller's memcpy, and let two mem_entry tables land in one request
     * body that a single descriptor then advertises to the device. */
    mutex_lock(&inst->command_lock);
    if (body > inst->big_req_cap) {
        uint8_t *nb = kmalloc(body);
        if (!nb) { mutex_unlock(&inst->command_lock); return -ENOMEM; }
        if (inst->big_req)
            kfree(inst->big_req);
        inst->big_req = nb;
        inst->big_req_cap = body;
    }

    /* Body filled after the resize so the s0 descriptor below never advertises
     * the pointer kfree() just dropped. */
    struct virtio_gpu_resource_attach_backing *hdr =
        (struct virtio_gpu_resource_attach_backing *)inst->big_req;
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    hdr->resource_id = resource_id;
    hdr->nr_entries = nr_entries;
    memcpy(inst->big_req + sizeof(*hdr), entries,
           (size_t)nr_entries * sizeof(*entries));
    arch_dma_sync_for_device(inst->big_req, body);

    if (inst->big_resp_cap < sizeof(struct virtio_gpu_ctrl_hdr)) {
        uint8_t *nb = kmalloc(sizeof(struct virtio_gpu_ctrl_hdr));
        if (!nb) { mutex_unlock(&inst->command_lock); return -ENOMEM; }
        if (inst->big_resp) kfree(inst->big_resp);
        inst->big_resp = nb;
        inst->big_resp_cap = sizeof(struct virtio_gpu_ctrl_hdr);
    }
    struct virtio_gpu_ctrl_hdr *resp =
        (struct virtio_gpu_ctrl_hdr *)inst->big_resp;
    memset(resp, 0, sizeof(*resp));

    uint16_t s0 = 0, s1 = 1;
    inst->desc[s0].addr  = va_to_pa(inst->big_req);
    inst->desc[s0].len   = (uint32_t)body;
    inst->desc[s0].flags = VIRTQ_DESC_F_NEXT;
    inst->desc[s0].next  = s1;
    inst->desc[s1].addr  = va_to_pa(resp);
    inst->desc[s1].len   = (uint32_t)sizeof(*resp);
    inst->desc[s1].flags = VIRTQ_DESC_F_WRITE;
    inst->desc[s1].next  = 0;
    arch_dma_sync_for_device(inst->desc, sizeof(virtq_desc_t) * 2);
    arch_dma_sync_for_device(resp, sizeof(*resp));

    arch_dma_sync_for_cpu(&inst->used, sizeof(inst->used));
    uint16_t used_before = ((volatile virtq_used_t *)&inst->used)->idx;
    uint16_t avail_slot = inst->avail.idx % VIRTIO_GPU_QUEUE_SIZE;
    inst->avail.ring[avail_slot] = s0;
    wmb();
    inst->avail.idx++;
    wmb();
    arch_dma_sync_for_device(&inst->avail, sizeof(inst->avail));
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();

    volatile virtq_used_t *used = &inst->used;
    uint64_t start = clock_get_ticks();
    uint64_t frequency = clock_ticks_per_sec();
    uint64_t deadline = (start && frequency) ? start + frequency : 0;
    uint32_t spins = 100000000U;
    int completed = 0;
    while (spins--) {
        arch_dma_sync_for_cpu((void *)used, sizeof(*used));
        if (used->idx != used_before) { completed = 1; break; }
        /* Same one-second bound the other completion loops use: an absent
         * deadline let a device that never returns this entry spin the full
         * 100M iterations with command_lock held, blocking every later
         * command outright instead of failing the call. */
        if (deadline && clock_get_ticks() >= deadline)
            break;
        arch_cpu_relax();
        if ((spins & 0xffffU) == 0 && proc_current()) {
            if (inst->irq_registered && deadline) {
                uint64_t now = clock_get_ticks();
                if (now < deadline) {
                    uint64_t chunk = now + MS_TO_TICKS(20);
                    if (chunk > deadline) chunk = deadline;
                    proc_wait_token_t token =
                        proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, chunk);
                    wait_queue_entry_t entry = {0};
                    wait_queue_link(&inst->waiters, &entry, token, 0);
                    arch_dma_sync_for_cpu((void *)used, sizeof(*used));
                    if (used->idx != used_before) {
                        wait_queue_unlink(&inst->waiters, &entry);
                        (void)proc_park_cancel(token);
                        proc_park_finish(token);
                        completed = 1;
                        break;
                    }
                    (void)proc_park_commit(token);
                    wait_queue_unlink(&inst->waiters, &entry);
                    proc_park_finish(token);
                }
            } else {
                proc_yield();
            }
        }
    }
    if (!completed) {
        mutex_unlock(&inst->command_lock);
        return -EIO;
    }
    uint16_t ring_idx = (uint16_t)(used_before % VIRTIO_GPU_QUEUE_SIZE);
    arch_dma_sync_for_cpu(resp, sizeof(*resp));
    int ok = (used->ring[ring_idx].id == s0) &&
             (resp->type == VIRTIO_GPU_RESP_OK_NODATA);
    uint32_t isr = inst->vt.read32(&inst->vt, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!inst->vt.legacy && isr)
        inst->vt.write32(&inst->vt, VIRTIO_MMIO_INTERRUPT_ACK, isr);
    inst->last_used = used->idx;
    inst->desc_idx = 0;
    mutex_unlock(&inst->command_lock);
    return ok ? 0 : -EIO;
}

static int gpu_capset_info(struct device *dev, uint32_t index,
                           uint32_t *id, uint32_t *max_version, uint32_t *max_size)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    return virtio_gpu_get_capset_info(inst, index, id, max_version, max_size);
}

static int gpu_get_features(struct device *dev, uint32_t *out_3d,
                            uint32_t *out_context_init)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    if (out_3d)
        *out_3d = inst->virgl ? 1u : 0u;
    if (out_context_init)
        *out_context_init = inst->context_init ? 1u : 0u;
    return 0;
}

static int gpu_get_capset(struct device *dev, uint32_t ctx_id, uint32_t index,
                          uint32_t version, void *buf, size_t len,
                          size_t *out_len)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    return virtio_gpu_get_capset(inst, ctx_id, index, version, buf, len, out_len);
}

static int gpu_resource_attach_backing(struct device *dev, uint32_t resource_id,
                                       const struct virtio_gpu_mem_entry *entries,
                                       uint32_t nr_entries)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    return virtio_gpu_resource_attach_backing(inst, resource_id, entries, nr_entries);
}

static int gpu_ctx_create(struct device *dev, uint32_t ctx_id, uint32_t context_init,
                          const char *name, size_t nlen)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    return virtio_gpu_ctx_create(inst, ctx_id, context_init, name, nlen);
}

static int gpu_ctx_destroy(struct device *dev, uint32_t ctx_id)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    return virtio_gpu_ctx_destroy(inst, ctx_id);
}

static int gpu_res_create_3d(struct device *dev, uint32_t ctx_id,
                             uint32_t resource_id, uint32_t target, uint32_t format,
                             uint32_t bind, uint32_t width, uint32_t height,
                             uint32_t depth, uint32_t array_size,
                             uint32_t last_level, uint32_t nr_samples, uint32_t flags)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    return virtio_gpu_resource_create_3d(inst, ctx_id, resource_id, target, format,
                                         bind, width, height, depth, array_size,
                                         last_level, nr_samples, flags);
}

static int gpu_res_unref(struct device *dev, uint32_t resource_id)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    return virtio_gpu_resource_unref(inst, resource_id);
}

static int gpu_submit_3d(struct device *dev, uint32_t ctx_id,
                         const void *cmdbuf, size_t len)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    return virtio_gpu_submit_3d(inst, ctx_id, cmdbuf, len);
}

static int gpu_ctx_attach_resource(struct device *dev, uint32_t ctx_id,
                                   uint32_t resource_id)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    return virtio_gpu_ctx_attach_resource(inst, ctx_id, resource_id);
}

static int gpu_transfer_from_host_3d(struct device *dev, uint32_t ctx_id,
                                     uint32_t resource_id,
                                     const struct virtio_gpu_box *box,
                                     uint32_t level, uint32_t stride,
                                     uint32_t layer_stride, uint64_t offset,
                                     const struct virtio_gpu_mem_entry *entries,
                                     uint32_t nr_entries)
{
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return -ENODEV;
    return virtio_gpu_transfer_from_host_3d(inst, ctx_id, resource_id, box, level,
                                            stride, layer_stride, offset, entries,
                                            nr_entries);
}

static const gpu_dev_ops_t gpu_ops = {
    .get_info = gpu_get_info,
    .get_fb   = gpu_get_fb,
    .flush    = gpu_flush,
    .get_edid = gpu_get_edid,
    .get_capset = gpu_get_capset,
    .capset_info = gpu_capset_info,
    .get_features = gpu_get_features,
    .resource_attach_backing = gpu_resource_attach_backing,
    .ctx_create = gpu_ctx_create,
    .ctx_destroy = gpu_ctx_destroy,
    .resource_create_3d = gpu_res_create_3d,
    .resource_unref = gpu_res_unref,
    .ctx_attach_resource = gpu_ctx_attach_resource,
    .transfer_from_host_3d = gpu_transfer_from_host_3d,
    .submit_3d = gpu_submit_3d,
};

static void virtio_gpu_release_buffers(virtio_gpu_inst_t *inst);

static int virtio_gpu_init_transport(device_t *dev, const virtio_transport_t *transport) {
    virtio_gpu_inst_t *inst = &g_gpu_inst;
    int order = 0;
    pfn_t fb_pfn = PFN_NONE;
    memset(inst, 0, sizeof(*inst));
    mutex_init(&inst->command_lock);
    
    inst->vt = *transport;
    
    virtio_transport_t *vt = &inst->vt;

    uint32_t magic = vt->read32(vt, VIRTIO_MMIO_MAGIC);
    uint32_t version = vt->read32(vt, VIRTIO_MMIO_VERSION);
    uint32_t device_id = vt->read32(vt, VIRTIO_MMIO_DEVICE_ID);
    if (magic != 0x74726976U || version != 2U || device_id != 16U)
        return -1;
    
    vt->write32(vt, VIRTIO_MMIO_STATUS, 0);
    mb();
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    
    vt->write32(vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint32_t features_lo = vt->read32(vt, VIRTIO_MMIO_DEVICE_FEATURES);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    /* Advertise the 2D display features plus virgl (3D) if the host offers
     * it; the virtio-gpu-gl device enables VIRTIO_GPU_F_VIRGL. */
    uint32_t driver_lo = 0;
    if (features_lo & (1U << VIRTIO_GPU_F_VIRGL))
        driver_lo |= (1U << VIRTIO_GPU_F_VIRGL);
    if (features_lo & (1U << VIRTIO_GPU_F_EDID))
        driver_lo |= (1U << VIRTIO_GPU_F_EDID);
    /* CONTEXT_INIT has to be *negotiated*, not merely observed.  The host only
     * honours the context_init field of CTX_CREATE when the bit is in the
     * driver's feature set, so reading it from the device's bits and leaving it
     * out of ours made GETPARAM report CONTEXT_INIT=1 while every context was
     * created the legacy way. */
    if (features_lo & (1U << VIRTIO_GPU_F_CONTEXT_INIT))
        driver_lo |= (1U << VIRTIO_GPU_F_CONTEXT_INIT);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES, driver_lo);
    inst->virgl = (driver_lo & (1U << VIRTIO_GPU_F_VIRGL)) != 0;
    inst->context_init = (driver_lo & (1U << VIRTIO_GPU_F_CONTEXT_INIT)) != 0;
    inst->has_edid = (driver_lo & (1U << VIRTIO_GPU_F_EDID)) != 0;
    
    vt->write32(vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = vt->read32(vt, VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(features_hi & VIRTIO_F_VERSION_1_BIT))
        goto fail;
    uint32_t driver_hi = features_hi & VIRTIO_F_VERSION_1_BIT;
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES, driver_hi);
    mb();
    
    status |= VIRTIO_STATUS_FEATURES_OK;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    if (!(vt->read32(vt, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
        goto fail;
    
    // Setup controlq (queue 0)
    vt->write32(vt, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = vt->read32(vt, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax < VIRTIO_GPU_QUEUE_SIZE)
        goto fail;
    if (vt->read32(vt, VIRTIO_MMIO_QUEUE_READY) != 0)
        goto fail;
    vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_GPU_QUEUE_SIZE);

    memset(inst->desc, 0, sizeof(inst->desc));
    memset(&inst->avail, 0, sizeof(inst->avail));
    memset(&inst->used, 0, sizeof(inst->used));
    arch_dma_sync_for_device(inst->desc, sizeof(inst->desc));
    arch_dma_sync_for_device(&inst->avail, sizeof(inst->avail));
    arch_dma_sync_for_device(&inst->used, sizeof(inst->used));
    
    uint64_t desc_pa  = va_to_pa(inst->desc);
    uint64_t avail_pa = va_to_pa(&inst->avail);
    uint64_t used_pa  = va_to_pa(&inst->used);
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_LOW,   (uint32_t)(desc_pa));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_HIGH,  (uint32_t)(desc_pa >> 32));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)(avail_pa));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,(uint32_t)(avail_pa >> 32));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)(used_pa));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,(uint32_t)(used_pa >> 32));
    mb();
    vt->write32(vt, VIRTIO_MMIO_QUEUE_READY, 1);
    mb();
    
    status |= VIRTIO_STATUS_DRIVER_OK;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();

    wait_queue_init(&inst->waiters);
    if (vt->irq >= 0) {
        unsigned long irq_flags = vt->shared_irq ? IRQF_SHARED : 0;
        if (request_irq((uint32_t)vt->irq, virtio_gpu_irq_handler,
                        irq_flags, inst) == 0) {
            inst->irq_registered = 1;
        } else {
            /* Never park on an interrupt that will not arrive; the bounded
             * poll/yield path remains the completion model. */
            kinfo("[GPU] IRQ %d registration failed; using polling\n",
                  vt->irq);
            vt->irq = -1;
        }
    }

    /* Ask the host for scanout 0's real geometry.  VIRTIO_GPU_CMD_GET_DISPLAY_INFO
     * was defined here from the start but never sent, so the mode the guest
     * advertises to the compositor was a hardcoded 1024x768 that had nothing to
     * do with the window QEMU was actually showing.  A host that reports zero
     * (headless, or a window smaller than the fallback) leaves the fallback in
     * place rather than producing a zero-sized mode. */
    inst->width = 1024;
    inst->height = 768;
    inst->bpp = 32;
    {
        struct virtio_gpu_resp_display_info *resp =
            kmalloc(sizeof(*resp));
        if (resp) {
            struct virtio_gpu_get_display_info req;
            memset(&req, 0, sizeof(req));
            req.hdr.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
            req.scanout = 0;
            memset(resp, 0, sizeof(*resp));
            if (virtio_gpu_send_cmd(inst, &req, sizeof(req),
                                    resp, sizeof(*resp)) == 0 &&
                resp->hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO &&
                resp->width > 0 && resp->height > 0) {
                inst->width = resp->width;
                inst->height = resp->height;
            }
            kfree(resp);
        }
    }
    inst->fb_size = inst->width * inst->height * (inst->bpp / 8);
    
    // Allocate framebuffer as continuous physical memory
    size_t req_pages = inst->fb_size / PAGE_SIZE + ((inst->fb_size % PAGE_SIZE) ? 1 : 0);
    while ((1UL << order) < req_pages) {
        order++;
    }
    fb_pfn = pfa_alloc(order);
    if (fb_pfn == PFN_NONE) {
        goto fail;
    }
    inst->fb_phys = pfn_to_phys(fb_pfn);
    inst->fb_order = order;
    memset(pfn_to_virt(fb_pfn), 0, (size_t)PAGE_SIZE << order);
    arch_dma_sync_for_device(pfn_to_virt(fb_pfn), inst->fb_size);
    
    struct virtio_gpu_resource_create_2d create ALIGNED(64);
    memset(&create, 0, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = 1;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width = inst->width;
    create.height = inst->height;
    
    struct virtio_gpu_ctrl_hdr resp ALIGNED(64);
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &create, sizeof(create), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        goto fail;
    
    struct {
        struct virtio_gpu_resource_attach_backing req;
        struct virtio_gpu_mem_entry entry;
    } attach ALIGNED(64);
    memset(&attach, 0, sizeof(attach));
    attach.req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.req.resource_id = 1;
    attach.req.nr_entries = 1;
    attach.entry.addr = inst->fb_phys;
    attach.entry.length = inst->fb_size;
    
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &attach, sizeof(attach), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        goto fail;
    
    struct virtio_gpu_set_scanout scanout ALIGNED(64);
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.x = 0;
    scanout.r.y = 0;
    scanout.r.width = inst->width;
    scanout.r.height = inst->height;
    scanout.scanout_id = 0;
    scanout.resource_id = 1;
    
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &scanout, sizeof(scanout), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        goto fail;

    virtio_gpu_fetch_edid(inst);

    dev->drv_priv = inst;

    // Initial FLUSH to transfer data and trigger QEMU window resize
    if (gpu_flush(dev, 0, 0, inst->width, inst->height) < 0) {
        dev->drv_priv = NULL;
        goto fail;
    }

    inst->valid = 1;
    if (gpu_device_register(dev) < 0)
        goto fail;
    if (inst->virgl) {
        uint32_t cid = 0, cver = 0, csize = 0;
        if (virtio_gpu_get_capset_info(inst, 0, &cid, &cver, &csize) == 0) {
            kinfo("[GPU] virtio-gpu 3D (virgl): capset[0] id=%u ver=%u size=%u ctx_init=%d\n",
                  cid, cver, csize, inst->context_init);
        } else {
            kinfo("[GPU] virtio-gpu 3D negotiated but capset query failed\n");
        }
    } else {
        kinfo("[GPU] virtio-gpu 2D only (no VIRGL feature)\n");
    }
    kinfo("[GPU] virtio-gpu ready: %dx%d (FB: %lu MB at 0x%lx)\n", 
          inst->width, inst->height, inst->fb_size/1024/1024, inst->fb_phys);
    return 0;

fail:
    vt->write32(vt, VIRTIO_MMIO_STATUS,
                vt->read32(vt, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FAILED);
    if (inst->irq_registered) {
        free_irq((uint32_t)vt->irq, inst);
    }
    if (fb_pfn != PFN_NONE)
        pfa_free(fb_pfn, order);
    virtio_gpu_release_buffers(inst);
    return -1;
}

static int virtio_gpu_probe(device_t *dev) {
    if (dev->bus == &pci_bus) {
        virtio_transport_t vt;
        if (pci_virtio_transport_init(dev, 16, &vt) != 0)
            return -1;
        return virtio_gpu_init_transport(dev, &vt);
    }

    resource_t *mmio_res = device_get_resource(dev, RES_MMIO, 0);
    if (!mmio_res)
        return -1;

    virtio_transport_t vt = {
        .read32 = virtio_gpu_mmio_read32,
        .write32 = virtio_gpu_mmio_write32,
        .priv = (void *)(uintptr_t)mmio_res->start,
        .legacy = 0,
        .irq = -1,
    };
    resource_t *mmio_irq = device_get_resource(dev, RES_IRQ, 0);
    if (mmio_irq)
        vt.irq = (int)mmio_irq->start;
    return virtio_gpu_init_transport(dev, &vt);
}

/* Release what init_transport allocated, without touching the parts of the
 * instance that other CPUs can be inside.
 *
 * This deliberately does not memset the instance.  command_lock is a mutex that
 * a concurrent transfer or submit may be blocked inside, and waiters is a queue
 * with entries already linked onto it; zeroing either destroys a lock a thread
 * is parked on and a wait chain it is waiting to be woken from.  The instance
 * lives in static storage, so leaving those two initialised is both safe and
 * correct -- this is a teardown, not a reinitialisation.
 *
 * An earlier revision of this comment claimed an in-flight reference count was
 * still needed, on the grounds that a caller already inside the command path
 * would keep running against a reset transport.  Investigated, that is not what
 * happens, and the refcount would have protected against nothing:
 *
 *   - the instance is the file-static g_gpu_inst, so its storage outlives every
 *     caller; "reset transport" cannot be a use-after-free;
 *   - every command entry point re-reads dev->drv_priv, which remove() clears,
 *     and the wrappers answer -ENODEV when it is NULL;
 *   - the ops table is a static const, so the function pointers a racing caller
 *     already loaded stay valid;
 *   - reaching the driver at all requires the device to be the current default,
 *     and unregister() clears that slot first, so drm_gpu_ops() returns NULL and
 *     the ioctl answers -ENODEV before touching the instance.
 *
 * So the ordering that makes this safe is: unregister() (slot) -> unpublish()
 * (which drains class_device_call_begin users) -> release_buffers().  A future
 * change that caches dev or ops across an ioctl, or that calls the driver
 * without going through the default-device slot, would invalidate this and
 * would then need the reference count. */
static void virtio_gpu_release_buffers(virtio_gpu_inst_t *inst)
{
    if (inst->big_req) {
        kfree(inst->big_req);
        inst->big_req = NULL;
        inst->big_req_cap = 0;
    }
    if (inst->big_resp) {
        kfree(inst->big_resp);
        inst->big_resp = NULL;
        inst->big_resp_cap = 0;
    }
    inst->virgl = 0;
    inst->context_init = 0;
    inst->width = 0;
    inst->height = 0;
    inst->bpp = 0;
    inst->fb_phys = 0;
    inst->fb_size = 0;
    inst->fb_order = 0;
    inst->irq_registered = 0;
    inst->last_used = 0;
}

static int virtio_gpu_remove(device_t *dev) {
    virtio_gpu_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return 0;

    /* Device reset stops all interrupt sources before the handler goes. */
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_STATUS, 0);
    mb();
    if (inst->irq_registered)
        free_irq((uint32_t)inst->vt.irq, inst);
    gpu_device_unregister(dev);
    if (inst->fb_phys)
        pfa_free(phys_to_pfn(inst->fb_phys), inst->fb_order);
    dev->drv_priv = NULL;
    virtio_gpu_release_buffers(inst);
    return 0;
}

static const device_id_t virtio_gpu_ids[] = {
    /* VirtIO-MMIO matches the protocol device type, not a PCI device ID. */
    { .vendor = 0, .device = 16,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1050,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1010, .subvendor = VENDOR_ANY, .subdevice = 16 },
    { 0 },
};

static driver_t virtio_gpu_driver = {
    .name       = "virtio-gpu",
    .id_table   = virtio_gpu_ids,
    .bus        = NULL,
    .probe      = virtio_gpu_probe,
    .remove     = virtio_gpu_remove,
    .class_ops  = &gpu_ops,
    .class_type = DEV_CLASS_DISPLAY,
};

DRIVER_REGISTER(virtio_gpu_driver);

struct device *virtio_gpu_get_dev(void) {
    return gpu_device_get_default();
}
