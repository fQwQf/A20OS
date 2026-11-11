#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/core/riscv_iommu.c"

/* Registration keeps the probe linked under ASan. It must never run here. */
pfa_t pfa;
bus_type_t pci_bus;
int pci_enable_and_assign_bars(device_t *dev) { (void)dev; assert(0); return -1; }
resource_t *pci_get_bar_resource(device_t *dev, unsigned bar)
{ (void)dev; (void)bar; assert(0); return NULL; }
void *pci_map_bar(device_t *dev, unsigned bar)
{ (void)dev; (void)bar; assert(0); return NULL; }
pfn_t pfa_alloc_page(void) { assert(0); return PFN_NONE; }

static uint32_t registers[0x300 / 4];
static uint64_t commands[(IOMMU_CQ_MASK + 1) * 2];
static unsigned publications, failures;
static int complete, error_on_publish;

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
    if (error_on_publish) registers[IOMMU_REG_CQCSR / 4] |= IOMMU_QUEUE_ERROR;
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
    g_iommu_stats.mapped_pages = 1;
    assert(!riscv_iommu_domain_unmap(8, 42, IOMMU_USER_IOVA + PAGE_SIZE*63, 1));
    assert(!l0[63] && !domain->mapped_pages && !g_iommu_stats.mapped_pages);
    puts("RISC-V IOMMU CQ: timeout ownership, late completion, full/wrap/error PASS");
    return 0;
}
