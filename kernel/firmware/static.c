#include "firmware/static.h"
#include "core/errno.h"
#include "core/string.h"
static const fw_static_node_t *get(const fw_node_t *n) {
    const fw_static_tree_t *tree = n->context;
    return tree && n->id < tree->count ? &tree->nodes[n->id] : NULL;
}
static const char *name(const fw_node_t *n) {
    const fw_static_node_t *entry = get(n);
    return entry ? entry->name : NULL;
}
static int parent(const fw_node_t *n, fw_node_t *out) {
    const fw_static_node_t *entry = get(n);
    if (!entry)
        return -EINVAL;
    return entry->parent == SIZE_MAX
               ? -ENOENT
               : fw_static_get(n->context, entry->parent, out);
}
static int next_child(const fw_node_t *n, const fw_node_t *previous,
                      fw_node_t *out) {
    const fw_static_tree_t *tree = n->context;
    size_t first = 0;
    if (previous) {
        if (previous->ops != n->ops || previous->context != tree)
            return -EINVAL;
        const fw_static_node_t *entry = get(previous);
        if (!entry || entry->parent != n->id)
            return -EINVAL;
        first = (size_t)previous->id + 1;
    }
    for (size_t i = first; i < tree->count; i++)
        if (tree->nodes[i].parent == n->id)
            return fw_static_get(tree, i, out);
    return -ENOENT;
}
static int property(const fw_node_t *n, const char *key, fw_property_t *out) {
    const fw_static_node_t *entry = get(n);
    if (!entry)
        return -EINVAL;
    for (size_t i = 0; i < entry->property_count; i++)
        if (!strcmp(entry->properties[i].name, key)) {
            *out = entry->properties[i].value;
            return 0;
        }
    return -ENOENT;
}
static int available(const fw_node_t *n) {
    const fw_static_tree_t *tree = n->context;
    size_t at = (size_t)n->id;
    for (size_t depth = 0; depth < tree->count; depth++) {
        if (at >= tree->count)
            return -EINVAL;
        if (tree->nodes[at].disabled)
            return 0;
        at = tree->nodes[at].parent;
        if (at == SIZE_MAX)
            return 1;
    }
    return -ELOOP;
}
static int resource(const fw_node_t *n, size_t index, fw_resource_t *out) {
    const fw_static_node_t *entry = get(n);
    if (!entry)
        return -EINVAL;
    if (index >= entry->resource_count)
        return -ENOENT;
    *out = entry->resources[index];
    return 0;
}
static int reference_get(const fw_node_t *n, const fw_static_reference_t *r,
                         fw_reference_t *out) {
    if (r->count > FW_REFERENCE_MAX_ARGS || (r->count && !r->args))
        return -EINVAL;
    int rc = fw_static_get(n->context, r->provider, &out->node);
    if (rc)
        return rc;
    out->count = r->count;
    memcpy(out->args, r->args, r->count * sizeof(uint32_t));
    return 0;
}
static int reference(const fw_node_t *n, const char *key, const char *cells,
                     size_t index, fw_reference_t *out) {
    (void)cells;
    const fw_static_node_t *entry = get(n);
    if (!entry)
        return -EINVAL;
    size_t found = 0;
    for (size_t i = 0; i < entry->reference_count; i++)
        if (!strcmp(entry->references[i].property, key) && found++ == index)
            return reference_get(n, &entry->references[i], out);
    return -ENOENT;
}
static int irq(const fw_node_t *n, size_t index, fw_reference_t *out) {
    const fw_static_node_t *entry = get(n);
    if (!entry)
        return -EINVAL;
    if (index >= entry->irq_count)
        return -ENOENT;
    return reference_get(n, &entry->irqs[index], out);
}
static const fw_node_ops_t ops = {.source = "static",
                                  .name = name,
                                  .parent = parent,
                                  .next_child = next_child,
                                  .property = property,
                                  .available = available,
                                  .resource = resource,
                                  .reference = reference,
                                  .irq = irq};
int fw_static_get(const fw_static_tree_t *tree, size_t index, fw_node_t *out) {
    if (!tree || !tree->nodes || !out || index >= tree->count)
        return -EINVAL;
    *out = (fw_node_t){&ops, tree, index};
    return 0;
}
int fw_static_init(const fw_static_tree_t *tree, fw_node_t *root) {
    if (!tree || !tree->nodes || !tree->count || !root)
        return -EINVAL;
    size_t roots = 0, root_index = 0;
    for (size_t i = 0; i < tree->count; i++) {
        const fw_static_node_t *n = &tree->nodes[i];
        if (!n->name || (n->property_count && !n->properties) ||
            (n->resource_count && !n->resources) ||
            (n->reference_count && !n->references) ||
            (n->irq_count && !n->irqs))
            return -EINVAL;
        for (size_t j = 0; j < n->property_count; j++)
            if (!n->properties[j].name)
                return -EINVAL;
        for (size_t j = 0; j < n->reference_count; j++)
            if (!n->references[j].property)
                return -EINVAL;
        if (n->parent == SIZE_MAX) {
            roots++;
            root_index = i;
        } else if (n->parent >= tree->count || n->parent == i)
            return -EINVAL;
        size_t at = i, depth = 0;
        while (at != SIZE_MAX && depth++ < tree->count) {
            if (at >= tree->count)
                return -EINVAL;
            at = tree->nodes[at].parent;
        }
        if (at != SIZE_MAX)
            return -ELOOP;
    }
    if (roots != 1)
        return -EINVAL;
    return fw_static_get(tree, root_index, root);
}
