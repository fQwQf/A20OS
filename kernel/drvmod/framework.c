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

#include "drvmod/drvmod.h"

#include "core/klog.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/lock.h"
#include "core/cpu.h"
#include "proc/proc.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/char/uart.h"
extern void input_mux_wake(void);
#include "core/sync.h"
#include "core/timer.h"
#include "proc/park.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "drivers/gpu/gpu_core.h"
#include "drivers/usb/usb.h"
#include "net/lwip_stack.h"

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
    /* The arch hook validates the range against the platform's direct-map
     * window (implemented in kernel/arch/<arch>/platform/arch_hooks.c). */
    if (!arch_drv_mmio_window_ok(phys, size))
        return 0;
    uint64_t flags = spin_lock_irqsave(&drv_framework_lock);
    for (int i = 0; i < (int)(sizeof(drv_mmio_cache) / sizeof(drv_mmio_cache[0])); i++) {
        if (!drv_mmio_cache[i].dev) {
            drv_mmio_cache[i].dev = dev;
            drv_mmio_cache[i].va = PAGE_OFFSET + phys;
            drv_mmio_cache[i].size = size;
            uintptr_t va = drv_mmio_cache[i].va;
            spin_unlock_irqrestore(&drv_framework_lock, flags);
            return va;
        }
    }
    spin_unlock_irqrestore(&drv_framework_lock, flags);
    return 0;
}

void drv_unmap_mmio(void *dev)
{
    if (!dev)
        return;
    uint64_t flags = spin_lock_irqsave(&drv_framework_lock);
    drv_mmio_entry_t *e = drv_mmio_find(dev);
    if (e)
        memset(e, 0, sizeof(*e));
    spin_unlock_irqrestore(&drv_framework_lock, flags);
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

void *drv_dma_alloc_coherent(size_t size, uint64_t *dma_handle)
{
    return dma_alloc_coherent(size, dma_handle);
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

void drv_driver_probe_all(void)
{
    driver_probe_all();
}

/* ---- export table (the loader's symbol whitelist) ---- */

struct drv_export {
    const char *name;
    void *addr;
};

/* platform_bus: shared platform device bus the modules bind against. */
extern bus_type_t platform_bus;
#if defined(CONFIG_X86_64)
extern unsigned x86_64_apic_to_cpu(unsigned apic_id);
#endif

const struct drv_export drv_export_table[] = {
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
    { "drv_map_mmio",        drv_map_mmio },
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
    { "drv_driver_probe_all", drv_driver_probe_all },
    { "device_get_resource",  (void *)device_get_resource },
    { "device_find_by_class",  (void *)device_find_by_class },
    { "platform_bus",        &platform_bus },
    /* PCI class-driver accessors (device_t-based; modules bind through
     * drv_driver_register with bus = &pci_bus) */
    { "pci_bus",             &pci_bus },
    { "pci_class_code",      (void *)pci_class_code },
    { "pci_device_id",       (void *)pci_device_id },
    { "pci_get_bar_resource", (void *)pci_get_bar_resource },
    { "pci_intx_irq",        (void *)pci_intx_irq },
    { "pci_enable_and_assign_bars", (void *)pci_enable_and_assign_bars },
    /* console input path (PS/2 module) */
    { "uart_receive_char",   (void *)uart_receive_char },
    /* scheduling / wait primitives used by module completion paths */
    { "proc_park_prepare",   (void *)proc_park_prepare },
    { "proc_yield",          (void *)proc_yield },
    { "arch_current_cpu_id", (void *)arch_current_cpu_id },
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
    { "mutex_lock",          (void *)mutex_lock },
    { "mutex_unlock",        (void *)mutex_unlock },
    { "timer_get_ticks",     (void *)timer_get_ticks },
    { "clock_ticks_per_sec", (void *)clock_ticks_per_sec },
    { "clock_get_ticks",     (void *)clock_get_ticks },
    { "klog_write",          (void *)klog_write },
    { "klog_level",          (void *)&klog_level },
    { "mdelay",              (void *)mdelay },
    { "udelay",              (void *)udelay },
    { "kmalloc",             (void *)kmalloc },
    { "kfree",               (void *)kfree },
    { "kcalloc",             (void *)kcalloc },
    /* DMA enhancements (aligned coherent + cache sync) */
    { "dma_alloc_coherent_aligned", (void *)dma_alloc_coherent_aligned },
    { "dma_free_coherent_aligned",  (void *)dma_free_coherent_aligned },
    { "dma_sync_for_cpu",    (void *)dma_sync_for_cpu },
    { "dma_sync_for_device", (void *)dma_sync_for_device },
    /* input mux wake path (vinput module ISRs) */
    { "input_mux_wake",      (void *)input_mux_wake },
    /* virtio PCI transport setup (vinput module PCI path) */
    { "pci_virtio_transport_init", (void *)pci_virtio_transport_init },
#if defined(CONFIG_X86_64)
    { "firmware_acpi_tpm2",  (void *)firmware_acpi_tpm2 },
#endif
    /* lock / scheduler primitives used by inline spinlock helpers
     * (arch_irqs_enabled & friends are static inline and compile into
     * the module itself; only the extern calls below are exported) */
    { "proc_current",        (void *)proc_current },
    { "proc_task_pid",       (void *)proc_task_pid },
    { "printf",              (void *)printf },
    { "panic",               (void *)panic },
    { "arch_virtio_blk_probe", (void *)arch_virtio_blk_probe },
    { "arch_virtio_net_probe", (void *)arch_virtio_net_probe },
#if defined(CONFIG_X86_64)
    { "x86_64_apic_to_cpu",  (void *)x86_64_apic_to_cpu },
#endif
    /* hwapi passthroughs used by module class drivers */
    { "dma_alloc_coherent",  (void *)dma_alloc_coherent },
    { "dma_free_coherent",   (void *)dma_free_coherent },
    { "request_irq",         (void *)request_irq },
    { "free_irq",            (void *)free_irq },
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
    /* USB core bridge (xhci / usb-hid / usb-storage) */
    { "usb_core_register_hcd",   (void *)usb_core_register_hcd },
    { "usb_core_unregister_hcd", (void *)usb_core_unregister_hcd },
    { "usb_control_msg",         (void *)usb_control_msg },
    { "usb_submit_urb",          (void *)usb_submit_urb },
    /* in-kernel lwIP bridge (virtio-net) */
    { "a20_lwip_lock",                 (void *)a20_lwip_lock },
    { "a20_lwip_unlock",               (void *)a20_lwip_unlock },
    { "a20_lwip_poll_locked",          (void *)a20_lwip_poll_locked },
    { "a20_lwip_process_netif_irq_locked", (void *)a20_lwip_process_netif_irq_locked },
    { "a20_lwip_rx_pending_any",       (void *)a20_lwip_rx_pending_any },
    { "a20_lwip_signal_rx_pending",    (void *)a20_lwip_signal_rx_pending },
};
const unsigned drv_export_count =
    sizeof(drv_export_table) / sizeof(drv_export_table[0]);
