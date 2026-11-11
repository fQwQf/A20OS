#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "drivers/core/riscv_iommu.c"

/* Registration keeps the probe linked under ASan. It must never run here. */
pfa_t pfa;
bus_type_t pci_bus;
struct test_bdf { unsigned bus, slot, function; uint16_t command; };
static unsigned allocations;
void *kmalloc(size_t size) { void *p = malloc(size); assert(p); allocations++; return p; }
void kfree(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
device_t *device_get(device_t *d) { assert(d->references); d->references++; return d; }
void device_put(device_t *d) { assert(d->references > 1); d->references--; }
uint64_t dma_get_mask(const device_t *d) { return d->dma_mask ? d->dma_mask : ~(uint64_t)0; }
void dma_sync_for_device(void *p, size_t n) { assert(p && n == PAGE_SIZE); }
void dma_sync_for_cpu(void *p, size_t n) { assert(p && n == PAGE_SIZE); }
int pci_device_location(const device_t *d, unsigned *bus, unsigned *slot, unsigned *function)
{
    if (d->bus != &pci_bus || !d->plat_data) return -ENODEV;
    struct test_bdf *bdf = d->plat_data;
    *bus = bdf->bus; *slot = bdf->slot; *function = bdf->function; return 0;
}
uint16_t pci_cfg_read16(const device_t *d, uint32_t reg)
{ assert(reg == 4); return ((struct test_bdf *)d->plat_data)->command; }
int pci_enable_and_assign_bars(device_t *dev) { (void)dev; assert(0); return -1; }
resource_t *pci_get_bar_resource(device_t *dev, unsigned bar)
{ (void)dev; (void)bar; assert(0); return NULL; }
void *pci_map_bar(device_t *dev, unsigned bar)
{ (void)dev; (void)bar; assert(0); return NULL; }
static unsigned refs[256];
static unsigned char ram[156 * PAGE_SIZE] __attribute__((aligned(4096)));
pfn_t pfa_alloc_page(void)
{
    for (pfn_t p = 100; p < 110; p++)
        if (!refs[p]) { refs[p] = 1; return p; }
    return PFN_NONE;
}
void frame_get(pfn_t p) { assert(pfn_valid(p) && refs[p]); refs[p]++; }
void frame_put(pfn_t p) { assert(pfn_valid(p) && refs[p]); refs[p]--; }
void pfa_free_page(pfn_t p) { frame_put(p); }

static uint32_t registers[0x300 / 4];
static uint64_t commands[(IOMMU_CQ_MASK + 1) * 2];
static unsigned publications, failures;
static int complete, error_on_publish;
static unsigned fail_publish_at;

void host_log(const char *format, ...) { (void)format; failures++; }
uint32_t host_read32(const volatile void *address)
{
    uintptr_t offset = (uintptr_t)address - g_iommu_base;
    assert(offset < sizeof(registers) && !(offset & 3));
    return registers[offset / 4];
}
void host_write32(uint32_t value, volatile void *address)
{
    uintptr_t offset = (uintptr_t)address - g_iommu_base;
    assert(offset == IOMMU_REG_CQT);
    assert(g_iommu_cq_tail == value); /* Published ownership is immediate. */
    registers[offset / 4] = value;
    publications++;
    if (complete) registers[IOMMU_REG_CQH / 4] = value;
    if (error_on_publish || publications == fail_publish_at)
        registers[IOMMU_REG_CQCSR / 4] |= IOMMU_QUEUE_ERROR;
}
static void setup(void)
{
    memset(registers, 0, sizeof(registers));
    memset(commands, 0, sizeof(commands));
    g_iommu_base = 0x1000;
    g_iommu_cq = commands;
    g_iommu_cq_tail = 0;
    publications = failures = 0;
    complete = error_on_publish = 0;
    fail_publish_at = 0;
    memset(refs, 0, sizeof(refs));
    memset(&pfa, 0, sizeof(pfa));
    pfa.nr_ranges = 1;
    pfa.ranges[0] = (pfa_range_t){(uintptr_t)ram - PAGE_OFFSET,
        (uintptr_t)ram - PAGE_OFFSET + sizeof(ram), 100, 256};
    memset(g_user_domains, 0, sizeof(g_user_domains));
    memset(&g_iommu_stats, 0, sizeof(g_iommu_stats));
}

static void domain_lifetimes(void)
{
    static uint64_t contexts[512];
    for (unsigned fail_claim = 0; fail_claim < 2; fail_claim++) {
        setup(); complete = 1;
        memset(contexts, 0, sizeof(contexts));
        g_iommu_ddt = (uint8_t *)contexts;
        g_iommu_dc_stride = 32;
        g_iommu_ready = 1;
        if (fail_claim) {
            /* Fail after the translating context became visible. */
            fail_publish_at = 6;
            assert(riscv_iommu_domain_claim(8, 42) < 0);
            assert(refs[100] == 1 && refs[101] == 1 && refs[102] == 1);
            assert(g_user_domains[0].claim_failed && g_user_domains[0].blocked);
            assert(riscv_iommu_domain_claim(8, 42) < 0);
            assert(refs[103] == 0);
            registers[IOMMU_REG_CQCSR / 4] = 0;
            fail_publish_at = 0;
            /* Retry must first safely dispose of quarantined page tables. */
        }
        assert(!riscv_iommu_domain_claim(8, 42));
        refs[255] = 1;
        uint64_t invalid = 99;
        unsigned before = publications;
        assert(riscv_iommu_domain_map(8, 42, pfn_to_phys(255), 2, &invalid) < 0);
        assert(!invalid && refs[255] == 1 && publications == before);
        assert(riscv_iommu_domain_map(8, 42, 0, 1, &invalid) < 0 && !invalid);
        refs[110] = refs[111] = 1;
        uint64_t iova = 99;
        if (!fail_claim) complete = 0;
        int ret = riscv_iommu_domain_map(8, 42, pfn_to_phys(110), 2, &iova);
        assert(refs[110] == 2 && refs[111] == 2);
        if (!fail_claim) {
            assert(ret < 0 && !iova && g_user_domains[0].blocked);
            assert(g_user_domains[0].mapped_pages == 3);
            /* Destroying the original VMO cannot reclaim quarantined RAM. */
            frame_put(110); frame_put(111);
            assert(refs[110] == 1 && refs[111] == 1);
            assert(riscv_iommu_domain_claim(8, 42) < 0);
            assert(riscv_iommu_domain_release(8, 42) < 0);
            assert(refs[100] == 1 && refs[110] == 1);
            complete = 1;
        } else {
            assert(!ret && iova == IOMMU_USER_IOVA);
            complete = 0;
            assert(riscv_iommu_domain_unmap(8, 42, iova, 2) < 0);
            assert(refs[110] == 2 && g_user_domains[0].mapped_pages == 3);
            complete = 1;
            assert(!riscv_iommu_domain_unmap(8, 42, iova, 2));
            assert(refs[110] == 1 && refs[111] == 1);
        }
        assert(!riscv_iommu_domain_release(8, 42));
        assert(!refs[100] && !refs[101] && !refs[102]);
        assert(refs[110] == (fail_claim ? 1U : 0U));
        assert(!g_iommu_stats.mapped_pages && !g_user_domains[0].active);
    }
}

static void native_domains(void)
{
    static uint64_t contexts[512];
    for (unsigned failure = 0; failure < 3; failure++) {
        setup(); complete = 1;
        memset(contexts, 0, sizeof(contexts));
        g_iommu_ddt = (uint8_t *)contexts;
        g_iommu_dc_stride = 32; g_iommu_ready = 1;
        struct test_bdf bdf = {0, 1, 0, 0};
        device_t d = {.bus=&pci_bus, .plat_data=&bdf, .references=1};
        struct driver_dma_domain *domain, *other;
        d.user_owned = 1;
        assert(riscv_iommu_dma_domain_create(&d, &domain) == -EBUSY && !domain);
        d.user_owned = 0; bdf.command = 4;
        assert(riscv_iommu_dma_domain_create(&d, &domain) == -EBUSY && !domain);
        bdf.command = 0; bdf.bus = 1;
        assert(riscv_iommu_dma_domain_create(&d, &domain) == -EOPNOTSUPP && !domain);
        bdf.bus = 0;
        assert(!riscv_iommu_domain_claim(8, 42));
        assert(riscv_iommu_dma_domain_create(&d, &domain) < 0 && !domain);
        assert(!riscv_iommu_domain_release(8, 42) && !allocations);
        if (failure == 2) fail_publish_at = publications + 6;
        int ret = riscv_iommu_dma_domain_create(&d, &domain);
        assert(domain && d.references == 2 && g_user_domains[0].owner_pid < 0);
        if (failure == 2) {
            assert(ret < 0);
            assert(driver_dma_domain_shutdown(domain) < 0 && allocations == 2);
            registers[IOMMU_REG_CQCSR / 4] = 0; fail_publish_at = 0;
        } else {
            assert(!ret);
            assert(riscv_iommu_dma_domain_create(&d, &other) < 0 && !other);
            assert(riscv_iommu_domain_claim(8, 42) < 0);
            refs[110] = refs[111] = 1;
            pfn_t pages[] = {110, 111};
            struct driver_dma_mapping *mapping;
            if (failure == 1) fail_publish_at = publications + 1;
            ret = driver_dma_map_pages(domain, pages, 2, DRIVER_DMA_TO_DEVICE, &mapping);
            assert(mapping);
            if (failure == 1) {
                assert(ret < 0 && refs[110] == 3 && refs[111] == 1);
                assert(driver_dma_try_unmap_pages(&mapping) < 0 && refs[110] == 3);
                registers[IOMMU_REG_CQCSR / 4] = 0; fail_publish_at = 0;
            } else {
                uint64_t address;
                assert(!ret && refs[110] == 3 && refs[111] == 3);
                assert(!driver_dma_page_address(mapping, 0, &address));
                assert(address == IOMMU_USER_IOVA && address != pfn_to_phys(110));
                assert(!(g_user_domains[0].l0[0] & IOMMU_PTE_W));
            }
            assert(driver_dma_domain_shutdown(domain) == -EBUSY);
            assert(!driver_dma_try_unmap_pages(&mapping) && !mapping);
            assert(refs[110] == 1 && refs[111] == 1);
            bdf.command = 4;
            assert(driver_dma_domain_shutdown(domain) == -EBUSY);
            bdf.command = 0;
        }
        assert(!driver_dma_domain_shutdown(domain));
        driver_dma_domain_put(domain);
        assert(!allocations && d.references == 1 && !g_user_domains[0].active);
    }
}
int main(void)
{
    setup();
    /* First command times out but remains hardware-owned. Publishing the
     * next command must not rewrite either dword of that outstanding slot. */
    assert(iommu_cmd(IOMMU_CMD_IOTINVAL, 0x1234) < 0);
    assert(publications == 1 && g_iommu_cq_tail == 1 && failures == 1);
    assert(registers[IOMMU_REG_CQH / 4] == 0);
    complete = 1; /* Hardware now consumes both commands in order. */
    assert(!iommu_cmd(IOMMU_CMD_IOFENCE, 0x5678));
    assert(commands[0] == IOMMU_CMD_IOTINVAL && commands[1] == 0x1234);
    assert(commands[2] == IOMMU_CMD_IOFENCE && commands[3] == 0x5678);
    assert(publications == 2 && g_iommu_cq_tail == 2);

    setup();
    g_iommu_cq_tail = IOMMU_CQ_MASK;
    commands[IOMMU_CQ_MASK * 2] = 0xdead;
    /* Ring full: the slot is not touched and no doorbell is issued. */
    assert(iommu_cmd(IOMMU_CMD_IODIR, 1) < 0);
    assert(!publications && commands[IOMMU_CQ_MASK * 2] == 0xdead);
    registers[IOMMU_REG_CQH / 4] = IOMMU_CQ_MASK;
    complete = 1;
    assert(!iommu_cmd(IOMMU_CMD_IODIR, 2));
    assert(g_iommu_cq_tail == 0 && commands[IOMMU_CQ_MASK * 2 + 1] == 2);
    assert(!iommu_cmd(IOMMU_CMD_IOFENCE, 3) && g_iommu_cq_tail == 1);

    setup();
    complete = error_on_publish = 1;
    assert(iommu_cmd(IOMMU_CMD_IOTINVAL, 4) < 0);
    assert(g_iommu_cq_tail == 1 && publications == 1);
    assert(iommu_cmd(IOMMU_CMD_IOFENCE, 5) < 0);
    assert(g_iommu_cq_tail == 1 && publications == 1 && !commands[2]);
    setup();
    g_iommu_cq = NULL;
    assert(iommu_cmd(IOMMU_CMD_IOFENCE, 0) < 0 && !publications);

    /* Validate in 64 bits before narrowing the IOVA slot or shifting the
     * run mask. A huge IOVA must not alias slot 0 after uint32_t truncation. */
    setup(); complete = 1;
    uint64_t l0[IOMMU_USER_MAX_PAGES] = {0};
    iommu_user_domain_t *domain = &g_user_domains[0];
    *domain = (iommu_user_domain_t){.active=1, .devid=8, .owner_pid=42,
                                  .l0=l0, .mapped_pages=1};
    l0[0] = 0xbeef;
    assert(riscv_iommu_domain_unmap(8, 42, IOMMU_USER_IOVA, 65) < 0);
    assert(riscv_iommu_domain_unmap(8, 42, IOMMU_USER_IOVA, (uint32_t)-1) < 0);
    assert(riscv_iommu_domain_unmap(8, 42, IOMMU_USER_IOVA + (1ull << 44), 1) < 0);
    assert(riscv_iommu_domain_unmap(8, 42, IOMMU_USER_IOVA + PAGE_SIZE*63, 2) < 0);
    assert(!publications && l0[0] == 0xbeef && domain->mapped_pages == 1);
    domain->mapped_pages = 1ull << 63; l0[63] = 0xbeef;
    domain->pinned[63] = 100; refs[100] = 1;
    g_iommu_stats.mapped_pages = 1;
    assert(!riscv_iommu_domain_unmap(8, 42, IOMMU_USER_IOVA + PAGE_SIZE*63, 1));
    assert(!l0[63] && !domain->mapped_pages && !g_iommu_stats.mapped_pages);
    domain_lifetimes();
    native_domains();
    puts("RISC-V IOMMU: CQ retries, bounds, data/table pins and recovery PASS");
    return 0;
}
