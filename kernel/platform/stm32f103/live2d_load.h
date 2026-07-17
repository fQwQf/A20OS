#ifndef _STM32F103_LIVE2D_LOAD_H
#define _STM32F103_LIVE2D_LOAD_H

#include "fs/fat32lite.h"
#include "live2d.h"

/*
 * Loader state, for the `live2d` console command. The sprite either appears or
 * it doesn't, and "it doesn't" has several very different causes (no frames on
 * the card, external SRAM too small/fragmented for this state's frame set, the
 * state changing faster than a frame can be read). These make them separable.
 */
typedef struct live2d_load_stats {
    int cached_state;   /* the state the cache is holding (live2d_state_t)  */
    unsigned count;     /* frames that state wants                          */
    unsigned loaded;    /* frames fully read so far (draw needs >= 1)       */
    unsigned row;       /* row reached inside the frame being read          */
    int ready;          /* whole set cached                                 */
    int failed;         /* gave up on this state (alloc or read failure)    */
    uint32_t bytes;     /* external SRAM the cache asked for                */
} live2d_load_stats_t;

void live2d_load_get_stats(live2d_load_stats_t *out);
#include "ui_render.h"

#define LIVE2D_IMG_W 144
#define LIVE2D_IMG_H 140

/* Four RGB565 rows use 1152 bytes, preserving scarce internal SRAM. */
#define LIVE2D_BAND_ROWS 4

/* Returns 1 if the frame was drawn, 0 when its file is absent. */
int live2d_load_draw_fs(ui_gfx_t *gfx, fat32lite_fs_t *fs,
                         const live2d_t *cat);
/* Incrementally prefetch one animation state's frames into external SRAM.
 * Drawing becomes available after the first complete frame; later frames join
 * the animation as they finish loading. */
void live2d_load_service(const live2d_t *cat);
int live2d_load_draw(ui_gfx_t *gfx, const live2d_t *cat);

#endif /* _STM32F103_LIVE2D_LOAD_H */
