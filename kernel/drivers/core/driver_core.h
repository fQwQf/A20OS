/*
 * A20OS Unified Driver Model — Core Definitions
 *
 * Provides the fundamental data structures for device/driver/bus
 * abstraction.  All driver code includes only this header (plus
 * driver_class.h and driver_hwapi.h) — never any arch/ headers.
 *
 * Design inspired by Linux platform_driver / RT-Thread rt_device,
 * simplified for a teaching/reference kernel.
 */
#ifndef _DRIVER_CORE_H
#define _DRIVER_CORE_H

#include "core/types.h"
#include "core/defs.h"
#include "drivers/driver_descriptor.h"
/* For CLASS_DEVICE_NAME_MAX (the class-device publication object and the
 * devfs_name field of device_t must agree on the name length). */
#include "drivers/core/driver_class.h"

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
    int                user_owned; /* owned by a user-service driver; only
                                    * read-only kernel probes may bind */
    struct class_device *class_dev; /* core-owned userspace publication */

    /* Bus-master DMA address mask: the address bits this device's DMA engine
     * can be handed.  Appended last because a driver_t/device_t pair is shared
     * with loadable .a20drv modules, whose layout must keep matching.
     *
     * 0 -- the value every zero-initialised device_t carries -- means "not
     * declared", and dma_get_mask() reports it as DMA_MASK_64BIT.  That is what
     * keeps a bus enumerator or board file that never calls dma_set_mask() on
     * the full 64-bit window without having to edit every static device_t, and
     * it is also the honest default: nothing narrows a device until its driver
     * has read a capability out of the hardware and said so.
     * dma_set_mask() rejects a mask that is not a run of low-order ones rather
     * than rounding it, so this field never holds a half-declared window. */
    uint64_t           dma_mask;

    /* Node name the driver wants for the devfs entry class_device_publish()
     * creates, e.g. "vport0" instead of the generated "char0".  NULL -- the
     * zero-initialised default -- keeps the generated name.  Appended last for
     * the same reason as dma_mask: the layout is shared with loadable .a20drv
     * modules.  Set it from probe() with device_set_devfs_name(), which the
     * driver calls before returning 0, because publication happens right after
     * probe returns. */
    char               devfs_name[CLASS_DEVICE_NAME_MAX];
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

    /* Optional device-tree compatible string this driver claims, e.g.
     * "starfive,jh7110-mmc".  With it set, the driver binds to the node carrying
     * that compatible at the addresses the firmware described, instead of every
     * board file hand-copying an MMIO base.  NULL when not device-tree described. */
    const char         *of_compatible;

    /* Optional protocol-level narrowing after the bus ID match.  It must not
     * access device registers or allocate resources. */
    int  (*match)(device_t *dev);

    /* lifecycle */
    int  (*probe)(device_t *dev);
    int  (*remove)(device_t *dev);
    int  (*suspend)(device_t *dev);   /* optional */
    int  (*resume)(device_t *dev);    /* optional */
    void (*progress)(device_t *dev);  /* optional non-IRQ completion drain */

    /* subsystem interface */
    const void         *class_ops;    /* block_dev_ops_t*, net_dev_ops_t*, etc. */
    uint32_t            class_type;   /* DEV_CLASS_BLOCK, DEV_CLASS_NET, ... */

    /* module linkage (NULL = built-in) */
    void               *module;

    /* read-only probe: may bind user-owned devices but must not claim or
     * destructively initialize them (dual-placement kernel shell). */
    int                 read_only_probe;
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
/* Format registered drivers for /proc/drivers. */
int  driver_core_list_drivers(char *buf, size_t sz);
int  driver_register(driver_t *drv);
int  driver_unregister(driver_t *drv);
int  device_register(device_t *dev);
void device_unregister(device_t *dev);
/* Publish a bus add/remove event through the normal device lifecycle.  Bus
 * callbacks observe ADD after probe and REMOVE after remove has completed. */
int  device_hotplug(device_t *dev, int event);
int  bus_register(bus_type_t *bus);
void bus_unregister(bus_type_t *bus);

/* probe a specific device against all registered drivers */
int  bus_probe_device(device_t *dev);

resource_t *device_get_resource(device_t *dev, enum resource_type type, int index);

/*
 * Ask for a specific devfs node name for this device, e.g. "vport0" instead of
 * the generated "char3".  Callable only from probe(), before it returns: the
 * class device is published immediately afterwards and copies the name then.
 * Rejects an empty name, a name longer than CLASS_DEVICE_NAME_MAX - 1, and any
 * name carrying '/', so a driver cannot steer the name outside the device's
 * own devfs directory.  Returns 0 on success, -EINVAL otherwise.
 */
int device_set_devfs_name(device_t *dev, const char *name);

/* iterate devices by class */
device_t *device_find_by_class(uint32_t class_type, int index);

/* probe all unbound devices against registered drivers */
void driver_probe_all(void);
void driver_progress_class(uint32_t class_type);

/* Find the platform-bus driver claiming a device-tree compatible string and
 * report the first id_table entry it would bind as.  Returns 0 and leaves the
 * outputs untouched when no driver claims it, so a caller walking a device tree
 * can skip nodes nothing supports instead of inventing an identity for them. */
int  driver_lookup_compatible(const char *compatible,
                              uint32_t *vendor, uint32_t *device);

/* ------------------------------------------------------------------ */
/*  Unified driver manager                                             */
/* ------------------------------------------------------------------ */

/* Read the .a20drv descriptor section from an open ELF file. */
int driver_descriptor_read(int fd, a20_driver_descriptor_t *out);

/* Driver manager bootstrap: register module-owned devices, scan the
 * DriverStore (/bin/lib/drivers) and activate every .a20drv package.
 * Called once from init_kthread after the filesystem is available. */
/* generic deployment activation is deliberately split around root mounting:
 * early packages may provide the root device, while runtime activation may
 * create user-service processes after proc_init(). */
void driver_manager_early_init(void);
void driver_manager_init(void);

/* Activate a single driver package from the store (used by rescan). */
int driver_manager_activate(const char *store_path);

/* Spawn a user-service driver as a native user process. */
int driver_manager_spawn_user(const char *path);

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

    /*
     * Set when the board's timer interrupt cannot preempt, so idle_loop() must
     * spin with interrupts left enabled rather than sleep on arch_idle_wait().
     * The LS2K1000 cooperative recovery profile is the case: its firmware hands
     * over in a state where the timer never delivers, so nothing would ever wake
     * a sleeping idle task.
     *
     * Polarity matters -- every other field here is a pointer, so a
     * static initializer leaves this at 0, and 0 has to mean "the timer works"
     * or every board that does not mention it would silently stop sleeping.
     */
    int                  idle_cannot_sleep;

    /*
     * Both of these are firmware facts about the board's boot handoff, not
     * properties of the instruction set, so they live here rather than in an
     * ARCH_HAS_* macro.  The StarFive VisionFive 2 is the case in tree: its
     * firmware does not grant U-mode access to the RISC-V time CSR on every
     * boot hart, and its console UART has no wired interrupt.
     *
     * Like idle_cannot_sleep, 0 must be the normal case so that a board which
     * does not mention them gets the working behaviour.
     */

    /* Set when U-mode cannot reliably read the arch's time counter, so the vDSO
     * must not advertise it.  libc would otherwise retry the faulting rdtime
     * forever instead of falling back to the syscall path. */
    int                  vdso_user_timer_unreliable;

    /* Set when the console UART delivers no interrupt, so receive has to be
     * driven by polling. */
    int                  uart_rx_is_polled;
} board_config_t;

/* Global board config — defined in kernel/board/<board>/board.c */
extern const board_config_t *const current_board;

#endif /* _DRIVER_CORE_H */
