/*
 * Framebuffer text console.
 *
 * Renders the kernel/tty byte stream onto the firmware-preserved linear
 * scanout.  The x86_64 UEFI loader is what makes this possible on real
 * hardware: it keeps the active GOP mode, copies a firmware-independent
 * description into the boot context, and the kernel maps that aperture
 * through the boot direct map (see arch/x86_64/boot/framebuffer.c).
 *
 * The console keeps a text cell grid in RAM.  A write updates the grid and
 * presents the whole visible screen from that model, the way a TUI redraws
 * instead of scrolling a pixel buffer.  Scrolling therefore copies character
 * rows in RAM and never moves the (possibly uncached) framebuffer.  The full
 * present is also what makes a write-combining GOP mapping worthwhile: one
 * linear pass over the scanout instead of a read-modify-write scroll.
 *
 * This file deliberately contains no architecture conditionals.  The one
 * architecture-specific step -- turning fb->physical into a CPU pointer -- is
 * a weak hook so an architecture without a scanout map simply keeps the UART
 * console it always had.
 */
#include "boot/context.h"
#include "boot/framebuffer.h"
#include "core/string.h"
#include "core/types.h"
#include "drivers/char/fbcon.h"

#include "fbcon_font.h"

#define FBCON_GLYPH_W 8U
#define FBCON_GLYPH_H 8U
#define FBCON_FG_R 0xd8U
#define FBCON_FG_G 0xd8U
#define FBCON_FG_B 0xd8U

/* A 4K scanout at 2x is 240x135 cells.  Leave room for wider firmware modes
 * and a larger glyph scale without ever allocating from the cell grid path:
 * this storage is live from the first instruction, before the page allocator. */
#define FBCON_MAX_COLS 256U
#define FBCON_MAX_ROWS 160U

typedef struct {
    uint8_t *base;
    uint32_t width, height, stride, bytes_per_pixel;
    uint32_t red_mask, green_mask, blue_mask;
    uint32_t scale;
    uint32_t cols, rows;
    uint32_t cx, cy;
    uint32_t fg, bg;
    uint8_t cells[FBCON_MAX_ROWS * FBCON_MAX_COLS];
    int ready;
    int suspended;  /* a userspace compositor owns the scanout */
    int deferred;   /* grid is live but the WC alias is not ready yet */
    int esc_state; /* 0 text, 1 after ESC, 2 CSI body, 3 OSC body */
} fbcon_state_t;

static fbcon_state_t fb;

/* Console bytes produced before the scanout could be mapped.  The direct map
 * covers a sub-4 GiB GOP framebuffer from the first instruction; a scanout in
 * the iGPU VRAM BAR above 4 GiB needs the page allocator, so fbcon attaches
 * after mm_init() and replays this buffer.  Bounded so a broken boot cannot
 * grow it without limit. */
#define FBCON_EARLY_BYTES 32768U
static char fbcon_early[FBCON_EARLY_BYTES];
static size_t fbcon_early_len;

/* Architecture hook: return a CPU-addressable pointer for the boot scanout,
 * or NULL when this architecture has no such mapping. */
void *arch_fbcon_map(const boot_framebuffer_t *scanout) __attribute__((weak));
void *arch_fbcon_map(const boot_framebuffer_t *scanout) {
    (void)scanout;
    return NULL;
}

static uint32_t mask_component(uint32_t mask, uint8_t value) {
    if (!mask)
        return 0;
    unsigned shift = 0;
    uint32_t m = mask;
    while (!(m & 1U)) {
        m >>= 1;
        shift++;
    }
    unsigned width = 0;
    while (m & 1U) {
        width++;
        m >>= 1;
    }
    uint32_t scaled;
    if (width >= 8)
        scaled = (uint32_t)value << (width - 8);
    else
        scaled = (uint32_t)value >> (8 - width);
    return (scaled << shift) & mask;
}

static uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return mask_component(fb.red_mask, r) |
           mask_component(fb.green_mask, g) |
           mask_component(fb.blue_mask, b);
}

static void store_pixel(uint32_t x, uint32_t y, uint32_t color) {
    uint8_t *p = fb.base + (size_t)y * fb.stride +
                 (size_t)x * fb.bytes_per_pixel;
    switch (fb.bytes_per_pixel) {
    case 2: {
        uint16_t v = (uint16_t)color;
        memcpy(p, &v, sizeof(v));
        break;
    }
    case 3:
        p[0] = (uint8_t)color;
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16);
        break;
    default: {
        uint32_t v = color;
        memcpy(p, &v, sizeof(v));
        break;
    }
    }
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t color) {
    if (x >= fb.width || y >= fb.height)
        return;
    if (w > fb.width - x)
        w = fb.width - x;
    if (h > fb.height - y)
        h = fb.height - y;
    if (!w || !h)
        return;
    if (fb.bytes_per_pixel == 4) {
        for (uint32_t row = 0; row < h; row++) {
            uint32_t *p = (uint32_t *)(fb.base +
                           (size_t)(y + row) * fb.stride) + x;
            for (uint32_t col = 0; col < w; col++)
                p[col] = color;
        }
        return;
    }
    for (uint32_t row = 0; row < h; row++)
        for (uint32_t col = 0; col < w; col++)
            store_pixel(x + col, y + row, color);
}

static uint32_t cell_w(void) { return FBCON_GLYPH_W * fb.scale; }
static uint32_t cell_h(void) { return FBCON_GLYPH_H * fb.scale; }

static void draw_glyph(uint8_t ch, uint32_t cx, uint32_t cy) {
    uint32_t x0 = cx * cell_w();
    uint32_t y0 = cy * cell_h();
    if (!fb.base)
        return;
    /* The 32-bit lane dominates both laptop GOP modes.  Write each scaled
     * pixel directly and skip the generic per-pixel switch; 1/3/2-byte modes
     * keep the portable path. */
    if (fb.bytes_per_pixel == 4 && fb.scale <= 2) {
        for (unsigned row = 0; row < FBCON_GLYPH_H; row++) {
            uint8_t bits = fbcon_font8x8[ch & 0x7f][row];
            for (unsigned col = 0; col < FBCON_GLYPH_W; col++) {
                uint32_t color = (bits & (1U << col)) ? fb.fg : fb.bg;
                for (unsigned sy = 0; sy < fb.scale; sy++) {
                    uint32_t *p = (uint32_t *)(fb.base +
                        (size_t)(y0 + row * fb.scale + sy) * fb.stride) +
                        x0 + col * fb.scale;
                    for (unsigned sx = 0; sx < fb.scale; sx++)
                        p[sx] = color;
                }
            }
        }
        return;
    }
    for (unsigned row = 0; row < FBCON_GLYPH_H; row++) {
        uint8_t bits = fbcon_font8x8[ch & 0x7f][row];
        for (unsigned col = 0; col < FBCON_GLYPH_W; col++) {
            uint32_t color = (bits & (1U << col)) ? fb.fg : fb.bg;
            fill_rect(x0 + col * fb.scale, y0 + row * fb.scale,
                      fb.scale, fb.scale, color);
        }
    }
}

static uint8_t *cell_at(uint32_t x, uint32_t y) {
    return &fb.cells[(size_t)y * fb.cols + x];
}

static void grid_clear(void) {
    memset(fb.cells, ' ', (size_t)fb.cols * fb.rows);
}

static void grid_scroll(unsigned lines) {
    if (!fb.cols || fb.rows <= 1 || !lines)
        return;
    if (lines >= fb.rows) {
        grid_clear();
        return;
    }
    memmove(fb.cells, fb.cells + (size_t)lines * fb.cols,
            (size_t)(fb.rows - lines) * fb.cols);
    memset(fb.cells + (size_t)(fb.rows - lines) * fb.cols, ' ',
           (size_t)lines * fb.cols);
}

/* Present the entire text grid.  This is the TUI refresh: every visible cell
 * is redrawn from the RAM model, so no framebuffer read or scroll copy is
 * needed and a partial update can never leave stale pixels behind. */
static void fbcon_present(void) {
    if (!fb.ready || fb.deferred || fb.suspended || !fb.base)
        return;
    for (uint32_t y = 0; y < fb.rows; y++)
        for (uint32_t x = 0; x < fb.cols; x++)
            draw_glyph(*cell_at(x, y), x, y);
    uint32_t text_w = fb.cols * cell_w();
    uint32_t text_h = fb.rows * cell_h();
    if (text_w < fb.width)
        fill_rect(text_w, 0, fb.width - text_w, fb.height, fb.bg);
    if (text_h < fb.height)
        fill_rect(0, text_h, fb.width, fb.height - text_h, fb.bg);
}

static void newline(void) {
    fb.cx = 0;
    if (fb.rows <= 1) {
        fb.cy = 0;
        grid_clear();
        return;
    }
    if (fb.cy + 1 >= fb.rows) {
        grid_scroll(1);
        fb.cy = fb.rows - 1;
    } else {
        fb.cy++;
    }
}

static void put_char(char c) {
    if (!fb.ready)
        return;
    switch (c) {
    case '\r':
        fb.cx = 0;
        return;
    case '\n':
        newline();
        return;
    case '\b':
        if (!fb.cx && fb.cy) {
            fb.cy--;
            fb.cx = fb.cols - 1;
        } else if (fb.cx) {
            fb.cx--;
        }
        *cell_at(fb.cx, fb.cy) = ' ';
        return;
    case '\t': {
        uint32_t next = (fb.cx + 8U) & ~7U;
        if (next >= fb.cols) {
            newline();
            return;
        }
        while (fb.cx < next)
            *cell_at(fb.cx++, fb.cy) = ' ';
        return;
    }
    default:
        break;
    }
    if ((unsigned char)c < 0x20)
        return;
    *cell_at(fb.cx, fb.cy) = (uint8_t)c;
    if (++fb.cx >= fb.cols)
        newline();
}

void fbcon_putc(char c) {
    if (!fb.ready)
        return;
    switch (fb.esc_state) {
    case 1: /* after ESC */
        if (c == '[') {
            fb.esc_state = 2;
            return;
        }
        if (c == ']') {
            fb.esc_state = 3;
            return;
        }
        fb.esc_state = 0;
        if (c == 0x1b)
            fb.esc_state = 1;
        else if (c == '\n' || c == '\r' || c == '\b' || c == '\t')
            put_char(c);
        return;
    case 2: /* CSI: final byte is 0x40..0x7e */
        if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7e)
            fb.esc_state = 0;
        return;
    case 3: /* OSC: terminated by BEL, ESC or CAN */
        if (c == 0x07 || c == 0x18)
            fb.esc_state = 0;
        else if (c == 0x1b)
            fb.esc_state = 1;
        return;
    default:
        break;
    }
    if (c == 0x1b) {
        fb.esc_state = 1;
        return;
    }
    put_char(c);
}

void fbcon_write(const char *data, size_t length) {
    if (!fb.ready)
        return;
    for (size_t i = 0; i < length; i++)
        fbcon_putc(data[i]);
    fbcon_present();
}

void fbcon_capture(const char *data, size_t length) {
    if (fb.ready || fb.suspended || !data || !length)
        return;
    for (size_t i = 0; i < length; i++) {
        if (fbcon_early_len == FBCON_EARLY_BYTES) {
            memmove(fbcon_early, fbcon_early + 1, FBCON_EARLY_BYTES - 1);
            fbcon_early_len--;
        }
        fbcon_early[fbcon_early_len++] = data[i];
    }
}

int fbcon_active(void) {
    return fb.ready;
}

void fbcon_init(void) {
    const boot_context_t *boot = boot_context_get();
    if (!boot || fb.ready || fb.suspended)
        return;
    const boot_framebuffer_t *scanout = &boot->framebuffer;
    if (!scanout->magic || boot_framebuffer_validate(scanout) < 0)
        return;
    void *base = arch_fbcon_map(scanout);
    if (!base)
        return;

    memset(&fb, 0, sizeof(fb));
    fb.base = base;
    fb.width = scanout->width;
    fb.height = scanout->height;
    fb.stride = scanout->stride;
    fb.bytes_per_pixel = scanout->bits_per_pixel / 8;
    fb.red_mask = scanout->red_mask;
    fb.green_mask = scanout->green_mask;
    fb.blue_mask = scanout->blue_mask;

    /* Two-pixel cells are readable at 1080p and above; 800x600 firmware modes
     * stay at 1:1 so the screen still holds a useful number of columns. */
    fb.scale = fb.width >= 1024 && fb.height >= 600 ? 2U : 1U;
    fb.cols = fb.width / cell_w();
    fb.rows = fb.height / cell_h();
    if (!fb.cols || !fb.rows) {
        fb.scale = 1;
        fb.cols = fb.width / FBCON_GLYPH_W;
        fb.rows = fb.height / FBCON_GLYPH_H;
    }
    /* Keep the fixed grid within its boot-time storage.  A larger glyph scale
     * is the least surprising way to fit an 8K firmware mode. */
    while (fb.cols > FBCON_MAX_COLS || fb.rows > FBCON_MAX_ROWS) {
        fb.scale++;
        fb.cols = fb.width / cell_w();
        fb.rows = fb.height / cell_h();
        if (!fb.cols || !fb.rows || fb.scale > 8)
            return;
    }
    if (!fb.cols || !fb.rows)
        return;
    fb.fg = pack_rgb(FBCON_FG_R, FBCON_FG_G, FBCON_FG_B);
    fb.bg = pack_rgb(0, 0, 0);
    grid_clear();
    fb.cx = 0;
    fb.cy = 0;
    fb.ready = 1;
    /* The early direct-map alias can be uncached.  Keep the grid live but do
     * not paint it until fbcon_attach_late() has the write-combining alias;
     * this avoids one full-screen slow copy per early boot line. */
    fb.deferred = 1;
}

void fbcon_attach_late(void) {
    if (fb.suspended)
        return;
    if (fb.ready) {
        /* Early boot may have used the direct (possibly UC) alias.  Try the
         * write-combining mapping now that the page allocator can build it;
         * both aliases point at the same physical scanout. */
        const boot_context_t *boot = boot_context_get();
        if (boot && boot->framebuffer.magic) {
            void *base = arch_fbcon_map(&boot->framebuffer);
            if (base)
                fb.base = base;
        }
        fb.deferred = 0;
        fbcon_present();
        return;
    }
    fbcon_init();
    if (!fb.ready)
        return;
    /* The scanout only became mappable once the page allocator existed.
     * Replay what the kernel already wrote to the console. */
    fb.deferred = 0;
    fbcon_write(fbcon_early, fbcon_early_len);
    fbcon_early_len = 0;
}

/* A userspace compositor now owns the scanout (first DRM present).  Stop
 * drawing kernel/tty text over it.  The early buffer must not restart either:
 * console output after this point belongs to the serial log, if any. */
void fbcon_suspend(void) {
    fb.ready = 0;
    fb.suspended = 1;
}

/* Take the scanout back when its desktop owner exits, or on panic.  A stale
 * desktop frame hides shutdown progress on machines with no serial console. */
void fbcon_resume(void) {
    if (fb.suspended) {
        fb.suspended = 0;
        fbcon_init();
    }
    if (!fb.ready)
        return;
    /* Restored output must not be deferred: show the grid immediately even if
     * the WC remap never ran. */
    fb.deferred = 0;
    fbcon_present();
}
