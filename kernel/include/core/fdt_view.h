#ifndef _CORE_FDT_VIEW_H
#define _CORE_FDT_VIEW_H
#include "core/types.h"

/* Bounded, allocation-free FDT v17 reader. Caller owns a readable mapping
 * of available bytes; the reader never infers mapping size from the blob. */
#define FDT_VIEW_MAX_DEPTH 32
#define FDT_NODE_BEGIN 1U
#define FDT_NODE_END 2U
#define FDT_PROPERTY 3U

typedef struct {
    const uint8_t *blob, *structure, *strings, *reserves;
    size_t size, structure_size, strings_size, reserve_size;
} fdt_view_t;

typedef struct {
    const fdt_view_t *view;
    size_t offset;
    int depth, rooted, finished;
    uint32_t children;
} fdt_cursor_t;

typedef struct {
    unsigned type, depth;
    const char *name;
    const uint8_t *data;
    size_t size;
} fdt_event_t;

uint32_t fdt_u32(const void *bytes);
uint64_t fdt_u64(const void *bytes);
int fdt_view_open(fdt_view_t *view, const void *blob, size_t available);
void fdt_cursor_init(fdt_cursor_t *cursor, const fdt_view_t *view);
/* 1 = event, 0 = well-formed END, -1 = malformed/truncated. */
int fdt_cursor_next(fdt_cursor_t *cursor, fdt_event_t *event);
/* 1 = entry, 0 = terminator, -1 = invalid index. */
int fdt_view_reserve(const fdt_view_t *view, size_t index, uint64_t *address,
                     uint64_t *size);
int fdt_string_is(const fdt_event_t *event, const char *value);
int fdt_string_list_has(const fdt_event_t *event, const char *value);
#endif
