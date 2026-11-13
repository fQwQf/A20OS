/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/dma_mapping.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "mm/slab.h"

struct driver_dma_domain {
    refcount_t references;
    device_t *device;
    struct driver_dma_checked_ops ops;
    void (*legacy_unmap)(void *, uint64_t, enum driver_dma_direction);
    void *context;
    uint64_t mask;
    struct driver_dma_managed_ops managed;
    mutex_t lock;
    size_t mappings;
    bool is_managed, closed, provider_live;
    struct driver_dma_policy *policy;
};

struct driver_dma_page {
    pfn_t pfn;
    uint64_t address;
};

struct driver_dma_mapping {
    struct driver_dma_domain *domain;
    size_t count, mapped;
    bool usable, sync_cpu;
    enum driver_dma_direction direction;
    struct driver_dma_page pages[];
};

extern int arch_device_dma_ready(void) __attribute__((weak));

static int create_domain(device_t *device,
                           const struct driver_dma_checked_ops *ops,
                           void (*legacy_unmap)(void *, uint64_t, enum driver_dma_direction),
                           const struct driver_dma_managed_ops *managed,
                           struct driver_dma_policy *policy,
                           void *context, struct driver_dma_domain **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!device || !ops || (!ops->map_page && !managed) || (!ops->unmap_page && !legacy_unmap))
        return -EINVAL;
    if (device->state == DEV_STATE_REMOVING || device->state == DEV_STATE_REMOVED)
        return -ENODEV;
    if (arch_device_dma_ready && !arch_device_dma_ready())
        return -EOPNOTSUPP;
    uint64_t mask = dma_get_mask(device);
    if (!mask || (mask & (mask + 1)))
        return -EINVAL;
    struct driver_dma_domain *domain = kmalloc(sizeof(*domain));
    if (!domain)
        return -ENOMEM;
    if (!device_get(device)) {
        kfree(domain);
        return -ENODEV;
    }
    refcount_set(&domain->references, managed ? 2 : 1);
    domain->is_managed = managed != NULL;
    domain->closed = false;
    domain->provider_live = managed != NULL;
    domain->mappings = 0;
    mutex_init(&domain->lock);
    if (managed)
        domain->managed = *managed;
    domain->device = device;
    domain->ops = *ops;
    domain->legacy_unmap = legacy_unmap;
    domain->context = context;
    domain->mask = mask;
    domain->policy = policy;
    driver_dma_policy_get(policy);
    *out = domain;
    return 0;
}

int driver_dma_domain_create(device_t *device,
                             const struct driver_dma_ops *ops, void *context,
                             struct driver_dma_domain **out)
{
    if (!ops) {
        if (out) *out = NULL;
        return -EINVAL;
    }
    const struct driver_dma_checked_ops checked = {
        .map_page = ops->map_page, .release = ops->release,
    };
    return create_domain(device, &checked, ops->unmap_page, NULL, NULL, context, out);
}

int driver_dma_domain_create_checked(device_t *device,
                                     const struct driver_dma_checked_ops *ops,
                                     void *context, struct driver_dma_domain **out)
{
    return create_domain(device, ops, NULL, NULL, NULL, context, out);
}

int driver_dma_domain_create_managed(device_t *device,
                                     const struct driver_dma_managed_ops *ops,
                                     void *context, struct driver_dma_domain **out)
{
    if (out) *out = NULL;
    if (!ops || !ops->map_page || !ops->unmap_page || !ops->deactivate)
        return -EINVAL;
    const struct driver_dma_checked_ops checked = {
        .unmap_page = ops->unmap_page, .release = ops->release,
    };
    return create_domain(device, &checked, NULL, ops, NULL, context, out);
}

int driver_dma_domain_create_managed_policy(struct device *device,
                    const struct driver_dma_managed_ops *ops, void *context,
                    struct driver_dma_policy *policy,
                    struct driver_dma_domain **out)
{
    if (out) *out = NULL;
    if (!policy || !ops || !ops->map_page || !ops->unmap_page || !ops->deactivate)
        return -EINVAL;
    const struct driver_dma_checked_ops checked = {
        .unmap_page = ops->unmap_page, .release = ops->release,
    };
    return create_domain(device, &checked, NULL, ops, policy, context, out);
}

int driver_dma_domain_shutdown(struct driver_dma_domain *domain)
{
    if (!domain)
        return -EINVAL;
    if (!domain->is_managed)
        return -EOPNOTSUPP;
    mutex_lock(&domain->lock);
    domain->closed = true;
    int ret = 0;
    bool drop = false;
    if (domain->mappings)
        ret = -EBUSY;
    else if (domain->provider_live) {
        ret = domain->managed.deactivate(domain->context);
        if (!ret) {
            domain->provider_live = false;
            drop = true;
        } else if (ret > 0) {
            ret = -EIO;
        }
    }
    mutex_unlock(&domain->lock);
    if (drop)
        driver_dma_domain_put(domain);
    return ret;
}

static int direct_map(void *context, uint64_t physical,
                       enum driver_dma_direction direction, uint64_t *address)
{
    (void)context;
    (void)direction;
    *address = physical;
    return 0;
}

static void direct_unmap(void *context, uint64_t address,
                          enum driver_dma_direction direction)
{
    (void)context;
    (void)address;
    (void)direction;
}

int driver_dma_domain_create_direct(device_t *device,
                                    struct driver_dma_domain **out)
{
    const struct driver_dma_ops ops = {
        .map_page = direct_map, .unmap_page = direct_unmap,
    };
    return driver_dma_domain_create(device, &ops, NULL, out);
}

void driver_dma_domain_get(struct driver_dma_domain *domain)
{
    if (domain)
        refcount_inc(&domain->references);
}

void driver_dma_domain_put(struct driver_dma_domain *domain)
{
    if (!domain || !refcount_dec_and_test(&domain->references))
        return;
    if (domain->ops.release)
        domain->ops.release(domain->context);
    driver_dma_policy_put(domain->policy);
    device_put(domain->device);
    kfree(domain);
}

int driver_dma_mapping_sync_for_device(struct driver_dma_mapping *mapping)
{
    if (!mapping)
        return -EINVAL;
    if (!mapping->usable)
        return -ESHUTDOWN;
    for (size_t i = 0; i < mapping->count; i++)
        dma_sync_for_device(pfn_to_virt(mapping->pages[i].pfn), PAGE_SIZE);
    return 0;
}

int driver_dma_mapping_sync_for_cpu(struct driver_dma_mapping *mapping)
{
    if (!mapping)
        return -EINVAL;
    if (!mapping->usable)
        return -ESHUTDOWN;
    if (mapping->direction != DRIVER_DMA_TO_DEVICE)
        for (size_t i = 0; i < mapping->count; i++)
            dma_sync_for_cpu(pfn_to_virt(mapping->pages[i].pfn), PAGE_SIZE);
    return 0;
}

static int release_mapping(struct driver_dma_mapping *mapping)
{
    struct driver_dma_domain *domain = mapping->domain;
    mapping->usable = false;
    while (mapping->mapped) {
        uint64_t address = mapping->pages[mapping->mapped - 1].address;
        if (domain->ops.unmap_page) {
            int ret = domain->ops.unmap_page(domain->context, address,
                                              mapping->direction);
            if (ret)
                return ret < 0 ? ret : -EIO;
        } else {
            domain->legacy_unmap(domain->context, address, mapping->direction);
        }
        mapping->mapped--;
    }
    if (mapping->sync_cpu && mapping->direction != DRIVER_DMA_TO_DEVICE)
        for (size_t i = 0; i < mapping->count; i++)
            dma_sync_for_cpu(pfn_to_virt(mapping->pages[i].pfn), PAGE_SIZE);
    for (size_t i = 0; i < mapping->count; i++)
        frame_put(mapping->pages[i].pfn);
    if (domain->is_managed)
        domain->mappings--;
    driver_dma_domain_put(domain);
    kfree(mapping);
    return 0;
}

int driver_dma_try_unmap_pages(struct driver_dma_mapping **pointer)
{
    if (!pointer)
        return -EINVAL;
    if (!*pointer)
        return 0;
    struct driver_dma_domain *domain = (*pointer)->domain;
    driver_dma_domain_get(domain);
    if (domain->is_managed)
        mutex_lock(&domain->lock);
    int ret = release_mapping(*pointer);
    if (!ret)
        *pointer = NULL;
    if (domain->is_managed)
        mutex_unlock(&domain->lock);
    driver_dma_domain_put(domain);
    return ret;
}

void driver_dma_unmap_pages(struct driver_dma_mapping **pointer)
{
    /* Legacy infallible providers keep their ABI. A checked provider caller
     * must use try_unmap to observe errors; never consume a failed mapping. */
    (void)driver_dma_try_unmap_pages(pointer);
}

static int map_pages(struct driver_dma_domain *domain, const pfn_t *pages,
                          size_t count, enum driver_dma_direction direction,
                          struct driver_dma_mapping **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!domain || !pages || !count ||
        (direction != DRIVER_DMA_TO_DEVICE &&
         direction != DRIVER_DMA_FROM_DEVICE &&
         direction != DRIVER_DMA_BIDIRECTIONAL))
        return -EINVAL;
    if (count > (SIZE_MAX - sizeof(struct driver_dma_mapping)) /
                    sizeof(struct driver_dma_page))
        return -EOVERFLOW;
    if (arch_device_dma_ready && !arch_device_dma_ready())
        return -EOPNOTSUPP;
    if (domain->device->state == DEV_STATE_REMOVING ||
        domain->device->state == DEV_STATE_REMOVED)
        return -ENODEV;
    for (size_t i = 0; i < count; i++) {
        if (!pfn_valid(pages[i]))
            return -EINVAL;
        if (domain->policy) {
            bool identity;
            int ret = driver_dma_policy_check(domain->policy,
                           pfn_to_phys(pages[i]), PAGE_SIZE, direction, &identity);
            if (ret)
                return ret;
        }
    }
    struct driver_dma_mapping *mapping = kmalloc(sizeof(*mapping) +
                                         count * sizeof(mapping->pages[0]));
    if (!mapping)
        return -ENOMEM;
    mapping->domain = domain;
    mapping->count = mapping->mapped = 0;
    mapping->usable = true;
    mapping->sync_cpu = false;
    mapping->direction = direction;
    driver_dma_domain_get(domain);
    if (domain->is_managed)
        domain->mappings++;
    for (size_t i = 0; i < count; i++) {
        pfn_t pfn = pages[i];
        frame_get(pfn);
        uint64_t address = 0;
        bool needs_unmap = false;
        int rc;
        if (domain->is_managed) {
            rc = domain->managed.map_page(domain->context, pfn_to_phys(pfn),
                                            direction, &address, &needs_unmap);
            if (!rc && !needs_unmap)
                rc = -EIO;
        } else {
            rc = domain->ops.map_page(domain->context, pfn_to_phys(pfn),
                                       direction, &address);
            needs_unmap = !rc;
        }
        if (needs_unmap) {
            mapping->pages[mapping->count++] = (struct driver_dma_page){pfn, address};
            mapping->mapped++;
            if (!rc && ((address & (PAGE_SIZE - 1)) || address > domain->mask ||
                PAGE_SIZE - 1 > domain->mask - address))
                rc = -ERANGE;
            if (!rc && domain->policy) {
                bool identity;
                rc = driver_dma_policy_check(domain->policy, pfn_to_phys(pfn),
                                               PAGE_SIZE, direction, &identity);
                if (!rc && identity && address != pfn_to_phys(pfn))
                    rc = -ERANGE;
            }
        } else {
            frame_put(pfn);
        }
        if (rc) {
            /* A failed rollback still owns pins and translations. Expose only
             * a cleanup handle, never a partially usable DMA mapping. */
            if (release_mapping(mapping))
                *out = mapping;
            return rc < 0 ? rc : -EIO;
        }
    }
    driver_dma_mapping_sync_for_device(mapping);
    mapping->sync_cpu = true;
    *out = mapping;
    return 0;
}

int driver_dma_map_pages(struct driver_dma_domain *domain, const pfn_t *pages,
                          size_t count, enum driver_dma_direction direction,
                          struct driver_dma_mapping **out)
{
    if (!domain || !domain->is_managed)
        return map_pages(domain, pages, count, direction, out);
    if (!out)
        return -EINVAL;
    *out = NULL;
    mutex_lock(&domain->lock);
    int ret = domain->closed ? -ESHUTDOWN :
              map_pages(domain, pages, count, direction, out);
    mutex_unlock(&domain->lock);
    return ret;
}

size_t driver_dma_mapping_count(const struct driver_dma_mapping *mapping)
{
    return mapping ? mapping->count : 0;
}

int driver_dma_page_address(const struct driver_dma_mapping *mapping,
                             size_t index, uint64_t *out)
{
    if (!mapping || !out || index >= mapping->count)
        return -EINVAL;
    if (!mapping->usable)
        return -ESHUTDOWN;
    *out = mapping->pages[index].address;
    return 0;
}
#endif
