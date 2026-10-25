#include "firmware/node.h"
#include "core/errno.h"
#include "core/string.h"

int fw_node_valid(const fw_node_t *n) {
    return n && n->ops;
}
int fw_node_equal(const fw_node_t *a, const fw_node_t *b) {
    return fw_node_valid(a) && fw_node_valid(b) && a->ops == b->ops &&
           a->context == b->context && a->id == b->id;
}
const char *fw_node_name(const fw_node_t *n) {
    return fw_node_valid(n) && n->ops->name ? n->ops->name(n) : NULL;
}
const char *fw_node_source(const fw_node_t *n) {
    return fw_node_valid(n) ? n->ops->source : NULL;
}
int fw_node_parent(const fw_node_t *n, fw_node_t *out) {
    if (!fw_node_valid(n) || !out)
        return -EINVAL;
    fw_node_t copy = *n;
    n = &copy;
    memset(out, 0, sizeof(*out));
    return n->ops->parent ? n->ops->parent(n, out) : -ENOENT;
}
int fw_node_next_child(const fw_node_t *n, const fw_node_t *prev,
                       fw_node_t *out) {
    if (!fw_node_valid(n) || !out || (prev && !fw_node_valid(prev)))
        return -EINVAL;
    fw_node_t current = *n;
    n = &current;
    /* out may alias previous; providers receive an independent value. */
    fw_node_t copy;
    if (prev) {
        copy = *prev;
        prev = &copy;
    }
    memset(out, 0, sizeof(*out));
    return n->ops->next_child ? n->ops->next_child(n, prev, out) : -ENOENT;
}
int fw_node_property(const fw_node_t *n, const char *name, fw_property_t *out) {
    if (!fw_node_valid(n) || !name || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    int rc = n->ops->property ? n->ops->property(n, name, out) : -ENOENT;
    if (!rc && ((!out->data && out->size) ||
                (unsigned)out->order > FW_ORDER_LITTLE_ENDIAN))
        return -EINVAL;
    return rc;
}
int fw_node_has_property(const fw_node_t *n, const char *name) {
    fw_property_t p;
    int rc = fw_node_property(n, name, &p);
    return !rc ? 1 : rc == -ENOENT ? 0 : rc;
}
static uint64_t integer(const uint8_t *p, size_t width,
                        enum fw_property_order order) {
    uint64_t value = 0;
    if (order == FW_ORDER_NATIVE) {
        if (width == 4) {
            uint32_t v;
            memcpy(&v, p, 4);
            return v;
        }
        memcpy(&value, p, 8);
        return value;
    }
    for (size_t i = 0; i < width; i++) {
        size_t at = order == FW_ORDER_BIG_ENDIAN ? i : width - i - 1;
        value = (value << 8) | p[at];
    }
    return value;
}
static int read_array(const fw_node_t *n, const char *name, void *out,
                      size_t count, size_t width) {
    if (!out || !count || count > SIZE_MAX / width)
        return -EINVAL;
    fw_property_t p;
    int rc = fw_node_property(n, name, &p);
    if (rc)
        return rc;
    /* Reject partial arrays and truncation rather than hiding binding errors. */
    if (p.size % width || p.size < count * width)
        return -EINVAL;
    for (size_t i = 0; i < count; i++) {
        uint64_t value =
            integer((const uint8_t *)p.data + i * width, width, p.order);
        if (width == 4)
            ((uint32_t *)out)[i] = (uint32_t)value;
        else
            ((uint64_t *)out)[i] = value;
    }
    return 0;
}
int fw_node_read_u32_array(const fw_node_t *n, const char *name, uint32_t *out,
                           size_t count) {
    return read_array(n, name, out, count, 4);
}
int fw_node_read_u64_array(const fw_node_t *n, const char *name, uint64_t *out,
                           size_t count) {
    return read_array(n, name, out, count, 8);
}
int fw_node_read_u32(const fw_node_t *n, const char *name, uint32_t *out) {
    return fw_node_read_u32_array(n, name, out, 1);
}
int fw_node_read_u64(const fw_node_t *n, const char *name, uint64_t *out) {
    return fw_node_read_u64_array(n, name, out, 1);
}
int fw_node_read_string(const fw_node_t *n, const char *name, size_t index,
                        const char **out) {
    if (!out)
        return -EINVAL;
    *out = NULL;
    fw_property_t p;
    int rc = fw_node_property(n, name, &p);
    if (rc)
        return rc;
    const char *s = p.data;
    size_t at = 0, found = 0;
    while (at < p.size) {
        const char *end = memchr(s + at, 0, p.size - at);
        if (!end)
            return -EINVAL;
        if (found++ == index)
            *out = s + at;
        at = (size_t)(end - s) + 1;
    }
    return *out ? 0 : -ENOENT;
}
int fw_node_reference(const fw_node_t *n, const char *name, const char *cells,
                      size_t index, fw_reference_t *out) {
    if (!fw_node_valid(n) || !name || !cells || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    int rc = n->ops->reference ? n->ops->reference(n, name, cells, index, out)
                               : -ENOENT;
    if (!rc &&
        (!fw_node_valid(&out->node) || out->count > FW_REFERENCE_MAX_ARGS))
        return -EINVAL;
    return rc;
}
int fw_node_resource(const fw_node_t *n, size_t index, fw_resource_t *out) {
    if (!fw_node_valid(n) || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    int rc = n->ops->resource ? n->ops->resource(n, index, out) : -ENOENT;
    if (!rc && (!out->size || out->address > (uint64_t)-1 - (out->size - 1) ||
                (unsigned)out->type > FW_RESOURCE_IO))
        return -EINVAL;
    return rc;
}
int fw_node_irq(const fw_node_t *n, size_t index, fw_reference_t *out) {
    if (!fw_node_valid(n) || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    int rc = n->ops->irq ? n->ops->irq(n, index, out) : -ENOENT;
    if (!rc &&
        (!fw_node_valid(&out->node) || out->count > FW_REFERENCE_MAX_ARGS))
        return -EINVAL;
    return rc;
}
int fw_node_available(const fw_node_t *n) {
    if (!fw_node_valid(n))
        return -EINVAL;
    return n->ops->available ? n->ops->available(n) : 1;
}
int fw_node_children_available(const fw_node_t *n) {
    if (!fw_node_valid(n))
        return -EINVAL;
    return n->ops->children_available ? n->ops->children_available(n)
                                      : fw_node_available(n);
}
int fw_node_match_id(const fw_node_t *n, const char *id) {
    if (!fw_node_valid(n) || !id || !id[0])
        return 0;
    if (n->ops->match_id)
        return n->ops->match_id(n, id);
    const char *value;
    for (size_t i = 0; fw_node_read_string(n, "compatible", i, &value) == 0;
         i++)
        if (!strcmp(value, id))
            return 1;
    for (size_t i = 0; fw_node_read_string(n, "hardware-id", i, &value) == 0;
         i++)
        if (!strcmp(value, id))
            return 1;
    return 0;
}
