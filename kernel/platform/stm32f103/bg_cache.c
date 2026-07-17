/*
 * Background-image cache — reassemble IMAGE_CHUNK data into a FAT32 file.
 * Hardware-independent (fat32lite only); see bg_cache.h.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "bg_cache.h"

#define BG_CACHE_ENOTOPEN -100

int bg_cache_begin(bg_cache_t *bc, fat32lite_fs_t *fs, const char *path) {
    if (!bc || !fs || !path)
        return FAT32LITE_EINVAL;
    bc->fs = fs;
    bc->next_offset = 0;
    bc->open = 0;
    bc->complete = 0;
    int r = fat32lite_create(fs, path, &bc->file);
    if (r != FAT32LITE_OK)
        return r;
    bc->open = 1;
    return FAT32LITE_OK;
}

int bg_cache_chunk(bg_cache_t *bc, uint32_t offset, const uint8_t *data,
                   uint16_t len, int last) {
    if (!bc || !bc->open)
        return BG_CACHE_ENOTOPEN;

    if (len > 0) {
        if (!data)
            return FAT32LITE_EINVAL;
        if (offset != bc->next_offset) { /* out-of-order: seek to place it */
            int r = fat32lite_seek(&bc->file, offset);
            if (r != FAT32LITE_OK)
                return r;
        }
        uint32_t written = 0;
        while (written < len) {
            int n = fat32lite_write(&bc->file, data + written, len - written);
            if (n < 0) {
                bc->open = 0;
                fat32lite_close(&bc->file);
                return n;
            }
            written += (uint32_t)n;
        }
        bc->next_offset = offset + len;
    }

    if (last) {
        int r = fat32lite_close(&bc->file);
        bc->open = 0;
        bc->complete = 1;
        return r;
    }
    return FAT32LITE_OK;
}

int bg_cache_abort(bg_cache_t *bc) {
    if (!bc || !bc->open)
        return FAT32LITE_OK;
    int r = fat32lite_close(&bc->file);
    bc->open = 0;
    return r;
}

#endif /* CONFIG_BOARD_STM32F103 */
