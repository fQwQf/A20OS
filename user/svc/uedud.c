/* Minimal PCI user driver used to prove hardware-enforced DMA isolation.
 *
 * With one EDU function present it runs the single-device fault/recovery
 * sequence.  With two, it first claims both domains, proves each one
 * translates, faults device 0 and shows device 1 still transfers while the
 * faulted domain is still held, then releases both domains. */
#define DRV_ENV_USER 1
#include "drivers/dual/drv_env.h"
#include "drivers/driver_descriptor.h"
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_USER_SERVICE,
                      A20_DRIVER_TYPE_SECURITY, "edu-iommu-user",
                      A20_DRIVER_ABI, A20_DRIVER_RES_MMIO | A20_DRIVER_RES_DMA,
                      0, 1,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x1234, 0x11e8));

#define EDU_VENDOR       0x1234u
#define EDU_DEVICE       0x11e8u
#define EDU_DMA_SRC      0x80u
#define EDU_DMA_DST      0x88u
#define EDU_DMA_COUNT    0x90u
#define EDU_DMA_CMD      0x98u
#define EDU_DMA_RUN      0x1u
#define EDU_DMA_TO_GUEST 0x2u
#define EDU_BUF           0x40000u

static a20_handle_t g_out;

static void log_str(const char *s)
{
    if (g_out != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_out, s, a20_strlen(s), (void *)0);
}

static void log_hex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    char b[18];
    b[0] = '0';
    b[1] = 'x';
    for (unsigned i = 0; i < 16; i++)
        b[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xf];
    a20_hdl_write_buf(g_out, b, sizeof(b), (void *)0);
}

static void info_init(a20_device_info_args_t *info, unsigned index)
{
    *info = (a20_device_info_args_t){0};
    info->bus = A20_DRIVER_BUS_PCI;
    info->vendor = EDU_VENDOR;
    info->device = EDU_DEVICE;
    info->index = index;
}

static int edu_wait(uint64_t mmio)
{
    for (unsigned i = 0; i < 1000000; i++)
        if (!(drv_mmio_read32(mmio, EDU_DMA_CMD) & EDU_DMA_RUN))
            return 0;
    return -1;
}

static int edu_roundtrip(uint64_t mmio, drv_dma_t *dma)
{
    volatile uint8_t *buf = (volatile uint8_t *)(uintptr_t)dma->va0;
    uint64_t iova = drv_dma_phys(dma, 0);
    for (unsigned i = 0; i < 64; i++)
        buf[i] = (uint8_t)(0xa5u ^ i);
    __sync_synchronize();
    drv_mmio_write32(mmio, EDU_DMA_SRC, (uint32_t)iova);
    drv_mmio_write32(mmio, EDU_DMA_DST, EDU_BUF);
    drv_mmio_write32(mmio, EDU_DMA_COUNT, 64);
    drv_mmio_write32(mmio, EDU_DMA_CMD, EDU_DMA_RUN);
    if (edu_wait(mmio) < 0)
        return -2;

    for (unsigned i = 0; i < 64; i++)
        buf[i] = 0;
    __sync_synchronize();
    drv_mmio_write32(mmio, EDU_DMA_SRC, EDU_BUF);
    drv_mmio_write32(mmio, EDU_DMA_DST, (uint32_t)iova);
    drv_mmio_write32(mmio, EDU_DMA_COUNT, 64);
    drv_mmio_write32(mmio, EDU_DMA_CMD, EDU_DMA_RUN | EDU_DMA_TO_GUEST);
    if (edu_wait(mmio) < 0)
        return -3;
    __sync_synchronize();
    for (unsigned i = 0; i < 64; i++)
        if (buf[i] != (uint8_t)(0xa5u ^ i))
            return -4;
    return 0;
}

/* Trigger one write to an IOVA that is not mapped in this device's domain. */
static int edu_fault(uint64_t mmio, uint64_t iova)
{
    drv_mmio_write32(mmio, EDU_DMA_SRC, EDU_BUF);
    drv_mmio_write32(mmio, EDU_DMA_DST, (uint32_t)iova);
    drv_mmio_write32(mmio, EDU_DMA_COUNT, 64);
    drv_mmio_write32(mmio, EDU_DMA_CMD, EDU_DMA_RUN | EDU_DMA_TO_GUEST);
    return edu_wait(mmio);
}

static int device_ready(unsigned index, a20_device_info_args_t *info,
                        uint64_t *mmio)
{
    info_init(info, index);
    if (a20_device_get_info(info) != A20_OK ||
        !(info->out_flags & A20_DEVICE_INFO_F_IOMMU))
        return -1;
    if (a20_device_claim(info->out_mmio_base) != A20_OK)
        return -1;
    *mmio = drv_mmio_map(info->out_mmio_base, info->out_mmio_size, 3);
    return *mmio ? 0 : -1;
}

static int run_single(void)
{
    a20_device_info_args_t info;
    uint64_t mmio = 0;
    if (device_ready(0, &info, &mmio) < 0) {
        log_str("UEDUD: isolated device discovery failed\n");
        return 2;
    }

    drv_dma_t dma;
    if (drv_dma_alloc(&dma, 1) < 0 || edu_roundtrip(mmio, &dma) < 0) {
        log_str("UEDUD: mapped DMA failed\n");
        return 5;
    }
    log_str("UEDUD: mapped DMA ok iova=");
    log_hex(drv_dma_phys(&dma, 0));
    log_str("\n");

    uint64_t bad_iova = drv_dma_phys(&dma, 0) + DRV_PAGE_SIZE;
    if (edu_fault(mmio, bad_iova) < 0)
        return 6;

    info_init(&info, 0);
    if (a20_device_get_info(&info) != A20_OK ||
        !(info.out_flags & A20_DEVICE_INFO_F_BLOCKED) ||
        info.out_fault_count == 0 || info.out_fault_cause != 15 ||
        info.out_fault_iova < bad_iova ||
        info.out_fault_iova >= bad_iova + 64) {
        log_str("UEDUD: unmapped DMA was not isolated\n");
        return 7;
    }
    log_str("UEDUD: unmapped DMA fault iova=");
    log_hex(info.out_fault_iova);
    log_str("\n");

    drv_dma_free(&dma);
    if (a20_device_release(info.out_mmio_base) != A20_OK)
        return 8;

    /* A faulted process cannot leave stale translations or a poisoned device
     * behind: a fresh claim gets a fresh domain and can DMA normally. */
    if (a20_device_claim(info.out_mmio_base) != A20_OK)
        return 9;
    mmio = drv_mmio_map(info.out_mmio_base, info.out_mmio_size, 3);
    if (!mmio)
        return 10;
    drv_dma_t recovered;
    if (drv_dma_alloc(&recovered, 1) < 0 ||
        edu_roundtrip(mmio, &recovered) < 0)
        return 11;
    drv_dma_free(&recovered);
    if (a20_device_release(info.out_mmio_base) != A20_OK)
        return 12;
    log_str("UEDUD: recovered\nUEDUD: PASS\n");
    return 0;
}

static int run_matrix(void)
{
    a20_device_info_args_t info0, info1;
    uint64_t mmio0 = 0, mmio1 = 0;
    if (device_ready(0, &info0, &mmio0) < 0) {
        log_str("UEDUD: dev0 setup failed\n");
        return 20;
    }
    drv_dma_t dma0;
    if (drv_dma_alloc(&dma0, 1) < 0) {
        log_str("UEDUD: dev0 DMA allocation failed\n");
        return 21;
    }

    /* Claiming device 1 makes it the target of the next DMA allocation while
     * device 0 stays claimed and mapped. */
    if (device_ready(1, &info1, &mmio1) < 0) {
        log_str("UEDUD: dev1 setup failed\n");
        return 22;
    }
    drv_dma_t dma1;
    if (drv_dma_alloc(&dma1, 1) < 0) {
        log_str("UEDUD: dev1 DMA allocation failed\n");
        return 23;
    }
    log_str("UEDUD: two domains claimed\n");

    int r0 = edu_roundtrip(mmio0, &dma0);
    int r1 = edu_roundtrip(mmio1, &dma1);
    if (r0 < 0 || r1 < 0) {
        log_str("UEDUD: initial DMA failed\n");
        return 24;
    }

    uint64_t bad_iova = drv_dma_phys(&dma0, 0) + DRV_PAGE_SIZE;
    if (edu_fault(mmio0, bad_iova) < 0)
        return 25;
    info_init(&info0, 0);
    if (a20_device_get_info(&info0) != A20_OK ||
        !(info0.out_flags & A20_DEVICE_INFO_F_BLOCKED) ||
        info0.out_fault_count == 0) {
        log_str("UEDUD: dev0 fault was not reported\n");
        return 26;
    }
    log_str("UEDUD: dev0 faulted iova=");
    log_hex(info0.out_fault_iova);
    log_str("\n");

    /* Device 1 must keep working while device 0's domain is blocked and still
     * held: the fault queue is shared, so a drain started for device 0 must
     * not have attributed device 1's records to it. */
    if (edu_roundtrip(mmio1, &dma1) < 0) {
        log_str("UEDUD: dev1 affected by dev0 fault\n");
        return 27;
    }
    log_str("UEDUD: dev1 unaffected\n");

    drv_dma_free(&dma0);
    if (a20_device_release(info0.out_mmio_base) != A20_OK)
        return 28;
    /* Device 1's translation must survive releasing device 0's domain. */
    if (edu_roundtrip(mmio1, &dma1) < 0) {
        log_str("UEDUD: dev1 affected by dev0 release\n");
        return 29;
    }
    drv_dma_free(&dma1);
    if (a20_device_release(info1.out_mmio_base) != A20_OK)
        return 30;

    log_str("UEDUD: domains released\nUEDUD: PASS\n");
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 1;

    a20_device_info_args_t probe;
    info_init(&probe, 1);
    if (a20_device_get_info(&probe) == A20_OK)
        return run_matrix();
    return run_single();
}
