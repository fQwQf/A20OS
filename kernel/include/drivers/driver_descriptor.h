#ifndef _DRIVERS_DRIVER_DESCRIPTOR_H
#define _DRIVERS_DRIVER_DESCRIPTOR_H

#include "core/types.h"

/*
 * A20OS unified driver package metadata.
 *
 * Every optional driver is a single `.a20drv` file whose ELF `.a20drv`
 * section carries this descriptor.  It is the ONLY metadata source for
 * driver discovery and placement: name, device type, deployment placement
 * (kernel-module vs user-service), driver ABI, resource requirements and
 * the set of device identities the driver may own.
 *
 * The descriptor is written by A20_DRIVER_DESCRIPTOR() in the driver
 * source, read by the loader/manager (kernel) and by drvctl (user) for
 * validation and listing.  There is deliberately no sidecar manifest.
 */

#define A20_DRIVER_DESCRIPTOR_MAGIC   0x41323044U /* "A20D" */
#define A20_DRIVER_DESCRIPTOR_VERSION 2U

#define A20_DRIVER_ABI 1U

enum a20_driver_placement {
    A20_DRIVER_PLACEMENT_KERNEL_MODULE = 1,
    A20_DRIVER_PLACEMENT_USER_SERVICE = 2,
};

enum a20_driver_type {
    A20_DRIVER_TYPE_RTC = 1,
    A20_DRIVER_TYPE_BLOCK,
    A20_DRIVER_TYPE_INPUT,
    A20_DRIVER_TYPE_AUDIO,
    A20_DRIVER_TYPE_SECURITY,
    A20_DRIVER_TYPE_NET,
    A20_DRIVER_TYPE_DISPLAY,
    A20_DRIVER_TYPE_USB,
};

/* Device identity buses (kept in sync with drvmod drv_device_id.bus). */
enum a20_driver_bus {
    A20_DRIVER_BUS_FIXED = 0,
    A20_DRIVER_BUS_PCI = 1,
    A20_DRIVER_BUS_USB = 2,
    A20_DRIVER_BUS_MMIO = 3,
};

/* Resource requirement flags (resource_mask). */
#define A20_DRIVER_RES_MMIO    0x1U
#define A20_DRIVER_RES_IRQ     0x2U
#define A20_DRIVER_RES_IOPORT  0x4U
#define A20_DRIVER_RES_DMA     0x8U

/* Lifecycle ownership flags (flags). */
#define A20_DRIVER_FLAG_SUPERVISED 0x1U /* user-service lifecycle (spawn,
                                         * restart, health) owned by an
                                         * external service supervisor
                                         * (svcmgr); the kernel driver
                                         * manager records it but does not
                                         * auto-activate it */

#define A20_DRIVER_MAX_MATCH 4

typedef struct a20_driver_match {
    uint32_t bus;      /* A20_DRIVER_BUS_* */
    uint32_t vendor;
    uint32_t device;
} a20_driver_match_t;

typedef struct a20_driver_descriptor {
    uint32_t magic;
    uint32_t version;
    uint32_t placement;      /* A20_DRIVER_PLACEMENT_* */
    uint32_t type;           /* A20_DRIVER_TYPE_* */
    char name[32];
    uint32_t abi;            /* A20_DRIVER_ABI */
    uint32_t resource_mask;  /* A20_DRIVER_RES_* */
    uint32_t flags;          /* A20_DRIVER_FLAG_* */
    a20_driver_match_t match[A20_DRIVER_MAX_MATCH];
    uint32_t match_count;
} a20_driver_descriptor_t;

#define A20_DRIVER_MATCH(bus_, vendor_, device_) \
    { (bus_), (vendor_), (device_) }

#define A20_DRIVER_DESCRIPTOR(driver_placement, driver_type, driver_name,     \
                              driver_abi, driver_resmask, driver_flags,      \
                              driver_nmatch, ...)                             \
    static const a20_driver_descriptor_t __a20_driver_descriptor             \
    __attribute__((used, section(".a20drv"))) = {                            \
        .magic = A20_DRIVER_DESCRIPTOR_MAGIC,                                \
        .version = A20_DRIVER_DESCRIPTOR_VERSION,                            \
        .placement = (driver_placement),                                     \
        .type = (driver_type),                                               \
        .name = (driver_name),                                               \
        .abi = (driver_abi),                                                 \
        .resource_mask = (driver_resmask),                                   \
        .flags = (driver_flags),                                             \
        .match_count = (driver_nmatch),                                      \
        .match = { __VA_ARGS__ },                                            \
    }

#endif /* _DRIVERS_DRIVER_DESCRIPTOR_H */
