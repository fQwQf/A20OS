#ifndef _STM32F103_UI_RENDER_H
#define _STM32F103_UI_RENDER_H

/*
 * Home-screen renderer (device-independent draw layer).
 *
 * ui_render_home() walks a ui_home_model_t (see ui_home.h) plus a Live2D state
 * and paints it through a tiny gfx sink (fill_rect + blit of RGB565). The sink
 * is a vtable so the SAME renderer runs two ways:
 *   - on the STM32: fill_rect/blit map to the FSMC LCD (display.c windowed
 *     writes) — the on-board wiring step;
 *   - in the host hardware simulator (tools/hub-sim): they write into a real
 *     320x480 RGB565 framebuffer that is dumped to a PNG, so the exact pixels
 *     the panel would show are viewable off-target.
 *
 * The renderer only ever calls the two sink ops, so it is hardware-independent
 * and produces identical geometry on both backends. Text uses a built-in 5x7
 * ASCII font (the same glyph style as display.c); CJK speech uses an optional
 * SD-resident 16x16 font and falls back to placeholder blocks when unavailable.
 */

#include "core/types.h"
#include "ui_home.h"
#include "ui_diag.h"
#include "live2d.h"

/* RGB565 pixel sink. All coordinates are in panel pixels; the backend is
 * responsible for clipping to the physical panel. */
typedef struct ui_gfx {
    void *ctx;
    void (*fill_rect)(void *ctx, int x, int y, int w, int h, uint16_t color);
    void (*blit)(void *ctx, int x, int y, int w, int h, const uint16_t *px);
} ui_gfx_t;

/* Optional 16x16 Unicode bitmap provider. Each glyph is 32 row-major bytes,
 * two bytes per row, most-significant bit first. NULL keeps placeholders. */
typedef const uint8_t *(*ui_cjk_lookup_t)(uint32_t codepoint);
void ui_render_set_cjk_lookup(ui_cjk_lookup_t lookup);

/* Panel geometry the renderer targets. */
#define UI_RENDER_W 320
#define UI_RENDER_H 480
#define UI_RENDER_KEEP_BACKGROUND 0x01U
#define UI_RENDER_CAL_INSET 32

/*
 * Paint the whole home screen. cat_frame (optional) is the current Live2D
 * sprite as row-major RGB565 of size cat_w x cat_h; when NULL a procedural
 * placeholder catgirl is drawn so the layout is still complete. cat supplies
 * the mood (for the placeholder's expression) and may be NULL.
 */
void ui_render_home(const ui_gfx_t *g, const ui_home_model_t *m,
                     const live2d_t *cat, const uint16_t *cat_frame, int cat_w,
                     int cat_h);

/* Extended entry point. UI_RENDER_KEEP_BACKGROUND preserves pixels already
 * painted by the caller instead of filling the procedural theme background. */
void ui_render_home_ex(const ui_gfx_t *g, const ui_home_model_t *m,
                       const live2d_t *cat, const uint16_t *cat_frame,
                       int cat_w, int cat_h, unsigned flags);

/* Redraw only the cat sprite region using the procedural fallback. */
void ui_render_cat(const ui_gfx_t *g, const ui_home_model_t *m,
                   const live2d_t *cat);

/*
 * Outline the hit region containing (x,y) in the accent colour — the press
 * feedback drawn on touch-down. Draws nothing if the point hits no region.
 * The outline is erased by the next full repaint, which every real action
 * triggers on release.
 */
void ui_render_press(const ui_gfx_t *g, const ui_home_model_t *m, uint16_t x,
                     uint16_t y);

/*
 * Paint the diagnostics page (the secondary menu). `m` supplies the live theme
 * palette so the page matches the home screen; only its colour fields are read.
 */
void ui_render_diag(const ui_gfx_t *g, const ui_diag_model_t *d,
                    const ui_home_model_t *m);

/*
 * Paint one four-corner touch-calibration target (a crosshair) on a blank
 * field — the sampling UI that collects raw touches for touch_cal_solve().
 * index in [0,total) selects which corner; the rest of the screen is cleared.
 */
void ui_render_cal_target(const ui_gfx_t *g, int index, int total);

/* Draw a NUL-terminated string in the 5x7 font at (x,y), scaled. Exposed so
 * the simulator can annotate frames; returns the x just past the text. */
int ui_render_text(const ui_gfx_t *g, int x, int y, const char *s,
                   uint16_t color, int scale);

#endif /* _STM32F103_UI_RENDER_H */
