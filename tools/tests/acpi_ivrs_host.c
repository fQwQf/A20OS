#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "firmware/ivrs.h"

static uint8_t storage[1025], *bytes = storage + 1;
static size_t length;
static void put(size_t off, uint64_t value, unsigned width)
{ for (unsigned i = 0; i < width; i++) bytes[off + i] = value >> (8 * i); }
static acpi_table_t finish(void)
{
    put(4, length, 4); bytes[9] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) sum += bytes[i];
    bytes[9] = (uint8_t)-sum;
    return (acpi_table_t){bytes, length, 0x1000};
}
static void begin(unsigned revision)
{
    memset(storage, 0, sizeof(storage)); memcpy(bytes, "IVRS", 4);
    bytes[8] = revision; put(36, 1, 4); length = 48;
}
static size_t hardware(unsigned type, size_t extra)
{
    size_t off = length, prefix = type == 0x10 ? 24 : 40;
    bytes[off] = type; put(off + 2, prefix + extra, 2);
    put(off + 4, 0x102, 2); put(off + 6, 0x40, 2);
    put(off + 8, 0xfed80000, 8); put(off + 16, 3, 2);
    put(off + 18, 0x102, 2); put(off + 20, 0x11223344, 4);
    if (prefix == 40) { put(off + 24, 0x55667788, 8); put(off + 32, 0x99, 8); }
    length += prefix + extra; return off + prefix;
}
static void invalid(acpi_table_t *t, int expected)
{
    acpi_ivrs_iterator_t it = {t, 99};
    assert(acpi_ivrs_begin(t, &it) == expected && !it.table && !it.offset);
}
int main(void)
{
    begin(2);
    size_t devices = hardware(0x10, 16);
    bytes[devices] = 0x43; put(devices + 1, 0x200, 2); put(devices + 5, 0x100, 2);
    bytes[devices + 8] = 4; put(devices + 9, 0x2ff, 2);
    bytes[devices + 12] = 0x12; /* Unknown bounded fixed type is preserved. */
    hardware(0x11, 4); /* Padding entry. Same unit alternative format retained. */
    size_t hid = hardware(0x40, 25);
    bytes[hid] = 0xf0; bytes[hid + 20] = 2; bytes[hid + 21] = 3;
    memcpy(bytes + hid + 22, "GPU", 3);
    size_t mem = length; bytes[mem] = 0x22; bytes[mem + 1] = 7;
    put(mem + 2, 32, 2); put(mem + 4, 0x100, 2); put(mem + 6, 0x1ff, 2);
    put(mem + 8, 3, 2); put(mem + 16, 0x100000, 8); put(mem + 24, 0x2000, 8);
    length += 32; bytes[length] = 0x7f; put(length + 2, 4, 2); length += 4;
    acpi_table_t t = finish(); acpi_ivrs_iterator_t it;
    assert(!acpi_ivrs_begin(&t, &it));
    acpi_ivrs_block_t block; acpi_ivrs_hardware_t h;
    assert(acpi_ivrs_next(&it, &block) == 1 && !acpi_ivrs_hardware(&block, &h));
    assert(h.type == 0x10 && h.base == 0xfed80000 && h.segment == 3 && h.capability == 0x40);
    size_t offset = 0; acpi_ivrs_device_t entry;
    assert(acpi_ivrs_device_next(&h, &offset, &entry) == 1 && entry.type == 0x43);
    assert(acpi_le16(entry.data + 5) == 0x100);
    assert(acpi_ivrs_device_next(&h, &offset, &entry) == 1 && entry.type == 4);
    assert(acpi_ivrs_device_next(&h, &offset, &entry) == 1 && entry.type == 0x12);
    assert(!acpi_ivrs_device_next(&h, &offset, &entry));
    assert(acpi_ivrs_next(&it, &block) == 1 && !acpi_ivrs_hardware(&block, &h) && h.efr2 == 0x99);
    assert(acpi_ivrs_next(&it, &block) == 1 && !acpi_ivrs_hardware(&block, &h));
    offset = 0; assert(acpi_ivrs_device_next(&h, &offset, &entry) == 1 && entry.length == 25);
    assert(acpi_ivrs_next(&it, &block) == 1);
    acpi_ivrs_memory_t m; assert(!acpi_ivrs_memory(&block, &m));
    assert(m.start == 0x100000 && m.length == 0x2000 && m.segment == 3 && m.auxiliary == 0x1ff);
    assert(acpi_ivrs_next(&it, &block) == 1 && block.type == 0x7f);
    assert(!acpi_ivrs_next(&it, &block));
    bytes[length - 2] = 0; bytes[length - 1] = 0; t = finish(); invalid(&t, ACPI_INVALID);
    put(length - 2, 4, 2); bytes[hid + 21] = 4; t = finish(); invalid(&t, ACPI_INVALID);
    bytes[hid + 21] = 3; bytes[hid] = 0x80; t = finish(); invalid(&t, ACPI_IVRS_UNSUPPORTED);
    bytes[hid] = 0xf0; put(mem + 24, ~(uint64_t)0, 8); t = finish(); invalid(&t, ACPI_INVALID);
    put(mem + 24, 0x2000, 8); put(devices + 9, 0x100, 2); t = finish(); invalid(&t, ACPI_INVALID);
    put(devices + 9, 0x2ff, 2); bytes[8] = 1; t = finish(); invalid(&t, ACPI_INVALID);
    bytes[8] = 2; put(36, 0, 4); t = finish(); invalid(&t, ACPI_INVALID);
    put(36, 1, 4); t = finish(); bytes[9]++; invalid(&t, ACPI_INVALID);
    t = finish(); t.length--; invalid(&t, ACPI_INVALID);
    begin(1); hardware(0x10, 4); t = finish(); assert(!acpi_ivrs_begin(&t, &it));
    for (size_t short_size = 0; short_size < 48; short_size++) {
        t.length = short_size; invalid(&t, ACPI_INVALID);
    }
    puts("IVRS whole-table validation and unaligned typed views: PASS"); return 0;
}
