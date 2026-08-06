/*
 * Dual-placement driver environment (docs/hybrid-kernel/04-dual-placement.md).
 *
 * A dual-placement driver is written once against the drv_* ops below and
 * compiled either into the kernel (DRV_ENV_KERNEL) or into a user-space
 * driver task (DRV_ENV_USER).  The ops cover only what both placements can
 * provide with identical semantics; anything inherently placement-specific
 * (service main loop vs kernel ISR/init hooks) stays in the driver's shell.
 *
 * Current op set (skeleton, phase 3): MMIO map + 32-bit register access.
 * DMA buffer ops arrive with the IOMMU work; IRQ delivery stays in the
 * shell for now (kernel handler vs user EventQ wait) because the two
 * placements legitimately differ in threading model.
 */
#ifndef _DRIVERS_DUAL_DRV_ENV_H
#define _DRIVERS_DUAL_DRV_ENV_H

#if defined(DRV_ENV_KERNEL)

#include "core/types.h"
#include "mm/frame.h" /* PAGE_OFFSET */

/* Kernel placement: qemu-virt device windows are direct-mapped.  Platforms
 * without a full direct map must route this through a real ioremap (TODO). */
static inline uint64_t drv_mmio_map(uint64_t phys, uint64_t size, uint32_t prot)
{
    (void)size;
    (void)prot;
    return phys + PAGE_OFFSET;
}

static inline void drv_mmio_unmap(uint64_t va, uint64_t size)
{
    (void)va;
    (void)size;
}

#elif defined(DRV_ENV_USER)

#include "liba20rt/a20_sdk.h"

/* User placement: the kernel maps the whitelisted window into the driver
 * task (PFNMAP) via the udriver contract (kernel/include/drivers/core/udriver.h). */
static inline uint64_t drv_mmio_map(uint64_t phys, uint64_t size, uint32_t prot)
{
    uint64_t va = 0;
    if (a20_device_map_mmio(phys, size, prot, &va) != A20_OK)
        return 0;
    return va;
}

static inline void drv_mmio_unmap(uint64_t va, uint64_t size)
{
    if (va)
        a20_vm_unmap(va, size);
}

#else
#error "drv_env.h requires DRV_ENV_KERNEL or DRV_ENV_USER"
#endif

/* Identical on both sides: a mapped window is plain volatile memory. */
static inline uint32_t drv_mmio_read32(uint64_t base, uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}

static inline void drv_mmio_write32(uint64_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(base + off) = val;
}

static inline uint8_t drv_mmio_read8(uint64_t base, uint32_t off)
{
    return *(volatile uint8_t *)(uintptr_t)(base + off);
}

static inline void drv_mmio_write8(uint64_t base, uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(uintptr_t)(base + off) = val;
}

/* ---- DMA buffers (page-granular; trust model until IOMMU lands) ----
 *
 * Identical contract on both sides: allocate @npages of DMA-capable
 * memory, query each page's device-visible physical address.  Caveats
 * that keep this honest:
 *  - kernel placement hands out physically contiguous frames; user
 *    placement gets a VMO whose pages may be non-contiguous, so
 *    protocols requiring multi-page contiguous DMA must stay
 *    single-page until the DMA-heap/IOMMU work (04-dual-placement.md);
 *  - without IOMMU there is no hardware enforcement: user placement
 *    is protected only by the udriver contract (kernel allocates,
 *    pins and translates); do not widen what the op promises.
 */
#define DRV_PAGE_SIZE 4096u
typedef struct drv_dma {
    uint64_t va0;                 /* VA of page 0 */
    uint64_t cookie;              /* kernel: base pfn; user: VMO handle */
    uint32_t npages;
    uint32_t _pad;
    uint64_t phys[64];            /* per-page device-visible address */
} drv_dma_t;

#if defined(DRV_ENV_KERNEL)

#include "mm/frame.h"

static inline int drv_dma_alloc(drv_dma_t *out, uint32_t npages)
{
    if (!out || npages == 0 || npages > 64)
        return -1;
    unsigned order = 0;
    while ((1u << order) < npages)
        order++;
    pfn_t pfn = pfa_alloc((int)order);
    if (pfn == PFN_NONE)
        return -1;
    void *va = pfn_to_virt(pfn);
    extern void *memset(void *, int, size_t);
    memset(va, 0, (uint64_t)npages * DRV_PAGE_SIZE);
    uint64_t pa = (uint64_t)pfn * DRV_PAGE_SIZE;
    for (uint32_t i = 0; i < npages; i++)
        out->phys[i] = pa + (uint64_t)i * DRV_PAGE_SIZE;
    for (uint32_t i = npages; i < 64; i++)
        out->phys[i] = 0;
    out->va0 = (uint64_t)(uintptr_t)va;
    out->cookie = (uint64_t)pfn;
    out->npages = npages;
    return 0;
}

static inline void drv_dma_free(drv_dma_t *d)
{
    if (!d || !d->va0)
        return;
    unsigned order = 0;
    while ((1u << order) < d->npages)
        order++;
    pfa_free((pfn_t)d->cookie, (int)order);
    d->va0 = d->cookie = 0;
    d->npages = 0;
}

#elif defined(DRV_ENV_USER)

static inline int drv_dma_alloc(drv_dma_t *out, uint32_t npages)
{
    if (!out || npages == 0 || npages > 64)
        return -1;
    a20_status_t r = a20_device_alloc_dma((uint64_t)npages * DRV_PAGE_SIZE);
    if (r < 0)
        return -1;
    a20_handle_t vmo = (a20_handle_t)r;
    uint64_t va = 0;
    if (a20_status_is_err(a20_vm_map(vmo, (uint64_t)npages * DRV_PAGE_SIZE, 0,
                                     A20_PROT_READ | A20_PROT_WRITE, &va))) {
        a20_hdl_close(vmo);
        return -1;
    }
    /* The contiguous-DMA syscall pre-materializes and zeroes pages. */
    uint64_t paddrs[64];
    uint32_t count = 0;
    if (a20_device_vmo_phys(vmo, paddrs, npages, &count) != A20_OK ||
        count != npages) {
        a20_vm_unmap(va, (uint64_t)npages * DRV_PAGE_SIZE);
        a20_hdl_close(vmo);
        return -1;
    }
    for (uint32_t i = 0; i < npages; i++)
        out->phys[i] = paddrs[i];
    for (uint32_t i = npages; i < 64; i++)
        out->phys[i] = 0;
    out->va0 = va;
    out->cookie = (uint64_t)vmo;
    out->npages = npages;
    return 0;
}

static inline void drv_dma_free(drv_dma_t *d)
{
    if (!d || !d->va0)
        return;
    a20_vm_unmap(d->va0, (uint64_t)d->npages * DRV_PAGE_SIZE);
    a20_hdl_close((a20_handle_t)d->cookie);
    d->va0 = d->cookie = 0;
    d->npages = 0;
}

#endif

static inline uint64_t drv_dma_phys(const drv_dma_t *d, uint32_t page)
{
    if (!d || page >= d->npages)
        return 0;
    return d->phys[page];
}

static inline uint64_t drv_dma_va(const drv_dma_t *d, uint32_t page)
{
    if (!d || page >= d->npages)
        return 0;
    return d->va0 + (uint64_t)page * DRV_PAGE_SIZE;
}

#endif
