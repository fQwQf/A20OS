/* SPDX-License-Identifier: Apache-2.0 */
/* Included after driver_resource_host.c's allocator/task/hardware fixture. */
#include "drivers/core/memory.h"
#include "mm/frame.h"
#include "firmware/acpi.h"

static void memory_intervals(void) {
    driver_memory_range_t ranges[] = {{0x1800, 0x1fff},
                                      {0, 0},
                                      {(~(uint64_t)0) - 4095, (~(uint64_t)0)},
                                      {0x1000, 0x17ff},
                                      {0x1200, 0x1900}};
    driver_memory_reservation_t *first, *second;
    assert(!driver_memory_reserve(ranges, 5, &first));
    memset(ranges, 0, sizeof(ranges)); /* No borrowed input. */
    driver_resource_claim_t *claim = (void *)1;
    assert(driver_resource_request(&a, &region, &claim) == -EBUSY && !claim);
    uint64_t end;
    assert(driver_resource_check_range(&a, &region, &end) == -EBUSY &&
           end == 0x1fff);
    resource_t zero = {.type = RES_MMIO, .start = 0, .end = 0};
    assert(driver_resource_check_range(NULL, &zero, &end) == -EBUSY && !end);
    resource_t last = {
        .type = RES_MMIO, .start = (~(uint64_t)0), .end = (~(uint64_t)0)};
    assert(driver_resource_check_range(NULL, &last, &end) == -EBUSY &&
           end == (~(uint64_t)0));
    resource_t ports = region;
    ports.type = RES_IOPORT;
    assert(!driver_resource_request(&a, &ports, &claim));
    driver_resource_release(claim);
    assert(device_register(&a) == -EBUSY && a.references == 1);
    resource_t storage = region;
    storage.type = RES_MEM;
    a.res = &storage;
    assert(!device_register(&a));
    assert(driver_resource_request_checked(&a, &region, &claim) == -EBUSY &&
           !claim);
    device_unregister(&a);
    a.res = &region;
    driver_memory_range_t duplicate = {0x1000, 0x1fff};
    assert(!driver_memory_reserve(&duplicate, 1, &second));
    driver_memory_release(first);
    assert(driver_resource_check_claims(&a, &region) == -EBUSY);
    assert(!driver_resource_check_claims(&a, &zero));
    driver_memory_release(second);
    driver_memory_release(NULL);
    assert(!driver_resource_request(&a, &region, &claim));
    driver_resource_release(claim);
}

static void memory_failures(void) {
    driver_memory_range_t ranges[] = {{0x5000, 0x5fff}, {0x1000, 0x1fff}};
    driver_memory_reservation_t *reservation = (void *)1;
    assert(driver_memory_reserve(NULL, 1, &reservation) == -EINVAL &&
           !reservation);
    assert(driver_memory_reserve(ranges, 0, &reservation) == -EINVAL &&
           !reservation);
    assert(driver_memory_reserve(ranges, 1, NULL) == -EINVAL);
    assert(driver_memory_reserve(ranges, SIZE_MAX, &reservation) ==
               -EOVERFLOW &&
           !reservation);
    ranges[1].start = 0x2000;
    assert(driver_memory_reserve(ranges, 2, &reservation) == -EINVAL &&
           !reservation);
    ranges[1].start = 0x1000;
    fail_after = 0;
    assert(driver_memory_reserve(ranges, 2, &reservation) == -ENOMEM &&
           !reservation);
    fail_after = -1;
    driver_resource_claim_t *claim;
    assert(!driver_resource_request(&a, &region, &claim));
    assert(driver_memory_reserve(ranges, 2, &reservation) == -EBUSY &&
           !reservation);
    driver_resource_release(claim);
    assert(!device_register(&a)); /* An unbound MMIO declaration also wins. */
    assert(driver_memory_reserve(ranges, 2, &reservation) == -EBUSY &&
           !reservation);
    device_unregister(&a);
    resource_t untouched = {.type = RES_MMIO, .start = 0x5000, .end = 0x5fff};
    assert(!driver_resource_check_claims(&a, &untouched));
    assert(!driver_memory_reserve(ranges, 2, &reservation));
    driver_memory_release(reservation);
}

static void memory_mapping(void) {
    driver_memory_range_t storage = {region.start, region.end};
    driver_memory_reservation_t *reservation;
    assert(!driver_memory_reserve(&storage, 1, &reservation));
    void *address = (void *)1;
    assert(device_map_resource_exclusive(&a, &region, &address) == -EBUSY &&
           !address);
    assert(device_claim_resource(&a, &region) == -EBUSY);
    assert(!maps && !unmaps && a.references == 1 && !a.resource_actions);
    driver_memory_release(reservation);
    assert(!device_map_resource_exclusive(&a, &region, &address) &&
           address == registers);
    assert(driver_memory_reserve(&storage, 1, &reservation) == -EBUSY &&
           !reservation);
    device_release_resources(&a);
    assert(maps == 1 && unmaps == 1 && a.references == 1);
    assert(!driver_memory_reserve(&storage, 1, &reservation));
    driver_memory_release(reservation);
}

struct memory_race {
    unsigned mode;
    int result;
    driver_resource_claim_t *claim;
    driver_memory_reservation_t *reservation;
};
static void *memory_compete(void *opaque) {
    struct memory_race *race = opaque;
    driver_memory_range_t storage = {region.start, region.end};
    pthread_barrier_wait(&start);
    if (race->mode == 0)
        race->result = driver_memory_reserve(&storage, 1, &race->reservation);
    else if (race->mode == 1)
        race->result = driver_resource_request(&a, &region, &race->claim);
    else
        race->result = device_register(&a);
    pthread_barrier_wait(&attempted);
    pthread_barrier_wait(&proceed);
    if (!race->result && race->mode == 2)
        device_unregister(&a);
    driver_resource_release(race->claim);
    driver_memory_release(race->reservation);
    return NULL;
}
static void memory_concurrency(unsigned mode) {
    pthread_barrier_init(&start, NULL, 3);
    pthread_barrier_init(&attempted, NULL, 3);
    pthread_barrier_init(&proceed, NULL, 3);
    for (unsigned i = 0; i < 32; i++) {
        struct memory_race races[2] = {{.mode = 0}, {.mode = mode}};
        pthread_t threads[2];
        for (unsigned j = 0; j < 2; j++)
            assert(
                !pthread_create(&threads[j], NULL, memory_compete, &races[j]));
        pthread_barrier_wait(&start);
        pthread_barrier_wait(&attempted);
        assert((!races[0].result && races[1].result == -EBUSY) ||
               (!races[1].result && races[0].result == -EBUSY));
        pthread_barrier_wait(&proceed);
        for (unsigned j = 0; j < 2; j++)
            assert(!pthread_join(threads[j], NULL));
        assert(a.references == 1);
    }
    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&attempted);
    pthread_barrier_destroy(&proceed);
}

static void memory_normalization(void) {
    unsigned char used[256] = {0};
    driver_memory_range_t ranges[32];
    for (unsigned i = 0; i < 32; i++) {
        unsigned begin = (i * 71) % 251, end = begin + (i % 5);
        ranges[i] = (driver_memory_range_t){begin, end};
        for (unsigned j = begin; j <= end; j++)
            used[j] = 1;
    }
    driver_memory_reservation_t *reservation;
    assert(!driver_memory_reserve(ranges, 32, &reservation));
    for (unsigned start = 0; start < 256; start++)
        for (unsigned last = start; last < 256; last += 13) {
            resource_t query = {.type = RES_MMIO, .start = start, .end = last};
            uint64_t end = 1234;
            int busy = 0;
            unsigned greatest = 0;
            for (unsigned i = start; i <= last; i++)
                if (used[i]) {
                    busy = 1;
                    greatest = i;
                }
            while (busy && greatest < 255 && used[greatest + 1])
                greatest++;
            assert(driver_resource_check_range(NULL, &query, &end) ==
                   (busy ? -EBUSY : 0));
            assert(end == (busy ? greatest : 0));
        }
    driver_memory_release(reservation);
}

pfa_t pfa;
static unsigned char rsdp[36], xsdt[52], table_a[64], table_b[64];
static int table_maps, table_unmaps, table_fail, root_full_maps,
    change_second_pass;
static acpi_context_t boot_acpi;
static void store_bytes(unsigned char *p, unsigned offset, uint64_t value,
                        unsigned n) {
    for (unsigned i = 0; i < n; i++)
        p[offset + i] = value >> (8 * i);
}
static void table_checksum(unsigned char *p, size_t n, size_t checksum) {
    p[checksum] = 0;
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += p[i];
    p[checksum] = (unsigned char)-sum;
}
static const void *boot_map(void *opaque, uint64_t pa, size_t bytes) {
    (void)opaque;
    const void *p = NULL;
    if (table_fail && pa == 0x4000)
        return NULL;
    if (pa == 0x100 && bytes <= sizeof(rsdp))
        p = rsdp;
    if (pa == 0x2000 && bytes <= sizeof(xsdt)) {
        if (bytes == sizeof(xsdt) && ++root_full_maps == 3 &&
            change_second_pass) {
            store_bytes(xsdt, 4, 44, 4);
            table_checksum(xsdt, 44, 9);
        }
        p = xsdt;
    }
    if (pa == 0x3000 && bytes <= sizeof(table_a))
        p = table_a;
    if (pa == 0x4000 && bytes <= sizeof(table_b))
        p = table_b;
    if (p)
        table_maps++;
    return p;
}
static void boot_unmap(void *opaque, const void *p, size_t bytes) {
    (void)opaque;
    (void)p;
    (void)bytes;
    table_unmaps++;
}
const acpi_context_t *arch_acpi_context(void) {
    return boot_acpi.rsdp ? &boot_acpi : NULL;
}
static void memory_boot_case(const char *name) {
    pfa.nr_ranges = 2;
    pfa.ranges[0].base = 0x100000;
    pfa.ranges[0].end = 0x200000;
    pfa.ranges[1].base = 0x400000;
    pfa.ranges[1].end = 0x500000;
    if (strcmp(name, "boot-ram")) {
        memcpy(rsdp, "RSD PTR ", 8);
        rsdp[15] = 2;
        store_bytes(rsdp, 20, 36, 4);
        store_bytes(rsdp, 24, 0x2000, 8);
        table_checksum(rsdp, 20, 8);
        table_checksum(rsdp, 36, 32);
        memcpy(xsdt, "XSDT", 4);
        store_bytes(xsdt, 4, 52, 4);
        xsdt[8] = 1;
        store_bytes(xsdt, 36, 0x3000, 8);
        store_bytes(xsdt, 44, 0x4000, 8);
        table_checksum(xsdt, 52, 9);
        memcpy(table_a, "TEST", 4);
        store_bytes(table_a, 4, 64, 4);
        table_a[8] = 1;
        table_checksum(table_a, 64, 9);
        memcpy(table_b, table_a, sizeof(table_b));
        boot_acpi =
            (acpi_context_t){.mapper = {.map = boot_map, .unmap = boot_unmap},
                             .rsdp = 0x100,
                             .root = 0x2000,
                             .entry_width = 8};
    }
    if (strstr(name, "table-fail"))
        table_fail = 1;
    if (strstr(name, "changed"))
        change_second_pass = 1;
    if (strstr(name, "bad-ram"))
        pfa.ranges[1].end = pfa.ranges[1].base;
    if (strstr(name, "alloc-"))
        fail_after = name[strlen(name) - 1] - '0';
    int before = allocations, result = driver_memory_protect_boot();
    fail_after = -1;
    assert(table_maps == table_unmaps);
    if (result) {
        assert(allocations == before);
        assert(result == (strstr(name, "alloc-")    ? -ENOMEM
                          : strstr(name, "bad-ram") ? -EINVAL
                                                    : -EIO));
        resource_t ram = {.type = RES_MMIO, .start = 0x100000, .end = 0x100fff};
        assert(!driver_resource_check_claims(NULL, &ram));
        /* Failed imports remain retryable. */
        table_fail = change_second_pass = 0;
        root_full_maps = 0;
        store_bytes(xsdt, 4, 52, 4);
        table_checksum(xsdt, 52, 9);
        pfa.ranges[1].end = 0x500000;
        assert(!driver_memory_protect_boot());
    } else
        assert(allocations == before + 1); /* Kernel-lifetime reservation. */
    assert(driver_memory_protect_boot() == -EALREADY);
    resource_t query = {.type = RES_MMIO, .start = 0x100000, .end = 0x1fffff};
    driver_resource_claim_t *claim;
    assert(driver_resource_request(&a, &query, &claim) == -EBUSY && !claim);
    query = (resource_t){.type = RES_MMIO, .start = 0x200000, .end = 0x3fffff};
    assert(!driver_resource_request(&a, &query, &claim));
    driver_resource_release(claim);
    if (boot_acpi.rsdp) {
        query = (resource_t){.type = RES_MMIO, .start = 0x4000, .end = 0x403f};
        assert(driver_resource_request(&a, &query, &claim) == -EBUSY && !claim);
        query.start = 0x4040;
        query.end = 0x4fff;
        assert(driver_resource_request(&a, &query, &claim) == -EBUSY && !claim);
        query.start = 0x5000;
        query.end = 0x5fff;
        assert(!driver_resource_request(&a, &query, &claim));
        driver_resource_release(claim);
    }
    assert(table_maps == table_unmaps);
}

static void memory_case(const char *name) {
    if (!strcmp(name, "intervals"))
        memory_intervals();
    else if (!strcmp(name, "failures"))
        memory_failures();
    else if (!strcmp(name, "mapping"))
        memory_mapping();
    else if (!strcmp(name, "claim-race"))
        memory_concurrency(1);
    else if (!strcmp(name, "publish-race"))
        memory_concurrency(2);
    else if (!strcmp(name, "normalize"))
        memory_normalization();
    else {
        assert(!strncmp(name, "boot-", 5));
        memory_boot_case(name);
    }
}
