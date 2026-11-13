/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_MEMORY_H
#define A20_DRIVER_MEMORY_H
#include "core/types.h"

/* CPU physical storage, inclusive endpoints. This is not an MMIO resource,
 * page allocation, cache policy, DMA mapping or a Linux resource tree. */
typedef struct driver_memory_range {
    uint64_t start, end;
} driver_memory_range_t;
typedef struct driver_memory_reservation driver_memory_reservation_t;

/* Task context. Copies the complete immutable input before atomic publication.
 * Overlapping storage reservations may coexist, each with an independent
 * lifetime. Active MMIO claims and published MMIO declarations reject a new
 * reservation (-EBUSY); RES_MEM declarations describe storage and are allowed.
 * The reservation blocks MMIO claims/maps, conflicting device publication and
 * PCI range checks even for the same device owner. It does not revoke raw
 * architecture access or reserve frames in the physical allocator.
 * Failure clears out, leaves no partial reservation and borrows nothing.
 * Caller owns the successful handle until exactly one release; release(NULL)
 * is a no-op. There is no device/binding reference or automatic devres release. */
int driver_memory_reserve(const driver_memory_range_t *, size_t count,
                          driver_memory_reservation_t **out);
void driver_memory_release(driver_memory_reservation_t *);

#ifndef CONFIG_MCU
/* Boot CPU after driver_core_init and PFA initialization, before publishing
 * devices. Protects managed RAM and all ACPI table pages atomically for the
 * kernel lifetime. A failed import must stop device initialization; a repeated
 * successful import returns -EALREADY. PFA geometry/firmware must be immutable.
 * No inference about arbitrary firmware-reserved MMIO holes is made. */
int driver_memory_protect_boot(void);
#endif
#endif
