/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/dma_mapping.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
#include "core/refcount.h"
#include "mm/slab.h"

struct driver_dma_domain {
    refcount_t references;
    device_t *device;
    struct driver_dma_ops ops;
    void *context;
    uint64_t mask;
};

struct driver_dma_page {
    pfn_t pfn;
    uint64_t address;
};

struct driver_dma_mapping {
    struct driver_dma_domain *domain;
    size_t count;
    enum driver_dma_direction direction;
    struct driver_dma_page pages[];
};

extern int arch_device_dma_ready(void) __attribute__((weak));

int driver_dma_domain_create(device_t *device,
                             const struct driver_dma_ops *ops, void *context,
                             struct driver_dma_domain **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!device || !ops || !ops->map_page || !ops->unmap_page)
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
    refcount_set(&domain->references, 1);
    domain->device = device;
    domain->ops = *ops;
    domain->context = context;
    domain->mask = mask;
    *out = domain;
    return 0;
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
    device_put(domain->device);
    kfree(domain);
}

int driver_dma_mapping_sync_for_device(struct driver_dma_mapping *mapping)
{
    if (!mapping)
        return -EINVAL;
    for (size_t i = 0; i < mapping->count; i++)
        dma_sync_for_device(pfn_to_virt(mapping->pages[i].pfn), PAGE_SIZE);
    return 0;
}

int driver_dma_mapping_sync_for_cpu(struct driver_dma_mapping *mapping)
{
    if (!mapping)
        return -EINVAL;
    if (mapping->direction != DRIVER_DMA_TO_DEVICE)
        for (size_t i = 0; i < mapping->count; i++)
            dma_sync_for_cpu(pfn_to_virt(mapping->pages[i].pfn), PAGE_SIZE);
    return 0;
}

static void release_mapping(struct driver_dma_mapping *mapping, bool sync_cpu)
{
    struct driver_dma_domain *domain = mapping->domain;
    for (size_t i = mapping->count; i > 0; i--)
        domain->ops.unmap_page(domain->context, mapping->pages[i - 1].address,
                                mapping->direction);
    if (sync_cpu)
        driver_dma_mapping_sync_for_cpu(mapping);
    for (size_t i = 0; i < mapping->count; i++)
        frame_put(mapping->pages[i].pfn);
    driver_dma_domain_put(domain);
    kfree(mapping);
}

void driver_dma_unmap_pages(struct driver_dma_mapping **pointer)
{
    if (pointer && *pointer) {
        struct driver_dma_mapping *mapping = *pointer;
        *pointer = NULL;
        release_mapping(mapping, true);
    }
}

int driver_dma_map_pages(struct driver_dma_domain *domain, const pfn_t *pages,
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
    for (size_t i = 0; i < count; i++)
        if (!pfn_valid(pages[i]))
            return -EINVAL;
    struct driver_dma_mapping *mapping = kmalloc(sizeof(*mapping) +
                                         count * sizeof(mapping->pages[0]));
    if (!mapping)
        return -ENOMEM;
    mapping->domain = domain;
    mapping->count = 0;
    mapping->direction = direction;
    driver_dma_domain_get(domain);
    for (size_t i = 0; i < count; i++) {
        pfn_t pfn = pages[i];
        frame_get(pfn);
        uint64_t address = 0;
        int rc = domain->ops.map_page(domain->context, pfn_to_phys(pfn),
                                       direction, &address);
        if (rc == 0 && ((address & (PAGE_SIZE - 1)) ||
                       address > domain->mask ||
                       PAGE_SIZE - 1 > domain->mask - address)) {
            domain->ops.unmap_page(domain->context, address, direction);
            rc = -ERANGE;
        }
        if (rc) {
            frame_put(pfn);
            release_mapping(mapping, false);
            return rc < 0 ? rc : -EIO;
        }
        mapping->pages[mapping->count++] = (struct driver_dma_page){pfn, address};
    }
    driver_dma_mapping_sync_for_device(mapping);
    *out = mapping;
    return 0;
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
    *out = mapping->pages[index].address;
    return 0;
}
#endif
