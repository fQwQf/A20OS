/*
 * A20OS Unified Driver Model — Core Definitions
 *
 * Provides the fundamental data structures for device/driver/bus
 * abstraction.  All driver code includes only this header (plus
 * driver_class.h and driver_hwapi.h) — never any arch/ headers.
 *
 * Design inspired by Linux platform_driver / RT-Thread rt_device,
 * simplified for a teaching/competition kernel.
 */
#ifndef _DRIVER_CORE_H
#define _DRIVER_CORE_H

#include "core/types.h"
#include "core/defs.h"

/* ============================================================
 * Forward declarations
 * ============================================================ */
struct device;
struct driver;
struct bus_type;
struct class_device;

/* ============================================================
 * device_id — identifies a device for driver matching
 *
 * For PCI:  vendor/device are PCI vendor/device IDs
 * For VirtIO: vendor = VIRTIO_VENDOR, device = VIRTIO_DEV_*
 * For platform: vendor = DT compatible hash, device = 0
 * ============================================================ */
#define VENDOR_ANY   0xFFFFFFFFUL
#define DEVICE_ANY   0xFFFFFFFFUL

typedef struct device_id {
    uint32_t vendor;
    uint32_t device;
    uint32_t subvendor;       /* optional, VENDOR_ANY if unused */
    uint32_t subdevice;       /* optional, DEVICE_ANY if unused */
    uint64_t driver_data;     /* opaque data passed to driver probe */
} device_id_t;

/* ============================================================
 * resource — describes a hardware resource (MMIO, IRQ, DMA)
 * ============================================================ */
enum resource_type {
    RES_UNUSED = 0,
    RES_IRQ,
    RES_MMIO,
    RES_DMA,
    RES_MEM,
    RES_IOPORT,
};

#define IORESOURCE_IRQ_EDGE     0x01
#define IORESOURCE_IRQ_LEVEL    0x02
#define IORESOURCE_MMIO_32BIT   0x04
#define IORESOURCE_MMIO_64BIT   0x08
#define IORESOURCE_DMA_COHERENT 0x10
#define IORESOURCE_PREFETCH     0x20

typedef struct resource {
    enum resource_type type;
    uint64_t           start;      /* inclusive */
    uint64_t           end;        /* inclusive */
    uint32_t           flags;
    const char        *name;       /* optional label */
} resource_t;

/* ============================================================
 * device — represents a hardware device instance
 *
 * Created by bus enumeration or board preset.  Bound to at most
 * one driver at a time.
 * ============================================================ */
#define DEV_STATE_UNINIT    0
#define DEV_STATE_PROBED    1
#define DEV_STATE_RUNNING   2
#define DEV_STATE_SUSPENDED 3
#define DEV_STATE_REMOVED   4
#define DEV_STATE_REMOVING  5

typedef struct device {
    const char        *name;       /* e.g. "virtio-net0" */
    struct device     *parent;     /* bus device or NULL */
    struct bus_type   *bus;        /* owning bus */
    struct driver     *drv;        /* bound driver (NULL = unbound) */
    void              *drv_priv;   /* driver private data (driver allocates) */
    void              *plat_data;  /* platform/board data */
    const device_id_t *matched_id; /* ID entry that caused the match */
    const device_id_t *hardware_id; /* platform/non-enumerable identity */
    resource_t        *res;        /* resource array */
    int                res_count;
    int                state;      /* DEV_STATE_* */
    struct class_device *class_dev; /* core-owned userspace publication */
} device_t;

/* ============================================================
 * driver — represents a driver implementation
 *
 * One driver can match many devices (via id_table).
 * class_type + class_ops provide the subsystem-level interface.
 * ============================================================ */
#define DEV_CLASS_NONE   0
#define DEV_CLASS_CHAR   1
#define DEV_CLASS_BLOCK  2
#define DEV_CLASS_NET    3
#define DEV_CLASS_INPUT  4
#define DEV_CLASS_DISPLAY 5
#define DEV_CLASS_AUDIO  6

typedef struct driver {
    const char         *name;      /* e.g. "virtio-net" */
    const device_id_t  *id_table;  /* NULL-terminated array */
    struct bus_type    *bus;       /* bus this driver lives on */

    /* lifecycle */
    int  (*probe)(device_t *dev);
    int  (*remove)(device_t *dev);
    int  (*suspend)(device_t *dev);   /* optional */
    int  (*resume)(device_t *dev);    /* optional */

    /* subsystem interface */
    const void         *class_ops;    /* block_dev_ops_t*, net_dev_ops_t*, etc. */
    uint32_t            class_type;   /* DEV_CLASS_BLOCK, DEV_CLASS_NET, ... */

    /* module linkage (NULL = built-in) */
    void               *module;
} driver_t;

/* ============================================================
 * bus_type — represents a bus (PCI, VirtIO-MMIO, platform)
 *
 * Responsible for device discovery and driver matching.
 * ============================================================ */
typedef struct bus_type {
    const char  *name;          /* "pci", "virtio-mmio", "platform" */

    /* driver-device matching */
    int   (*match)(device_t *dev, const driver_t *drv);
    int   (*probe)(device_t *dev);
    int   (*remove)(device_t *dev);

    /* resource management (optional) */
    int   (*alloc_resource)(device_t *dev, resource_t *res);
    void  (*free_resource)(device_t *dev, resource_t *res);

    /* hotplug notification (optional) */
    void  (*hotplug)(device_t *dev, int event);
} bus_type_t;

/* bus hotplug events */
#define BUS_EVENT_ADD       0
#define BUS_EVENT_REMOVE    1

/* ============================================================
 * Core API — registration and discovery
 * DRIVER_SMOKE_MATRIX: static gate covers virtio-blk, virtio-net, UART, PTY,
 * loop, PCI, and virtio-mmio build/probe anchors before section 7 is complete.
 * ============================================================ */

/*
 * Registration APIs reject NULL/incomplete objects with -EINVAL and duplicate
 * pointer registration with -EEXIST.  Registration may synchronously invoke
 * probe on existing objects; unregistration synchronously invokes remove.
 * These entry points run in task/boot context and must not be called by IRQ
 * handlers or recursively from lifecycle callbacks.
 */
void driver_core_init(void);
int  driver_register(driver_t *drv);
int  driver_unregister(driver_t *drv);
int  device_register(device_t *dev);
void device_unregister(device_t *dev);
int  bus_register(bus_type_t *bus);
void bus_unregister(bus_type_t *bus);

/* probe a specific device against all registered drivers */
int  bus_probe_device(device_t *dev);

/* find resource by type and index */
resource_t *device_get_resource(device_t *dev, enum resource_type type, int index);

/* iterate devices by class */
device_t *device_find_by_class(uint32_t class_type, int index);

/* probe all unbound devices against registered drivers */
void driver_probe_all(void);

/* ============================================================
 * Board configuration — one per board preset
 *
 * Provided by kernel/board/<board>/board.c
 * Selected at compile time by BOARD=<name> in Makefile
 * ============================================================ */
typedef struct irqchip_ops {
    void     (*init)(void);
    void     (*enable_irq)(uint32_t irq);
    void     (*disable_irq)(uint32_t irq);
    uint32_t (*ack)(void);
    void     (*eoi)(uint32_t irq);
} irqchip_ops_t;

typedef struct timer_ops {
    void     (*init)(void);
    void     (*set_interval)(uint64_t ticks);
    uint64_t (*read_ticks)(void);
    uint64_t (*ticks_per_sec)(void);
} timer_ops_t;

typedef struct smp_platform_ops smp_platform_ops_t;

typedef struct board_config {
    const char            *name;         /* "qemu-virt-rv64" */
    paddr_t                ram_base;
    paddr_t                ram_end;
    const irqchip_ops_t   *irqchip;
    const timer_ops_t     *timer;
    const smp_platform_ops_t *smp;
    void                 (*early_init)(void);   /* UART + MMIO setup */
    void                 (*poweroff)(void);
    void                 (*reboot)(void);

    /* bus enumeration: board calls device_register() for each device */
    void                 (*enumerate_devices)(void);
} board_config_t;

/* Global board config — defined in kernel/board/<board>/board.c */
extern const board_config_t *const current_board;

#endif /* _DRIVER_CORE_H */
