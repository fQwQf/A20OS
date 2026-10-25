#ifndef RISCV64_FDT_MEMORY_H
#define RISCV64_FDT_MEMORY_H

#include "core/fdt_view.h"
#include "core/string.h"

/* The current RISC-V board interface exposes one allocator interval.  Keep
 * the prefix containing the kernel and discard everything after the first
 * reservation.  This intentionally trades capacity for firmware isolation. */
typedef struct {
    uint64_t base, end, kernel_base, kernel_end;
} riscv_fdt_memory_t;

static inline uint64_t riscv_fdt_cells(const uint8_t *data, unsigned count) {
    return count == 2 ? fdt_u64(data) : fdt_u32(data);
}

static inline int riscv_fdt_reserve(riscv_fdt_memory_t *m, uint64_t base,
                                   uint64_t size) {
    if (!size || size > (uint64_t)-1 - base)
        return -1;
    uint64_t end = base + size;
    if (base >= m->end || end <= m->base)
        return 0;
    if (base < m->kernel_end && end > m->kernel_base)
        return -1;
    /* Prefix pages below the kernel are already unavailable to the PFA. */
    if (base >= m->kernel_end && base < m->end)
        m->end = base & ~(uint64_t)4095;
    return m->end > m->kernel_end ? 0 : -1;
}

/* 0: valid FDT memory+reservations; 1: no DTB (board-window fallback);
 * -1: invalid blob/geometry or a firmware/kernel conflict.  On failure the
 * caller must stop booting; ignoring a malformed reservation is unsafe. */
static inline int riscv_fdt_memory_window(
    const void *blob, size_t available, uint64_t blob_phys,
    uint64_t board_base, uint64_t board_end, uint64_t kernel_base,
    uint64_t kernel_end, riscv_fdt_memory_t *out) {
    *out = (riscv_fdt_memory_t){board_base, board_end, kernel_base, kernel_end};
    if (board_base > kernel_base || kernel_base >= kernel_end ||
        kernel_end >= board_end)
        return -1;
    if (!blob)
        return 1;
    fdt_view_t view;
    if (fdt_view_open(&view, blob, available) != 0)
        return -1;
    unsigned ac = 2, sc = 1;
    int memory = 0, enabled = 1, seen = 0;
    const uint8_t *reg = 0;
    size_t reg_size = 0;
    fdt_cursor_t cursor;
    fdt_event_t e;
    fdt_cursor_init(&cursor, &view);
    while (fdt_cursor_next(&cursor, &e) > 0) {
        if (e.type == FDT_NODE_BEGIN && e.depth == 1) {
            memory = !strcmp(e.name, "memory") || !strncmp(e.name, "memory@", 7);
            enabled = 1; reg = 0; reg_size = 0;
        } else if (e.type == FDT_PROPERTY) {
            if (e.depth == 0 && (!strcmp(e.name, "#address-cells") ||
                                 !strcmp(e.name, "#size-cells"))) {
                if (e.size != 4) return -1;
                unsigned value = fdt_u32(e.data);
                if (value != 1 && value != 2) return -1;
                if (!strcmp(e.name, "#address-cells")) ac = value;
                else sc = value;
            }
            if (memory && e.depth == 1) {
                if (!strcmp(e.name, "reg")) { reg = e.data; reg_size = e.size; }
                if (!strcmp(e.name, "status"))
                    enabled = fdt_string_is(&e, "okay") || fdt_string_is(&e, "ok");
            }
        } else if (e.type == FDT_NODE_END && e.depth == 1 && memory) {
            unsigned step = 4 * (ac + sc);
            if (enabled && (!reg || !reg_size || reg_size % step)) return -1;
            for (size_t off = 0; enabled && off < reg_size; off += step) {
                uint64_t base = riscv_fdt_cells(reg + off, ac);
                uint64_t size = riscv_fdt_cells(reg + off + 4 * ac, sc);
                if (!size || size > (uint64_t)-1 - base) return -1;
                uint64_t end = base + size;
                if (base > kernel_base || end < kernel_end) continue;
                if (seen++) return -1;
                out->base = base > board_base ? base : board_base;
                out->end = end < board_end ? end : board_end;
                out->base = (out->base + 4095) & ~(uint64_t)4095;
                out->end &= ~(uint64_t)4095;
            }
            memory = 0;
        }
    }
    if (!seen || out->end <= kernel_end) return -1;

    /* DTB remains needed by platform enumeration after PFA initialization.
     * Preserve it even when the firmware forgot to reserve its own buffer. */
    if (riscv_fdt_reserve(out, blob_phys, view.size) != 0) return -1;
    uint64_t base, size;
    for (size_t i = 0; fdt_view_reserve(&view, i, &base, &size) == 1; ++i)
        if (riscv_fdt_reserve(out, base, size) != 0) return -1;

    unsigned rac = ac, rsc = sc;
    int reserved = 0;
    fdt_cursor_init(&cursor, &view);
    while (fdt_cursor_next(&cursor, &e) > 0) {
        if (e.type == FDT_NODE_BEGIN && e.depth == 1)
            reserved = !strcmp(e.name, "reserved-memory");
        else if (e.type == FDT_NODE_END && e.depth == 1)
            reserved = 0;
        else if (e.type == FDT_PROPERTY && reserved) {
            if (e.depth == 1 && (!strcmp(e.name, "#address-cells") ||
                                 !strcmp(e.name, "#size-cells"))) {
                if (e.size != 4) return -1;
                unsigned value = fdt_u32(e.data);
                if (value != 1 && value != 2) return -1;
                if (!strcmp(e.name, "#address-cells")) rac = value;
                else rsc = value;
            } else if (e.depth == 1 && !strcmp(e.name, "ranges") && e.size)
                return -1; /* translating reserved-memory is unsupported */
            else if (e.depth == 2 && !strcmp(e.name, "size"))
                return -1; /* dynamic reservations require an allocator contract */
            else if (e.depth == 2 && !strcmp(e.name, "reg")) {
                unsigned step = 4 * (rac + rsc);
                if (!e.size || e.size % step) return -1;
                for (size_t off = 0; off < e.size; off += step)
                    if (riscv_fdt_reserve(out, riscv_fdt_cells(e.data + off, rac),
                        riscv_fdt_cells(e.data + off + 4 * rac, rsc)) != 0)
                        return -1;
            }
        }
    }
    return 0;
}
#endif
