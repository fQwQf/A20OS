#include "firmware/acpi.h"
#include "core/string.h"

uint16_t acpi_le16(const void *p) {
    const uint8_t *b = p;
    return b[0] | (uint16_t)b[1] << 8;
}
uint32_t acpi_le32(const void *p) {
    const uint8_t *b = p;
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}
uint64_t acpi_le64(const void *p) {
    const uint8_t *b = p;
    return acpi_le32(b) | (uint64_t)acpi_le32(b + 4) << 32;
}
static int span(uint64_t pa, size_t length) {
    return pa && length && (uint64_t)(length - 1) <= ~(uint64_t)0 - pa;
}
static int checksum(const void *data, size_t length) {
    const uint8_t *b = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}
static const void *map(const acpi_context_t *c, uint64_t pa, size_t size) {
    if (!c || !c->mapper.map || !c->mapper.unmap || !span(pa, size))
        return NULL;
    return c->mapper.map(c->mapper.opaque, pa, size);
}
static void unmap(const acpi_context_t *c, const void *p, size_t size) {
    c->mapper.unmap(c->mapper.opaque, p, size);
}
int acpi_table_map(const acpi_context_t *c, uint64_t pa,
                   const char signature[4], acpi_table_t *out) {
    if (!out)
        return ACPI_INVALID;
    *out = (acpi_table_t){0};
    if (!span(pa, ACPI_SDT_HEADER_SIZE))
        return ACPI_INVALID;
    const uint8_t *header = map(c, pa, ACPI_SDT_HEADER_SIZE);
    if (!header)
        return ACPI_UNMAPPED;
    size_t size = acpi_le32(header + 4);
    int valid = size >= ACPI_SDT_HEADER_SIZE && size <= ACPI_TABLE_MAX_SIZE &&
                span(pa, size) && (!signature || !memcmp(header, signature, 4));
    unmap(c, header, ACPI_SDT_HEADER_SIZE);
    if (!valid)
        return ACPI_INVALID;
    const uint8_t *data = map(c, pa, size);
    if (!data)
        return ACPI_UNMAPPED;
    /* A mapper may return different storage for the full mapping. */
    if (acpi_le32(data + 4) != size ||
        (signature && memcmp(data, signature, 4)) || !checksum(data, size)) {
        unmap(c, data, size);
        return ACPI_INVALID;
    }
    *out = (acpi_table_t){data, size, pa};
    return ACPI_OK;
}
void acpi_table_release(const acpi_context_t *c, acpi_table_t *table) {
    if (c && table && table->data) {
        unmap(c, table->data, table->length);
        *table = (acpi_table_t){0};
    }
}
int acpi_context_open(acpi_context_t *out, const acpi_mapper_t *mapper,
                      uint64_t rsdp) {
    if (!out || !mapper || !mapper->map || !mapper->unmap)
        return ACPI_INVALID;
    *out = (acpi_context_t){0};
    acpi_context_t c = {.mapper = *mapper, .rsdp = rsdp};
    if (!span(rsdp, 20))
        return ACPI_INVALID;
    const uint8_t *p = map(&c, rsdp, 20);
    if (!p)
        return ACPI_UNMAPPED;
    int valid = !memcmp(p, "RSD PTR ", 8) && checksum(p, 20);
    unsigned revision = p[15];
    uint64_t root = acpi_le32(p + 16);
    unmap(&c, p, 20);
    if (!valid)
        return ACPI_INVALID;
    unsigned width = 4;
    if (revision >= 2) {
        p = map(&c, rsdp, 36);
        if (!p)
            return ACPI_UNMAPPED;
        size_t size = acpi_le32(p + 20);
        unmap(&c, p, 36);
        if (size < 36 || size > ACPI_RSDP_MAX_SIZE || !span(rsdp, size))
            return ACPI_INVALID;
        p = map(&c, rsdp, size);
        if (!p)
            return ACPI_UNMAPPED;
        valid = !memcmp(p, "RSD PTR ", 8) && p[15] == revision &&
                acpi_le32(p + 20) == size && checksum(p, 20) &&
                checksum(p, size);
        uint64_t xsdt = acpi_le64(p + 24);
        root = acpi_le32(p + 16);
        unmap(&c, p, size);
        if (!valid)
            return ACPI_INVALID;
        if (xsdt) {
            root = xsdt;
            width = 8;
        }
    }
    acpi_table_t table;
    int rc = acpi_table_map(&c, root, width == 8 ? "XSDT" : "RSDT", &table);
    if (rc)
        return rc;
    valid = (table.length - ACPI_SDT_HEADER_SIZE) % width == 0;
    acpi_table_release(&c, &table);
    if (!valid)
        return ACPI_INVALID;
    c.root = root;
    c.entry_width = width;
    *out = c;
    return ACPI_OK;
}
int acpi_table_find(const acpi_context_t *c, const char signature[4],
                    size_t occurrence, acpi_table_t *out) {
    if (!c || !signature || !out ||
        (c->entry_width != 4 && c->entry_width != 8))
        return ACPI_INVALID;
    *out = (acpi_table_t){0};
    acpi_table_t root;
    int rc = acpi_table_map(c, c->root, c->entry_width == 8 ? "XSDT" : "RSDT",
                            &root);
    if (rc)
        return rc;
    if ((root.length - ACPI_SDT_HEADER_SIZE) % c->entry_width) {
        acpi_table_release(c, &root);
        return ACPI_INVALID;
    }
    rc = ACPI_NOT_FOUND;
    for (size_t off = ACPI_SDT_HEADER_SIZE; off < root.length;
         off += c->entry_width) {
        const uint8_t *p = root.data + off;
        uint64_t pa = c->entry_width == 8 ? acpi_le64(p) : acpi_le32(p);
        acpi_table_t table;
        int found = acpi_table_map(c, pa, NULL, &table);
        if (found) {
            rc = found;
            break;
        }
        if (!memcmp(table.data, signature, 4) && occurrence-- == 0) {
            *out = table;
            rc = ACPI_OK;
            break;
        }
        acpi_table_release(c, &table);
    }
    acpi_table_release(c, &root);
    return rc;
}
int acpi_table_read(const acpi_table_t *t, size_t off, unsigned width,
                    uint64_t *value) {
    if (!t || !t->data || !value || !width || width > 8 || off > t->length ||
        width > t->length - off)
        return ACPI_INVALID;
    uint64_t n = 0;
    for (unsigned i = 0; i < width; i++)
        n |= (uint64_t)t->data[off + i] << (8 * i);
    *value = n;
    return ACPI_OK;
}
int acpi_dsdt_find(const acpi_context_t *c, acpi_table_t *out) {
    if (!out)
        return ACPI_INVALID;
    *out = (acpi_table_t){0};
    acpi_table_t fadt;
    int rc = acpi_table_find(c, "FACP", 0, &fadt);
    if (rc)
        return rc;
    uint64_t dsdt = 0, extended = 0;
    acpi_table_read(&fadt, 40, 4, &dsdt);
    if (fadt.data[8] >= 2 && acpi_table_read(&fadt, 140, 8, &extended) == 0 &&
        extended)
        dsdt = extended;
    acpi_table_release(c, &fadt);
    return dsdt ? acpi_table_map(c, dsdt, "DSDT", out) : ACPI_NOT_FOUND;
}

/* FACS has no common SDT header or checksum. Its fixed prefix and declared
 * length must be validated independently before reserving shared firmware
 * waking-vector/global-lock storage. */
static int reserve_facs(const acpi_context_t *c, uint64_t pa,
                        acpi_reserve_callback_t reserve, void *opaque) {
    if ((pa & 63) || !span(pa, 8))
        return ACPI_INVALID;
    const uint8_t *header = map(c, pa, 8);
    if (!header)
        return ACPI_UNMAPPED;
    size_t length = acpi_le32(header + 4);
    int valid = !memcmp(header, "FACS", 4) && length >= 64 &&
                length <= ACPI_TABLE_MAX_SIZE && span(pa, length);
    unmap(c, header, 8);
    if (!valid)
        return ACPI_INVALID;
    const uint8_t *data = map(c, pa, length);
    if (!data)
        return ACPI_UNMAPPED;
    valid = !memcmp(data, "FACS", 4) && acpi_le32(data + 4) == length;
    int rc = valid ? reserve(opaque, pa, length) : ACPI_INVALID;
    unmap(c, data, length);
    return rc;
}
static int reserve_fadt_references(const acpi_context_t *c,
                                   const acpi_table_t *fadt,
                                   acpi_reserve_callback_t reserve,
                                   void *opaque) {
    uint64_t dsdt = 0, facs = 0, extended = 0;
    if (acpi_table_read(fadt, 36, 4, &facs) ||
        acpi_table_read(fadt, 40, 4, &dsdt))
        return ACPI_INVALID;
    if (fadt->data[8] >= 2) {
        if (!acpi_table_read(fadt, 132, 8, &extended) && extended)
            facs = extended;
        if (!acpi_table_read(fadt, 140, 8, &extended) && extended)
            dsdt = extended;
    }
    if (!dsdt)
        return ACPI_INVALID;
    acpi_table_t table;
    int rc = acpi_table_map(c, dsdt, "DSDT", &table);
    if (rc)
        return rc;
    rc = reserve(opaque, table.physical, table.length);
    acpi_table_release(c, &table);
    if (rc)
        return rc;
    return facs ? reserve_facs(c, facs, reserve, opaque) : ACPI_OK;
}
int acpi_reserve_tables(const acpi_context_t *c,
                        acpi_reserve_callback_t reserve, void *opaque) {
    if (!c || !reserve)
        return ACPI_INVALID;
    acpi_context_t checked;
    int rc = acpi_context_open(&checked, &c->mapper, c->rsdp);
    if (rc)
        return rc;
    if (checked.root != c->root || checked.entry_width != c->entry_width)
        return ACPI_INVALID;
    const uint8_t *rsdp = map(c, c->rsdp, 20);
    if (!rsdp)
        return ACPI_UNMAPPED;
    int extended = rsdp[15] >= 2;
    unmap(c, rsdp, 20);
    size_t rsdp_bytes = 20;
    if (extended) {
        rsdp = map(c, c->rsdp, 36);
        if (!rsdp)
            return ACPI_UNMAPPED;
        rsdp_bytes = acpi_le32(rsdp + 20);
        unmap(c, rsdp, 36);
        if (rsdp_bytes < 36 || rsdp_bytes > ACPI_RSDP_MAX_SIZE)
            return ACPI_INVALID;
    }
    rc = reserve(opaque, c->rsdp, rsdp_bytes);
    if (rc)
        return rc;
    acpi_table_t root;
    rc = acpi_table_map(c, c->root, c->entry_width == 8 ? "XSDT" : "RSDT",
                        &root);
    if (rc)
        return rc;
    size_t entries = (root.length - ACPI_SDT_HEADER_SIZE) / c->entry_width;
    if ((root.length - ACPI_SDT_HEADER_SIZE) % c->entry_width ||
        entries > ACPI_MAX_ROOT_ENTRIES) {
        acpi_table_release(c, &root);
        return ACPI_INVALID;
    }
    rc = reserve(opaque, root.physical, root.length);
    for (size_t i = 0; !rc && i < entries; i++) {
        const uint8_t *entry =
            root.data + ACPI_SDT_HEADER_SIZE + i * c->entry_width;
        uint64_t pa = c->entry_width == 8 ? acpi_le64(entry) : acpi_le32(entry);
        acpi_table_t table;
        rc = acpi_table_map(c, pa, NULL, &table);
        if (rc)
            break;
        rc = reserve(opaque, table.physical, table.length);
        if (!rc && !memcmp(table.data, "FACP", 4))
            rc = reserve_fadt_references(c, &table, reserve, opaque);
        acpi_table_release(c, &table);
    }
    acpi_table_release(c, &root);
    return rc;
}
