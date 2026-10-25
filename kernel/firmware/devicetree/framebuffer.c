/* DT simple-framebuffer is a firmware handoff, not a display controller. */
#include "firmware/devicetree.h"
#include "core/errno.h"
#include "core/string.h"

struct pixel_format {
    const char *name;
    uint32_t bits, r, g, b, reserved;
};
static const struct pixel_format formats[] = {
    {"r5g6b5", 16, 0xf800, 0x07e0, 0x001f, 0},
    {"x1r5g5b5", 16, 0x7c00, 0x03e0, 0x001f, 0x8000},
    {"r8g8b8", 24, 0xff0000, 0xff00, 0xff, 0},
    {"x8r8g8b8", 32, 0xff0000, 0xff00, 0xff, 0xff000000},
    {"x8b8g8r8", 32, 0xff, 0xff00, 0xff0000, 0xff000000},
    {"x2r10g10b10", 32, 0x3ff00000, 0x000ffc00, 0x3ff, 0xc0000000},
};
static int u32(const fw_node_t *node, const char *key, uint32_t *out) {
    fw_property_t value;
    int rc = node->ops->property(node, key, &value);
    if (rc)
        return rc;
    if (value.size != 4)
        return -EINVAL;
    *out = fdt_u32(value.data);
    return 0;
}
static int string(const fw_node_t *node, const char *key, const char **out) {
    fw_property_t value;
    int rc = node->ops->property(node, key, &value);
    if (rc)
        return rc;
    if (!value.size || ((const uint8_t *)value.data)[value.size - 1] ||
        strlen(value.data) + 1 != value.size)
        return -EINVAL;
    *out = value.data;
    return 0;
}
int fw_dt_framebuffer_resource(const fw_node_t *node, size_t index,
                               fw_resource_t *out) {
    fw_node_t memory = *node;
    uint32_t phandle;
    int rc = u32(node, "memory-region", &phandle);
    if (!rc) {
        fw_node_t root;
        rc = fw_dt_root(node->context, &root);
        if (!rc)
            rc = fw_dt_find_phandle(&root, phandle, &memory);
        if (rc)
            return rc;
        fw_node_t reserved;
        rc = memory.ops->parent(&memory, &reserved);
        const char *parent_name = rc ? NULL : reserved.ops->name(&reserved);
        if (rc || !parent_name || strcmp(parent_name, "reserved-memory"))
            return -EINVAL;
        rc = memory.ops->available(&memory);
        if (rc <= 0)
            return rc < 0 ? rc : -EINVAL;
        fw_resource_t extra;
        rc = memory.ops->resource(&memory, 1, &extra);
        if (rc != -ENOENT)
            return rc ? rc : -EINVAL;
        return index ? -ENOENT : memory.ops->resource(&memory, 0, out);
    }
    if (rc != -ENOENT)
        return rc;
    fw_node_t parent, root, ancestor;
    rc = node->ops->parent(node, &parent);
    if (rc)
        return rc;
    const char *name = parent.ops->name(&parent);
    if (!name || strcmp(name, "chosen"))
        return -ENOSYS;
    rc = parent.ops->parent(&parent, &root);
    if (rc || root.ops->parent(&root, &ancestor) != -ENOENT)
        return -EINVAL;
    fw_property_t ranges;
    rc = parent.ops->property(&parent, "ranges", &ranges);
    if ((!rc && ranges.size) || (rc && rc != -ENOENT))
        return -EOPNOTSUPP;
    /* /chosen is not a translated hardware bus. U-Boot uses root cells
     * unless chosen explicitly describes its framebuffer address cells. */
    uint32_t ac, sc;
    rc = u32(&parent, "#address-cells", &ac);
    if (rc == -ENOENT) {
        rc = u32(&root, "#address-cells", &ac);
        if (rc == -ENOENT) {
            ac = 2;
            rc = 0;
        }
    }
    if (rc)
        return rc;
    rc = u32(&parent, "#size-cells", &sc);
    if (rc == -ENOENT) {
        rc = u32(&root, "#size-cells", &sc);
        if (rc == -ENOENT) {
            sc = 1;
            rc = 0;
        }
    }
    if (rc)
        return rc;
    if (!ac || ac > 2 || !sc || sc > 2)
        return -EOPNOTSUPP;
    fw_property_t reg;
    rc = node->ops->property(node, "reg", &reg);
    if (rc)
        return rc;
    if (reg.size != (ac + sc) * 4)
        return -EINVAL;
    if (index)
        return -ENOENT;
    const uint8_t *bytes = reg.data;
    uint64_t address = ac == 2 ? fdt_u64(bytes) : fdt_u32(bytes);
    bytes += ac * 4;
    uint64_t size = sc == 2 ? fdt_u64(bytes) : fdt_u32(bytes);
    if (!size || size > (uint64_t)-1 - address)
        return -EOVERFLOW;
    *out = (fw_resource_t){FW_RESOURCE_MEMORY, address, size, 0, "scanout"};
    return 0;
}
static int decode(const fw_node_t *node, boot_framebuffer_t *out) {
    boot_framebuffer_t fb = {.magic = BOOT_FRAMEBUFFER_MAGIC,
                             .version = BOOT_FRAMEBUFFER_VERSION};
    int rc = u32(node, "width", &fb.width);
    if (!rc)
        rc = u32(node, "height", &fb.height);
    if (!rc)
        rc = u32(node, "stride", &fb.stride);
    const char *name;
    if (!rc)
        rc = string(node, "format", &name);
    if (rc)
        return rc;
    const struct pixel_format *format = NULL;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
        if (!strcmp(name, formats[i].name))
            format = &formats[i];
    if (format) {
        fb.bits_per_pixel = format->bits;
        fb.red_mask = format->r;
        fb.green_mask = format->g;
        fb.blue_mask = format->b;
        fb.reserved_mask = format->reserved;
    }
    fw_resource_t resource, extra;
    rc = node->ops->resource(node, 0, &resource);
    if (rc)
        return rc;
    if (resource.type != FW_RESOURCE_MEMORY ||
        node->ops->resource(node, 1, &extra) != -ENOENT)
        return -EINVAL;
    fb.physical = resource.address;
    fb.aperture_bytes = resource.size;
    /* Unknown but well-formed display modes still own their physical memory.
     * Preserve that complete extent and let the serial console boot. */
    uint64_t visible = (uint64_t)fb.stride * fb.height;
    if (!fb.physical || (fb.physical & 4095) || !fb.aperture_bytes ||
        fb.aperture_bytes > 256ULL * 1024 * 1024 ||
        fb.aperture_bytes > (uint64_t)-1 - fb.physical || !fb.width ||
        !fb.height || fb.width > 16384 || fb.height > 16384 || !fb.stride ||
        !visible || visible > fb.aperture_bytes)
        return -EINVAL;
    if (!format || ((visible + 4095) & ~4095ULL) > fb.aperture_bytes)
        fb.magic =
            0; /* Safely reserved; not renderable by the current fbdev ABI. */
    else {
        rc = boot_framebuffer_validate(&fb);
        if (rc)
            return rc;
    }
    *out = fb;
    return 0;
}
static int visit(const fw_node_t *node, fw_dt_framebuffer_visitor_t callback,
                 void *opaque, unsigned depth) {
    if (depth >= FDT_VIEW_MAX_DEPTH)
        return -E2BIG;
    int rc = node->ops->available(node);
    if (rc <= 0)
        return rc;
    rc = node->ops->match_id(node, "simple-framebuffer");
    if (rc < 0)
        return rc;
    if (rc) {
        boot_framebuffer_t fb;
        rc = decode(node, &fb);
        if (!rc && callback)
            rc = callback(opaque, node, &fb);
        if (rc)
            return rc;
    }
    fw_node_t child, next;
    rc = node->ops->next_child(node, NULL, &child);
    while (!rc) {
        rc = visit(&child, callback, opaque, depth + 1);
        if (rc)
            return rc;
        rc = node->ops->next_child(node, &child, &next);
        if (!rc)
            child = next;
    }
    return rc == -ENOENT ? 0 : rc;
}
int fw_dt_framebuffers(const fw_dt_t *tree,
                       fw_dt_framebuffer_visitor_t callback, void *opaque) {
    fw_node_t root;
    int rc = fw_dt_root(tree, &root);
    return rc ? rc : visit(&root, callback, opaque, 0);
}
/* Memory adapters call this before PFA, including for unselected scanouts. */
__attribute__((weak)) int
arch_boot_framebuffer_reservation(const boot_framebuffer_t *fb, uint64_t *base,
                                  uint64_t *bytes) {
    *base = fb->physical;
    *bytes = (fb->aperture_bytes + 4095) & ~4095ULL;
    return *bytes < fb->aperture_bytes || *bytes > (uint64_t)-1 - *base
               ? -EOVERFLOW
               : 0;
}
struct selection {
    const fw_dt_t *tree;
    fw_node_t preferred, display0, only_node, preferred_node, display0_node;
    boot_framebuffer_t only, preferred_fb, display0_fb;
    unsigned count, supported_count, preferred_count, display0_count;
};
static int same(const fw_node_t *a, const fw_node_t *b) {
    return a->ops == b->ops && a->context == b->context && a->id == b->id;
}
static int matches(const fw_node_t *candidate, const fw_node_t *target) {
    if (!target->ops)
        return 0;
    if (same(candidate, target))
        return 1;
    uint32_t phandle;
    if (u32(candidate, "display", &phandle))
        return 0;
    fw_node_t root, display;
    int rc = fw_dt_root(candidate->context, &root);
    if (!rc)
        rc = fw_dt_find_phandle(&root, phandle, &display);
    return !rc && same(&display, target);
}
static int candidate(void *opaque, const fw_node_t *node,
                     const boot_framebuffer_t *fb) {
    struct selection *s = opaque;
    s->count++;
    if (fb->magic) {
        s->only = *fb;
        s->only_node = *node;
        s->supported_count++;
    }
    if (matches(node, &s->preferred)) {
        s->preferred_fb = *fb;
        s->preferred_node = *node;
        s->preferred_count++;
    }
    if (matches(node, &s->display0)) {
        s->display0_fb = *fb;
        s->display0_node = *node;
        s->display0_count++;
    }
    return 0;
}
static int resolve_path(const fw_node_t *root, const char *path,
                        fw_node_t *node) {
    char value[256];
    size_t length = 0;
    while (path[length] && path[length] != ':') {
        if (length == sizeof(value) - 1)
            return -E2BIG;
        value[length] = path[length];
        length++;
    }
    value[length] = 0;
    if (!length)
        return -EINVAL;
    if (value[0] == '/')
        return fw_dt_find_path(root, value, node);
    fw_node_t aliases;
    int rc = fw_dt_find_path(root, "/aliases", &aliases);
    const char *resolved;
    if (!rc)
        rc = string(&aliases, value, &resolved);
    return rc ? rc : fw_dt_find_path(root, resolved, node);
}
int fw_dt_framebuffer_select(const fw_dt_t *tree, boot_framebuffer_t *out,
                             enum boot_framebuffer_memory *memory,
                             fw_node_t *selected_node) {
    if (!out || !memory || !selected_node)
        return -EINVAL;
    memset(selected_node, 0, sizeof(*selected_node));
    memset(out, 0, sizeof(*out));
    *memory = BOOT_FRAMEBUFFER_DEVICE;
    struct selection selection = {.tree = tree};
    fw_node_t root, chosen, aliases;
    int rc = fw_dt_root(tree, &root);
    if (rc)
        return rc;
    rc = fw_dt_framebuffers(tree, candidate, &selection);
    if (rc || !selection.supported_count)
        return rc;
    selection.count = selection.supported_count = 0;
    const char *path;
    rc = fw_dt_find_path(&root, "/chosen", &chosen);
    if (!rc) {
        rc = string(&chosen, "stdout-path", &path);
        if (!rc) {
            rc = resolve_path(&root, path, &selection.preferred);
            if (rc)
                return rc;
        } else if (rc != -ENOENT)
            return rc;
    } else if (rc != -ENOENT)
        return rc;
    rc = fw_dt_find_path(&root, "/aliases", &aliases);
    if (!rc) {
        rc = string(&aliases, "display0", &path);
        if (!rc) {
            rc = resolve_path(&root, path, &selection.display0);
            if (rc)
                return rc;
        } else if (rc != -ENOENT)
            return rc;
    } else if (rc != -ENOENT)
        return rc;
    rc = fw_dt_framebuffers(tree, candidate, &selection);
    if (rc)
        return rc;
    if (!selection.count)
        return 0;
    boot_framebuffer_t selected;
    fw_node_t selected_handle;
    if (selection.preferred_count == 1) {
        selected = selection.preferred_fb;
        selected_handle = selection.preferred_node;
    } else if (selection.preferred_count > 1)
        return -EEXIST;
    else if (selection.display0_count == 1) {
        selected = selection.display0_fb;
        selected_handle = selection.display0_node;
    } else if (selection.display0_count > 1 || selection.supported_count > 1)
        return -EEXIST;
    else {
        selected = selection.only;
        selected_handle = selection.only_node;
    }
    if (!selected.magic)
        return 0;
    fw_dt_range_t ram[64], reserved[128];
    fw_dt_memory_t ranges = {.ram = ram,
                             .reserved = reserved,
                             .ram_capacity = 64,
                             .reserved_capacity = 128};
    rc = fw_dt_memory(tree, 0, &ranges);
    if (rc)
        return rc;
    *memory = BOOT_FRAMEBUFFER_FIRMWARE;
    uint64_t end = selected.physical + selected.aperture_bytes;
    for (size_t i = 0; i < ranges.ram_count; i++) {
        uint64_t ram_end = ram[i].base + ram[i].size;
        if (selected.physical < ram_end && ram[i].base < end) {
            if (selected.physical < ram[i].base || end > ram_end)
                return -EINVAL;
            *memory = BOOT_FRAMEBUFFER_RAM;
        }
    }
    *out = selected;
    *selected_node = selected_handle;
    return 0;
}
