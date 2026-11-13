/* SPDX-License-Identifier: Apache-2.0 */
/* Reuse only byte-table builders; production routing code is linked below. */
#define main previous_policy_fixture_main
#include "acpi_ivrs_policy_host.c"
#undef main

static acpi_ivrs_requesters_t requesters(acpi_table_t *table, unsigned segment,
                                         unsigned rid, int expected) {
    *table = finish();
    acpi_ivrs_requesters_t view;
    memset(&view, 0xa5, sizeof(view));
    assert(acpi_ivrs_requesters(table, segment, rid, &view) == expected);
    if (expected) {
        assert(!view.table && !view.hardware.entries && !view.count && !view.memory_count);
        for (size_t i = 0; i < ACPI_IVRS_REQUESTER_MAX; i++) assert(!view.ids[i]);
    }
    return view;
}

static void direct_range_alternatives_and_memory(void) {
    begin();
    size_t e = hardware(0x11, 1, 3, 44);
    entry(e, 2, 0x100, 0, 0);
    entry(e + 4, 0x42, 0x200, 0x13, 0x100);
    entry(e + 12, 0x42, 0x300, 0x17, 0x100);
    entry(e + 20, 0x42, 0x300, 0x17, 0x100); /* Identical duplicate. */
    entry(e + 28, 0x43, 0x400, 0, 0x100);
    entry(e + 36, 4, 0x402, 0, 0);
    entry(e + 40, 2, 0x700, 0, 0);
    memory(0x20, 0, 0, 0, 0x100000);
    memory(0x21, 0x300, 0, 3, 0x200000);
    memory(0x22, 0x200, 0x402, 3, 0x300000); /* Once, not once per peer. */
    memory(0x21, 0x700, 0, 3, 0x400000);
    e = hardware(0x10, 1, 3, 12);
    entry(e, 2, 0x100, 0, 0);
    entry(e + 4, 0x42, 0x900, 0, 0x100); /* Nonselected alias is not routing. */
    memory(0x21, 0x300, 0, 3, 0x500000);  /* Its IVMD is still retained. */
    e = hardware(0x11, 2, 3, 4); entry(e, 2, 0x900, 0, 0);
    e = hardware(0x11, 3, 4, 12);
    entry(e, 2, 0x100, 0, 0); entry(e + 4, 0x42, 0x800, 0, 0x100);
    memory(0x20, 0, 0, 0, 0x600000);

    acpi_table_t table;
    acpi_ivrs_requesters_t view = requesters(&table, 3, 0x200, 0);
    const uint16_t expected[] = {0x100, 0x200, 0x300, 0x400, 0x401, 0x402};
    assert(view.count == 6 && !memcmp(view.ids, expected, sizeof(expected)));
    assert(view.requester == 0x200 && view.dma_requester == 0x100 && view.hardware.type == 0x11);
    assert(view.memory_count == 4);
    const uint64_t addresses[] = {0x100000, 0x200000, 0x300000, 0x500000};
    for (size_t i = 0; i < view.memory_count; i++) {
        acpi_ivrs_memory_t m;
        assert(!acpi_ivrs_requester_memory(&view, i, &m) && m.start == addresses[i]);
    }
    acpi_ivrs_memory_t missing;
    memset(&missing, 0xa5, sizeof(missing));
    assert(acpi_ivrs_requester_memory(&view, 4, &missing) == ACPI_NOT_FOUND && !missing.start && !missing.flags);
    view = requesters(&table, 3, 0x100, 0); /* Direct target sees incoming aliases too. */
    assert(view.count == 6 && !memcmp(view.ids, expected, sizeof(expected)));
    view = requesters(&table, 3, 0x300, 0);
    assert(view.count == 6 && !memcmp(view.ids, expected, sizeof(expected)));
    view = requesters(&table, 4, 0x800, 0);
    assert(view.count == 2 && view.ids[0] == 0x100 && view.ids[1] == 0x800 && view.memory_count == 1);
}

static void incoming_errors(void) {
    acpi_table_t table;
    begin();
    size_t e = hardware(0x11, 1, 3, 4); entry(e, 2, 0x100, 0, 0);
    e = hardware(0x11, 2, 3, 8); entry(e, 0x42, 0x200, 0, 0x100);
    route(3, 0x100, 0); /* Ordinary target routing alone misses this conflict. */
    requesters(&table, 3, 0x100, ACPI_IVRS_AMBIGUOUS);

    begin(); e = hardware(0x11, 1, 3, 24);
    entry(e, 2, 0x100, 0, 0); entry(e + 4, 2, 0x101, 0, 0);
    entry(e + 8, 0x42, 0x200, 0, 0x100); entry(e + 16, 0x42, 0x200, 0, 0x101);
    route(3, 0x100, 0); requesters(&table, 3, 0x100, ACPI_IVRS_AMBIGUOUS);

    /* Unsupported incoming chains must not disappear based on record order. */
    for (unsigned reverse = 0; reverse < 2; reverse++) {
        begin(); e = hardware(0x11, 1, 3, 20); entry(e, 2, 0x100, 0, 0);
        entry(e + (reverse ? 12 : 4), 0x42, 0x300, 0, 0x200);
        entry(e + (reverse ? 4 : 12), 0x42, 0x200, 0, 0x100);
        route(3, 0x100, 0); requesters(&table, 3, 0x100, ACPI_IVRS_UNSUPPORTED);
    }
    begin(); e = hardware(0x11, 1, 3, 12);
    entry(e, 1, 0, 0, 0); entry(e + 4, 0x42, 0x200, 0, 0x100);
    route(3, 0x100, 0); requesters(&table, 3, 0x100, ACPI_IVRS_AMBIGUOUS);
}

static void limits_and_arguments(void) {
    acpi_table_t table;
    begin(); size_t e = hardware(0x11, 1, 0, 20);
    entry(e, 0x43, 0xff80, 0, 0xffff); entry(e + 8, 4, 0xffff, 0, 0);
    entry(e + 12, 0x42, 0xffff, 0, 0xffff);
    acpi_ivrs_requesters_t view = requesters(&table, 0, 0xff80, 0);
    assert(view.count == ACPI_IVRS_REQUESTER_MAX && view.ids[0] == 0xff80 && view.ids[127] == 0xffff);
    begin(); e = hardware(0x11, 1, 0, 12);
    entry(e, 0x43, 0xff7f, 0, 0xffff); entry(e + 8, 4, 0xffff, 0, 0);
    requesters(&table, 0, 0xff7f, ACPI_IVRS_UNSUPPORTED);
    requesters(&table, 1, 0xff7f, ACPI_NOT_FOUND);

    begin(); e = hardware(0x11, 1, 0, 4); entry(e, 1, 0, 0, 0);
    view = requesters(&table, 0, 0, 0); assert(view.count == 1 && !view.ids[0]);
    assert(acpi_ivrs_requesters(&table, 0, 0, NULL) == ACPI_INVALID);
    assert(acpi_ivrs_requesters(NULL, 0, 0, &view) == ACPI_INVALID && !view.table && !view.count);
    view = requesters(&table, 0, 0, 0);
    acpi_ivrs_memory_t memory;
    assert(acpi_ivrs_requester_memory(&view, 0, &memory) == ACPI_NOT_FOUND && !memory.length);
    assert(acpi_ivrs_requester_memory(NULL, 0, &memory) == ACPI_INVALID && !memory.length);
    assert(acpi_ivrs_requester_memory(&view, 0, NULL) == ACPI_INVALID);
    bytes[9]++; assert(acpi_ivrs_requesters(&table, 0, 0, &view) == ACPI_INVALID && !view.count && !view.table);
}

int main(void) {
    direct_range_alternatives_and_memory(); incoming_errors(); limits_and_arguments();
    puts("IVRS requester sets: complete aliases, shared memory, conflicts and bounds PASS");
    return 0;
}
