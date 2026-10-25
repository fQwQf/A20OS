#include "firmware/acpi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char ram[65536];
static unsigned maps, unmaps;
static const void *test_map(void *opaque, uint64_t pa, size_t size) {
    (void)opaque;
    if (pa >= sizeof(ram) || size > sizeof(ram) - pa)
        return NULL;
    maps++;
    return ram + pa;
}
static void test_unmap(void *o, const void *p, size_t s) {
    (void)o;
    (void)p;
    (void)s;
    unmaps++;
}
static acpi_mapper_t mapper = {.map = test_map, .unmap = test_unmap};
static void put(unsigned char *p, uint64_t n, unsigned width) {
    for (unsigned i = 0; i < width; i++)
        p[i] = n >> (8 * i);
}
static void sum(unsigned char *p, size_t len, size_t offset) {
    p[offset] = 0;
    unsigned n = 0;
    for (size_t i = 0; i < len; i++)
        n += p[i];
    p[offset] = (unsigned char)-n;
}
static void table(unsigned pa, const char *sig, unsigned len) {
    memset(ram + pa, 0, len);
    memcpy(ram + pa, sig, 4);
    put(ram + pa + 4, len, 4);
    ram[pa + 8] = 6;
    sum(ram + pa, len, 9);
}
static void fix(unsigned pa) {
    sum(ram + pa, acpi_le32(ram + pa + 4), 9);
}
static void rsdp(unsigned revision) {
    memset(ram, 0, sizeof(ram));
    maps = unmaps = 0;
    memcpy(ram + 0x100, "RSD PTR ", 8);
    ram[0x10f] = revision;
    if (revision >= 2) {
        put(ram + 0x114, 36, 4);
        put(ram + 0x118, 0x1000, 8);
    } else
        put(ram + 0x110, 0x1000, 4);
    sum(ram + 0x100, 20, 8);
    if (revision >= 2)
        sum(ram + 0x100, 36, 32);
    table(0x1000, revision >= 2 ? "XSDT" : "RSDT", revision >= 2 ? 44 : 40);
    put(ram + 0x1000 + 36, 0x2000, revision >= 2 ? 8 : 4);
    fix(0x1000);
    table(0x2000, "TEST", 40);
}
static acpi_context_t open_context(void) {
    acpi_context_t c;
    assert(acpi_context_open(&c, &mapper, 0x100) == 0);
    return c;
}
static void roots(void) {
    for (unsigned revision = 0; revision <= 2; revision += 2) {
        rsdp(revision);
        acpi_context_t c = open_context();
        acpi_table_t t;
        assert(acpi_table_find(&c, "TEST", 0, &t) == 0 && t.physical == 0x2000);
        acpi_table_release(&c, &t);
        assert(maps == unmaps);
        assert(acpi_table_find(&c, "NONE", 0, &t) == ACPI_NOT_FOUND);
        assert(maps == unmaps);
        ram[0x108]++;
        assert(acpi_context_open(&c, &mapper, 0x100) == ACPI_INVALID);
        assert(maps == unmaps);
    }
    rsdp(2);
    ram[0x120]++;
    acpi_context_t c;
    assert(acpi_context_open(&c, &mapper, 0x100) == ACPI_INVALID);
    rsdp(2);
    put(ram + 0x114, 35, 4);
    assert(acpi_context_open(&c, &mapper, 0x100) == ACPI_INVALID);
    rsdp(2);
    put(ram + 0x114, 4097, 4);
    assert(acpi_context_open(&c, &mapper, 0x100) == ACPI_INVALID);
    rsdp(2);
    put(ram + 0x1004, 43, 4);
    fix(0x1000);
    assert(acpi_context_open(&c, &mapper, 0x100) == ACPI_INVALID);
    rsdp(2);
    memcpy(ram + 0x1000, "RSDT", 4);
    fix(0x1000);
    assert(acpi_context_open(&c, &mapper, 0x100) == ACPI_INVALID);
    assert(acpi_context_open(&c, &mapper, ~(uint64_t)0 - 18) == ACPI_INVALID);
    assert(maps == unmaps);
}
static void invalid_children(void) {
    rsdp(2);
    acpi_context_t c = open_context();
    acpi_table_t t;
    put(ram + 0x2004, 35, 4);
    assert(acpi_table_find(&c, "TEST", 0, &t) == ACPI_INVALID);
    put(ram + 0x2004, ACPI_TABLE_MAX_SIZE + 1, 4);
    assert(acpi_table_find(&c, "TEST", 0, &t) == ACPI_INVALID);
    table(0x2000, "TEST", 40);
    ram[0x2009]++;
    assert(acpi_table_find(&c, "TEST", 0, &t) == ACPI_INVALID);
    assert(acpi_table_map(&c, ~(uint64_t)0 - 20, NULL, &t) == ACPI_INVALID);
    assert(acpi_table_map(&c, 0xffff, NULL, &t) == ACPI_UNMAPPED);
    table(0x2000, "TEST", 40);
    put(ram + 0x1004, 52, 4);
    put(ram + 0x1000 + 44, 0x3000, 8);
    fix(0x1000);
    table(0x3000, "TEST", 36);
    assert(acpi_table_find(&c, "TEST", 1, &t) == 0 && t.physical == 0x3000);
    acpi_table_release(&c, &t);
    assert(maps == unmaps);
}
static void dsdt(void) {
    rsdp(2);
    acpi_context_t c = open_context();
    table(0x2000, "FACP", 148);
    put(ram + 0x2000 + 40, 0x3000, 4);
    put(ram + 0x2000 + 140, 0x4000, 8);
    fix(0x2000);
    table(0x3000, "DSDT", 40);
    table(0x4000, "DSDT", 44);
    acpi_table_t t;
    assert(acpi_dsdt_find(&c, &t) == 0 && t.physical == 0x4000);
    acpi_table_release(&c, &t);
    put(ram + 0x2000 + 140, 0, 8);
    fix(0x2000);
    assert(acpi_dsdt_find(&c, &t) == 0 && t.physical == 0x3000);
    acpi_table_release(&c, &t);
    memcpy(ram + 0x3000, "BAD!", 4);
    fix(0x3000);
    assert(acpi_dsdt_find(&c, &t) == ACPI_INVALID);
    assert(maps == unmaps);
}
static void topology(void) {
    rsdp(2);
    acpi_context_t c = open_context();
    table(0x2000, "MCFG", 76);
    put(ram + 0x2000 + 44, 0xe0000000, 8);
    ram[0x2000 + 55] = 127;
    put(ram + 0x2000 + 60, 0x120000000ULL, 8);
    put(ram + 0x2000 + 68, 42, 2);
    ram[0x2000 + 71] = 255;
    fix(0x2000);
    acpi_table_t t;
    assert(acpi_table_find(&c, "MCFG", 0, &t) == 0);
    size_t n;
    acpi_mcfg_entry_t e;
    assert(acpi_mcfg_count(&t, &n) == 0 && n == 2);
    assert(acpi_mcfg_get(&t, 1, &e) == 0 && e.segment == 42 &&
           e.base == 0x120000000ULL);
    ram[0x2000 + 70] = 255;
    ram[0x2000 + 71] = 0;
    assert(acpi_mcfg_count(&t, &n) == ACPI_INVALID);
    ram[0x2000 + 70] = 0;
    ram[0x2000 + 71] = 255;
    ram[0x2000 + 72] = 1;
    assert(acpi_mcfg_count(&t, &n) == ACPI_INVALID);
    acpi_table_release(&c, &t);
    table(0x2000, "APIC", 44 + 8 + 12 + 16);
    unsigned char *p = ram + 0x2000 + 44;
    p[0] = 0;
    p[1] = 8;
    p[3] = 3;
    put(p + 4, 1, 4);
    p += 8;
    p[0] = 1;
    p[1] = 12;
    p[2] = 7;
    put(p + 4, 0xfec00000, 4);
    put(p + 8, 24, 4);
    p += 12;
    p[0] = 9;
    p[1] = 16;
    put(p + 4, 0x1234, 4);
    put(p + 8, 2, 4);
    fix(0x2000);
    assert(acpi_table_find(&c, "APIC", 0, &t) == 0);
    acpi_madt_iterator_t it;
    acpi_madt_entry_t ent;
    uint32_t id, flags, base;
    assert(acpi_madt_begin(&t, &it) == 0);
    assert(acpi_madt_next(&it, &ent) == 1 &&
           acpi_madt_cpu(&ent, &id, &flags) == 0);
    assert(id == 3 && flags == 1);
    assert(acpi_madt_next(&it, &ent) == 1 &&
           acpi_madt_ioapic(&ent, &id, &flags, &base) == 0);
    assert(id == 7 && flags == 0xfec00000 && base == 24);
    assert(acpi_madt_next(&it, &ent) == 1 &&
           acpi_madt_cpu(&ent, &id, &flags) == 0);
    assert(id == 0x1234 && flags == 2);
    assert(acpi_madt_next(&it, &ent) == 0);
    ram[0x2000 + 44 + 8 + 12 + 1] = 0;
    assert(acpi_madt_begin(&t, &it) == ACPI_INVALID);
    acpi_table_release(&c, &t);
    assert(maps == unmaps);
}
static uint64_t reserved_pa[16];
static size_t reserved_length[16], reserved_count;
static int reservation_error;
static int reserve(void *opaque, uint64_t pa, size_t length) {
    (void)opaque;
    if (reservation_error)
        return reservation_error;
    assert(reserved_count < 16);
    reserved_pa[reserved_count] = pa;
    reserved_length[reserved_count++] = length;
    return 0;
}
static int reserved(uint64_t pa, size_t length) {
    for (size_t i = 0; i < reserved_count; i++)
        if (reserved_pa[i] == pa && reserved_length[i] == length)
            return 1;
    return 0;
}
static acpi_context_t reservation_fixture(unsigned revision) {
    rsdp(revision);
    reserved_count = 0;
    reservation_error = 0;
    table(0x2000, "FACP", 148);
    put(ram + 0x2000 + 36, 0x5000, 4);
    put(ram + 0x2000 + 40, 0x3000, 4);
    if (revision >= 2) {
        put(ram + 0x2000 + 132, 0x6000, 8);
        put(ram + 0x2000 + 140, 0x4000, 8);
    }
    fix(0x2000);
    table(0x3000, "DSDT", 40);
    table(0x4000, "DSDT", 44);
    memcpy(ram + 0x5000, "FACS", 4);
    put(ram + 0x5004, 64, 4);
    memcpy(ram + 0x6000, "FACS", 4);
    put(ram + 0x6004, 64, 4);
    ram[0x6008] = 0x5a; /* FACS deliberately has no SDT checksum. */
    return open_context();
}
static void reservations(void) {
    for (unsigned revision = 0; revision <= 2; revision += 2) {
        acpi_context_t c = reservation_fixture(revision);
        assert(acpi_reserve_tables(&c, reserve, NULL) == 0 &&
               reserved_count == 5);
        assert(reserved(0x100, revision >= 2 ? 36 : 20));
        assert(reserved(0x1000, revision >= 2 ? 44 : 40) &&
               reserved(0x2000, 148));
        assert(
            reserved(revision >= 2 ? 0x4000 : 0x3000, revision >= 2 ? 44 : 40));
        assert(reserved(revision >= 2 ? 0x6000 : 0x5000, 64) && maps == unmaps);
    }
    acpi_context_t c = reservation_fixture(2);
    reservation_error = -28;
    assert(acpi_reserve_tables(&c, reserve, NULL) == -28 && maps == unmaps);
    c = reservation_fixture(2);
    ram[0x4009]++;
    assert(acpi_reserve_tables(&c, reserve, NULL) == ACPI_INVALID);
    assert(maps == unmaps);
    c = reservation_fixture(2);
    memcpy(ram + 0x6000, "BAD!", 4);
    assert(acpi_reserve_tables(&c, reserve, NULL) == ACPI_INVALID);
    assert(maps == unmaps);
    c = reservation_fixture(2);
    put(ram + 0x6004, 63, 4);
    assert(acpi_reserve_tables(&c, reserve, NULL) == ACPI_INVALID);
    assert(maps == unmaps);
    c = reservation_fixture(2);
    put(ram + 0x6004, ACPI_TABLE_MAX_SIZE + 1, 4);
    assert(acpi_reserve_tables(&c, reserve, NULL) == ACPI_INVALID &&
           maps == unmaps);
    c = reservation_fixture(2);
    ram[0x108]++;
    assert(acpi_reserve_tables(&c, reserve, NULL) == ACPI_INVALID);
    assert(maps == unmaps);
}
int main(void) {
    roots();
    invalid_children();
    dsdt();
    topology();
    reservations();
    puts("ACPI table/root/topology fixtures PASS");
    return 0;
}
