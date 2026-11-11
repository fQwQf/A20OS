#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/core/dma_mapping.h"
#include "drivers/core/driver_core.h"
#include "core/errno.h"

pfa_t pfa;
static unsigned refs[256], allocations, cpu_syncs, device_syncs, freed;
static int fail_alloc, ready = 1;
void *kmalloc(size_t n)
{
    if (fail_alloc) { fail_alloc = 0; return NULL; }
    void *p = malloc(n); assert(p); allocations++; return p;
}
void kfree(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
device_t *device_get(device_t *d)
{ if (!d || !d->references) return NULL; d->references++; return d; }
void device_put(device_t *d) { assert(d->references); d->references--; }
int arch_device_dma_ready(void) { return ready; }
uint64_t dma_get_mask(const device_t *d) { return d->dma_mask ? d->dma_mask : ~(uint64_t)0; }
void dma_sync_for_device(void *p, size_t n) { assert(p && n == PAGE_SIZE); device_syncs++; }
void dma_sync_for_cpu(void *p, size_t n) { assert(p && n == PAGE_SIZE); cpu_syncs++; }
void frame_get(pfn_t pfn) { assert(pfn_valid(pfn) && refs[pfn]); refs[pfn]++; }
void frame_put(pfn_t pfn)
{ assert(pfn_valid(pfn) && refs[pfn]); if (!--refs[pfn]) freed++; }

struct provider {
    uint64_t addresses[128];
    uint64_t offset;
    unsigned attempts, count, releases, fail_at, bad_at;
    int failure;
};
static int map(void *opaque, uint64_t physical,
                 enum driver_dma_direction direction, uint64_t *address)
{
    struct provider *p = opaque;
    assert(direction <= DRIVER_DMA_BIDIRECTIONAL);
    if (++p->attempts == p->fail_at)
        return p->failure;
    *address = physical + p->offset;
    if (p->attempts == p->bad_at)
        (*address)++;
    p->addresses[p->count++] = *address;
    return 0;
}
static void unmap(void *opaque, uint64_t address, enum driver_dma_direction direction)
{
    struct provider *p = opaque;
    (void)direction;
    assert(p->count && p->addresses[--p->count] == address);
}
static void release(void *opaque)
{ struct provider *p = opaque; assert(!p->count); p->releases++; }
static const struct driver_dma_ops ops = {map, unmap, release};

static void setup(void)
{
    assert(!allocations);
    memset(&pfa, 0, sizeof(pfa));
    pfa.nr_ranges = 1;
    /* Compact PFNs intentionally differ from physical page numbers. */
    pfa.ranges[0] = (pfa_range_t){0x400000, 0x480000, 100, 228};
    for (unsigned i = 100; i < 228; i++) refs[i] = 1;
    cpu_syncs = device_syncs = freed = 0;
    ready = 1;
}
static void direct_and_lifetime(void)
{
    setup();
    device_t d = {.references=1};
    struct driver_dma_domain *domain;
    struct driver_dma_mapping *mapping;
    pfn_t pages[96];
    for (unsigned i = 0; i < 96; i++) pages[i] = 100 + i;
    assert(!driver_dma_domain_create_direct(&d, &domain));
    assert(d.references == 2);
    assert(!driver_dma_map_pages(domain, pages, 96, DRIVER_DMA_FROM_DEVICE, &mapping));
    assert(driver_dma_mapping_count(mapping) == 96 && device_syncs == 96);
    uint64_t address;
    assert(!driver_dma_page_address(mapping, 3, &address) && address == 0x403000);
    assert(driver_dma_page_address(mapping, 96, &address) == -EINVAL);
    for (unsigned i = 0; i < 96; i++) { assert(refs[pages[i]] == 2); frame_put(pages[i]); }
    driver_dma_domain_put(domain); /* Mapping retains domain and device. */
    device_put(&d);
    assert(d.references == 1 && !freed);
    driver_dma_unmap_pages(&mapping);
    assert(!mapping && !d.references && freed == 96 && cpu_syncs == 96);
    driver_dma_unmap_pages(&mapping);
    assert(!allocations);
}
static void translated(void)
{
    setup();
    device_t d = {.references=1, .dma_mask=0xffffffff};
    struct provider p = {.offset=0x200000};
    struct driver_dma_domain *domain;
    struct driver_dma_mapping *mapping;
    pfn_t pages[] = {100, 112, 103};
    assert(!driver_dma_domain_create(&d, &ops, &p, &domain));
    assert(!driver_dma_map_pages(domain, pages, 3, DRIVER_DMA_BIDIRECTIONAL, &mapping));
    uint64_t address;
    assert(!driver_dma_page_address(mapping, 1, &address) && address == 0x60c000);
    assert(!driver_dma_mapping_sync_for_cpu(mapping) && cpu_syncs == 3);
    assert(!driver_dma_mapping_sync_for_device(mapping) && device_syncs == 6);
    driver_dma_domain_put(domain);
    assert(!p.releases);
    driver_dma_unmap_pages(&mapping);
    assert(p.releases == 1 && d.references == 1 && !allocations);
    for (unsigned i = 0; i < 3; i++) assert(refs[pages[i]] == 1);
}
static void rollback(void)
{
    for (unsigned fail = 1; fail <= 4; fail++) {
        setup();
        device_t d = {.references=1};
        struct provider p = {.offset=0x200000, .fail_at=fail, .failure=-ENOMEM};
        struct driver_dma_domain *domain;
        struct driver_dma_mapping *mapping = (void *)1;
        pfn_t pages[] = {100, 102, 110, 104};
        assert(!driver_dma_domain_create(&d, &ops, &p, &domain));
        assert(driver_dma_map_pages(domain, pages, 4, DRIVER_DMA_FROM_DEVICE,
                                      &mapping) == -ENOMEM);
        assert(!mapping && !p.count && !p.releases);
        assert(!cpu_syncs && !device_syncs); /* No invalidate of dirty CPU data on rollback. */
        for (unsigned i = 0; i < 4; i++) assert(refs[pages[i]] == 1);
        driver_dma_domain_put(domain);
        assert(p.releases == 1 && d.references == 1 && !allocations);
    }
}
static void failures(void)
{
    setup();
    device_t d = {.references=1, .dma_mask=0xffffffff};
    struct provider p = {.offset=0x100000000};
    struct driver_dma_domain *domain = (void *)1;
    struct driver_dma_mapping *mapping = (void *)1;
    pfn_t pages[] = {100, PFN_NONE};
    ready = 0;
    assert(driver_dma_domain_create(&d, &ops, &p, &domain) == -EOPNOTSUPP);
    assert(!domain && !p.releases);
    ready = 1; fail_alloc = 1;
    assert(driver_dma_domain_create(&d, &ops, &p, &domain) == -ENOMEM && !domain);
    assert(!driver_dma_domain_create(&d, &ops, &p, &domain));
    assert(driver_dma_map_pages(domain, pages, 2, DRIVER_DMA_TO_DEVICE, &mapping) == -EINVAL);
    assert(!mapping && !p.attempts && refs[100] == 1);
    assert(driver_dma_map_pages(domain, pages, SIZE_MAX, DRIVER_DMA_TO_DEVICE,
                                  &mapping) == -EOVERFLOW);
    assert(driver_dma_map_pages(domain, pages, 1, -1, &mapping) == -EINVAL);
    assert(driver_dma_map_pages(domain, pages, 1, DRIVER_DMA_TO_DEVICE, &mapping) == -ERANGE);
    assert(!mapping && !p.count && p.attempts == 1); /* No physical fallback. */
    p.offset = 0; p.bad_at = 2;
    assert(driver_dma_map_pages(domain, pages, 1, DRIVER_DMA_TO_DEVICE, &mapping) == -ERANGE);
    assert(!p.count && refs[100] == 1);
    fail_alloc = 1;
    assert(driver_dma_map_pages(domain, pages, 1, DRIVER_DMA_TO_DEVICE, &mapping) == -ENOMEM);
    ready = 0;
    assert(driver_dma_map_pages(domain, pages, 1, DRIVER_DMA_TO_DEVICE, &mapping) == -EOPNOTSUPP);
    ready = 1; d.state = DEV_STATE_REMOVING;
    assert(driver_dma_map_pages(domain, pages, 1, DRIVER_DMA_TO_DEVICE, &mapping) == -ENODEV);
    driver_dma_domain_put(domain);
    assert(p.releases == 1 && d.references == 1 && !allocations);
}
int main(void)
{
    direct_and_lifetime(); translated(); rollback(); failures();
    puts("native DMA mappings: PASS"); return 0;
}
