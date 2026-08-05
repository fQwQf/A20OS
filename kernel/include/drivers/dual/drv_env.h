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

#endif
