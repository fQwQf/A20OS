#include "core/fdt_view.h"
#include "core/string.h"

uint32_t fdt_u32(const void *bytes) {
    const uint8_t *p = bytes;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           p[3];
}

uint64_t fdt_u64(const void *bytes) {
    const uint8_t *p = bytes;
    return (uint64_t)fdt_u32(p) << 32 | fdt_u32(p + 4);
}

static int within(size_t offset, size_t size, size_t limit) {
    return offset <= limit && size <= limit - offset;
}

static size_t bounded_string(const uint8_t *p, size_t limit) {
    for (size_t i = 0; i < limit; i++)
        if (!p[i])
            return i;
    return limit;
}

int fdt_view_open(fdt_view_t *v, const void *blob, size_t available) {
    if (!v || !blob || available < 40)
        return -1;
    const uint8_t *p = blob;
    size_t total = fdt_u32(p + 4), st = fdt_u32(p + 8);
    size_t str = fdt_u32(p + 12), res = fdt_u32(p + 16);
    size_t strings = fdt_u32(p + 32), structure = fdt_u32(p + 36);
    if (fdt_u32(p) != 0xd00dfeedU || fdt_u32(p + 20) < 17 ||
        fdt_u32(p + 24) > 17 || total > available || total < 40 || st < 40 ||
        str < 40 || res < 40 || (st & 3) || (res & 7) ||
        !within(st, structure, total) || !within(str, strings, total) ||
        !within(res, 16, total) || !structure ||
        (st < str + strings && str < st + structure))
        return -1;
    size_t reserve_end = res;
    for (;;) {
        if (!within(reserve_end, 16, total) ||
            (reserve_end < st + structure && st < reserve_end + 16) ||
            (reserve_end < str + strings && str < reserve_end + 16))
            return -1;
        uint64_t address = fdt_u64(p + reserve_end);
        uint64_t size = fdt_u64(p + reserve_end + 8);
        reserve_end += 16;
        if (!address && !size)
            break;
        if (!size || address + size < address)
            return -1;
    }
    *v = (fdt_view_t){p,     p + st,    p + str, p + res,
                      total, structure, strings, reserve_end - res};
    /* Validate the complete structure before any caller acts on early data. */
    fdt_cursor_t c;
    fdt_event_t e;
    fdt_cursor_init(&c, v);
    int rc;
    while ((rc = fdt_cursor_next(&c, &e)) > 0)
        ;
    return rc;
}

void fdt_cursor_init(fdt_cursor_t *c, const fdt_view_t *v) {
    *c = (fdt_cursor_t){.view = v, .depth = -1};
}

int fdt_cursor_next(fdt_cursor_t *c, fdt_event_t *e) {
    const fdt_view_t *v = c->view;
    if (c->finished)
        return c->finished == 1 ? 0 : -1;
    while (within(c->offset, 4, v->structure_size)) {
        uint32_t token = fdt_u32(v->structure + c->offset);
        c->offset += 4;
        *e = (fdt_event_t){.type = token};
        if (token == 4)
            continue;
        if (token == 9 && c->depth == -1 && c->rooted) {
            c->finished = 1;
            return 0;
        }
        if (token == FDT_NODE_BEGIN) {
            if (c->depth + 1 >= FDT_VIEW_MAX_DEPTH ||
                (c->depth == -1 && c->rooted))
                break;
            size_t left = v->structure_size - c->offset;
            const uint8_t *name = v->structure + c->offset;
            size_t len = bounded_string(name, left);
            size_t padded = (len + 4) & ~(size_t)3;
            if (len == left || padded > left ||
                (c->depth == -1 ? len != 0 : len == 0))
                break;
            if (c->depth >= 0)
                c->children |= 1U << c->depth;
            c->depth++;
            c->children &= ~(1U << c->depth);
            c->rooted = 1;
            e->depth = (unsigned)c->depth;
            e->name = (const char *)name;
            c->offset += padded;
            return 1;
        }
        if (token == FDT_NODE_END && c->depth >= 0) {
            e->depth = (unsigned)c->depth--;
            return 1;
        }
        if (token == FDT_PROPERTY && c->depth >= 0 &&
            !(c->children & (1U << c->depth)) &&
            within(c->offset, 8, v->structure_size)) {
            size_t len = fdt_u32(v->structure + c->offset);
            size_t name = fdt_u32(v->structure + c->offset + 4);
            c->offset += 8;
            size_t padded = (len + 3) & ~(size_t)3;
            if (padded < len || !within(c->offset, padded, v->structure_size) ||
                name >= v->strings_size ||
                bounded_string(v->strings + name, v->strings_size - name) ==
                    v->strings_size - name)
                break;
            e->depth = (unsigned)c->depth;
            e->name = (const char *)v->strings + name;
            e->data = v->structure + c->offset;
            e->size = len;
            c->offset += padded;
            return 1;
        }
        break;
    }
    c->finished = -1;
    return -1;
}

int fdt_view_reserve(const fdt_view_t *v, size_t index, uint64_t *address,
                     uint64_t *size) {
    if (index >= v->reserve_size / 16 || !address || !size)
        return -1;
    *address = fdt_u64(v->reserves + index * 16);
    *size = fdt_u64(v->reserves + index * 16 + 8);
    return *address || *size ? 1 : 0;
}

int fdt_string_is(const fdt_event_t *e, const char *value) {
    size_t n = strlen(value) + 1;
    return e->size == n && memcmp(e->data, value, n) == 0;
}

int fdt_string_list_has(const fdt_event_t *e, const char *value) {
    size_t off = 0, n = strlen(value);
    int found = 0;
    while (off < e->size) {
        size_t len = bounded_string(e->data + off, e->size - off);
        if (len == e->size - off)
            return 0;
        if (len == n && memcmp(e->data + off, value, n) == 0)
            found = 1;
        off += len + 1;
    }
    return found;
}
