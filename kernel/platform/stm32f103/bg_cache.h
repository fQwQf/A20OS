#ifndef _STM32F103_BG_CACHE_H
#define _STM32F103_BG_CACHE_H

/*
 * Background-image cache: reassemble the RGB565 IMAGE_CHUNK stream the proxy
 * sends (§6.1 /image) into a file on the TF card, so a downloaded background is
 * cached and can later be blitted from the card (fast) instead of re-fetched.
 *
 * The network side (receiving IMAGE_CHUNK frames over wifi.c) is on-board; the
 * SD-writing half here is hardware-independent (it talks to fat32 only) and is
 * covered by the FAT32 host test against a real image.
 *
 * Usage: bg_cache_begin() to open the target file, bg_cache_chunk() for each
 * decoded chunk (from hub_proto_decode_image_chunk), until a chunk with
 * last!=0 completes it.
 */

#include "core/types.h"
#include "fat32.h"

typedef struct bg_cache {
    fat32_fs_t *fs;
    fat32_file_t file;
    uint32_t next_offset; /* expected byte offset of the next in-order chunk */
    int open;
    int complete;
} bg_cache_t;

/* Create/truncate `path` and start a new reassembly. Returns 0 or a FAT32_E*. */
int bg_cache_begin(bg_cache_t *bc, fat32_fs_t *fs, const char *path);

/*
 * Write one decoded image chunk at byte `offset`. Chunks are expected in order
 * (the proxy sends them so); any offset is honoured via seek. `last` marks the
 * final chunk and closes the file. Returns 0 on success, a negative FAT32_E* on
 * I/O error, or -100 if called on a cache that isn't open.
 */
int bg_cache_chunk(bg_cache_t *bc, uint32_t offset, const uint8_t *data,
                   uint16_t len, int last);

/* Abort an in-progress reassembly (closes the file). */
int bg_cache_abort(bg_cache_t *bc);

#endif /* _STM32F103_BG_CACHE_H */
