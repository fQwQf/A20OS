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
#include "firmware/node.h"
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
 * For platform: firmware IDs are matched by the bus; static presets use IDs
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
    uint64_t           start;      /* inclusive CPU physical address / port / IRQ */
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
#define DEV_STATE_DEFERRED  6

typedef struct firmware_device_id {
    const char *id;
    const char *source; /* NULL accepts any firmware provider */
    uint64_t driver_data;
} firmware_device_id_t;
struct device_resource_action;
struct device_dependency;

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
    fw_node_t          firmware_node;
    /* Board-verified identity supplement for incomplete firmware; immutable
     * string storage outlives this device. It never replaces firmware data. */
    const char        *software_id;
    const firmware_device_id_t *firmware_match;
    struct device_resource_action *resource_actions;
    struct device_dependency *dependencies;
    unsigned references;
    struct device *parent_reference; /* Core lifetime pin, released at final put. */
    void (*release)(struct device *);
    int                probe_error; /* last probe result; -EPROBE_DEFER is retriable */
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

    /* Firmware identity table, terminated by an entry with id == NULL. */
    const firmware_device_id_t *firmware_ids;

    /* Optional protocol-level narrowing after the bus ID match.  It must not
     * access device registers or allocate resources. */
    int  (*match)(device_t *dev);

    /* lifecycle */
    int  (*probe)(device_t *dev);
    /* After quiesce/class drain. 0 commits teardown; failure retains binding,
     * drv_priv, devres and dependencies offline in REMOVING. Must retain all
     * hardware-accessible storage on error and support explicit retry. */
    int  (*remove)(device_t *dev);
    int  (*suspend)(device_t *dev);   /* optional */
    int  (*resume)(device_t *dev);    /* optional */
    void (*shutdown)(device_t *dev);  /* terminal quiesce, no registry teardown */
    void (*progress)(device_t *dev);  /* optional non-IRQ completion drain */

    /* subsystem interface */
    const void         *class_ops;    /* block_dev_ops_t*, net_dev_ops_t*, etc. */
    uint32_t            class_type;   /* DEV_CLASS_BLOCK, DEV_CLASS_NET, ... */

    /* module linkage (NULL = built-in) */
    void               *module;

    /* read-only probe: may bind user-owned devices but must not claim or
     * destructively initialize them (dual-placement kernel shell). */
    int                 read_only_probe;
    /* Polling-only adapters may omit IRQ resources while their controller
     * domain is unavailable. Malformed IRQ descriptors remain fatal. */
    int                 irq_optional;
    /* Optional task-context stop phase, after class calls have been disabled
     * but BEFORE the core waits for active calls. Cancel/wake requests and
     * stop DMA/IRQ/work producers here; retain storage until remove(). This
     * does not permit freeing memory after an unconfirmed hardware stop:
     * record that failure privately and report it from remove(). It
     * must also tolerate a successful probe whose class publication failed.
     * The lifecycle mutex is held: do not wait for work that needs it. A
     * failed probe still rolls back its own hardware before returning.
     * Runs once per teardown; failed remove retries do not repeat quiesce. */
    void (*quiesce)(device_t *dev);
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
 * handlers. Bus probes may publish child devices; arbitrary recursive driver
 * registration and self-unregistration from lifecycle callbacks are invalid.
 */
void driver_core_init(void);
#ifdef CONFIG_DRIVER_REGISTRY_RUST
typedef void *(*driver_registry_grow_fn)(void *old, size_t bytes);
int a20_driver_registry_insert(void **slots, int *count, int *capacity,
                               void *item, driver_registry_grow_fn grow);
int a20_driver_registry_remove(void **slots, int *count, void *item);
#endif
/* Format registered drivers for /proc/drivers. */
int  driver_core_list_drivers(char *buf, size_t sz);
int  driver_core_list_devices(char *buf, size_t sz);
int  driver_register(driver_t *drv);
/* Failure retains driver registration/code ownership and remaining bindings.
 * Suppresses new matching bindings during withdrawal; recursive teardown is
 * -EBUSY. Already stopped devices are not automatically rebound on failure. */
int  driver_unregister(driver_t *drv);
int  device_register(device_t *dev);
/* Synchronous, task context. Commit unregister only after successful remove.
 * Failure keeps the registry pin/binding/resources for retry; some consumers
 * may already have stopped. No automatic rollback/re-probe or forced free.
 * NULL -> -EINVAL; not registered -> -ENOENT; positive remove -> -EIO. */
int device_unregister(device_t *dev);
/* Publish a bus add/remove event through the normal device lifecycle.  Bus
 * callbacks observe ADD after probe and REMOVE after remove has completed. */
int  device_hotplug(device_t *dev, int event);
int  bus_register(bus_type_t *bus);
void bus_unregister(bus_type_t *bus);

/* probe a specific device against all registered drivers */
int  bus_probe_device(device_t *dev);

resource_t *device_get_resource(device_t *dev, enum resource_type type, int index);
typedef struct driver_resource_claim driver_resource_claim_t;
/* Process-context cooperative ownership of inclusive MMIO or I/O-port ranges.
 * Copies type/start/end; owner must carry a live reference and remains pinned
 * until release. Different owners cannot overlap within the same namespace;
 * one owner may hold overlapping claims, each with its own lifetime. I/O ports
 * are limited to 0..ffffh. Request clears out on error; release(NULL) is a no-op.
 * Manual claims survive unbind and must be released explicitly, exactly once.
 * The owner pin protects storage, not a live binding: callers must serialize
 * admission with their device's stopping/removal state.
 * All MMIO claims also reject storage protected by drivers/core/memory.h.
 * This is not a physical-page reservation, allocator or CPU access revocation;
 * raw architecture mapping/port access does not participate automatically. */
int driver_resource_request(device_t *owner, const resource_t *,
                              driver_resource_claim_t **out);
/* Stronger, process-context claim: owner must be registered. Under the
 * lifecycle mutex, checks every other published device's MMIO/RES_MEM or
 * I/O-port declarations, then creates a claim. No ancestor/window exceptions
 * are inferred. Registration of a conflicting declaration is rejected while
 * this claim survives, including after its owner has been unregistered.
 * Resource mutation must use the bus discovery transaction and range check
 * below. Boot storage reservations are imported separately (memory.h);
 * this interface does not revoke raw hardware access. */
int driver_resource_request_checked(device_t *owner, const resource_t *,
                                      driver_resource_claim_t **out);
/* Observation only, not a claim. Checks published declarations and active
 * claims, ignoring owner (NULL ignores none). busy_end receives the greatest conflicting inclusive
 * end on -EBUSY, zero otherwise. Optional output; valid address zero is allowed.
 * Bus mutators must hold driver_discovery_begin/end across this check AND the
 * hardware/metadata update. Checked claims use that same lifecycle mutex. */
int driver_resource_check_range(device_t *owner, const resource_t *, uint64_t *busy_end);
/* Active claims plus protected storage, for retaining a firmware BAR without
 * treating enclosing/duplicate firmware declarations as active users. Bus
 * mutators use the same discovery transaction as check_range(). */
int driver_resource_check_claims(device_t *owner, const resource_t *);
void driver_resource_release(driver_resource_claim_t *);
/* Managed claims/maps are for serialized probe/lifecycle context, like other
 * devres operations. Resource must be an element of dev->res. Explicit claim
 * is also useful for port I/O; it is released on probe failure or after remove.
 * Checked map clears out on error and retains its own claim until unmap ends.
 * Existing pointer-returning MMIO helpers use this same exclusive path. */
int device_claim_resource(device_t *dev, const resource_t *);
int device_map_resource_exclusive(device_t *dev, const resource_t *, void **out);
/* MMIO resources always carry physical addresses. Mapping is architecture-owned. */
void *device_map_mmio(device_t *dev, int index);
void *device_map_resource(device_t *dev, const resource_t *resource);
int device_add_resource_action(device_t *dev, void (*release)(void *), void *data);
void device_release_resources(device_t *dev);
/* Registration, child-parent topology and progress snapshots retain device
 * objects. Dynamically allocated buses provide release(); the creator drops
 * its initial reference after unregistering (or transfers ownership to the
 * registry after registration). Static devices may leave release unset. A
 * registered child's parent remains pinned until the child's final put. */
device_t *device_get(device_t *dev);
/* Registry membership observation, independent of binding/state. Caller holds
 * a device reference; a pin alone does not imply registration. Use a discovery
 * transaction across this check and lifecycle-dependent use. No extra pin. */
bool device_is_registered(const device_t *dev);
void device_put(device_t *dev);
int device_add_dependency(device_t *consumer, device_t *provider);
/* Returned devices carry a reference; release it with device_put(). */
device_t *device_find_get_by_firmware(const fw_node_t *node);
device_t *device_find_get_by_firmware_id(const char *id);
device_t *device_find_get_by_mmio(uint64_t physical);
void *arch_device_map_mmio(uint64_t physical, size_t size, uint32_t flags);
void arch_device_unmap_mmio(void *address, size_t size);
/* Serialize discovery and suppress probes until its paired end. */
void driver_discovery_begin(void);
void driver_discovery_end(void);
int driver_retry_deferred(void);
int driver_device_count(void);
int driver_core_kexec_prepare(void);
void driver_core_shutdown_all(void);

/*
 * Ask for a specific devfs node name for this device, e.g. "vport0" instead of
 * the generated "char3".  Callable only from probe(), before it returns: the
 * class device is published immediately afterwards and copies the name then.
 * Rejects an empty name, a name longer than CLASS_DEVICE_NAME_MAX - 1, and any
 * name carrying '/', so a driver cannot steer the name outside the device's
 * own devfs directory.  Returns 0 on success, -EINVAL otherwise.
 */
int device_set_devfs_name(device_t *dev, const char *name);

/* IRQ-safe lookup by rank among currently published devices of this class.
 * Returned device carries a reference for device_put. Persistent disk handles
 * use class_device_get_by_index and its stable class index instead. */
device_t *device_find_get_by_class(uint32_t class_type, int index);

/* probe all unbound devices against registered drivers */
void driver_probe_all(void);
void driver_progress_class(uint32_t class_type);

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
 * Provided by kernel/arch/<arch>/boards/<board>/board.c
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
    /* Self-enumerating buses supplement firmware platform devices. */
    void                 (*enumerate_buses)(void);
    /* Devices reserved for runtime module/user-service discovery. */
    void                 (*configure_devices)(void);

    /* Board-specific handoff and user-service resource policy. */
    void                 (*prepare_scheduler)(void);
    int                    defer_network_init;
    int                  (*user_mmio_allowed)(uint64_t phys, uint64_t size);
    int                  (*user_mmio_owned)(uint64_t phys);
    int                  (*user_mmio_present)(uint64_t phys);
    int                  (*user_mmio_index)(uint64_t phys);
    int                  (*format_interrupts)(char *buf, size_t size);

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

/* Global board config — defined in kernel/arch/<arch>/boards/<board>/board.c */
extern const board_config_t *const current_board;

#endif /* _DRIVER_CORE_H */
