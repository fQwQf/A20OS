/*
 * A20OS NVMe PCI driver
 */
#include "drivers/block/nvme.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/consts.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/sync.h"
#include "mm/slab.h"

#define NVME_REG_CAP   0x00U
#define NVME_REG_INTMS 0x0cU
#define NVME_REG_CC    0x14U
#define NVME_REG_CSTS  0x1cU
#define NVME_REG_AQA   0x24U
#define NVME_REG_ASQ   0x28U
#define NVME_REG_ACQ   0x30U
#define NVME_REG_DBS   0x1000U

#define NVME_CC_EN             1U
#define NVME_CSTS_RDY          1U
#define NVME_CSTS_CFS          2U
#define NVME_ADMIN_CREATE_SQ   0x01U
#define NVME_ADMIN_CREATE_CQ   0x05U
#define NVME_ADMIN_IDENTIFY    0x06U
#define NVME_ADMIN_SET_FEATURE 0x09U
#define NVME_NVM_FLUSH         0x00U
#define NVME_NVM_WRITE         0x01U
#define NVME_NVM_READ          0x02U
#define NVME_FID_NUM_QUEUES    0x07U

#define NVME_ADMIN_DEPTH 16U
#define NVME_IO_DEPTH    64U
#define NVME_DMA_BYTES   (2U * PAGE_SIZE)
#define NVME_TIMEOUT_MS  5000U

typedef struct __attribute__((packed)) nvme_sqe {
    uint32_t cdw0;
    uint32_t nsid;
    uint32_t reserved[2];
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_sqe_t;

typedef struct __attribute__((packed)) nvme_cqe {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} nvme_cqe_t;

_Static_assert(sizeof(nvme_sqe_t) == 64, "NVMe SQE size");
_Static_assert(sizeof(nvme_cqe_t) == 16, "NVMe CQE size");

typedef struct nvme_queue {
    nvme_sqe_t *sq;
    nvme_cqe_t *cq;
    uint64_t sq_dma;
    uint64_t cq_dma;
    volatile uint32_t *sq_db;
    volatile uint32_t *cq_db;
    uint16_t depth;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t next_cid;
    uint8_t phase;
} nvme_queue_t;

typedef struct nvme_controller {
    uintptr_t regs;
    uint32_t stride;
    uint32_t namespace_id;
    uint32_t sector_size;
    uint64_t capacity;
    nvme_queue_t admin;
    nvme_queue_t io;
    void *identify;
    uint64_t identify_dma;
    void *bounce;
    uint64_t bounce_dma;
    mutex_t io_lock;
    volatile int failed;
} nvme_controller_t;

static inline volatile void *nvme_reg(nvme_controller_t *ctrl, uint32_t off)
{
    return (volatile void *)(ctrl->regs + off);
}

static int nvme_wait_ready(nvme_controller_t *ctrl, int ready)
{
    for (unsigned ms = 0; ms < NVME_TIMEOUT_MS; ms++) {
        uint32_t csts = readl(nvme_reg(ctrl, NVME_REG_CSTS));
        if (csts & NVME_CSTS_CFS)
            return -EIO;
        if (!!(csts & NVME_CSTS_RDY) == !!ready)
            return 0;
        mdelay(1);
    }
    return -ETIMEDOUT;
}

static int nvme_queue_alloc(nvme_controller_t *ctrl, nvme_queue_t *q,
                            uint16_t qid, uint16_t depth)
{
    memset(q, 0, sizeof(*q));
    q->sq = dma_alloc_coherent_aligned(PAGE_SIZE, PAGE_SIZE, &q->sq_dma);
    q->cq = dma_alloc_coherent_aligned(PAGE_SIZE, PAGE_SIZE, &q->cq_dma);
    if (!q->sq || !q->cq)
        return -ENOMEM;
    q->depth = depth;
    q->phase = 1;
    uintptr_t db = ctrl->regs + NVME_REG_DBS;
    q->sq_db = (volatile uint32_t *)(db + (2U * qid) * ctrl->stride);
    q->cq_db = (volatile uint32_t *)(db + (2U * qid + 1U) * ctrl->stride);
    return 0;
}

static void nvme_queue_free(nvme_queue_t *q)
{
    if (q->cq)
        dma_free_coherent_aligned(q->cq, PAGE_SIZE, q->cq_dma);
    if (q->sq)
        dma_free_coherent_aligned(q->sq, PAGE_SIZE, q->sq_dma);
    memset(q, 0, sizeof(*q));
}

static int nvme_submit(nvme_controller_t *ctrl, nvme_queue_t *q,
                       const nvme_sqe_t *request,
                       uint32_t *result)
{
    uint16_t tail = q->sq_tail;
    uint16_t cid = q->next_cid++;
    nvme_sqe_t *cmd = &q->sq[tail];
    *cmd = *request;
    cmd->cdw0 = (cmd->cdw0 & 0xffffU) | ((uint32_t)cid << 16);
    dma_sync_for_device(cmd, sizeof(*cmd));
    q->sq_tail = (uint16_t)((tail + 1U) % q->depth);
    writel(q->sq_tail, q->sq_db);

    for (unsigned ms = 0; ms < NVME_TIMEOUT_MS; ms++) {
        nvme_cqe_t *cqe = &q->cq[q->cq_head];
        dma_sync_for_cpu(cqe, sizeof(*cqe));
        if ((cqe->status & 1U) != q->phase) {
            udelay(1000);
            continue;
        }
        uint16_t status = (uint16_t)(cqe->status >> 1);
        uint16_t completed = cqe->cid;
        if (result)
            *result = cqe->result;
        q->cq_head++;
        if (q->cq_head == q->depth) {
            q->cq_head = 0;
            q->phase ^= 1U;
        }
        writel(q->cq_head, q->cq_db);
        if (completed != cid) {
            __atomic_store_n(&ctrl->failed, 1, __ATOMIC_RELEASE);
            return -EIO;
        }
        return status ? -EIO : 0;
    }
    __atomic_store_n(&ctrl->failed, 1, __ATOMIC_RELEASE);
    return -ETIMEDOUT;
}

static int nvme_admin(nvme_controller_t *ctrl, uint8_t opcode,
                      uint32_t nsid, uint64_t prp1, uint32_t cdw10,
                      uint32_t cdw11, uint32_t *result)
{
    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = opcode;
    cmd.nsid = nsid;
    cmd.prp1 = prp1;
    cmd.cdw10 = cdw10;
    cmd.cdw11 = cdw11;
    return nvme_submit(ctrl, &ctrl->admin, &cmd, result);
}

static int nvme_io_cmd(nvme_controller_t *ctrl, uint8_t opcode,
                       uint64_t lba, uint16_t sectors)
{
    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = opcode;
    cmd.nsid = ctrl->namespace_id;
    if (opcode != NVME_NVM_FLUSH) {
        cmd.prp1 = ctrl->bounce_dma;
        if ((uint32_t)sectors * ctrl->sector_size > PAGE_SIZE)
            cmd.prp2 = ctrl->bounce_dma + PAGE_SIZE;
        cmd.cdw10 = (uint32_t)lba;
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = sectors - 1U;
    }
    if (__atomic_load_n(&ctrl->failed, __ATOMIC_ACQUIRE))
        return -EIO;
    return nvme_submit(ctrl, &ctrl->io, &cmd, NULL);
}

static int nvme_identify(nvme_controller_t *ctrl)
{
    memset(ctrl->identify, 0, PAGE_SIZE);
    if (nvme_admin(ctrl, NVME_ADMIN_IDENTIFY, 0, ctrl->identify_dma,
                   1U, 0, NULL) < 0)
        return -EIO;
    dma_sync_for_cpu(ctrl->identify, PAGE_SIZE);
    uint32_t namespaces;
    memcpy(&namespaces, (uint8_t *)ctrl->identify + 516, sizeof(namespaces));
    if (!namespaces)
        return -ENODEV;

    for (uint32_t nsid = 1; nsid <= namespaces; nsid++) {
        memset(ctrl->identify, 0, PAGE_SIZE);
        if (nvme_admin(ctrl, NVME_ADMIN_IDENTIFY, nsid,
                       ctrl->identify_dma, 0, 0, NULL) < 0)
            continue;
        dma_sync_for_cpu(ctrl->identify, PAGE_SIZE);
        uint8_t *id = ctrl->identify;
        uint64_t nsze;
        memcpy(&nsze, id, sizeof(nsze));
        uint8_t flbas = id[26] & 0x0fU;
        uint32_t lbaf = 128U + (uint32_t)flbas * 4U;
        uint16_t metadata_size;
        memcpy(&metadata_size, id + lbaf, sizeof(metadata_size));
        uint8_t ds = id[lbaf + 2U];
        if (!nsze || metadata_size != 0 || ds < 9U || ds > 13U)
            continue;
        ctrl->namespace_id = nsid;
        ctrl->sector_size = 1U << ds;
        ctrl->capacity = nsze;
        return 0;
    }
    return -ENODEV;
}

static int nvme_rw(device_t *dev, uint64_t lba, void *buffer,
                   size_t sectors, int write)
{
    nvme_controller_t *ctrl = dev ? dev->drv_priv : NULL;
    if (!ctrl || !buffer || !sectors || lba >= ctrl->capacity ||
        sectors > ctrl->capacity - lba)
        return -EINVAL;
    uint8_t *cursor = buffer;
    uint32_t max_sectors = NVME_DMA_BYTES / ctrl->sector_size;
    if (!max_sectors)
        return -EOPNOTSUPP;
    mutex_lock(&ctrl->io_lock);
    while (sectors) {
        uint16_t chunk = sectors > max_sectors ? (uint16_t)max_sectors :
                                                  (uint16_t)sectors;
        size_t bytes = (size_t)chunk * ctrl->sector_size;
        if (write) {
            memcpy(ctrl->bounce, cursor, bytes);
        }
        dma_sync_for_device(ctrl->bounce, bytes);
        int ret = nvme_io_cmd(ctrl, write ? NVME_NVM_WRITE : NVME_NVM_READ,
                              lba, chunk);
        if (ret < 0) {
            mutex_unlock(&ctrl->io_lock);
            return ret;
        }
        if (!write) {
            dma_sync_for_cpu(ctrl->bounce, bytes);
            memcpy(cursor, ctrl->bounce, bytes);
        }
        cursor += bytes;
        lba += chunk;
        sectors -= chunk;
    }
    mutex_unlock(&ctrl->io_lock);
    return 0;
}

static int nvme_read(device_t *dev, uint64_t lba, void *buf, size_t sectors)
{
    return nvme_rw(dev, lba, buf, sectors, 0);
}

static int nvme_write(device_t *dev, uint64_t lba, const void *buf,
                      size_t sectors)
{
    return nvme_rw(dev, lba, (void *)buf, sectors, 1);
}

static int nvme_flush(device_t *dev)
{
    nvme_controller_t *ctrl = dev ? dev->drv_priv : NULL;
    if (!ctrl)
        return -ENODEV;
    mutex_lock(&ctrl->io_lock);
    int ret = nvme_io_cmd(ctrl, NVME_NVM_FLUSH, 0, 0);
    mutex_unlock(&ctrl->io_lock);
    return ret;
}

static uint64_t nvme_capacity(device_t *dev)
{
    nvme_controller_t *ctrl = dev ? dev->drv_priv : NULL;
    return ctrl ? ctrl->capacity : 0;
}

static uint32_t nvme_sector_size(device_t *dev)
{
    nvme_controller_t *ctrl = dev ? dev->drv_priv : NULL;
    return ctrl ? ctrl->sector_size : 0;
}

#ifdef CONFIG_NVME_SMOKE_TEST
static int nvme_io_smoke_test(device_t *dev, nvme_controller_t *ctrl)
{
    size_t max_sectors = NVME_DMA_BYTES / ctrl->sector_size;
    size_t sectors = max_sectors + 1U;
    if (ctrl->capacity < sectors)
        sectors = (size_t)ctrl->capacity;
    if (!sectors)
        return -ENODEV;

    size_t bytes = sectors * ctrl->sector_size;
    uint8_t *written = kmalloc(bytes);
    uint8_t *read_back = kmalloc(bytes);
    if (!written || !read_back) {
        kfree(read_back);
        kfree(written);
        return -ENOMEM;
    }
    for (size_t i = 0; i < bytes; i++)
        written[i] = (uint8_t)(0x5aU + i * 37U);
    memset(read_back, 0, bytes);

    dev->drv_priv = ctrl;
    int ret = nvme_write(dev, 0, written, sectors);
    if (ret == 0)
        ret = nvme_flush(dev);
    if (ret == 0)
        ret = nvme_read(dev, 0, read_back, sectors);
    dev->drv_priv = NULL;
    if (ret == 0 && memcmp(written, read_back, bytes) != 0)
        ret = -EIO;

    kfree(read_back);
    kfree(written);
    if (ret == 0)
        kinfo("NVME_IO_SMOKE: PASS sectors=%lu bytes=%lu\n",
              (unsigned long)sectors, (unsigned long)bytes);
    return ret;
}
#endif

static void nvme_release(nvme_controller_t *ctrl)
{
    if (!ctrl)
        return;
    if (ctrl->regs) {
        writel(0, nvme_reg(ctrl, NVME_REG_CC));
        (void)nvme_wait_ready(ctrl, 0);
    }
    if (ctrl->bounce)
        dma_free_coherent_aligned(ctrl->bounce, NVME_DMA_BYTES,
                                  ctrl->bounce_dma);
    if (ctrl->identify)
        dma_free_coherent_aligned(ctrl->identify, PAGE_SIZE,
                                  ctrl->identify_dma);
    nvme_queue_free(&ctrl->io);
    nvme_queue_free(&ctrl->admin);
    kfree(ctrl);
}

static int nvme_probe(device_t *dev)
{
    if (pci_class_code(dev) != 0x010802U)
        return -ENODEV;
    if (pci_enable_and_assign_bars(dev) < 0)
        return -ENODEV;
    resource_t *bar = pci_get_bar_resource(dev, 0);
    if (!bar || bar->end < bar->start ||
        bar->end - bar->start + 1U < NVME_REG_DBS + 8U)
        return -ENODEV;

    nvme_controller_t *ctrl = kcalloc(1, sizeof(*ctrl));
    if (!ctrl)
        return -ENOMEM;
    ctrl->regs = (uintptr_t)bar->start;
    mutex_init(&ctrl->io_lock);
    uint64_t cap = readq(nvme_reg(ctrl, NVME_REG_CAP));
    ctrl->stride = 4U << ((cap >> 32) & 0x0fU);
    uint64_t required_bar = NVME_REG_DBS + 3ULL * ctrl->stride + 4U;
    uint64_t bar_size = bar->end - bar->start + 1U;
    if (required_bar > bar_size) {
        nvme_release(ctrl);
        return -ENODEV;
    }
    uint32_t max_entries = (uint32_t)(cap & 0xffffU) + 1U;
    uint16_t admin_depth = max_entries < NVME_ADMIN_DEPTH ? (uint16_t)max_entries :
                                                               NVME_ADMIN_DEPTH;
    uint16_t io_depth = max_entries < NVME_IO_DEPTH ? (uint16_t)max_entries :
                                                       NVME_IO_DEPTH;
    if (admin_depth < 2 || io_depth < 2) {
        nvme_release(ctrl);
        return -ENODEV;
    }

    writel(0xffffffffU, nvme_reg(ctrl, NVME_REG_INTMS));
    writel(0, nvme_reg(ctrl, NVME_REG_CC));
    if (nvme_wait_ready(ctrl, 0) < 0 ||
        nvme_queue_alloc(ctrl, &ctrl->admin, 0, admin_depth) < 0)
        goto fail;
    uint32_t aqa = (admin_depth - 1U) | ((uint32_t)(admin_depth - 1U) << 16);
    writel(aqa, nvme_reg(ctrl, NVME_REG_AQA));
    writeq(ctrl->admin.sq_dma, nvme_reg(ctrl, NVME_REG_ASQ));
    writeq(ctrl->admin.cq_dma, nvme_reg(ctrl, NVME_REG_ACQ));
    writel(NVME_CC_EN | (6U << 16) | (4U << 20),
           nvme_reg(ctrl, NVME_REG_CC));
    if (nvme_wait_ready(ctrl, 1) < 0)
        goto fail;
    if (nvme_admin(ctrl, NVME_ADMIN_SET_FEATURE, 0, 0,
                   NVME_FID_NUM_QUEUES, 0, NULL) < 0)
        goto fail;
    if (nvme_queue_alloc(ctrl, &ctrl->io, 1, io_depth) < 0)
        goto fail;
    if (nvme_admin(ctrl, NVME_ADMIN_CREATE_CQ, 0, ctrl->io.cq_dma,
                   ((uint32_t)(io_depth - 1U) << 16) | 1U, 1U, NULL) < 0 ||
        nvme_admin(ctrl, NVME_ADMIN_CREATE_SQ, 0, ctrl->io.sq_dma,
                   ((uint32_t)(io_depth - 1U) << 16) | 1U,
                   (1U << 16) | 1U, NULL) < 0)
        goto fail;

    ctrl->identify = dma_alloc_coherent_aligned(PAGE_SIZE, PAGE_SIZE,
                                                 &ctrl->identify_dma);
    ctrl->bounce = dma_alloc_coherent_aligned(NVME_DMA_BYTES, PAGE_SIZE,
                                               &ctrl->bounce_dma);
    if (!ctrl->identify || !ctrl->bounce || nvme_identify(ctrl) < 0)
        goto fail;
#ifdef CONFIG_NVME_SMOKE_TEST
    if (nvme_io_smoke_test(dev, ctrl) < 0)
        goto fail;
#endif
    dev->drv_priv = ctrl;
    kinfo("[NVME] %s ns%u: %lu sectors, sector=%u\n", dev->name,
          ctrl->namespace_id, (unsigned long)ctrl->capacity,
          ctrl->sector_size);
    return 0;

fail:
    nvme_release(ctrl);
    return -EIO;
}

static int nvme_remove(device_t *dev)
{
    nvme_controller_t *ctrl = dev ? dev->drv_priv : NULL;
    if (!ctrl)
        return 0;
    dev->drv_priv = NULL;
    nvme_release(ctrl);
    return 0;
}

static const block_dev_ops_t nvme_ops = {
    .read = nvme_read,
    .write = nvme_write,
    .flush = nvme_flush,
    .capacity = nvme_capacity,
    .sector_size = nvme_sector_size,
};

static const device_id_t nvme_ids[] = {
    { .vendor = VENDOR_ANY, .device = DEVICE_ANY,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static int nvme_match(device_t *dev)
{
    return pci_class_code(dev) == 0x010802U;
}

static driver_t nvme_driver = {
    .name = "nvme",
    .id_table = nvme_ids,
    .bus = &pci_bus,
    .match = nvme_match,
    .probe = nvme_probe,
    .remove = nvme_remove,
    .class_ops = &nvme_ops,
    .class_type = DEV_CLASS_BLOCK,
};

DRIVER_REGISTER(nvme_driver);
