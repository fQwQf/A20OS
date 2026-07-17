/* SD-resident Unicode 16x16 bitmap font, cached in external SRAM. */
#ifdef CONFIG_BOARD_STM32F103

#include "cjk_font.h"
#include "extsram.h"
#include "sdfs.h"
#include "ui_render.h"

#define CJK_FONT_PATH "/FONT/CJK16.FNT"
#define CJK_HEADER_SIZE 8U
#define CJK_RECORD_SIZE 36U
#define CJK_MAX_GLYPHS 8192U

static uint8_t *font_records;
static unsigned font_count;

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const uint8_t *font_lookup(uint32_t codepoint) {
    unsigned lo = 0;
    unsigned hi = font_count;

    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2U;
        const uint8_t *record = font_records + mid * CJK_RECORD_SIZE;
        uint32_t current = read_le32(record);
        if (current < codepoint)
            lo = mid + 1U;
        else if (current > codepoint)
            hi = mid;
        else
            return record + 4;
    }
    return NULL;
}

void cjk_font_unload(void) {
    ui_render_set_cjk_lookup(NULL);
    if (font_records)
        stm32_extsram_free(font_records);
    font_records = NULL;
    font_count = 0;
}

int cjk_font_load(void) {
    fat32lite_fs_t *fs = stm32_sdfs();
    fat32lite_file_t file;
    uint8_t header[CJK_HEADER_SIZE];
    uint32_t count;
    uint32_t bytes;
    uint32_t done;
    int r;

    cjk_font_unload();
    if (!fs || !stm32_extsram_ready())
        return FAT32LITE_EIO;
    r = fat32lite_open(fs, CJK_FONT_PATH, &file);
    if (r != FAT32LITE_OK)
        return r;
    if (fat32lite_read(&file, header, sizeof(header)) != (int)sizeof(header) ||
        header[0] != 'C' || header[1] != 'J' || header[2] != 'K' ||
        header[3] != '1') {
        fat32lite_close(&file);
        return FAT32LITE_EINVAL;
    }
    count = read_le32(header + 4);
    if (count == 0 || count > CJK_MAX_GLYPHS) {
        fat32lite_close(&file);
        return FAT32LITE_EINVAL;
    }
    bytes = count * CJK_RECORD_SIZE;
    if (fat32lite_size(&file) != CJK_HEADER_SIZE + bytes) {
        fat32lite_close(&file);
        return FAT32LITE_EINVAL;
    }
    font_records = stm32_extsram_alloc(bytes);
    if (!font_records) {
        fat32lite_close(&file);
        return FAT32LITE_ENOSPC;
    }
    done = 0;
    while (done < bytes) {
        int n = fat32lite_read(&file, font_records + done, bytes - done);
        if (n <= 0) {
            r = n < 0 ? n : FAT32LITE_EIO;
            goto fail;
        }
        done += (uint32_t)n;
    }
    for (uint32_t i = 1; i < count; i++) {
        const uint8_t *prev = font_records + (i - 1U) * CJK_RECORD_SIZE;
        const uint8_t *current = font_records + i * CJK_RECORD_SIZE;
        if (read_le32(prev) >= read_le32(current)) {
            r = FAT32LITE_EINVAL;
            goto fail;
        }
    }
    r = fat32lite_close(&file);
    if (r != FAT32LITE_OK)
        goto fail_unclosed;
    font_count = count;
    ui_render_set_cjk_lookup(font_lookup);
    return FAT32LITE_OK;

fail:
    fat32lite_close(&file);
fail_unclosed:
    stm32_extsram_free(font_records);
    font_records = NULL;
    return r;
}

unsigned cjk_font_glyph_count(void) { return font_count; }

#endif /* CONFIG_BOARD_STM32F103 */
