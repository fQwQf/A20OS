#include "firmware/devicetree.h"
#include "core/errno.h"
#include "core/string.h"

static const fw_node_ops_t dt_ops;
static const fw_dt_t *tree_of(const fw_node_t *n) {
    return n && n->ops == &dt_ops ? n->context : NULL;
}
static uint64_t event_id(const fw_dt_t *t, const fdt_event_t *e) {
    return (uint64_t)((const uint8_t *)e->name - t->view.structure - 4);
}
static fw_node_t handle(const fw_dt_t *t, uint64_t id) {
    return (fw_node_t){&dt_ops, t, id};
}
int fw_dt_init(fw_dt_t *t, const void *blob, size_t available) {
    if (!t)
        return -EINVAL;
    memset(t, 0, sizeof(*t));
    if (fdt_view_open(&t->view, blob, available) != 0) {
        memset(t, 0, sizeof(*t));
        return -EINVAL;
    }
    return 0;
}
int fw_dt_root(const fw_dt_t *t, fw_node_t *out) {
    if (!t || !out || !t->view.blob)
        return -EINVAL;
    fdt_cursor_t c;
    fdt_event_t e;
    fdt_cursor_init(&c, &t->view);
    if (fdt_cursor_next(&c, &e) != 1 || e.type != FDT_NODE_BEGIN)
        return -EINVAL;
    *out = handle(t, event_id(t, &e));
    return 0;
}
static const char *dt_name(const fw_node_t *n) {
    const fw_dt_t *t = tree_of(n);
    if (!t)
        return NULL;
    fdt_cursor_t c;
    fdt_event_t e;
    fdt_cursor_init(&c, &t->view);
    while (fdt_cursor_next(&c, &e) > 0)
        if (e.type == FDT_NODE_BEGIN && event_id(t, &e) == n->id)
            return e.name;
    return NULL;
}
static int dt_parent(const fw_node_t *n, fw_node_t *out) {
    const fw_dt_t *t = tree_of(n);
    if (!t || !out)
        return -EINVAL;
    uint64_t stack[FDT_VIEW_MAX_DEPTH];
    fdt_cursor_t c;
    fdt_event_t e;
    fdt_cursor_init(&c, &t->view);
    while (fdt_cursor_next(&c, &e) > 0) {
        if (e.type != FDT_NODE_BEGIN)
            continue;
        uint64_t id = event_id(t, &e);
        if (id == n->id) {
            if (!e.depth)
                return -ENOENT;
            *out = handle(t, stack[e.depth - 1]);
            return 0;
        }
        stack[e.depth] = id;
    }
    return -EINVAL;
}
static int dt_child(const fw_node_t *n, const fw_node_t *prev, fw_node_t *out) {
    const fw_dt_t *t = tree_of(n);
    if (!t || !out || (prev && tree_of(prev) != t))
        return -EINVAL;
    fdt_cursor_t c;
    fdt_event_t e;
    int active = 0, ready = !prev;
    unsigned depth = 0;
    fdt_cursor_init(&c, &t->view);
    while (fdt_cursor_next(&c, &e) > 0) {
        if (e.type == FDT_NODE_BEGIN) {
            uint64_t id = event_id(t, &e);
            if (!active && id == n->id) {
                active = 1;
                depth = e.depth;
                continue;
            }
            if (active && e.depth == depth + 1) {
                if (ready) {
                    *out = handle(t, id);
                    return 0;
                }
                if (prev->id == id)
                    ready = 1;
            }
        }
        if (active && e.type == FDT_NODE_END && e.depth == depth)
            return ready ? -ENOENT : -EINVAL;
    }
    return -EINVAL;
}
static int dt_property(const fw_node_t *n, const char *name,
                       fw_property_t *out) {
    const fw_dt_t *t = tree_of(n);
    if (!t || !name || !out)
        return -EINVAL;
    fdt_cursor_t c;
    fdt_event_t e;
    int active = 0, found = 0;
    unsigned depth = 0;
    fdt_cursor_init(&c, &t->view);
    while (fdt_cursor_next(&c, &e) > 0) {
        if (e.type == FDT_NODE_BEGIN) {
            if (active)
                break;
            if (event_id(t, &e) == n->id) {
                active = 1;
                depth = e.depth;
            }
        } else if (active && e.type == FDT_NODE_END)
            break;
        else if (active && e.type == FDT_PROPERTY && e.depth == depth &&
                 !strcmp(name, e.name)) {
            if (found++)
                return -EINVAL;
            *out = (fw_property_t){e.data, e.size, FW_ORDER_BIG_ENDIAN};
        }
    }
    return !active ? -EINVAL : found ? 0 : -ENOENT;
}
static int u32(const fw_node_t *n, const char *key, uint32_t *v) {
    fw_property_t p;
    int rc = dt_property(n, key, &p);
    if (rc)
        return rc;
    if (p.size != 4)
        return -EINVAL;
    *v = fdt_u32(p.data);
    return 0;
}
static int string(const fw_property_t *p) {
    return p->size && ((const uint8_t *)p->data)[p->size - 1] == 0 &&
           strlen(p->data) + 1 == p->size;
}
static int dt_available(const fw_node_t *n) {
    if (!tree_of(n))
        return -EINVAL;
    fw_node_t cur = *n;
    for (unsigned i = 0; i < FDT_VIEW_MAX_DEPTH; i++) {
        fw_property_t p;
        int rc = dt_property(&cur, "status", &p);
        if (!rc) {
            if (!string(&p))
                return -EINVAL;
            if (strcmp(p.data, "okay") && strcmp(p.data, "ok"))
                return 0;
        } else if (rc != -ENOENT)
            return rc;
        fw_node_t parent;
        rc = dt_parent(&cur, &parent);
        if (rc == -ENOENT)
            return 1;
        if (rc)
            return rc;
        cur = parent;
    }
    return -E2BIG;
}
static int dt_match(const fw_node_t *n, const char *id) {
    fw_property_t p;
    int rc = dt_property(n, "compatible", &p);
    if (rc == -ENOENT)
        return 0;
    if (rc || !id)
        return rc ? rc : -EINVAL;
    const uint8_t *b = p.data;
    size_t off = 0;
    int found = 0;
    while (off < p.size) {
        size_t len = 0;
        while (len < p.size - off && b[off + len])
            len++;
        if (!len || len == p.size - off)
            return -EINVAL;
        if (strlen(id) == len && !memcmp(b + off, id, len))
            found = 1;
        off += len + 1;
    }
    return found;
}
int fw_dt_find_path(const fw_node_t *root, const char *path, fw_node_t *out) {
    if (!tree_of(root) || !path || path[0] != '/' || !out)
        return -EINVAL;
    fw_node_t current = *root;
    path++;
    while (*path) {
        const char *end = path;
        while (*end && *end != '/')
            end++;
        size_t len = (size_t)(end - path);
        if (!len)
            return -EINVAL;
        fw_node_t child;
        int rc = dt_child(&current, NULL, &child);
        while (!rc) {
            const char *name = dt_name(&child);
            if (name && strlen(name) == len && !memcmp(name, path, len))
                break;
            fw_node_t next;
            rc = dt_child(&current, &child, &next);
            if (!rc)
                child = next;
        }
        if (rc)
            return rc;
        current = child;
        path = *end ? end + 1 : end;
        if (*end && !*path)
            return -EINVAL;
    }
    *out = current;
    return 0;
}
int fw_dt_find_phandle(const fw_node_t *root, uint32_t phandle,
                       fw_node_t *out) {
    const fw_dt_t *t = tree_of(root);
    if (!t || !out || !phandle || phandle == 0xffffffffU)
        return -EINVAL;
    fdt_cursor_t c;
    fdt_event_t e;
    int found = 0;
    fdt_cursor_init(&c, &t->view);
    while (fdt_cursor_next(&c, &e) > 0) {
        if (e.type != FDT_NODE_BEGIN)
            continue;
        fw_node_t n = handle(t, event_id(t, &e));
        uint32_t a = 0, b = 0;
        int ra = u32(&n, "phandle", &a), rb = u32(&n, "linux,phandle", &b);
        if ((ra && ra != -ENOENT) || (rb && rb != -ENOENT) ||
            (!ra && !rb && a != b))
            return -EINVAL;
        if ((!ra && a == phandle) || (!rb && b == phandle)) {
            if (found++)
                return -EINVAL;
            *out = n;
        }
    }
    return found ? 0 : -ENOENT;
}
static int dt_reference(const fw_node_t *n, const char *key, const char *cells,
                        size_t index, fw_reference_t *out) {
    if (!tree_of(n) || !out)
        return -EINVAL;
    fw_property_t p;
    int rc = dt_property(n, key, &p);
    if (rc)
        return rc;
    if (p.size % 4)
        return -EINVAL;
    size_t off = 0, item = 0;
    int found = 0;
    while (off < p.size) {
        uint32_t ph = fdt_u32((const uint8_t *)p.data + off);
        off += 4;
        if (!ph) {
            if (!strcmp(key, "interrupts-extended"))
                return -EINVAL;
            item++;
            continue;
        }
        fw_node_t provider;
        rc = fw_dt_find_phandle(n, ph, &provider);
        if (rc)
            return rc == -ENOENT ? -ENODEV : rc;
        uint32_t count = 0;
        if (cells && (rc = u32(&provider, cells, &count)) != 0)
            return rc;
        if (count > FW_REFERENCE_MAX_ARGS)
            return -E2BIG;
        if ((size_t)count * 4 > p.size - off)
            return -EINVAL;
        if (item++ == index) {
            found = 1;
            out->node = provider;
            out->count = count;
            for (uint32_t i = 0; i < count; i++)
                out->args[i] = fdt_u32((const uint8_t *)p.data + off + 4 * i);
        }
        off += count * 4;
    }
    return found ? 0 : -ENOENT;
}
/* Interrupt nexuses use raw unit-address cells, including PCI's three-cell
 * address. They are never interpreted as a 64-bit MMIO address here.
 * DTSpec 2.4: https://devicetree-specification.readthedocs.io/en/stable/devicetree-basics.html
 */
static int irq_cells(const fw_node_t *n, uint32_t *count) {
    int rc = u32(n, "#interrupt-cells", count);
    if (rc)
        return rc;
    return !*count ? -EINVAL : *count > FW_REFERENCE_MAX_ARGS ? -E2BIG : 0;
}
static int irq_address_cells(const fw_node_t *n, uint32_t *count) {
    *count = 0;
    int rc = u32(n, "#address-cells", count);
    if (rc && rc != -ENOENT)
        return rc;
    return *count > FW_REFERENCE_MAX_ARGS ? -E2BIG : 0;
}
static int irq_parent(const fw_node_t *n, fw_node_t *out) {
    fw_node_t cur = *n;
    for (unsigned depth = 0; depth < FDT_VIEW_MAX_DEPTH; depth++) {
        fw_node_t next;
        uint32_t ph, count;
        int rc = u32(&cur, "interrupt-parent", &ph);
        if (!rc) {
            rc = fw_dt_find_phandle(n, ph, &next);
            if (rc)
                return rc == -ENOENT ? -ENODEV : rc;
        } else if (rc == -ENOENT) {
            rc = dt_parent(&cur, &next);
            if (rc)
                return rc == -ENOENT ? -ENODEV : rc;
        } else
            return rc;
        rc = irq_cells(&next, &count);
        if (!rc) {
            *out = next;
            return 0;
        }
        if (rc != -ENOENT)
            return rc;
        cur = next;
    }
    return -ELOOP;
}
static int irq_unit_address(const fw_node_t *consumer, uint32_t count,
                            uint32_t *out) {
    if (!count)
        return 0;
    fw_property_t reg;
    int rc = dt_property(consumer, "reg", &reg);
    if (rc)
        return rc == -ENOENT ? -EINVAL : rc;
    /* Validate complete reg tuples without imposing MMIO's two-cell limit. */
    fw_node_t parent;
    rc = dt_parent(consumer, &parent);
    if (rc)
        return rc;
    uint32_t ac = 2, sc = 1;
    rc = u32(&parent, "#address-cells", &ac);
    if (rc && rc != -ENOENT)
        return rc;
    rc = u32(&parent, "#size-cells", &sc);
    if (rc && rc != -ENOENT)
        return rc;
    if (ac > FW_REFERENCE_MAX_ARGS || sc > FW_REFERENCE_MAX_ARGS)
        return -E2BIG;
    size_t stride = ((size_t)ac + sc) * 4;
    if (!stride || !reg.size || reg.size % stride || count > ac)
        return -EINVAL;
    for (uint32_t i = 0; i < count; i++)
        out[i] = fdt_u32((const uint8_t *)reg.data + i * 4);
    return 0;
}
static int irq_route(const fw_node_t *consumer, const fw_reference_t *raw,
                     fw_reference_t *out) {
    fw_reference_t current = *raw;
    uint32_t address[FW_REFERENCE_MAX_ARGS], address_count = 0;
    int address_known = 0;
    uint64_t visited[FDT_VIEW_MAX_DEPTH];
    for (unsigned depth = 0; depth < FDT_VIEW_MAX_DEPTH; depth++) {
        for (unsigned i = 0; i < depth; i++)
            if (visited[i] == current.node.id)
                return -ELOOP;
        visited[depth] = current.node.id;
        int rc = dt_available(&current.node);
        if (rc <= 0)
            return rc ? rc : -ENODEV;
        uint32_t ic;
        rc = irq_cells(&current.node, &ic);
        if (rc)
            return rc;
        if (ic != current.count)
            return -EINVAL;
        fw_property_t map, flag;
        rc = dt_property(&current.node, "interrupt-map-pass-thru", &flag);
        if (!rc)
            return -EOPNOTSUPP;
        if (rc != -ENOENT)
            return rc;
        rc = dt_property(&current.node, "interrupt-map", &map);
        if (rc == -ENOENT) {
            rc = dt_property(&current.node, "interrupt-controller", &flag);
            if (!rc) {
                if (flag.size)
                    return -EINVAL;
                *out = current;
                return 0;
            }
            if (rc != -ENOENT)
                return rc;
            fw_node_t parent;
            rc = irq_parent(&current.node, &parent);
            if (rc)
                return rc;
            current.node = parent;
            continue;
        }
        if (rc)
            return rc;
        if (!map.size || map.size % 4)
            return -EINVAL;
        uint32_t ac;
        rc = irq_address_cells(&current.node, &ac);
        if (rc)
            return rc;
        if (!address_known) {
            rc = irq_unit_address(consumer, ac, address);
            if (rc)
                return rc;
            address_known = 1;
            address_count = ac;
        } else if (address_count != ac)
            return -EINVAL;
        size_t key_count = (size_t)ac + ic;
        fw_property_t mask;
        rc = dt_property(&current.node, "interrupt-map-mask", &mask);
        if (rc && rc != -ENOENT)
            return rc;
        int has_mask = !rc;
        if (has_mask && mask.size != key_count * 4)
            return -EINVAL;
        size_t offset = 0;
        unsigned matches = 0;
        fw_reference_t selected = {0};
        uint32_t selected_address[FW_REFERENCE_MAX_ARGS], selected_ac = 0;
        while (offset < map.size) {
            size_t remaining = map.size - offset;
            if (remaining < (key_count + 1) * 4)
                return -EINVAL;
            const uint8_t *entry = (const uint8_t *)map.data + offset;
            uint32_t ph = fdt_u32(entry + key_count * 4);
            fw_node_t parent;
            rc = fw_dt_find_phandle(&current.node, ph, &parent);
            if (rc)
                return rc == -ENOENT ? -ENODEV : rc;
            uint32_t pac, pic;
            rc = irq_address_cells(&parent, &pac);
            if (!rc)
                rc = irq_cells(&parent, &pic);
            if (rc)
                return rc;
            size_t entry_cells = key_count + 1 + (size_t)pac + pic;
            if (remaining < entry_cells * 4)
                return -EINVAL;
            int matched = 1;
            for (size_t i = 0; i < key_count; i++) {
                uint32_t bits =
                    has_mask ? fdt_u32((const uint8_t *)mask.data + i * 4)
                             : 0xffffffffU;
                uint32_t key = i < ac ? address[i] : current.args[i - ac];
                if ((key & bits) != (fdt_u32(entry + i * 4) & bits))
                    matched = 0;
            }
            if (matched) {
                if (matches++)
                    return -EINVAL;
                selected.node = parent;
                selected.count = pic;
                selected_ac = pac;
                const uint8_t *parent_data = entry + (key_count + 1) * 4;
                for (uint32_t i = 0; i < pac; i++)
                    selected_address[i] = fdt_u32(parent_data + i * 4);
                for (uint32_t i = 0; i < pic; i++)
                    selected.args[i] = fdt_u32(parent_data + (pac + i) * 4);
            }
            offset += entry_cells * 4;
        }
        if (!matches)
            return -ERANGE;
        current = selected;
        address_count = selected_ac;
        memcpy(address, selected_address, address_count * sizeof(*address));
    }
    return -ELOOP;
}
static int dt_irq(const fw_node_t *n, size_t index, fw_reference_t *out) {
    if (!out)
        return -EINVAL;
    fw_property_t p;
    int rc = dt_property(n, "interrupts-extended", &p);
    fw_reference_t raw;
    if (!rc) {
        rc = dt_reference(n, "interrupts-extended", "#interrupt-cells", index,
                          &raw);
        return rc ? rc : irq_route(n, &raw, out);
    }
    if (rc != -ENOENT)
        return rc;
    rc = dt_property(n, "interrupts", &p);
    if (rc)
        return rc;
    fw_node_t provider;
    rc = irq_parent(n, &provider);
    if (rc)
        return rc;
    uint32_t count;
    rc = irq_cells(&provider, &count);
    if (rc)
        return rc;
    size_t step = (size_t)count * 4;
    if (p.size % step)
        return -EINVAL;
    if (index >= p.size / step)
        return -ENOENT;
    raw.node = provider;
    raw.count = count;
    for (uint32_t i = 0; i < count; i++)
        raw.args[i] = fdt_u32((const uint8_t *)p.data + index * step + i * 4);
    return irq_route(n, &raw, out);
}
static int cells(const fw_node_t *bus, uint32_t *ac, uint32_t *sc) {
    *ac = 2;
    *sc = 1;
    int rc = u32(bus, "#address-cells", ac);
    if (rc && rc != -ENOENT)
        return rc;
    rc = u32(bus, "#size-cells", sc);
    if (rc && rc != -ENOENT)
        return rc;
    return *ac > 2 || *sc > 2 ? -EOPNOTSUPP : 0;
}
static uint64_t number(const uint8_t *b, unsigned count) {
    return count == 2 ? fdt_u64(b) : count == 1 ? fdt_u32(b) : 0;
}
static int reg_name(const fw_node_t *n, size_t index, const char **out) {
    fw_property_t p;
    int rc = dt_property(n, "reg-names", &p);
    *out = NULL;
    if (rc == -ENOENT)
        return 0;
    if (rc)
        return rc;
    const uint8_t *b = p.data;
    size_t off = 0, item = 0;
    while (off < p.size) {
        size_t length = 0;
        while (length < p.size - off && b[off + length])
            length++;
        if (!length || length == p.size - off)
            return -EINVAL;
        if (item++ == index)
            *out = (const char *)b + off;
        off += length + 1;
    }
    return 0;
}
static int dt_resource(const fw_node_t *n, size_t index, fw_resource_t *out) {
    if (!out)
        return -EINVAL;
    fw_node_t bus;
    int rc = dt_parent(n, &bus);
    if (rc)
        return rc;
    uint32_t ac, sc;
    rc = cells(&bus, &ac, &sc);
    if (rc)
        return rc;
    if (!ac || !sc)
        return -EOPNOTSUPP;
    fw_property_t p;
    rc = dt_property(n, "reg", &p);
    if (rc)
        return rc;
    size_t step = 4 * (ac + sc);
    if (p.size % step)
        return -EINVAL;
    if (index >= p.size / step)
        return -ENOENT;
    const uint8_t *b = (const uint8_t *)p.data + step * index;
    uint64_t address = number(b, ac), size = number(b + ac * 4, sc);
    if (!size || size > (~(uint64_t)0) - address)
        return -EOVERFLOW;
    const char *name;
    rc = reg_name(n, index, &name);
    if (rc)
        return rc;
    for (unsigned depth = 0; depth < FDT_VIEW_MAX_DEPTH; depth++) {
        fw_node_t parent;
        rc = dt_parent(&bus, &parent);
        if (rc == -ENOENT) {
            *out = (fw_resource_t){FW_RESOURCE_MEMORY, address, size, 0, name};
            return 0;
        }
        if (rc)
            return rc;
        uint32_t pac, psc;
        rc = cells(&parent, &pac, &psc);
        if (rc)
            return rc;
        if (!pac)
            return -EOPNOTSUPP;
        rc = dt_property(&bus, "ranges", &p);
        if (rc)
            return rc == -ENOENT ? -EOPNOTSUPP : rc;
        if (p.size) {
            step = 4 * (ac + pac + sc);
            if (!step || !sc || p.size % step)
                return -EINVAL;
            int found = 0;
            uint64_t translated = 0;
            for (size_t off = 0; off < p.size; off += step) {
                b = (const uint8_t *)p.data + off;
                uint64_t child = number(b, ac), phys = number(b + ac * 4, pac);
                uint64_t span = number(b + (ac + pac) * 4, sc);
                if (!span || span > (~(uint64_t)0) - child ||
                    span > (~(uint64_t)0) - phys)
                    return -EOVERFLOW;
                if (address >= child && address - child < span &&
                    size <= span - (address - child)) {
                    if (found++)
                        return -EINVAL;
                    translated = phys + (address - child);
                }
            }
            if (!found)
                return -ERANGE;
            address = translated;
        }
        bus = parent;
        ac = pac;
        sc = psc;
    }
    return -E2BIG;
}
int fw_dt_bootargs(const fw_dt_t *t, char *out, size_t capacity) {
    if (!out || !capacity)
        return -EINVAL;
    out[0] = 0;
    fw_node_t root, chosen;
    int rc = fw_dt_root(t, &root);
    if (!rc)
        rc = fw_dt_find_path(&root, "/chosen", &chosen);
    fw_property_t p;
    if (!rc)
        rc = dt_property(&chosen, "bootargs", &p);
    if (rc)
        return rc;
    if (!string(&p))
        return -EINVAL;
    if (p.size > capacity)
        return -ENOSPC;
    memcpy(out, p.data, p.size);
    return 0;
}
static const fw_node_ops_t dt_ops = {
    .source = "devicetree",
    .name = dt_name,
    .parent = dt_parent,
    .next_child = dt_child,
    .property = dt_property,
    .reference = dt_reference,
    .resource = dt_resource,
    .irq = dt_irq,
    .available = dt_available,
    .match_id = dt_match,
};
