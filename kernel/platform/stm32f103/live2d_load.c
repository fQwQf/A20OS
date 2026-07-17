/* Live2D RGB565 frame streaming from FAT32lite to a ui_gfx_t sink. */
#include "live2d_load.h"

#ifdef CONFIG_BOARD_STM32F103
#include "extsram.h"
#include "sdfs.h"
#endif

#define LIVE2D_IMG_BYTES ((uint32_t)LIVE2D_IMG_W * LIVE2D_IMG_H * 2U)
#define LIVE2D_X 176
#define LIVE2D_Y 340

/*
 * The frame cache needs SDIO to read from and external SRAM to cache into.
 * QEMU's stm32vldiscovery models neither, so on that target every byte of this
 * is dead weight — and it is not free: the 8KiB QEMU variant links against a
 * hard SRAM-overflow assert, and the band buffer alone is 1152 B of it.
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
static uint16_t live2d_band[LIVE2D_IMG_W * LIVE2D_BAND_ROWS];
static uint16_t *cached_frames;
static live2d_state_t cached_state;
static unsigned cached_count;
static unsigned load_frame;
static unsigned load_row;
static fat32lite_file_t load_file;
static int load_open;
static int cache_ready;
static int cache_failed;

static unsigned state_frame_count(live2d_state_t state) {
    static const uint8_t counts[] = {8, 12, 10, 6};
    return state >= LIVE2D_IDLE && state <= LIVE2D_WARN ? counts[state] : 0;
}

static void cache_reset(void) {
    if (load_open)
        fat32lite_close(&load_file);
    if (cached_frames)
        stm32_extsram_free(cached_frames);
    cached_frames = NULL;
    cached_count = 0;
    load_frame = 0;
    load_row = 0;
    load_open = 0;
    cache_ready = 0;
}

static int cache_open_frame(void) {
    live2d_t frame;
    char path[LIVE2D_PATH_MAX];
    fat32lite_fs_t *fs = stm32_sdfs();
    int is_dir;
    uint32_t size;

    if (!fs)
        return 0;
    frame.state = cached_state;
    frame.frame = load_frame;
    frame.last_advance_ms = 0;
    frame.talk_deadline_ms = 0;
    frame.dirty = 0;
    if (live2d_frame_path(&frame, path, sizeof(path)) < 0 ||
        fat32lite_stat(fs, path, &is_dir, &size) != FAT32LITE_OK || is_dir ||
        size != LIVE2D_IMG_BYTES ||
        fat32lite_open(fs, path, &load_file) != FAT32LITE_OK)
        return 0;
    load_open = 1;
    return 1;
}

/* Read one LIVE2D_BAND_ROWS band into the cache. 1 = band done, 0 = frame
 * finished (and the next one opened / cache_ready set), -1 = give up. */
static int load_one_band(void) {
    uint32_t rows = LIVE2D_IMG_H - load_row;
    uint32_t bytes;
    uint32_t done = 0;

    if (rows > LIVE2D_BAND_ROWS)
        rows = LIVE2D_BAND_ROWS;
    bytes = rows * LIVE2D_IMG_W * 2U;
    while (done < bytes) {
        int n = fat32lite_read(&load_file, (uint8_t *)live2d_band + done,
                               bytes - done);
        if (n <= 0)
            return -1;
        done += (uint32_t)n;
    }
    for (uint32_t i = 0; i < rows * LIVE2D_IMG_W; i++)
        cached_frames[(load_frame * LIVE2D_IMG_H + load_row) *
                      LIVE2D_IMG_W + i] = live2d_band[i];
    load_row += rows;
    if (load_row < LIVE2D_IMG_H)
        return 1;
    if (fat32lite_close(&load_file) != FAT32LITE_OK)
        return -1;
    load_open = 0;
    load_row = 0;
    load_frame++;
    if (load_frame == cached_count)
        cache_ready = 1;
    else if (!cache_open_frame())
        return -1;
    return 0;
}

void live2d_load_service(const live2d_t *cat) {
    if (!cat || !stm32_extsram_ready() || !stm32_sdfs())
        return;
    if (cache_failed && cached_state == cat->state)
        return;
    if (!cached_frames || cached_state != cat->state) {
        cache_reset();
        cached_state = cat->state;
        cache_failed = 0;
        cached_count = state_frame_count(cached_state);
        if (!cached_count)
            return;
        cached_frames = stm32_extsram_alloc(
            cached_count * LIVE2D_IMG_BYTES);
        if (!cached_frames || !cache_open_frame()) {
            cache_reset();
            cache_failed = 1;
            return;
        }
        /*
         * Frame 0 in one burst rather than a band per service call. A band is
         * only 4 rows, so frame 0 otherwise took ~35 service passes — measured
         * at ~0.7s on the board, and for every one of those milliseconds the
         * panel shows the crude procedural placeholder instead of the catgirl.
         * Since a state change wipes the cache, that happened on *every*
         * IDLE<->TALK flip: tapping her made the sprite vanish. The whole frame
         * is ~40KB, about 20ms of SDIO at 24MHz — one short stall beats a
         * long ugly one. Frames 1..N-1 stay progressive so the animation fills
         * in without holding the loop.
         */
        while (!cache_ready && load_frame == 0) {
            int r = load_one_band();
            if (r < 0) {
                cache_reset();
                cache_failed = 1;
                return;
            }
            if (r == 0)
                break; /* frame 0 complete — the sprite can be drawn now */
        }
        return;
    }
    if (cache_ready)
        return;
    if (load_one_band() < 0) {
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
    out->row = load_row;
    out->ready = cache_ready;
    out->failed = cache_failed;
    out->bytes = cached_frames ? cached_count * LIVE2D_IMG_BYTES : 0U;
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
    frame = cat->frame < load_frame ? cat->frame : cat->frame % load_frame;
    gfx->blit(gfx->ctx, LIVE2D_X, LIVE2D_Y, LIVE2D_IMG_W, LIVE2D_IMG_H,
              cached_frames + frame * LIVE2D_IMG_W * LIVE2D_IMG_H);
    return 1;
#else
    (void)gfx;
    (void)cat;
    return 0;
#endif
}
