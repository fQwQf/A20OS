#include "firmware/devicetree.h"
#include "core/errno.h"
#include "core/string.h"

static int add(fw_dt_range_t *out, size_t *count, size_t capacity,
               uint64_t base, uint64_t size) {
    if (!size || size > (~(uint64_t)0) - base)
        return -EOVERFLOW;
    if (*count >= capacity)
        return -ENOSPC;
    out[(*count)++] = (fw_dt_range_t){base, size};
    return 0;
}
static void merge(fw_dt_range_t *r, size_t *count) {
    for (size_t i = 1; i < *count; i++) {
        fw_dt_range_t v = r[i];
        size_t j = i;
        while (j && r[j - 1].base > v.base) {
            r[j] = r[j - 1];
            j--;
        }
        r[j] = v;
    }
    size_t n = 0;
    for (size_t i = 0; i < *count; i++) {
        uint64_t end = r[i].base + r[i].size;
        if (n && r[i].base <= r[n - 1].base + r[n - 1].size) {
            uint64_t old = r[n - 1].base + r[n - 1].size;
            if (end > old)
                r[n - 1].size = end - r[n - 1].base;
        } else
            r[n++] = r[i];
    }
    *count = n;
}
static int property_string(const fw_node_t *n, const char *key,
                           const char *value) {
    fw_property_t p;
    int rc = n->ops->property(n, key, &p);
    if (rc == -ENOENT)
        return 0;
    if (rc)
        return rc;
    if (!p.size || ((const uint8_t *)p.data)[p.size - 1] ||
        strlen(p.data) + 1 != p.size)
        return -EINVAL;
    size_t size = strlen(value) + 1;
    return p.size == size && !memcmp(p.data, value, size);
}
static int collect_regs(const fw_node_t *n, fw_dt_range_t *ranges,
                        size_t *count, size_t capacity) {
    fw_resource_t r;
    size_t index = 0;
    for (;;) {
        int rc = n->ops->resource(n, index++, &r);
        if (rc == -ENOENT)
            return index > 1 ? 0 : -EINVAL;
        if (rc)
            return rc;
        rc = add(ranges, count, capacity, r.address, r.size);
        if (rc)
            return rc;
    }
}
static int collect(const fw_node_t *n, fw_dt_memory_t *map, int reserved,
                   unsigned depth) {
    if (depth >= FDT_VIEW_MAX_DEPTH)
        return -E2BIG;
    int rc = n->ops->available(n);
    if (rc <= 0)
        return rc;
    int memory = property_string(n, "device_type", "memory");
    if (memory < 0)
        return memory;
    if (memory) {
        rc = collect_regs(n, map->ram, &map->ram_count, map->ram_capacity);
        if (rc)
            return rc;
    }
    if (reserved) {
        fw_property_t reg;
        rc = n->ops->property(n, "reg", &reg);
        if (rc == -ENOENT) {
            fw_property_t size;
            rc = n->ops->property(n, "size", &size);
            return rc == 0 ? -EOPNOTSUPP : rc == -ENOENT ? -EINVAL : rc;
        }
        if (rc)
            return rc;
        rc = collect_regs(n, map->reserved, &map->reserved_count,
                          map->reserved_capacity);
        if (rc)
            return rc;
    }
    fw_node_t child;
    rc = n->ops->next_child(n, NULL, &child);
    while (!rc) {
        const char *name = n->ops->name(n);
        int children_reserved =
            depth == 1 && name && !strcmp(name, "reserved-memory");
        rc = collect(&child, map, children_reserved, depth + 1);
        if (rc)
            return rc;
        fw_node_t next;
        rc = n->ops->next_child(n, &child, &next);
        if (!rc)
            child = next;
    }
    return rc == -ENOENT ? 0 : rc;
}
int fw_dt_memory(const fw_dt_t *tree, uint64_t physical, fw_dt_memory_t *out) {
    if (!out || (!out->ram && out->ram_capacity) ||
        (!out->reserved && out->reserved_capacity))
        return -EINVAL;
    out->ram_count = out->reserved_count = 0;
    fw_node_t root;
    int rc = fw_dt_root(tree, &root);
    if (rc)
        return rc;
    rc = collect(&root, out, 0, 0);
    uint64_t base, size;
    for (size_t i = 0; !rc; i++) {
        int found = fdt_view_reserve(&tree->view, i, &base, &size);
        if (found < 0) {
            rc = -EINVAL;
            break;
        }
        if (!found)
            break;
        rc = add(out->reserved, &out->reserved_count, out->reserved_capacity,
                 base, size);
    }
    if (!rc)
        rc = add(out->reserved, &out->reserved_count, out->reserved_capacity,
                 physical, tree->view.size);
    if (rc) {
        out->ram_count = out->reserved_count = 0;
        return rc;
    }
    merge(out->ram, &out->ram_count);
    merge(out->reserved, &out->reserved_count);
    return 0;
}
int fw_dt_memory_subtract(const fw_dt_memory_t *map, fw_dt_range_t *out,
                          size_t capacity, size_t *count) {
    if (!map || !count || (!out && capacity))
        return -EINVAL;
    *count = 0;
    if ((!map->ram && map->ram_count) ||
        (!map->reserved && map->reserved_count) ||
        map->ram_count > map->ram_capacity ||
        map->reserved_count > map->reserved_capacity)
        return -EINVAL;
    for (unsigned kind = 0; kind < 2; kind++) {
        const fw_dt_range_t *ranges = kind ? map->reserved : map->ram;
        size_t length = kind ? map->reserved_count : map->ram_count;
        uint64_t previous_end = 0;
        for (size_t i = 0; i < length; i++) {
            if (!ranges[i].size ||
                ranges[i].size > (~(uint64_t)0) - ranges[i].base ||
                (i && ranges[i].base < previous_end))
                return -EINVAL;
            previous_end = ranges[i].base + ranges[i].size;
        }
    }
    for (size_t i = 0; i < map->ram_count; i++) {
        uint64_t cursor = map->ram[i].base, end = cursor + map->ram[i].size;
        if (!map->ram[i].size || end < cursor)
            return -EINVAL;
        for (size_t j = 0; j < map->reserved_count && cursor < end; j++) {
            uint64_t base = map->reserved[j].base,
                     stop = base + map->reserved[j].size;
            if (!map->reserved[j].size || stop < base) {
                *count = 0;
                return -EINVAL;
            }
            if (stop <= cursor || base >= end)
                continue;
            if (base > cursor) {
                int rc = add(out, count, capacity, cursor, base - cursor);
                if (rc) {
                    *count = 0;
                    return rc;
                }
            }
            cursor = stop < end ? stop : end;
        }
        if (cursor < end) {
            int rc = add(out, count, capacity, cursor, end - cursor);
            if (rc) {
                *count = 0;
                return rc;
            }
        }
    }
    return 0;
}

int fw_dt_memory_window(const fw_dt_t *tree, uint64_t physical,
                        uint64_t window_base, uint64_t window_end,
                        uint64_t kernel_base, uint64_t kernel_end,
                        fw_dt_range_t *out, size_t capacity, size_t *count,
                        unsigned flags) {
    if ((!out && capacity) || !count || window_end <= window_base ||
        kernel_end <= kernel_base || kernel_base < window_base ||
        kernel_end > window_end || (flags & ~FW_DT_MEMORY_STATIC_IF_ABSENT))
        return -EINVAL;
    *count = 0;
    fw_dt_range_t ram[32], reserved[64], usable[96];
    fw_dt_memory_t map = {ram, reserved, 32, 64, 0, 0};
    int rc = fw_dt_memory(tree, physical, &map);
    if (rc)
        return rc;
    if (!map.ram_count) {
        if (!(flags & FW_DT_MEMORY_STATIC_IF_ABSENT))
            return -ENOENT;
        ram[0] = (fw_dt_range_t){window_base, window_end - window_base};
        map.ram_count = 1;
    }
    for (size_t i = 0; i < map.reserved_count; i++) {
        uint64_t base = reserved[i].base, end = base + reserved[i].size;
        if (base < kernel_end && end > kernel_base)
            return -EFAULT;
    }
    size_t available = 0;
    rc = fw_dt_memory_subtract(&map, usable, 96, &available);
    if (rc)
        return rc;
    int kernel_found = 0;
    for (size_t i = 0; i < available; i++) {
        uint64_t base = usable[i].base, end = base + usable[i].size;
        if (base < window_base)
            base = window_base;
        if (end > window_end)
            end = window_end;
        if (base > (~(uint64_t)0) - 4095) {
            *count = 0;
            return -EOVERFLOW;
        }
        base = (base + 4095) & ~(uint64_t)4095;
        end &= ~(uint64_t)4095;
        if (end <= base)
            continue;
        if (base <= kernel_base && end >= kernel_end)
            kernel_found = 1;
        rc = add(out, count, capacity, base, end - base);
        if (rc) {
            *count = 0;
            return rc;
        }
    }
    if (!kernel_found) {
        *count = 0;
        return -EFAULT;
    }
    return 0;
}
