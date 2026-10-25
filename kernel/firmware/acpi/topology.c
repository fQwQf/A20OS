#include "firmware/acpi.h"
#include "core/string.h"

int acpi_mcfg_count(const acpi_table_t *t, size_t *count) {
    if (!t || !t->data || !count || t->length < 44 ||
        memcmp(t->data, "MCFG", 4) || (t->length - 44) % 16 ||
        acpi_le64(t->data + 36))
        return ACPI_INVALID;
    size_t total = (t->length - 44) / 16;
    for (size_t i = 0; i < total; i++) {
        const uint8_t *p = t->data + 44 + i * 16;
        uint64_t base = acpi_le64(p);
        uint64_t size = ((uint64_t)p[11] + 1) << 20;
        if (!base || (base & 0xfffffffU) || p[10] > p[11] ||
            acpi_le32(p + 12) || size - 1 > ~(uint64_t)0 - base)
            return ACPI_INVALID;
    }
    *count = total;
    return ACPI_OK;
}
int acpi_mcfg_get(const acpi_table_t *t, size_t index, acpi_mcfg_entry_t *out) {
    size_t count;
    if (!out || acpi_mcfg_count(t, &count))
        return ACPI_INVALID;
    if (index >= count)
        return ACPI_NOT_FOUND;
    const uint8_t *p = t->data + 44 + index * 16;
    *out = (acpi_mcfg_entry_t){acpi_le64(p), acpi_le16(p + 8), p[10], p[11]};
    return ACPI_OK;
}
int acpi_madt_begin(const acpi_table_t *t, acpi_madt_iterator_t *it) {
    if (!t || !t->data || !it || t->length < 44 || memcmp(t->data, "APIC", 4))
        return ACPI_INVALID;
    for (size_t off = 44; off < t->length;) {
        if (t->length - off < 2 || t->data[off + 1] < 2 ||
            t->data[off + 1] > t->length - off)
            return ACPI_INVALID;
        unsigned type = t->data[off], len = t->data[off + 1];
        if ((type == 0 && len < 8) || (type == 1 && len < 12) ||
            (type == 9 && len < 16))
            return ACPI_INVALID;
        off += len;
    }
    *it = (acpi_madt_iterator_t){t, 44};
    return ACPI_OK;
}
int acpi_madt_next(acpi_madt_iterator_t *it, acpi_madt_entry_t *out) {
    if (!it || !it->table || !out || it->offset > it->table->length)
        return ACPI_INVALID;
    const acpi_table_t *t = it->table;
    size_t left = t->length - it->offset;
    if (!left)
        return 0;
    if (left < 2 || t->data[it->offset + 1] < 2 ||
        t->data[it->offset + 1] > left)
        return ACPI_INVALID;
    const uint8_t *p = t->data + it->offset;
    *out = (acpi_madt_entry_t){p, p[0], p[1]};
    it->offset += p[1];
    return 1;
}
int acpi_madt_cpu(const acpi_madt_entry_t *e, uint32_t *id, uint32_t *flags) {
    if (!e || !e->data || !id || !flags)
        return ACPI_INVALID;
    if (e->type == 0 && e->length >= 8) {
        *id = e->data[3];
        *flags = acpi_le32(e->data + 4);
    } else if (e->type == 9 && e->length >= 16) {
        *id = acpi_le32(e->data + 4);
        *flags = acpi_le32(e->data + 8);
    } else {
        return e->type == 0 || e->type == 9 ? ACPI_INVALID : ACPI_NOT_FOUND;
    }
    return ACPI_OK;
}
int acpi_madt_ioapic(const acpi_madt_entry_t *e, uint32_t *id, uint32_t *pa,
                     uint32_t *base) {
    if (!e || !e->data || !id || !pa || !base)
        return ACPI_INVALID;
    if (e->type != 1)
        return ACPI_NOT_FOUND;
    if (e->length < 12)
        return ACPI_INVALID;
    *id = e->data[2];
    *pa = acpi_le32(e->data + 4);
    *base = acpi_le32(e->data + 8);
    return ACPI_OK;
}
