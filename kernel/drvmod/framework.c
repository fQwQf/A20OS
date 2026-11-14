/*
 * A20OS kernel driver framework.
 *
 * The ONLY kernel API surface available to driver modules (see the
 * drv_export_table below — the loader resolves driver symbols exclusively
 * against this table).  Framework objects: devices with validated MMIO
 * mappings and interrupt registrations, plus a curated bridge into the
 * unified driver core (driver_register & friends) and the hwapi layer
 * (ioport/DMA/IRQ/delay) so that module drivers can implement full class
 * devices (block/net/audio/...) with the same driver_t model as built-in
 * drivers.
 *
 * MMIO: every supported architecture maps physical memory at
 * PAGE_OFFSET + phys in the kernel direct map; the framework validates the
 * range against the platform's mapped window before handing the VA out.
 *
 * Interrupt handling: drv_register_isr() bridges to the hwapi IRQ table
 * (request_irq), which is fed by the arch IRQ dispatch
 * (driver_irq_dispatch).  Vector numbers are validated against the
 * hwapi line limit.
 */

#include "drvmod/export.h"
#include "drivers/core/dma_mapping.h"
#include "mm/vmo.h"
#include "drivers/core/riscv_iommu.h"
#include "drivers/core/amd_iommu.h"
#include "drivers/core/fence.h"
#include "drivers/core/queue.h"
#include "drivers/core/protocol.h"
#include "drivers/core/workqueue.h"
#include "drivers/core/firmware.h"
#include "drvmod/drvmod.h"

#include "core/bootargs.h"
#ifdef CONFIG_SHARED_DEVICE_CALL
#include "core/shared_device_call.h"
#include "core/shared_mmio.h"
#endif
#include "core/kallsyms.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/lock.h"
#include "core/cpu.h"
#include "core/perf.h"
#include "core/progress.h"
#include "core/random.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "sys/usercopy.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/memory.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_msix.h"
#include "drivers/bus/pci_msi.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/char/uart.h"
#include "drivers/char/fbcon.h"
#include "core/timekeeping.h"
#ifdef CONFIG_X86_64
#include "firmware/irq.h"
#include "platform.h"
#endif
extern void input_mux_wake(void);
#include "core/sync.h"
#include "core/timer.h"
#include "proc/park.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "drivers/gpu/gpu_core.h"
#include "drivers/usb/usb.h"
#include "net/stack.h"

#define DRV_MAX_IRQ_VECTOR 255   /* hwapi IRQ line limit (0..255) */
#define DRV_MAX_ISRS 64

typedef struct drv_isr_entry {
    int used;
    uint32_t irq;
    void (*isr)(void *ctx);
    void *ctx;
} drv_isr_entry_t;

/* Per-device MMIO mapping cache for drv_map_mmio/drv_read32/drv_write32.
 * Keyed by the unified device_t the module bound to. */
typedef struct drv_mmio_entry {
    void *dev;
    uintptr_t va;
    uint64_t size;
} drv_mmio_entry_t;

static spinlock_t drv_framework_lock = SPINLOCK_INIT;
static drv_isr_entry_t drv_isr_table[DRV_MAX_ISRS];
static drv_mmio_entry_t drv_mmio_cache[8];

/* ---- allocation ---- */

void *drv_alloc(size_t size)
{
    return kmalloc(size);
}

void drv_free(void *ptr)
{
    if (ptr)
        kfree(ptr);
}

void drv_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog_write("%s", buf);
}

/* ---- MMIO ---- */

static drv_mmio_entry_t *drv_mmio_find(void *dev)
{
    for (int i = 0; i < (int)(sizeof(drv_mmio_cache) / sizeof(drv_mmio_cache[0])); i++)
        if (drv_mmio_cache[i].dev == dev)
            return &drv_mmio_cache[i];
    return NULL;
}

uintptr_t drv_map_mmio(void *dev, uintptr_t phys, size_t size)
{
    if (!dev || size == 0 || size > 64 * 1024 * 1024)
        return 0;
    if (drv_mmio_find(dev))
        return 0;
    void *mapping = arch_device_map_mmio(phys, size, 0);
    if (!mapping) return 0;
    uint64_t flags = spin_lock_irqsave(&drv_framework_lock);
    for (int i = 0; i < (int)(sizeof(drv_mmio_cache) / sizeof(drv_mmio_cache[0])); i++) {
        if (!drv_mmio_cache[i].dev) {
            drv_mmio_cache[i].dev = dev;
            drv_mmio_cache[i].va = (uintptr_t)mapping;
            drv_mmio_cache[i].size = size;
            uintptr_t va = drv_mmio_cache[i].va;
            spin_unlock_irqrestore(&drv_framework_lock, flags);
            return va;
        }
    }
    spin_unlock_irqrestore(&drv_framework_lock, flags);
    arch_device_unmap_mmio(mapping, size);
    return 0;
}

void drv_unmap_mmio(void *dev)
{
    if (!dev)
        return;
    uint64_t flags = spin_lock_irqsave(&drv_framework_lock);
    drv_mmio_entry_t *e = drv_mmio_find(dev);
    uintptr_t mapping = e ? e->va : 0;
    size_t size = e ? (size_t)e->size : 0;
    if (e) memset(e, 0, sizeof(*e));
    spin_unlock_irqrestore(&drv_framework_lock, flags);
    if (mapping) arch_device_unmap_mmio((void *)mapping, size);
}

uint32_t drv_read32(void *dev, uintptr_t off)
{
    if (!dev)
        return 0;
    uint64_t flags = spin_lock_irqsave(&drv_framework_lock);
    drv_mmio_entry_t *e = drv_mmio_find(dev);
    uint32_t v = 0;
    if (e && e->va && off + 4 <= e->size)
        v = *(volatile uint32_t *)((char *)e->va + off);
    spin_unlock_irqrestore(&drv_framework_lock, flags);
    return v;
}

void drv_write32(void *dev, uintptr_t off, uint32_t val)
{
    if (!dev)
        return;
    uint64_t flags = spin_lock_irqsave(&drv_framework_lock);
    drv_mmio_entry_t *e = drv_mmio_find(dev);
    if (e && e->va && off + 4 <= e->size)
        *(volatile uint32_t *)((char *)e->va + off) = val;
    spin_unlock_irqrestore(&drv_framework_lock, flags);
}

/* ---- port-mapped I/O (x86) ---- */

uint8_t drv_in8(uint16_t port)
{
    return ioport_read8(port);
}

void drv_out8(uint16_t port, uint8_t value)
{
    ioport_write8(port, value);
}

/* ---- DMA ---- */

/* The drv_env contract is identical on the kernel, user and module placements
 * and none of them carries a device_t, so this entry point is the unconstrained
 * one: it allocates from the full 64-bit window.  A module driver that owns a
 * device_t must call dma_set_mask() and dma_alloc_coherent(dev, ...) instead,
 * both of which the framework exports. */
void *drv_dma_alloc_coherent(size_t size, uint64_t *dma_handle)
{
    return dma_alloc_coherent(NULL, size, dma_handle);
}

void drv_dma_free_coherent(void *vaddr, size_t size, uint64_t dma_handle)
{
    dma_free_coherent(vaddr, size, dma_handle);
}

/* ---- interrupts (bridged to hwapi) ---- */

static int drv_isr_trampoline(int irq, void *priv)
{
    (void)irq;
    drv_isr_entry_t *e = (drv_isr_entry_t *)priv;
    if (!e || !e->isr)
        return 0;
    e->isr(e->ctx);
    return 1;
}

int drv_register_isr(uint32_t irq, void (*isr)(void *ctx), void *ctx)
{
    if (!isr || irq == 0)
        return -EINVAL;
    if (irq > DRV_MAX_IRQ_VECTOR) {
        kerr("[DRV] irq %u out of range\n", irq);
        return -ERANGE;
    }
    uint64_t flags = spin_lock_irqsave(&drv_framework_lock);
    for (int i = 0; i < DRV_MAX_ISRS; i++) {
        if (drv_isr_table[i].used && drv_isr_table[i].irq == irq) {
            spin_unlock_irqrestore(&drv_framework_lock, flags);
            return -EBUSY;
        }
        if (!drv_isr_table[i].used) {
            drv_isr_table[i].used = 1;
            drv_isr_table[i].irq = irq;
            drv_isr_table[i].isr = isr;
            drv_isr_table[i].ctx = ctx;
            spin_unlock_irqrestore(&drv_framework_lock, flags);
            return request_irq(irq, drv_isr_trampoline, 0, &drv_isr_table[i]);
        }
    }
    spin_unlock_irqrestore(&drv_framework_lock, flags);
    return -ENOSPC;
}

void drv_unregister_isr(uint32_t irq, void *ctx)
{
    (void)ctx;
    uint64_t flags = spin_lock_irqsave(&drv_framework_lock);
    for (int i = 0; i < DRV_MAX_ISRS; i++) {
        if (drv_isr_table[i].used && drv_isr_table[i].irq == irq) {
            drv_isr_table[i].used = 0;
            spin_unlock_irqrestore(&drv_framework_lock, flags);
            free_irq(irq, &drv_isr_table[i]);
            return;
        }
    }
    spin_unlock_irqrestore(&drv_framework_lock, flags);
}

/* ---- delay / time ---- */

void drv_udelay(unsigned usecs)
{
    udelay(usecs);
}

void drv_mdelay(unsigned msecs)
{
    mdelay(msecs);
}

uint64_t drv_clock_ticks(void)
{
    return clock_get_ticks();
}

/* ---- unified driver core bridge ---- */

int drv_driver_register(void *drv)
{
    return driver_register((driver_t *)drv);
}

int drv_driver_unregister(void *drv)
{
    return driver_unregister((driver_t *)drv);
}

void *drv_device_get_resource(void *dev, int type, int index)
{
    return (void *)device_get_resource((device_t *)dev,
                                       (enum resource_type)type, index);
}

int drv_device_set_devfs_name(void *dev, const char *name)
{
    return device_set_devfs_name((device_t *)dev, name);
}

void drv_driver_probe_all(void)
{
    driver_probe_all();
}

/* ---- export table (the loader's symbol whitelist) ---- */

/* Ports without module-specific symbols need no additional exports. */
__attribute__((weak)) const struct drv_export arch_drv_export_table[] = {};
__attribute__((weak)) const unsigned arch_drv_export_count = 0;

extern bus_type_t platform_bus;

const struct drv_export drv_export_table[] = {
#ifdef CONFIG_SHARED_DEVICE_CALL
    { "shared_device_poll", (void *)shared_device_poll },
    { "shared_mmio_read",   (void *)shared_mmio_read },
    { "shared_mmio_write",  (void *)shared_mmio_write },
#endif
    { "strncpy",             strncpy },
    { "memset",              memset },
    { "memcpy",              memcpy },
    { "memcmp",              memcmp },
    { "strcmp",              strcmp },
    { "strlen",              strlen },
    { "strstr",              strstr },
    { "drv_alloc",           drv_alloc },
    { "drv_free",            drv_free },
    { "drv_log",             drv_log },
    /* Native asynchronous driver runtime. Providers and producers retain
     * their module until domain disposal / final fence signal has returned. */
    { "driver_dma_domain_create", driver_dma_domain_create },
    { "driver_dma_domain_create_checked", driver_dma_domain_create_checked },
    { "driver_dma_domain_create_managed", driver_dma_domain_create_managed },
    { "driver_dma_domain_create_managed_policy", driver_dma_domain_create_managed_policy },
    { "driver_dma_policy_create", driver_dma_policy_create },
    { "driver_dma_policy_get", driver_dma_policy_get },
    { "driver_dma_policy_put", driver_dma_policy_put },
    { "driver_dma_policy_check", driver_dma_policy_check },
    { "driver_dma_domain_shutdown", driver_dma_domain_shutdown },
    { "riscv_iommu_dma_domain_create", riscv_iommu_dma_domain_create },
    { "riscv_iommu_dma_fault_poll", riscv_iommu_dma_fault_poll },
    { "riscv_iommu_dma_watch_start", riscv_iommu_dma_watch_start },
    { "riscv_iommu_dma_watch_stop", riscv_iommu_dma_watch_stop },
    { "driver_dma_domain_create_direct", driver_dma_domain_create_direct },
    { "driver_dma_domain_get", driver_dma_domain_get },
    { "driver_dma_domain_put", driver_dma_domain_put },
    { "driver_dma_map_pages", driver_dma_map_pages },
    { "driver_dma_map_buffer", driver_dma_map_buffer },
    { "driver_dma_map_vmo", driver_dma_map_vmo },
    { "driver_dma_map_owned_vmo", (void *)driver_dma_map_owned_vmo },
    { "driver_dma_mapping_count", driver_dma_mapping_count },
    { "driver_dma_page_address", driver_dma_page_address },
    { "driver_dma_mapping_sync_for_device", driver_dma_mapping_sync_for_device },
    { "driver_dma_mapping_sync_for_cpu", driver_dma_mapping_sync_for_cpu },
    { "driver_dma_unmap_pages", driver_dma_unmap_pages },
    { "driver_dma_try_unmap_pages", driver_dma_try_unmap_pages },
    { "device_add_protocol", (void *)device_add_protocol },
    { "device_endpoint_get", (void *)device_endpoint_get },
    { "driver_endpoint_get", (void *)driver_endpoint_get },
    { "driver_endpoint_put", (void *)driver_endpoint_put },
    { "driver_endpoint_protocol_count", (void *)driver_endpoint_protocol_count },
    { "driver_endpoint_protocol_info", (void *)driver_endpoint_protocol_info },
    { "driver_protocol_begin", (void *)driver_protocol_begin },
    { "driver_protocol_end", (void *)driver_protocol_end },
    { "class_net_read_stats", (void *)class_net_read_stats },
    { "driver_queue_create", driver_queue_create },
    { "driver_queue_get", driver_queue_get },
    { "driver_queue_put", driver_queue_put },
    { "driver_queue_start", driver_queue_start },
    { "driver_queue_run", driver_queue_run },
    { "driver_queue_stop", driver_queue_stop },
    { "driver_queue_status", driver_queue_status },
    { "driver_request_create", driver_request_create },
    { "driver_request_get", driver_request_get },
    { "driver_request_put", driver_request_put },
    { "driver_request_submit", driver_request_submit },
    { "driver_request_complete", driver_request_complete },
    { "driver_request_cancel", driver_request_cancel },
    { "driver_request_command", driver_request_command },
    { "driver_request_memory", driver_request_memory },
    { "driver_request_fence", driver_request_fence },
    { "driver_request_private", driver_request_private },
    { "driver_request_set_private", driver_request_set_private },
    { "driver_fence_create", driver_fence_create },
    { "driver_fence_get", driver_fence_get },
    { "driver_fence_put", driver_fence_put },
    { "driver_fence_signal", driver_fence_signal },
    { "driver_fence_status", driver_fence_status },
    { "driver_fence_wait", driver_fence_wait },
    { "driver_fence_watch_create", driver_fence_watch_create },
    { "driver_fence_watch_try_remove", driver_fence_watch_try_remove },
    { "driver_workqueue_create", driver_workqueue_create },
    { "driver_work_create", driver_work_create },
    { "driver_work_get", driver_work_get },
    { "driver_work_put", driver_work_put },
    { "driver_work_schedule", driver_work_schedule },
    { "driver_work_reschedule", driver_work_reschedule },
    { "driver_work_cancel_sync", driver_work_cancel_sync },
    { "driver_workqueue_flush", driver_workqueue_flush },
    { "driver_workqueue_destroy", driver_workqueue_destroy },
    { "driver_firmware_source_create", driver_firmware_source_create },
    { "driver_firmware_source_create_vfs", driver_firmware_source_create_vfs },
    { "driver_firmware_source_get", driver_firmware_source_get },
    { "driver_firmware_source_put", driver_firmware_source_put },
    { "driver_firmware_load", driver_firmware_load },
    { "driver_firmware_from_memory", driver_firmware_from_memory },
    { "driver_firmware_get", driver_firmware_get },
    { "driver_firmware_put", driver_firmware_put },
    { "driver_firmware_data", driver_firmware_data },
    { "driver_firmware_size", driver_firmware_size },
    { "drv_map_mmio",        drv_map_mmio },
    { "device_map_mmio",     device_map_mmio },
    { "device_map_resource", device_map_resource },
    { "device_map_resource_exclusive", device_map_resource_exclusive },
    { "device_claim_resource", device_claim_resource },
    { "driver_resource_request", driver_resource_request },
    { "driver_resource_request_checked", driver_resource_request_checked },
    { "driver_resource_check_range", driver_resource_check_range },
    { "driver_resource_check_claims", driver_resource_check_claims },
    { "driver_resource_release", driver_resource_release },
    { "driver_memory_reserve", driver_memory_reserve },
    { "driver_memory_release", driver_memory_release },
    { "arch_device_map_mmio", arch_device_map_mmio },
    { "arch_device_unmap_mmio", arch_device_unmap_mmio },
    { "device_add_resource_action", device_add_resource_action },
    { "fw_node_match_id", fw_node_match_id },
    { "fw_node_has_property", fw_node_has_property },
    { "fw_node_read_u32", fw_node_read_u32 },
    { "fw_node_read_u64", fw_node_read_u64 },
    { "fw_node_read_string", fw_node_read_string },
    { "device_find_get_by_firmware_id", device_find_get_by_firmware_id },
    { "device_find_get_by_mmio", device_find_get_by_mmio },
    { "device_add_dependency", device_add_dependency },
    { "device_get", device_get },
    { "device_put", device_put },
    { "drv_unmap_mmio",      drv_unmap_mmio },
    { "drv_read32",          drv_read32 },
    { "drv_write32",         drv_write32 },
    { "drv_in8",             drv_in8 },
    { "drv_out8",            drv_out8 },
    { "drv_dma_alloc_coherent",       drv_dma_alloc_coherent },
    { "drv_dma_free_coherent",        drv_dma_free_coherent },
    { "drv_register_isr",    drv_register_isr },
    { "drv_unregister_isr",  drv_unregister_isr },
    { "drv_udelay",          drv_udelay },
    { "drv_mdelay",          drv_mdelay },
    { "drv_clock_ticks",     drv_clock_ticks },
    /* unified driver core bridge */
    { "drv_driver_register", drv_driver_register },
    { "drv_driver_unregister", drv_driver_unregister },
    { "drv_device_get_resource", drv_device_get_resource },
    /* Node-name request for a module's class device (e.g. /dev/vport0).  The
     * name string itself has to live in the module's own memory, so the core
     * takes a raw pointer instead of being exported directly. */
    { "drv_device_set_devfs_name", drv_device_set_devfs_name },
    { "drv_driver_probe_all", drv_driver_probe_all },
    { "device_get_resource",  (void *)device_get_resource },
    { "device_find_get_by_class",  (void *)device_find_get_by_class },
    /* platform bus IRQ resource channel.  A module-bound platform device
     * learns its line from the same board resource -- or, on RISC-V, the same
     * device-tree `interrupts` property -- a built-in driver reads, so the
     * fallback decision (line published -> request_irq, otherwise poll) is
     * made once, in the driver, instead of per package.  Without this export
     * dw-sdio.a20drv is rejected at load with "unresolved symbol
     * 'platform_device_irq'" and the SD card disappears with it. */
    { "platform_device_irq",  (void *)platform_device_irq },
    /* Kernel command line.  A driver that chooses between an interrupt and a
     * polling fallback at probe has to be able to read a20.<driver>.poll, and
     * the cmdline is the only place that can say whether the machine this
     * image booted on wants the fallback.  bootargs_init() runs in kernel_main
     * before driver enumeration, so the value is always readable by probe.
     * It is also how a driver that owns a hardware data plane takes a runtime
     * override -- e1000's a20.e1000.poll=1 forces the polling path on hardware
     * where an interrupt would otherwise be claimed.  The export has to exist
     * even for a module that never polls: the loader rejects a package with an
     * unresolved symbol rather than let it load and misbehave. */
    { "bootargs_get",         (void *)bootargs_get },
    /* A class driver that initialises its own instance (rather than being
     * handed one by a bus scan) has to publish it, or device_find_get_by_class()
     * cannot see it and mount_setup_block_device() finds no disk. */
    { "device_register",     (void *)device_register },
    { "device_unregister",   (void *)device_unregister },
    { "device_probe_use_remove", (void *)device_probe_use_remove },
    { "device_add_remove_prepare", (void *)device_add_remove_prepare },
    { "device_add_execution_action", (void *)device_add_execution_action },
    { "driver_lifecycle_begin", (void *)driver_lifecycle_begin },
    { "driver_lifecycle_try_begin", (void *)driver_lifecycle_try_begin },
    { "driver_lifecycle_end", (void *)driver_lifecycle_end },
    { "platform_bus",        &platform_bus },
    /* PCI class-driver accessors (device_t-based; modules bind through
     * drv_driver_register with bus = &pci_bus) */
    { "pci_bus",             &pci_bus },
    { "pci_class_code",      (void *)pci_class_code },
    { "pci_device_id",       (void *)pci_device_id },
    { "pci_map_bar", pci_map_bar },
    { "pci_get_bar_resource", (void *)pci_get_bar_resource },
    { "pci_intx_irq",        (void *)pci_intx_irq },
    { "pci_enable_and_assign_bars", (void *)pci_enable_and_assign_bars },
    { "pci_find_capability", (void *)pci_find_capability },
    { "pci_cfg_read16",      (void *)pci_cfg_read16 },
    { "pci_cfg_read32",      (void *)pci_cfg_read32 },
    { "pci_device_find_get", (void *)pci_device_find_get },
    { "pci_device_scope_get", (void *)pci_device_scope_get },
    { "pci_requester_reserve", (void *)pci_requester_reserve },
    { "pci_requester_release", (void *)pci_requester_release },
    { "device_is_registered", (void *)device_is_registered },
    { "amd_iommu_scope_get", (void *)amd_iommu_scope_get },
    { "amd_iommu_route_acquire", (void *)amd_iommu_route_acquire },
    { "amd_iommu_route_validate", (void *)amd_iommu_route_validate },
    { "amd_iommu_route_reserve", (void *)amd_iommu_route_reserve },
    { "amd_iommu_route_requesters", (void *)amd_iommu_route_requesters },
    { "amd_iommu_route_memory", (void *)amd_iommu_route_memory },
    { "amd_iommu_route_policy_create", (void *)amd_iommu_route_policy_create },
    { "amd_iommu_route_release", (void *)amd_iommu_route_release },
    { "pci_cfg_write16",     (void *)pci_cfg_write16 },
    { "pci_cfg_write32",     (void *)pci_cfg_write32 },
    /* message-signalled interrupts: capability parse, table programming and
     * the vector allocator the table entries name */
    { "pci_msi_capability",  (void *)pci_msi_capability },
    { "pci_msi_enable",      (void *)pci_msi_enable },
    { "pci_msi_disable",     (void *)pci_msi_disable },
    { "pci_msix_capability", (void *)pci_msix_capability },
    { "pci_msix_enable",     (void *)pci_msix_enable },
    { "pci_msix_program_vector", (void *)pci_msix_program_vector },
    { "pci_msix_set_vector_mask", (void *)pci_msix_set_vector_mask },
    /* Which CPU a message-signalled entry is aimed at: read back, and move */
    { "pci_msix_set_affinity", (void *)pci_msix_set_affinity },
    { "pci_msix_get_affinity", (void *)pci_msix_get_affinity },
    { "pci_msix_commit",     (void *)pci_msix_commit },
    { "pci_msix_disable",    (void *)pci_msix_disable },
    { "irq_alloc_vectors",   (void *)irq_alloc_vectors },
    { "irq_free_vectors",    (void *)irq_free_vectors },
    /* console input path (PS/2 module) */
    { "uart_receive_char",   (void *)uart_receive_char },
    /* scheduling / wait primitives used by module completion paths */
    { "proc_park_prepare",   (void *)proc_park_prepare },
    { "proc_yield",          (void *)proc_yield },
    { "signal_task_has_unblocked", (void *)signal_task_has_unblocked },
    { "arch_current_cpu_id", (void *)arch_current_cpu_id },
    { "g_a20_perf_enabled", (void *)&g_a20_perf_enabled },
    { "g_a20_perf_percpu", (void *)g_a20_perf_percpu },
    { "proc_park_commit",    (void *)proc_park_commit },
    { "proc_park_cancel",    (void *)proc_park_cancel },
    { "proc_park_finish",    (void *)proc_park_finish },
    { "proc_wake_q_init",    (void *)proc_wake_q_init },
    { "proc_wake_q_flush",   (void *)proc_wake_q_flush },
    { "wait_queue_init",     (void *)wait_queue_init },
    { "wait_queue_link",     (void *)wait_queue_link },
    { "wait_queue_unlink",   (void *)wait_queue_unlink },
    { "wait_queue_collect_one", (void *)wait_queue_collect_one },
    { "wait_queue_collect_all", (void *)wait_queue_collect_all },
    /* mutex + timer + log + allocator primitives */
    { "mutex_init",          (void *)mutex_init },
    { "mutex_trylock",       (void *)mutex_trylock },
    { "mutex_lock",          (void *)mutex_lock },
    { "mutex_unlock",        (void *)mutex_unlock },
    { "timer_get_ticks",     (void *)timer_get_ticks },
    { "clock_ticks_per_sec", (void *)clock_ticks_per_sec },
    /* The block progress bridge is gated on an aggregated pending bit; a
     * deployed virtio-blk module ORs it when it publishes a request. */
    { "kernel_progress_note_pending", (void *)kernel_progress_note_pending },
    /* Kernel entropy pool (kernel/core/random.c).  A hardware RNG module
     * (virtio-rng) feeds what it reads back into the pool that sys_getrandom()
     * and the ASLR/stack-canary seed draw from; without this entry the loader
     * rejects the package with an unresolved symbol rather than letting it
     * load and silently run with the entropy dropped.  random_reseed() is
     * callable from a module read path but not from an ISR -- it reaches
     * proc_current() through arch_entropy_sample(). */
    { "random_reseed",        (void *)random_reseed },
    { "random_fill",          (void *)random_fill },
    { "clock_get_ticks",     (void *)clock_get_ticks },
    { "klog_write",          (void *)klog_write },
    { "klog_write_ex",       (void *)klog_write_ex },
    { "klog_level",          (void *)&klog_level },
    { "mdelay",              (void *)mdelay },
    { "udelay",              (void *)udelay },
    { "kmalloc",             (void *)kmalloc },
    { "kfree",               (void *)kfree },
    { "kcalloc",             (void *)kcalloc },
    /* DMA enhancements (aligned coherent + cache sync) plus the address mask:
     * a module that owns a device_t declares its window and then hands the
     * device to the allocator, so its handles are constrained the same way a
     * built-in driver's are. */
    { "driver_dma_buffer_create", (void *)driver_dma_buffer_create },
    { "driver_dma_buffer_get", (void *)driver_dma_buffer_get },
    { "driver_dma_buffer_put", (void *)driver_dma_buffer_put },
    { "driver_dma_buffer_data", (void *)driver_dma_buffer_data },
    { "driver_dma_buffer_address", (void *)driver_dma_buffer_address },
    { "driver_dma_buffer_size", (void *)driver_dma_buffer_size },
    { "dma_alloc_coherent_aligned", (void *)dma_alloc_coherent_aligned },
    { "dma_free_coherent_aligned",  (void *)dma_free_coherent_aligned },
    { "dma_set_mask",   (void *)dma_set_mask },
    { "dma_addr_ok",    (void *)dma_addr_ok },
    { "dma_range_ok",   (void *)dma_range_ok },
    { "dma_sync_for_cpu",    (void *)dma_sync_for_cpu },
    { "dma_sync_for_device", (void *)dma_sync_for_device },
    /* input mux wake path (vinput module ISRs) */
    { "input_mux_wake",      (void *)input_mux_wake },
    /* virtio PCI transport setup (vinput module PCI path) */
    { "pci_virtio_transport_init", (void *)pci_virtio_transport_init },
    { "virtio_transport_reset", (void *)virtio_transport_reset },
    { "timekeeping_wallclock_set_hw", (void *)timekeeping_wallclock_set_hw },
    { "timekeeping_wallclock_from_hw", (void *)timekeeping_wallclock_from_hw },
    /* lock / scheduler primitives used by inline spinlock helpers
     * (arch_irqs_enabled & friends are static inline and compile into
     * the module itself; only the extern calls below are exported) */
    { "proc_current",        (void *)proc_current },
    { "proc_task_pid",       (void *)proc_task_pid },
    { "printf",              (void *)printf },
    /* A module that formats into a fixed buffer needs the bounded variants.
     * Without these, any .a20drv whose driver builds a device name (or any
     * other small string) fails to load with "unresolved symbol
     * 'snprintf'", which takes the whole transport down with it -- the
     * aarch64 embedded build ships virtio-blk as a module, so its disks
     * silently vanished and /bin never mounted. */
    { "snprintf",            (void *)snprintf },
    { "vsnprintf",           (void *)vsnprintf },
    { "panic",               (void *)panic },
    /* kallsyms debug name lookup (spinlock self-acquire diagnostics).
     * kallsyms_print is referenced by the inline spin_lock_at stall reporter
     * (core/lock.h), so any .a20drv module that includes lock.h needs it
     * exported or the loader rejects the module with an unresolved symbol. */
    { "kallsyms_lookup",     (void *)kallsyms_lookup },
    { "kallsyms_print",      (void *)kallsyms_print },
    /* hwapi passthroughs used by module class drivers */
    { "dma_alloc_coherent",  (void *)dma_alloc_coherent },
    { "dma_free_coherent",   (void *)dma_free_coherent },
    { "request_irq",         (void *)request_irq },
    { "free_irq",            (void *)free_irq },
    { "synchronize_irq",     (void *)synchronize_irq },
#ifdef CONFIG_X86_64
    /* Shared core lock headers use this function as the early-CPU fallback;
     * do not export the APIC lookup table or let modules own CPU topology. */
    { "x86_64_apic_to_cpu", (void *)x86_64_apic_to_cpu },
    { "ioport_read8", (void *)ioport_read8 },
    { "ioport_write8", (void *)ioport_write8 },
    /* Legacy i8042 routing: PS/2 is a loadable x86 module and must be able
     * to program GSI1/GSI12 before request_irq() unmasks the line. */
    { "arch_firmware_irq_map", (void *)arch_firmware_irq_map },
    { "firmware_legacy_irq_map", (void *)firmware_legacy_irq_map },
    { "x86_64_route_isa_irq_forced", (void *)x86_64_route_isa_irq_forced },
#endif
    { "irq_enable",          (void *)irq_enable },
    { "irq_disable",         (void *)irq_disable },
    /* frame allocator passthroughs (virtio-gpu framebuffer pages) */
    { "pfa",                 (void *)&pfa },
    { "pfa_alloc",           (void *)pfa_alloc },
    { "pfa_free",            (void *)pfa_free },
    /* GPU class registry (virtio-gpu / vmsvga) */
    { "gpu_device_register",   (void *)gpu_device_register },
    { "gpu_device_unregister", (void *)gpu_device_unregister },
    { "gpu_device_get_default", (void *)gpu_device_get_default },
    { "gpu_device_default_get", (void *)gpu_device_default_get },
    { "gpu_device_access_begin", (void *)gpu_device_access_begin },
    { "gpu_device_capabilities_get", (void *)gpu_device_capabilities_get },
    { "gpu_device_default_begin", (void *)gpu_device_default_begin },
    { "gpu_device_access_end", (void *)gpu_device_access_end },
    { "class_device_binding_stopped", (void *)class_device_binding_stopped },
    { "device_release_resource_action", (void *)device_release_resource_action },
    { "gpu_device_scanout_get", (void *)gpu_device_scanout_get },
    { "gpu_scanout_put", (void *)gpu_scanout_put },
    { "vmo_create_contiguous", (void *)vmo_create_contiguous },
    { "vmo_create_resident", (void *)vmo_create_resident },
    { "vmo_contiguous_info", (void *)vmo_contiguous_info },
    { "vmo_ref", (void *)vmo_ref },
    { "vmo_release", (void *)vmo_release },
    /* user copy helpers (virtio-gpu 3D ioctl passthrough) */
    { "copy_from_user",        (void *)copy_from_user },
    { "copy_to_user",          (void *)copy_to_user },
    /* HID console bridging observes scanout ownership through a query;
     * modules do not receive the console state or take over rendering. */
    { "fbcon_active",            (void *)fbcon_active },
    /* USB core bridge (xhci / usb-hid / usb-hub / usb-storage) */
    { "usb_core_register_hcd",   (void *)usb_core_register_hcd },
    { "usb_core_unregister_hcd", (void *)usb_core_unregister_hcd },
    { "usb_core_alloc_address",  (void *)usb_core_alloc_address },
    { "usb_core_free_address",   (void *)usb_core_free_address },
    { "usb_control_msg",         (void *)usb_control_msg },
    { "usb_submit_urb",          (void *)usb_submit_urb },
    { "usb_device_poll",         (void *)usb_device_poll },
    { "usb_interface_release_endpoints", (void *)usb_interface_release_endpoints },
    /* Backend-neutral in-kernel network bridge. Loadable NICs never acquire a
     * protocol-private lock or enter FreeBSD/lwIP from their IRQ context. */
    { "net_stack_receive_progress",  (void *)net_stack_receive_progress },
    { "net_stack_protocol_progress", (void *)net_stack_protocol_progress },
    { "net_stack_timer_tick",        (void *)net_stack_timer_tick },
    { "net_stack_device_interrupt",  (void *)net_stack_device_interrupt },
    { "net_stack_work_pending",      (void *)net_stack_work_pending },
    { "net_stack_poll_waiter",       (void *)net_stack_poll_waiter },
};
const unsigned drv_export_count =
    sizeof(drv_export_table) / sizeof(drv_export_table[0]);
