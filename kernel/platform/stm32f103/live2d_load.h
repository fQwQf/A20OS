#ifndef _STM32F103_LIVE2D_LOAD_H
#define _STM32F103_LIVE2D_LOAD_H

#include "fs/fat32lite.h"
#include "live2d.h"
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
