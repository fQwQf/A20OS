/* Live2D RGB565 frame streaming from FAT32lite to a ui_gfx_t sink. */
#include "live2d_load.h"

#ifdef CONFIG_BOARD_STM32F103
#include "core/string.h"
#include "drivers/stm32f1/extsram.h"
#include "sdfs.h"
#endif

#define LIVE2D_IMG_BYTES ((uint32_t)LIVE2D_IMG_W * LIVE2D_IMG_H * 2U)
#define LIVE2D_PREVIEW_BYTES (LIVE2D_STATE_COUNT * LIVE2D_IMG_BYTES)
#define LIVE2D_MAX_CACHE_BYTES (12U * LIVE2D_IMG_BYTES)
#define LIVE2D_X 176
#define LIVE2D_Y 340

/*
 * The frame cache needs SDIO to read from and external SRAM to cache into.
 * QEMU's stm32vldiscovery models neither, so on that target every byte of this
 * is dead weight, so keep the hardware loader out of the 8KiB QEMU variant.
 */
/* Spelled out rather than a macro using defined() — that expansion is undefined
 * behaviour in #if and warns under -Wexpansion-to-defined. */
#if defined(CONFIG_BOARD_STM32F103) && defined(CONFIG_MCU) &&                  \
    !defined(CONFIG_STM32_QEMU)
#define LIVE2D_LOAD_HW 1
#else
#define LIVE2D_LOAD_HW 0
#endif

#if LIVE2D_LOAD_HW
static uint16_t *cached_frames;
static live2d_state_t cached_state;
static unsigned cached_count;
static unsigned load_frame;
static fat32lite_file_t load_file;
static int load_open;
static int cache_ready;
static int cache_failed;
static uint16_t *preview_frames;
static uint8_t preview_ready_mask;
static uint8_t preview_attempted_mask;

static unsigned state_frame_count(live2d_state_t state) {
    static const uint8_t counts[] = {8, 12, 10, 6};
    return (unsigned)state < LIVE2D_STATE_COUNT ? counts[state] : 0;
}

static void cache_reset(void) {
    if (load_open)
        fat32lite_close(&load_file);
    /* With previews enabled the maximum active cache is reserved once and
     * reused across states, avoiding large allocator churn and fragmentation. */
    if (cached_frames && !preview_frames) {
        stm32_extsram_free(cached_frames);
        cached_frames = NULL;
    }
    cached_count = 0;
    load_frame = 0;
    load_open = 0;
    cache_ready = 0;
}

static int open_frame(live2d_state_t state, unsigned index,
                      fat32lite_file_t *file) {
    live2d_t frame;
    char path[LIVE2D_PATH_MAX];
    fat32lite_fs_t *fs = stm32_sdfs();
    int is_dir;
    uint32_t size;

    if (!fs)
        return 0;
    frame.state = state;
    frame.frame = index;
    frame.last_advance_ms = 0;
    frame.talk_deadline_ms = 0;
    frame.dirty = 0;
    frame.stopped = 0;
    if (live2d_frame_path(&frame, path, sizeof(path)) < 0 ||
        fat32lite_stat(fs, path, &is_dir, &size) != FAT32LITE_OK || is_dir ||
        size != LIVE2D_IMG_BYTES ||
        fat32lite_open(fs, path, file) != FAT32LITE_OK)
        return 0;
    return 1;
}

static int cache_open_frame(void) {
    if (!open_frame(cached_state, load_frame, &load_file))
        return 0;
    load_open = 1;
    return 1;
}

static int read_frame(fat32lite_file_t *file, uint8_t *dest) {
    uint32_t done = 0;

    while (done < LIVE2D_IMG_BYTES) {
        int n = fat32lite_read(file, dest + done, LIVE2D_IMG_BYTES - done);
        if (n <= 0) {
            (void)fat32lite_close(file);
            return -1;
        }
        done += (uint32_t)n;
    }
    return fat32lite_close(file) == FAT32LITE_OK ? 0 : -1;
}

/* Read one complete frame directly into external SRAM. fat32lite_read() walks
 * the cluster chain once for the whole file; the old 4-row loop restarted that
 * walk 35 times and made a 40KB frame take roughly 0.7s on the board. */
static int load_one_frame(void) {
    uint8_t *dest = (uint8_t *)(cached_frames +
        load_frame * LIVE2D_IMG_W * LIVE2D_IMG_H);
    unsigned loaded_index = load_frame;

    if (read_frame(&load_file, dest) < 0) {
        load_open = 0;
        return -1;
    }
    load_open = 0;
    load_frame++;
    if (loaded_index == 0U && preview_frames) {
        memcpy(preview_frames + cached_state * LIVE2D_IMG_W * LIVE2D_IMG_H,
               dest, LIVE2D_IMG_BYTES);
        preview_ready_mask |= (uint8_t)(1U << cached_state);
        preview_attempted_mask |= (uint8_t)(1U << cached_state);
    }
    if (load_frame == cached_count)
        cache_ready = 1;
    else if (!cache_open_frame())
        return -1;
    return 0;
}

static void preview_init(void) {
    size_t required = LIVE2D_PREVIEW_BYTES + LIVE2D_MAX_CACHE_BYTES + 128U;

    if (preview_frames || stm32_extsram_available() < required)
        return;
    preview_frames = stm32_extsram_alloc(LIVE2D_PREVIEW_BYTES);
}

/* Load one missing state rest frame between frame 0 and the rest of the active
 * sequence. Within a few service passes every later state switch is SRAM-only. */
static int preview_load_one(void) {
    fat32lite_file_t file;

    if (!preview_frames)
        return 0;
    for (unsigned state = 0; state < LIVE2D_STATE_COUNT; state++) {
        uint8_t bit = (uint8_t)(1U << state);
        if (preview_attempted_mask & bit)
            continue;
        preview_attempted_mask |= bit;
        if (open_frame((live2d_state_t)state, 0, &file) &&
            read_frame(&file, (uint8_t *)(preview_frames +
                state * LIVE2D_IMG_W * LIVE2D_IMG_H)) == 0)
            preview_ready_mask |= bit;
        return 1;
    }
    return 0;
}

void live2d_load_service(const live2d_t *cat) {
    if (!cat || !stm32_extsram_ready() || !stm32_sdfs())
        return;
    if (cache_failed && cached_state == cat->state)
        return;
    if (!cached_frames || cached_state != cat->state) {
        cache_reset();
        preview_init();
        cached_state = cat->state;
        cache_failed = 0;
        cached_count = state_frame_count(cached_state);
        if (!cached_count)
            return;
        if (!cached_frames)
            cached_frames = stm32_extsram_alloc(
                preview_frames ? LIVE2D_MAX_CACHE_BYTES
                               : cached_count * LIVE2D_IMG_BYTES);
        if (!cached_frames && preview_frames) {
            stm32_extsram_free(preview_frames);
            preview_frames = NULL;
            preview_ready_mask = 0;
            preview_attempted_mask = 0;
            cached_frames = stm32_extsram_alloc(
                cached_count * LIVE2D_IMG_BYTES);
        }
        if (!cached_frames) {
            cache_reset();
            cache_failed = 1;
            return;
        }
        if (preview_frames &&
            (preview_ready_mask & (uint8_t)(1U << cached_state))) {
            memcpy(cached_frames,
                   preview_frames + cached_state * LIVE2D_IMG_W * LIVE2D_IMG_H,
                   LIVE2D_IMG_BYTES);
            load_frame = 1;
            if (load_frame == cached_count)
                cache_ready = 1;
            else if (!cache_open_frame()) {
                cache_reset();
                cache_failed = 1;
            }
            return;
        }
        if (!cache_open_frame()) {
            cache_reset();
            cache_failed = 1;
            return;
        }
        if (load_one_frame() < 0) {
            cache_reset();
            cache_failed = 1;
        }
        return;
    }
    if (cache_ready)
        return;
    if (load_frame > 0 && preview_load_one())
        return;
    if (load_one_frame() < 0) {
        cache_reset();
        cache_failed = 1;
    }
}
void live2d_load_get_stats(live2d_load_stats_t *out) {
    if (!out)
        return;
    out->cached_state = (int)cached_state;
    out->count = cached_count;
    out->loaded = load_frame;
    out->row = 0;
    out->ready = cache_ready;
    out->failed = cache_failed;
    out->bytes = cached_frames
                     ? (preview_frames ? LIVE2D_MAX_CACHE_BYTES
                                       : cached_count * LIVE2D_IMG_BYTES)
                     : 0U;
    if (preview_frames)
        out->bytes += LIVE2D_PREVIEW_BYTES;
}
#else
void live2d_load_service(const live2d_t *cat) { (void)cat; }

void live2d_load_get_stats(live2d_load_stats_t *out) {
    if (out)
        for (unsigned i = 0; i < sizeof(*out); i++)
            ((unsigned char *)out)[i] = 0;
}
#endif

int live2d_load_draw(ui_gfx_t *gfx, const live2d_t *cat) {
#if LIVE2D_LOAD_HW
    unsigned frame;

    if (!gfx || !gfx->blit || !cat || !cached_frames || load_frame == 0 ||
        cached_state != cat->state)
        return 0;
    /* Use only a fully loaded exact frame. Holding frame 0 for a short miss is
     * stable; modulo by load_frame caused visible forward/backward twitching. */
    frame = cat->frame < load_frame ? cat->frame : 0U;
    gfx->blit(gfx->ctx, LIVE2D_X, LIVE2D_Y, LIVE2D_IMG_W, LIVE2D_IMG_H,
              cached_frames + frame * LIVE2D_IMG_W * LIVE2D_IMG_H);
    return 1;
#else
    (void)gfx;
    (void)cat;
    return 0;
#endif
}
