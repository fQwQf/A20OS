/* SPDX-License-Identifier: Apache-2.0 */
#include "firmware/ivrs.h"

#define MAX_UNITS 64
#define MAX_BLOCKS 1024
struct units {
    size_t offsets[MAX_UNITS], count;
    uint8_t formats[MAX_UNITS];
};

/* Validate even alternatives which will not be selected. They must not hide
 * an unknown policy behind an older, otherwise usable hardware description. */
static int device_policy(const acpi_ivrs_hardware_t *h) {
    size_t offset = 0;
    acpi_ivrs_device_t e;
    bool range = false, variable = false;
    while (acpi_ivrs_device_next(h, &offset, &e) > 0) {
        if (range && e.type != 4)
            return ACPI_IVRS_UNSUPPORTED;
        if (variable && e.type < 0x80)
            return ACPI_IVRS_UNSUPPORTED;
        variable |= e.type >= 0x80;
        range = e.type == 3 || e.type == 0x43 || e.type == 0x47;
        switch (e.type) {
            case 0:
            case 0x40:
            case 0x41:
            case 0x44:
            case 0x45:
                for (size_t i = 1; i < e.length; i++)
                    if (e.data[i])
                        return ACPI_IVRS_UNSUPPORTED;
                break;
            case 1:
            case 2:
            case 3:
            case 0xf0:
                break;
            case 4:
                if (e.settings)
                    return ACPI_IVRS_UNSUPPORTED;
                break;
            case 0x42:
            case 0x43:
                if (e.data[4] || e.data[7])
                    return ACPI_IVRS_UNSUPPORTED;
                break;
            case 0x46:
            case 0x47:
                if (acpi_le32(e.data + 4) & 0x7fffffffU)
                    return ACPI_IVRS_UNSUPPORTED;
                break;
            case 0x48:
                if (e.device_id || (e.data[7] != 1 && e.data[7] != 2))
                    return ACPI_IVRS_UNSUPPORTED;
                break;
            default:
                return ACPI_IVRS_UNSUPPORTED;
        }
    }
    return 0;
}

static acpi_ivrs_hardware_t at(const acpi_table_t *t, size_t offset) {
    const uint8_t *p = t->data + offset;
    acpi_ivrs_block_t b = {p, acpi_le16(p + 2), p[0], p[1]};
    acpi_ivrs_hardware_t h;
    (void)acpi_ivrs_hardware(&b, &h);
    return h;
}
static bool same_unit(const acpi_ivrs_hardware_t *a,
                      const acpi_ivrs_hardware_t *b) {
    return a->segment == b->segment && a->device_id == b->device_id &&
           a->capability == b->capability;
}

static int collect(const acpi_table_t *t, struct units *units) {
    *units = (struct units){0};
    acpi_ivrs_iterator_t it;
    int ret = acpi_ivrs_begin(t, &it);
    if (ret)
        return ret;
    acpi_ivrs_block_t b;
    acpi_ivrs_hardware_t parent = {0};
    unsigned blocks = 0;
    while (acpi_ivrs_next(&it, &b) > 0) {
        if (++blocks > MAX_BLOCKS)
            return ACPI_IVRS_UNSUPPORTED;
        acpi_ivrs_hardware_t h;
        if (!acpi_ivrs_hardware(&b, &h)) {
            ret = device_policy(&h);
            if (ret)
                return ret;
            uint8_t format = h.type == 0x10 ? 1 : h.type == 0x11 ? 2 : 4;
            parent = h;
            size_t i;
            for (i = 0; i < units->count; i++) {
                acpi_ivrs_hardware_t old = at(t, units->offsets[i]);
                if (same_unit(&old, &h)) {
                    if (old.base != h.base || old.info != h.info ||
                        (units->formats[i] & format))
                        return ACPI_IVRS_AMBIGUOUS;
                    units->formats[i] |= format;
                    if (old.type < h.type)
                        units->offsets[i] = (size_t)(b.data - t->data);
                    break;
                }
                uint64_t distance =
                    old.base > h.base ? old.base - h.base : h.base - old.base;
                if (distance < 0x4000)
                    return ACPI_IVRS_AMBIGUOUS;
            }
            if (i == units->count) {
                if (i == MAX_UNITS)
                    return ACPI_IVRS_UNSUPPORTED;
                units->formats[i] = format;
                units->offsets[units->count++] = (size_t)(b.data - t->data);
            }
        } else if (b.type >= 0x20 && b.type <= 0x22) {
            acpi_ivrs_memory_t m;
            (void)acpi_ivrs_memory(&b, &m);
            if (!parent.type || (m.type != 0x20 && m.segment != parent.segment))
                return ACPI_IVRS_UNSUPPORTED;
        } else
            return ACPI_IVRS_UNSUPPORTED;
    }
    return 0;
}

struct match {
    uint16_t dma;
    uint8_t settings;
    uint32_t extended;
    bool alias, non_pci;
};
static struct match entry_match(const acpi_ivrs_device_t *e, uint16_t rid) {
    bool alias = e->type == 0x42 || e->type == 0x43;
    return (struct match){
        alias ? acpi_le16(e->data + 5) : rid, e->settings,
        e->type == 0x46 || e->type == 0x47 ? acpi_le32(e->data + 4) : 0, alias,
        false};
}
static bool equal(const struct match *a, const struct match *b) {
    return a->dma == b->dma && a->settings == b->settings &&
           a->extended == b->extended && a->alias == b->alias;
}

static int match(const acpi_ivrs_hardware_t *h, uint16_t rid,
                 struct match *out) {
    size_t offset = 0;
    acpi_ivrs_device_t e, start = {0};
    struct match specific = {0}, fallback = {0};
    bool found = false, all = false, non_pci = false;
    while (acpi_ivrs_device_next(h, &offset, &e) > 0) {
        bool applies = false;
        struct match candidate = {0};
        switch (e.type) {
            case 0:
            case 0x40:
            case 0x41:
            case 0x44:
            case 0x45:
                break;
            case 1:
                candidate = entry_match(&e, rid);
                if (all && !equal(&fallback, &candidate))
                    return ACPI_IVRS_AMBIGUOUS;
                all = true;
                fallback = candidate;
                break;
            case 2:
            case 0x42:
            case 0x46:
                applies = rid == e.device_id;
                candidate = entry_match(&e, rid);
                break;
            case 3:
            case 0x43:
            case 0x47:
                start = e;
                break;
            case 4:
                applies = rid >= start.device_id && rid <= e.device_id;
                candidate = entry_match(&start, rid);
                break;
            case 0x48:
            case 0xf0:
                non_pci = true;
                if (rid ==
                    (e.type == 0x48 ? acpi_le16(e.data + 5) : e.device_id))
                    return ACPI_IVRS_UNSUPPORTED;
                break;
            default:
                return ACPI_IVRS_UNSUPPORTED;
        }
        if (applies) {
            if (found && !equal(&specific, &candidate))
                return ACPI_IVRS_AMBIGUOUS;
            specific = candidate;
            found = true;
        }
    }
    if (!found && !all)
        return ACPI_NOT_FOUND;
    /* Alias peripherals cannot also be covered by an ALL declaration (AMD
     * IOMMU specification 3.10, IVHD 8-byte device entries). */
    if (found && all && specific.alias)
        return ACPI_IVRS_AMBIGUOUS;
    *out = found ? specific : fallback;
    out->non_pci = non_pci;
    return 0;
}

static int find(const acpi_table_t *t, const struct units *units,
                uint16_t segment, uint16_t rid, acpi_ivrs_hardware_t *hardware,
                struct match *result) {
    bool found = false;
    for (size_t i = 0; i < units->count; i++) {
        acpi_ivrs_hardware_t h = at(t, units->offsets[i]);
        if (h.segment != segment)
            continue;
        struct match m;
        int ret = match(&h, rid, &m);
        if (ret == ACPI_NOT_FOUND)
            continue;
        if (ret)
            return ret;
        if (found)
            return ACPI_IVRS_AMBIGUOUS;
        *hardware = h;
        *result = m;
        found = true;
    }
    return found ? 0 : ACPI_NOT_FOUND;
}

static size_t memory(const acpi_table_t *t, const acpi_ivrs_route_t *route,
                     size_t wanted, acpi_ivrs_memory_t *out) {
    acpi_ivrs_iterator_t it = {t, 48};
    acpi_ivrs_block_t b;
    acpi_ivrs_hardware_t parent = {0};
    size_t count = 0;
    while (acpi_ivrs_next(&it, &b) > 0) {
        acpi_ivrs_hardware_t h;
        if (!acpi_ivrs_hardware(&b, &h)) {
            parent = h;
            continue;
        }
        if (!same_unit(&parent, &route->hardware))
            continue;
        acpi_ivrs_memory_t m;
        if (acpi_ivrs_memory(&b, &m))
            continue;
        bool applies = m.type == 0x20;
        uint16_t end = m.type == 0x22 ? m.auxiliary : m.device_id;
        applies |=
            (route->requester >= m.device_id && route->requester <= end) ||
            (route->dma_requester >= m.device_id &&
             route->dma_requester <= end);
        if (applies) {
            if (count == wanted && out)
                *out = m;
            count++;
        }
    }
    return count;
}

int acpi_ivrs_route(const acpi_table_t *t, uint16_t segment, uint16_t rid,
                    acpi_ivrs_route_t *out) {
    if (!out)
        return ACPI_INVALID;
    *out = (acpi_ivrs_route_t){0};
    struct units units;
    int ret = collect(t, &units);
    if (ret)
        return ret;
    acpi_ivrs_hardware_t hardware;
    struct match m;
    ret = find(t, &units, segment, rid, &hardware, &m);
    if (ret)
        return ret;
    if (m.alias && m.dma != rid) {
        acpi_ivrs_hardware_t target;
        struct match target_match;
        ret = find(t, &units, segment, m.dma, &target, &target_match);
        if (ret == ACPI_NOT_FOUND)
            return ACPI_IVRS_UNSUPPORTED;
        if (ret)
            return ret;
        if (!same_unit(&hardware, &target))
            return ACPI_IVRS_AMBIGUOUS;
        if (target_match.alias && target_match.dma != m.dma)
            return ACPI_IVRS_UNSUPPORTED;
    }
    acpi_ivrs_route_t route = {hardware,   rid,     m.dma,     m.settings,
                               m.extended, m.alias, m.non_pci, 0};
    route.memory_count = memory(t, &route, SIZE_MAX, NULL);
    *out = route;
    return 0;
}

int acpi_ivrs_route_memory(const acpi_table_t *t, uint16_t segment,
                           uint16_t rid, size_t index,
                           acpi_ivrs_memory_t *out) {
    if (!out)
        return ACPI_INVALID;
    *out = (acpi_ivrs_memory_t){0};
    acpi_ivrs_route_t route;
    int ret = acpi_ivrs_route(t, segment, rid, &route);
    if (ret)
        return ret;
    if (index >= route.memory_count)
        return ACPI_NOT_FOUND;
    (void)memory(t, &route, index, out);
    return 0;
}
