/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/riscv_iommu_internal.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/workqueue.h"
#include "core/errno.h"
#include "mm/slab.h"

struct task_t;
extern struct task_t *proc_current(void);

struct riscv_iommu_dma_watch {
    device_t *device;
    int owner;
    uint64_t period;
    struct driver_workqueue *queue;
    struct driver_work *work;
    void (*callback)(void *, int, const struct riscv_iommu_dma_fault *);
    void *context;
    struct riscv_iommu_dma_fault last;
};

static bool changed(const struct riscv_iommu_dma_fault *a,
                      const struct riscv_iommu_dma_fault *b)
{
    return a->count != b->count || a->address != b->address ||
           a->cause != b->cause || a->queue_error != b->queue_error ||
           a->isolation_error != b->isolation_error ||
           a->blocked != b->blocked || a->isolated != b->isolated;
}

static void poll_watch(void *opaque)
{
    struct riscv_iommu_dma_watch *watch = opaque;
    struct riscv_iommu_dma_fault fault;
    int ret = riscv_iommu_fault_poll_owned(watch->device, watch->owner, &fault);
    if (ret || changed(&fault, &watch->last)) {
        watch->last = fault;
        watch->callback(watch->context, ret, &fault);
    }
    if (ret)
        return; /* One terminal status; never follow a replacement owner. */
    uint64_t now = clock_get_ticks();
    uint64_t due = now > ~(uint64_t)0 - watch->period ? ~(uint64_t)0 : now + watch->period;
    /* Destroy closes submissions before draining this callback. */
    (void)driver_work_schedule(watch->work, due);
}

int riscv_iommu_dma_watch_stop(struct riscv_iommu_dma_watch **pointer, uint64_t deadline)
{
    if (!pointer)
        return -EINVAL;
    struct riscv_iommu_dma_watch *watch = *pointer;
    if (!watch)
        return 0;
    if (watch->queue) {
        int ret = driver_workqueue_destroy(&watch->queue, deadline);
        if (ret)
            return ret;
    }
    driver_work_put(watch->work);
    device_put(watch->device);
    kfree(watch);
    *pointer = NULL;
    return 0;
}

int riscv_iommu_dma_watch_start(device_t *device, uint64_t period,
                                void (*callback)(void *, int, const struct riscv_iommu_dma_fault *),
                                void *context, struct riscv_iommu_dma_watch **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!device || !period || !callback)
        return -EINVAL;
    if (!proc_current())
        return -EAGAIN;
    uint16_t devid;
    int owner;
    int ret = riscv_iommu_native_identity(device, &devid, &owner);
    if (ret)
        return ret;
    struct riscv_iommu_dma_watch *watch = kcalloc(1, sizeof(*watch));
    if (!watch)
        return -ENOMEM;
    watch->device = device_get(device);
    if (!watch->device) {
        kfree(watch);
        return -ENODEV;
    }
    watch->owner = owner;
    watch->period = period;
    watch->callback = callback;
    watch->context = context;
    watch->queue = driver_workqueue_create();
    if (watch->queue)
        watch->work = driver_work_create(watch->queue, poll_watch, watch);
    *out = watch; /* Also preserves a cleanup handle if shutdown is interrupted. */
    if (!watch->work) {
        (void)riscv_iommu_dma_watch_stop(out, 0);
        return -ENOMEM;
    }
    ret = driver_work_schedule(watch->work, 0);
    if (ret < 0) {
        (void)riscv_iommu_dma_watch_stop(out, 0);
        return ret;
    }
    return 0;
}
#endif
