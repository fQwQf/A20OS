/* SPDX-License-Identifier: Apache-2.0 */
#include "firmware/ivrs.h"
#include "core/string.h"

static size_t hardware_prefix(unsigned type)
{ return type == 0x10 ? 24 : (type == 0x11 || type == 0x40) ? 40 : 0; }

static int device_size(const uint8_t *p, size_t left, unsigned format, size_t *size)
{
    if (!left) return ACPI_INVALID;
    if (p[0] < 0x80) {
        *size = p[0] < 0x40 ? 4 : 8;
        return *size <= left ? 0 : ACPI_INVALID;
    }
    if (format != 0x40 || p[0] != 0xf0)
        return ACPI_IVRS_UNSUPPORTED;
    if (left < 22 || p[20] > 2 || (!p[20] != !p[21]))
        return ACPI_INVALID;
    *size = 22U + p[21];
    return *size <= left ? 0 : ACPI_INVALID;
}

int acpi_ivrs_device_next(const acpi_ivrs_hardware_t *h, size_t *offset,
                           acpi_ivrs_device_t *out)
{
    if (!h || !offset || !out || !h->entries || !hardware_prefix(h->type) ||
        *offset > h->entries_length) return ACPI_INVALID;
    *out = (acpi_ivrs_device_t){0};
    if (*offset == h->entries_length) return 0;
    const uint8_t *p = h->entries + *offset;
    size_t size;
    int ret = device_size(p, h->entries_length - *offset, h->type, &size);
    if (ret) return ret;
    *out = (acpi_ivrs_device_t){p, size, p[0], p[3], acpi_le16(p + 1)};
    *offset += size;
    return 1;
}

static int block_valid(const acpi_ivrs_block_t *b)
{
    return b && b->data && b->length >= 4 && b->length <= 65535 &&
           b->type == b->data[0] && b->flags == b->data[1] &&
           b->length == acpi_le16(b->data + 2);
}

int acpi_ivrs_hardware(const acpi_ivrs_block_t *b, acpi_ivrs_hardware_t *out)
{
    if (!out) return ACPI_INVALID;
    *out = (acpi_ivrs_hardware_t){0};
    if (!block_valid(b)) return ACPI_INVALID;
    size_t prefix = hardware_prefix(b->type);
    if (!prefix) return ACPI_NOT_FOUND;
    if (b->length <= prefix) return ACPI_INVALID;
    const uint8_t *p = b->data;
    uint64_t base = acpi_le64(p + 8);
    uint16_t capability = acpi_le16(p + 6);
    if (!base || (base & 4095) || base > ~(uint64_t)0 - 0x3fff ||
        capability < 0x40 || capability > 0xfc || (capability & 3)) return ACPI_INVALID;
    *out = (acpi_ivrs_hardware_t){b->type, b->flags, acpi_le16(p + 4), capability,
        acpi_le16(p + 16), acpi_le16(p + 18), acpi_le32(p + 20), base,
        prefix == 40 ? acpi_le64(p + 24) : 0,
        prefix == 40 ? acpi_le64(p + 32) : 0, p + prefix, b->length - prefix};
    return 0;
}

int acpi_ivrs_memory(const acpi_ivrs_block_t *b, acpi_ivrs_memory_t *out)
{
    if (!out) return ACPI_INVALID;
    *out = (acpi_ivrs_memory_t){0};
    if (!block_valid(b)) return ACPI_INVALID;
    if (b->type < 0x20 || b->type > 0x22) return ACPI_NOT_FOUND;
    if (b->length != 32) return ACPI_INVALID;
    const uint8_t *p = b->data;
    uint64_t start = acpi_le64(p + 16), size = acpi_le64(p + 24);
    if (!size || size - 1 > ~(uint64_t)0 - start ||
        (b->type == 0x22 && acpi_le16(p + 4) > acpi_le16(p + 6))) return ACPI_INVALID;
    *out = (acpi_ivrs_memory_t){b->type, b->flags, acpi_le16(p + 4),
        acpi_le16(p + 6), b->type == 0x20 ? 0 : acpi_le16(p + 8), start, size};
    return 0;
}

int acpi_ivrs_next(acpi_ivrs_iterator_t *it, acpi_ivrs_block_t *out)
{
    if (!it || !it->table || !it->table->data || !out || it->offset < 48 ||
        it->offset > it->table->length) return ACPI_INVALID;
    *out = (acpi_ivrs_block_t){0};
    size_t left = it->table->length - it->offset;
    if (!left) return 0;
    if (left < 4) return ACPI_INVALID;
    const uint8_t *p = it->table->data + it->offset;
    size_t length = acpi_le16(p + 2);
    if (length < 4 || length > left) return ACPI_INVALID;
    *out = (acpi_ivrs_block_t){p, length, p[0], p[1]};
    it->offset += length;
    return 1;
}

int acpi_ivrs_begin(const acpi_table_t *table, acpi_ivrs_iterator_t *out)
{
    if (!out) return ACPI_INVALID;
    *out = (acpi_ivrs_iterator_t){0};
    if (!table || !table->data || table->length < 48 || table->length > ACPI_TABLE_MAX_SIZE ||
        memcmp(table->data, "IVRS", 4) || acpi_le32(table->data + 4) != table->length ||
        acpi_le64(table->data + 40)) return ACPI_INVALID;
    if (table->data[8] != 1 && table->data[8] != 2) return ACPI_IVRS_UNSUPPORTED;
    uint8_t checksum = 0;
    for (size_t i = 0; i < table->length; i++) checksum += table->data[i];
    if (checksum) return ACPI_INVALID;
    acpi_ivrs_iterator_t it = {table, 48};
    acpi_ivrs_block_t block;
    int ret;
    while ((ret = acpi_ivrs_next(&it, &block)) > 0) {
        if (hardware_prefix(block.type)) {
            if ((block.type != 0x10 && !(acpi_le32(table->data + 36) & 1)) ||
                (block.type == 0x40 && table->data[8] != 2)) return ACPI_INVALID;
            acpi_ivrs_hardware_t hardware;
            if (acpi_ivrs_hardware(&block, &hardware)) return ACPI_INVALID;
            size_t offset = 0;
            bool range = false;
            uint16_t start = 0;
            acpi_ivrs_device_t device;
            while ((ret = acpi_ivrs_device_next(&hardware, &offset, &device)) > 0) {
                if (device.type == 3 || device.type == 0x43 || device.type == 0x47) {
                    if (range) return ACPI_INVALID;
                    range = true; start = device.device_id;
                } else if (device.type == 4) {
                    if (!range || device.device_id < start) return ACPI_INVALID;
                    range = false;
                }
                if (device.type == 0xf0 && (acpi_le32(table->data + 36) & 2) &&
                    device.data[20] != 2) return ACPI_INVALID;
            }
            if (ret < 0) return ret;
            if (range) return ACPI_INVALID;
        } else if (block.type >= 0x20 && block.type <= 0x22) {
            acpi_ivrs_memory_t memory;
            if (acpi_ivrs_memory(&block, &memory)) return ACPI_INVALID;
        }
    }
    if (ret < 0) return ret;
    *out = (acpi_ivrs_iterator_t){table, 48};
    return 0;
}
