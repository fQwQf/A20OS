/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "firmware/ivrs.h"

/* Deliberately unaligned firmware storage; enough space for policy limits. */
static uint8_t storage[40001], *bytes = storage + 1;
static size_t length;
static void put(size_t off, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; i++)
        bytes[off + i] = value >> (8 * i);
}
static void begin(void) {
    memset(storage, 0, sizeof(storage));
    memcpy(bytes, "IVRS", 4);
    bytes[8] = 2;
    put(36, 1, 4);
    length = 48;
}
static acpi_table_t finish(void) {
    assert(length < sizeof(storage));
    put(4, length, 4);
    bytes[9] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum += bytes[i];
    bytes[9] = (uint8_t)-sum;
    return (acpi_table_t){bytes, length, 0x1000};
}
static size_t hardware(unsigned type, unsigned unit, unsigned segment,
                       size_t extra) {
    size_t off = length, prefix = type == 0x10 ? 24 : 40;
    bytes[off] = type;
    put(off + 2, prefix + extra, 2);
    put(off + 4, unit, 2);
    put(off + 6, 0x40, 2);
    put(off + 8, 0xfed80000ULL + unit * 0x4000, 8);
    put(off + 16, segment, 2);
    put(off + 18, 0x102, 2);
    length += prefix + extra;
    return off + prefix;
}
static void entry(size_t off, unsigned type, unsigned rid, unsigned settings,
                  unsigned extra) {
    bytes[off] = type;
    put(off + 1, rid, 2);
    bytes[off + 3] = settings;
    if (type == 0x42 || type == 0x43)
        put(off + 5, extra, 2);
    if (type == 0x46 || type == 0x47)
        put(off + 4, extra, 4);
}
static void memory(unsigned type, unsigned rid, unsigned end, unsigned segment,
                   uint64_t start) {
    bytes[length] = type;
    bytes[length + 1] = 7;
    put(length + 2, 32, 2);
    put(length + 4, rid, 2);
    put(length + 6, end, 2);
    put(length + 8, segment, 2);
    put(length + 16, start, 8);
    put(length + 24, 4096, 8);
    length += 32;
}
static acpi_ivrs_route_t route(unsigned segment, unsigned rid, int expected) {
    acpi_table_t t = finish();
    acpi_ivrs_route_t r;
    memset(&r, 0xa5, sizeof(r));
    assert(acpi_ivrs_route(&t, segment, rid, &r) == expected);
    if (expected)
        assert(!r.hardware.entries && !r.hardware.base && !r.requester &&
               !r.memory_count);
    return r;
}
static void alternatives(void) {
    /* Preference is independent of firmware order; repeated nonselected types
     * still fail, and a base conflict must not fall back to the older format. */
    const unsigned order[6][3] = {{0x10, 0x11, 0x40}, {0x10, 0x40, 0x11},
                                  {0x11, 0x10, 0x40}, {0x11, 0x40, 0x10},
                                  {0x40, 0x10, 0x11}, {0x40, 0x11, 0x10}};
    for (size_t i = 0; i < 6; i++) {
        begin();
        for (size_t j = 0; j < 3; j++)
            entry(hardware(order[i][j], 1, 3, 4), 1, 0, order[i][j], 0);
        acpi_ivrs_route_t r = route(3, 0xabcd, 0);
        assert(r.hardware.type == 0x40 && r.settings == 0x40 &&
               r.requester == 0xabcd);
        acpi_table_t t = finish();
        size_t count = 99;
        acpi_ivrs_hardware_t h;
        assert(!acpi_ivrs_unit_count(&t, &count) && count == 1);
        assert(!acpi_ivrs_unit_get(&t, 0, &h) && h.type == 0x40 &&
               h.segment == 3);
        assert(acpi_ivrs_unit_get(&t, 1, &h) == ACPI_NOT_FOUND && !h.entries);
        route(0, 0xabcd, ACPI_NOT_FOUND);
        entry(hardware(0x10, 1, 3, 4), 1, 0, 0, 0);
        route(3, 0xabcd, ACPI_IVRS_AMBIGUOUS);
        t = finish();
        count = 99;
        assert(acpi_ivrs_unit_count(&t, &count) == ACPI_IVRS_AMBIGUOUS &&
               !count);
        assert(acpi_ivrs_unit_get(&t, 0, &h) == ACPI_IVRS_AMBIGUOUS &&
               !h.entries);
    }
    begin();
    entry(hardware(0x10, 1, 3, 4), 1, 0, 0, 0);
    size_t d = hardware(0x11, 1, 3, 4);
    entry(d, 1, 0, 0, 0);
    put(d - 40 + 8, 0xc0000000, 8);
    route(3, 2, ACPI_IVRS_AMBIGUOUS);
    begin();
    entry(hardware(0x10, 1, 0, 4), 2, 1, 0, 0);
    d = hardware(0x10, 2, 0, 4);
    entry(d, 2, 2, 0, 0);
    put(d - 24 + 8, 0xfed84000, 8);
    route(0, 1, ACPI_IVRS_AMBIGUOUS);
    put(d - 24 + 8, 0xfed85000, 8);
    route(0, 1,
          ACPI_IVRS_AMBIGUOUS); /* Partially overlapping 16-KiB windows. */
    put(d - 24 + 8, 0xfed88000, 8);
    route(0, 1, 0); /* Adjacent windows and disjoint requester scopes. */
}
static void scopes(void) {
    begin();
    size_t d = hardware(0x11, 1, 3, 20);
    entry(d, 1, 0, 1, 0);
    entry(d + 4, 2, 0x123, 2, 0);
    entry(d + 8, 0x47, 0x200, 4, 0x80000000);
    entry(d + 16, 4, 0x2ff, 0, 0);
    acpi_ivrs_route_t r = route(3, 0x123, 0);
    assert(r.settings == 2 && !r.alias);
    r = route(3, 0x1ff, 0);
    assert(r.settings == 1);
    r = route(3, 0x200, 0);
    assert(r.settings == 4 && r.extended == 0x80000000);
    r = route(3, 0x2ff, 0);
    assert(r.settings == 4);
    r = route(3, 0x300, 0);
    assert(r.settings == 1);
    bytes[d] = 0;
    bytes[d + 3] = 0;
    route(3, 0x300, ACPI_NOT_FOUND);
    entry(hardware(0x10, 2, 3, 4), 2, 0x123, 2, 0);
    route(3, 0x123, ACPI_IVRS_AMBIGUOUS); /* Overlapping distinct units. */
    begin();
    d = hardware(0x10, 1, 0, 8);
    entry(d, 2, 0x123, 1, 0);
    entry(d + 4, 2, 0x123, 2, 0);
    route(0, 0x123, ACPI_IVRS_AMBIGUOUS);
    entry(d + 4, 2, 0x123, 1, 0);
    route(0, 0x123, 0);
}
static void unit_enumeration(void) {
    begin();
    entry(hardware(0x10, 3, 4, 4), 2, 0x100, 0, 0);
    entry(hardware(0x10, 1, 2, 4), 2, 0x100, 0, 0);
    entry(hardware(0x11, 3, 4, 4), 2, 0x100, 0, 0);
    acpi_table_t t = finish();
    size_t count;
    acpi_ivrs_hardware_t h;
    assert(!acpi_ivrs_unit_count(&t, &count) && count == 2);
    assert(!acpi_ivrs_unit_get(&t, 0, &h) && h.device_id == 3 &&
           h.type == 0x11);
    assert(!acpi_ivrs_unit_get(&t, 1, &h) && h.device_id == 1 &&
           h.type == 0x10);
    assert(acpi_ivrs_unit_get(&t, SIZE_MAX, &h) == ACPI_NOT_FOUND &&
           !h.entries);
    assert(acpi_ivrs_unit_count(NULL, &count) == ACPI_INVALID && !count);
    assert(acpi_ivrs_unit_get(NULL, 0, &h) == ACPI_INVALID && !h.entries);
    assert(acpi_ivrs_unit_count(&t, NULL) == ACPI_INVALID);
    assert(acpi_ivrs_unit_get(&t, 0, NULL) == ACPI_INVALID);
    bytes[9]++;
    count = 99;
    assert(acpi_ivrs_unit_count(&t, &count) == ACPI_INVALID && !count);
}
static void aliases_and_memory(void) {
    begin();
    size_t d = hardware(0x10, 1, 3, 16);
    entry(d, 0x43, 0x200, 1, 0x100);
    entry(d + 8, 4, 0x2ff, 0, 0);
    entry(d + 12, 2, 0x100, 2, 0);
    memory(0x21, 0x200, 0, 3, 0x100000);     /* Original requester. */
    memory(0x22, 0x100, 0x1ff, 3, 0x200000); /* Aliased DMA source. */
    memory(0x21, 0x300, 0, 3, 0x300000);     /* Unrelated: not returned. */
    d = hardware(0x11, 1, 3, 16);
    entry(d, 0x43, 0x200, 1, 0x100);
    entry(d + 8, 4, 0x2ff, 0, 0);
    entry(d + 12, 2, 0x100, 2, 0);
    memory(0x20, 0, 0, 0, 0x400000);
    entry(hardware(0x10, 2, 4, 4), 2, 0x300, 0, 0);
    memory(0x20, 0, 0, 0, 0x500000); /* Another unit's ALL must not leak. */
    acpi_ivrs_route_t r = route(3, 0x200, 0);
    assert(r.alias && r.dma_requester == 0x100 && r.memory_count == 3 &&
           r.hardware.type == 0x11);
    acpi_table_t t = finish();
    acpi_ivrs_memory_t m;
    for (size_t i = 0; i < 3; i++) {
        assert(!acpi_ivrs_route_memory(&t, 3, 0x200, i, &m));
        assert(m.start == (i == 2 ? 4 : i + 1) * 0x100000 && m.length == 4096 &&
               m.flags == 7);
    }
    assert(acpi_ivrs_route_memory(&t, 3, 0x200, 3, &m) == ACPI_NOT_FOUND &&
           !m.start);
    assert(route(3, 0x2ff, 0).memory_count == 2);
    entry(d + 12, 2, 0x101, 2, 0);
    route(3, 0x200, ACPI_IVRS_UNSUPPORTED); /* Missing target. */
    begin();
    d = hardware(0x10, 1, 0, 20);
    entry(d, 0x42, 0x200, 1, 0x100);
    entry(d + 8, 0x42, 0x100, 1, 0x80);
    entry(d + 16, 2, 0x80, 1, 0);
    route(0, 0x200, ACPI_IVRS_UNSUPPORTED); /* Chain. */
    put(d + 13, 0x200, 2);
    route(0, 0x200, ACPI_IVRS_UNSUPPORTED); /* Cycle. */
    begin();
    d = hardware(0x10, 1, 0, 8);
    entry(d, 0x42, 0x200, 1, 0x100);
    entry(hardware(0x10, 2, 0, 4), 2, 0x100, 0, 0);
    route(0, 0x200, ACPI_IVRS_AMBIGUOUS); /* Target owned by another unit. */
    begin();
    d = hardware(0x10, 1, 0, 16);
    entry(d, 1, 0, 0, 0);
    entry(d + 4, 0x42, 0x200, 1, 0x100);
    entry(d + 12, 2, 0x100, 0, 0);
    route(0, 0x200,
          ACPI_IVRS_AMBIGUOUS); /* ALL must not obscure alias scope. */
}
static void unsupported(void) {
    begin();
    size_t d = hardware(0x10, 1, 0, 8);
    entry(d, 1, 0, 0, 0);
    entry(d + 4, 5, 0, 0, 0);
    entry(hardware(0x11, 1, 0, 4), 1, 0, 0, 0);
    route(0, 0x200,
          ACPI_IVRS_UNSUPPORTED); /* Unknown even in nonselected format. */
    begin();
    d = hardware(0x10, 1, 0, 12);
    entry(d, 3, 0x100, 0, 0);
    entry(d + 4, 0, 0, 0, 0);
    entry(d + 8, 4, 0x200, 0, 0);
    route(0, 0x100, ACPI_IVRS_UNSUPPORTED); /* Nonadjacent range endpoints. */
    begin();
    d = hardware(0x11, 1, 0, 8);
    entry(d, 0x46, 0x100, 0, 1);
    route(0, 0x100, ACPI_IVRS_UNSUPPORTED); /* Reserved extended bits. */
    begin();
    d = hardware(0x10, 1, 0, 12);
    entry(d, 1, 0, 0, 0);
    entry(d + 4, 0x48, 0, 0, 0);
    put(d + 9, 0x100, 2);
    bytes[d + 11] = 1;
    assert(route(0, 0x200, 0).non_pci_entries);
    route(0, 0x100, ACPI_IVRS_UNSUPPORTED);
    begin();
    d = hardware(0x40, 1, 0, 26);
    entry(d, 1, 0, 0, 0);
    entry(d + 4, 0xf0, 0x100, 0, 0);
    assert(route(0, 0x200, 0).non_pci_entries);
    route(0, 0x100, ACPI_IVRS_UNSUPPORTED);
    begin();
    d = hardware(0x40, 1, 0, 26);
    entry(d, 0xf0, 0x100, 0, 0);
    entry(d + 22, 1, 0, 0, 0);
    route(0, 0x200, ACPI_IVRS_UNSUPPORTED);
    begin();
    memory(0x20, 0, 0, 0, 0x1000);
    entry(hardware(0x10, 1, 0, 4), 1, 0, 0, 0);
    route(0, 2, ACPI_IVRS_UNSUPPORTED);
    begin();
    entry(hardware(0x10, 1, 3, 4), 1, 0, 0, 0);
    memory(0x21, 2, 0, 0, 0x1000);
    route(3, 2, ACPI_IVRS_UNSUPPORTED);
    begin();
    entry(hardware(0x10, 1, 0, 4), 1, 0, 0, 0);
    bytes[length] = 0x7f;
    put(length + 2, 4, 2);
    length += 4;
    route(0, 2, ACPI_IVRS_UNSUPPORTED);
}
static void bounds(void) {
    begin();
    for (unsigned i = 0; i < 64; i++)
        entry(hardware(0x10, i, i, 4), 2, 0x100, 0, 0);
    route(63, 0x100, 0);
    entry(hardware(0x10, 64, 64, 4), 2, 0x100, 0, 0);
    route(0, 0x100, ACPI_IVRS_UNSUPPORTED);
    begin();
    entry(hardware(0x10, 1, 0, 4), 1, 0, 0, 0);
    for (unsigned i = 0; i < 1023; i++)
        memory(0x20, 0, 0, 0, 0x1000);
    assert(route(0, 0x100, 0).memory_count == 1023);
    memory(0x20, 0, 0, 0, 0x1000);
    route(0, 0x100, ACPI_IVRS_UNSUPPORTED);
    acpi_table_t t = finish();
    acpi_ivrs_route_t r;
    assert(acpi_ivrs_route(&t, 0, 0, NULL) == ACPI_INVALID);
    assert(acpi_ivrs_route(NULL, 0, 0, &r) == ACPI_INVALID && !r.hardware.base);
    bytes[9]++;
    assert(acpi_ivrs_route(&t, 0, 0, &r) == ACPI_INVALID && !r.hardware.base);
}
int main(void) {
    alternatives();
    scopes();
    unit_enumeration();
    aliases_and_memory();
    unsupported();
    bounds();
    puts("IVRS routing candidates, alias safety, raw memory constraints and "
         "bounds: PASS");
    return 0;
}
