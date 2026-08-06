#ifndef _DRVMOD_DRVMOD_H
#define _DRVMOD_DRVMOD_H

/*
 * A20OS kernel driver modules (drvmod).
 *
 * Runtime-installable in-kernel drivers: a driver module is an ELF
 * relocatable object loaded into the kernel direct-map, and its external
 * symbols resolve ONLY against the driver framework export table
 * (kernel/drvmod/framework.c).  Drivers cannot reference arbitrary kernel
 * symbols; resource access (MMIO ranges, IRQ vectors) goes through
 * framework objects that validate what the manifest declared.
 *
 * Crash domain is not isolated (a buggy driver panics the kernel);
 * admission control is: manifest resource declaration + mandatory
 * signature (future) + framework-only symbols.
 *
 * Driver source model (registration + lifecycle callbacks):
 *   uintptr_t DriverEntry(drv_driver_t **out);
 *     - fills drv->name / drv->match[] (device ids)
 *     - registers callbacks: drv->probe, drv->remove
 *     - returns 0 on success, sets *out to the driver object
 *   int probe(drv_device_t *dev);   // per-device init
 *   void remove(drv_device_t *dev); // per-device teardown
 *
 * Binding: kernel init registers hardware devices with
 * drv_device_register(); drvmod_bind_all() matches each module's
 * match[] table against the registered devices and calls probe on
 * every match.  Modules must not probe hardware from DriverEntry.
 */

#include "core/types.h"

#define DRV_MOD_MAX_SIZE       (256 * 1024)
#define DRV_MOD_BUF_ORDER      6 /* 64 pages >= 256 KiB read buffer */
#define DRV_MOD_MAX_NAME       64
#define DRV_MOD_MAX_MATCHES    16

typedef struct drv_driver drv_driver_t;
typedef struct drv_device drv_device_t;

/* Device id match entry (bus + vendor/device). */
typedef struct drv_device_id {
    uint32_t bus;      /* 0 = fixed/system, 1 = PCI, 2 = USB, 3 = mmio */
    uint32_t vendor;
    uint32_t device;
} drv_device_id_t;

typedef int (*drv_probe_fn)(drv_device_t *dev);
typedef void (*drv_remove_fn)(drv_device_t *dev);
typedef void (*drv_isr_fn)(void *ctx);

struct drv_driver {
    const char *name;
    drv_device_id_t match[DRV_MOD_MAX_MATCHES];
    uint32_t match_count;
    drv_probe_fn probe;
    drv_remove_fn remove;
};

struct drv_device {
    drv_driver_t *driver;
    char name[32];
    /* Identity used by the binding pass (drv_device_id.match[]).  For
     * mmio/fixed devices vendor encodes the MMIO base address. */
    uint32_t bus;        /* 0 = fixed/system, 1 = PCI, 2 = USB, 3 = mmio */
    uint32_t vendor;
    uint32_t device;
    uintptr_t mmio_phys;
    uintptr_t mmio_size;
    void     *mmio_va;       /* kernel mapping */
    int       irq;
    void     *priv;          /* driver-private state */
};

/* ---- loader ---- */

/* Load an ELF driver module from an already-open file.  Returns a module
 * id (>= 0) or a negative errno. */
int drvmod_load(int fd, const char *name);

/* Unload an unbound module. Bound modules are rejected until their devices
 * have been removed through the framework lifecycle. */
int drvmod_unload(int id);

/* Call DriverEntry for every loaded module (device enumeration hook). */
void drvmod_init_all(void);

/* Match every loaded driver against the framework device table and call
 * probe on each match (automatic device binding). */
void drvmod_bind_all(void);

/* ---- framework API (the only symbols driver modules may link) ---- */

void *drv_alloc(size_t size);
void  drv_free(void *ptr);
void  drv_log(const char *fmt, ...);

/* MMIO: maps phys range into the module's kernel window with validation. */
int   drv_map_mmio(drv_device_t *dev, uintptr_t phys, size_t size);
void  drv_unmap_mmio(drv_device_t *dev);
uint32_t drv_read32(drv_device_t *dev, uintptr_t off);
void  drv_write32(drv_device_t *dev, uintptr_t off, uint32_t val);

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

/* Interrupts. */
int   drv_register_isr(drv_device_t *dev, drv_isr_fn isr, void *ctx);
void  drv_unregister_isr(drv_device_t *dev);

/* Register a hardware device for later module binding.  The device's
 * identity fields (bus/vendor/device) are matched against module
 * match[] tables by drvmod_bind_all(). */
int   drv_device_register(drv_device_t *dev);
void  drv_device_unregister(drv_device_t *dev);

/* ---- unified driver core bridge (module class drivers) ----
 * A module may register a full driver_t (kernel/drivers/core/driver_core.h)
 * into the kernel's unified driver model; probe/remove/class_ops live in
 * module memory and are called through the core like built-in drivers. */
int   drv_driver_register(void *drv);
int   drv_driver_unregister(void *drv);
int   drv_device_register_core(void *dev);
void  drv_device_unregister_core(void *dev);
void *drv_device_get_resource(void *dev, int type, int index);
void  drv_driver_probe_all(void);

#endif /* _DRVMOD_DRVMOD_H */
