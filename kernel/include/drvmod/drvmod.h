#ifndef _DRVMOD_DRVMOD_H
#define _DRVMOD_DRVMOD_H

/*
 * A20OS kernel driver modules (drvmod).
 *
 * A driver module is an ELF relocatable object (ET_REL) loaded into the
 * kernel direct-map.  Its external symbols resolve ONLY against the driver
 * framework export table (kernel/drvmod/framework.c).  There is no second
 * driver model: a module registers the SAME unified `driver_t` that built-in
 * drivers use (via drv_driver_register), so probe/remove/class binding and
 * devfs publication all run through the driver core.
 *
 * The module's `.a20drv` ELF descriptor section (a20_driver_descriptor_t,
 * kernel/include/drivers/driver_descriptor.h) is the only driver metadata.
 *
 * Driver source model:
 *   uintptr_t DriverEntry(void);
 *     - registers a unified driver_t with drv_driver_register()
 *     - returns 0 on success (non-zero = keep module unbound)
 *
 * Binding: the driver core matches the registered driver_t against every
 * device the bus/board enumerated (device_register triggers the probe), so
 * a module never does its own matching.  The kernel driver manager loads
 * modules from the DriverStore; no per-module device table exists.
 */

#include "core/types.h"
#include "drivers/driver_descriptor.h"

#define DRV_MOD_MAX_SIZE       (512 * 1024)
#define DRV_MOD_BUF_ORDER      7 /* 128 pages >= 512 KiB read buffer */
#define DRV_MOD_MAX_NAME       64

/* ---- loader ---- */

/* Load an ELF driver module from an already-open file.  Returns a module
 * id (>= 0) or a negative errno.  Does not run DriverEntry yet. */
int drvmod_load(int fd, const char *name);

/* Unload an unbound module.  Modules that registered a driver (pinned) are
 * rejected until driver_unregister has been wired for them. */
int drvmod_unload(int id);

/* Run DriverEntry for every loaded module.  Each module registers its
 * unified driver_t with the driver core. */
void drvmod_init_all(void);

/* ---- framework API (the only symbols driver modules may link) ---- */

void *drv_alloc(size_t size);
void  drv_free(void *ptr);
void  drv_log(const char *fmt, ...);

/* MMIO: maps phys range into the module's kernel window with validation.
 * Returns the direct-map VA (0 on failure); the mapping is cached on the
 * device token for drv_read32/drv_write32. */
uintptr_t drv_map_mmio(void *dev, uintptr_t phys, size_t size);
void  drv_unmap_mmio(void *dev);
uint32_t drv_read32(void *dev, uintptr_t off);
void  drv_write32(void *dev, uintptr_t off, uint32_t val);

/* Port-mapped I/O (x86). */
uint8_t drv_in8(uint16_t port);
void  drv_out8(uint16_t port, uint8_t value);

/* DMA: coherent, zeroed allocation with a stable device address. */
void *drv_dma_alloc_coherent(size_t size, uint64_t *dma_handle);
void  drv_dma_free_coherent(void *vaddr, size_t size, uint64_t dma_handle);

/* Delay / time. */
void  drv_udelay(unsigned usecs);
void  drv_mdelay(unsigned msecs);
uint64_t drv_clock_ticks(void);

/* Interrupts: register a framework ISR on a physical IRQ vector. */
int   drv_register_isr(uint32_t irq, void (*isr)(void *ctx), void *ctx);
void  drv_unregister_isr(uint32_t irq, void *ctx);

/* ---- unified driver core bridge (module class drivers) ----
 * A module registers a full driver_t (kernel/drivers/core/driver_core.h)
 * into the kernel's unified driver model; probe/remove/class_ops live in
 * module memory and are called through the core like built-in drivers. */
int   drv_driver_register(void *drv);
int   drv_driver_unregister(void *drv);
void *drv_device_get_resource(void *dev, int type, int index);
void  drv_driver_probe_all(void);

/* Arch hook (kernel/arch/<arch>/platform/arch_hooks.c): validates that the
 * physical range lies inside the platform's direct-map window.  Returns 0
 * to reject the mapping. */
int arch_drv_mmio_window_ok(uintptr_t phys, size_t size);

#endif /* _DRVMOD_DRVMOD_H */
