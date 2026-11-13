/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/memory.h"
#include "core/errno.h"
#include "core/klog.h"
#include "mm/frame.h"
#include "mm/slab.h"
#ifdef CONFIG_ACPI
#include "firmware/acpi.h"
#endif

/* This reservation deliberately outlives every device and driver binding. */
static driver_memory_reservation_t *boot_memory;

struct memory_inventory {
    driver_memory_range_t *ranges;
    size_t count, capacity;
};

#ifdef CONFIG_ACPI
static int table_storage(void *opaque, uint64_t physical, size_t bytes) {
    struct memory_inventory *inventory = opaque;
    if (!bytes || (uint64_t)(bytes - 1) > ~(uint64_t)0 - physical)
        return -EINVAL;
    if (inventory->count == ~(size_t)0)
        return -EOVERFLOW;
    if (inventory->ranges) {
        if (inventory->count == inventory->capacity)
            return -EOVERFLOW;
        /* MMIO mappings use whole pages. Protect the table's boundary pages
         * too; a disjoint byte span in the same page is not device storage. */
        inventory->ranges[inventory->count] = (driver_memory_range_t){
            physical & ~((uint64_t)PAGE_SIZE - 1),
            (physical + bytes - 1) | ((uint64_t)PAGE_SIZE - 1)};
    }
    inventory->count++;
    return 0;
}
#endif

int driver_memory_protect_boot(void) {
    if (boot_memory)
        return -EALREADY;
    size_t ram = pfa.nr_ranges;
    if (!ram || ram > PFA_MAX_RANGES)
        return -EINVAL;
    struct memory_inventory inventory = {.count = ram};
    int ret = 0;
#ifdef CONFIG_ACPI
    const acpi_context_t *acpi = arch_acpi_context();
    if (acpi) {
        ret = acpi_reserve_tables(acpi, table_storage, &inventory);
        if (ret)
            return -EIO; /* No partial reservations have been published. */
    }
#endif
    if (inventory.count > ~(size_t)0 / sizeof(*inventory.ranges))
        return -EOVERFLOW;
    inventory.capacity = inventory.count;
    inventory.ranges = kmalloc(inventory.capacity * sizeof(*inventory.ranges));
    if (!inventory.ranges)
        return -ENOMEM;
    for (size_t i = 0; i < ram; i++) {
        uint64_t start = pfa.ranges[i].base, end = pfa.ranges[i].end;
        if (end <= start) {
            ret = -EINVAL;
            goto done;
        }
        inventory.ranges[i] = (driver_memory_range_t){start, end - 1};
    }
    inventory.count = ram;
#ifdef CONFIG_ACPI
    if (acpi) {
        ret = acpi_reserve_tables(acpi, table_storage, &inventory);
        if (ret) {
            ret = -EIO;
            goto done;
        }
    }
#endif
    if (inventory.count != inventory.capacity) {
        ret = -EIO;
        goto done;
    }
    ret =
        driver_memory_reserve(inventory.ranges, inventory.count, &boot_memory);
    if (!ret)
        kinfo("DRIVER_MEMORY: protected ram_ranges=%lu firmware_ranges=%lu\n",
              (unsigned long)ram, (unsigned long)(inventory.count - ram));
done:
    kfree(inventory.ranges);
    return ret;
}
#endif
