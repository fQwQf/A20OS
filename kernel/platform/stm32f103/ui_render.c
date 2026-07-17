/*
 * Home-screen renderer. Device-independent: only calls the ui_gfx_t sink
 * (fill_rect + blit). Runs on the FSMC LCD on-board and against a real
 * framebuffer in the host hardware simulator. See ui_render.h.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "ui_render.h"

/* Severity tones for the diagnostics rows — fixed, not theme-tinted, so a bad
 * row stays legible against every background. */
#define RGB565_OK 0x2646U   /* green  */
#define RGB565_WARN 0xFE20U /* amber  */

/* 5x7 column-major font (same style as display.c), covering ' '..'Z' plus '%'.
 * Unlisted entries are blank. */
static const uint8_t ui_font5x7[][5] = {
    [' ' - 32] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['%' - 32] = {0x23, 0x13, 0x08, 0x64, 0x62},
    ['+' - 32] = {0x08, 0x08, 0x3E, 0x08, 0x08},
    ['-' - 32] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.' - 32] = {0x00, 0x60, 0x60, 0x00, 0x00},
    ['/' - 32] = {0x60, 0x18, 0x06, 0x01, 0x00},
    ['0' - 32] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ['1' - 32] = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ['2' - 32] = {0x62, 0x51, 0x49, 0x49, 0x46},
    ['3' - 32] = {0x22, 0x49, 0x49, 0x49, 0x36},
    ['4' - 32] = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ['5' - 32] = {0x2F, 0x49, 0x49, 0x49, 0x31},
    ['6' - 32] = {0x3E, 0x49, 0x49, 0x49, 0x32},
    ['7' - 32] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8' - 32] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9' - 32] = {0x26, 0x49, 0x49, 0x49, 0x3E},
    [':' - 32] = {0x00, 0x36, 0x36, 0x00, 0x00},
    ['A' - 32] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
    ['B' - 32] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C' - 32] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D' - 32] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ['E' - 32] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['F' - 32] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['G' - 32] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
    ['H' - 32] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I' - 32] = {0x00, 0x41, 0x7F, 0x41, 0x00},
    ['J' - 32] = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ['K' - 32] = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ['L' - 32] = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ['M' - 32] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    ['N' - 32] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['O' - 32] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P' - 32] = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ['Q' - 32] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ['R' - 32] = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ['S' - 32] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T' - 32] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U' - 32] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V' - 32] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['W' - 32] = {0x7F, 0x20, 0x18, 0x20, 0x7F},
    ['X' - 32] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y' - 32] = {0x03, 0x04, 0x78, 0x04, 0x03},
    ['Z' - 32] = {0x61, 0x51, 0x49, 0x45, 0x43},
};

static ui_cjk_lookup_t cjk_lookup;

void ui_render_set_cjk_lookup(ui_cjk_lookup_t lookup) {
    cjk_lookup = lookup;
}

static void fill(const ui_gfx_t *g, int x, int y, int w, int h, uint16_t c) {
    if (w > 0 && h > 0)
        g->fill_rect(g->ctx, x, y, w, h, c);
}

static void draw_char(const ui_gfx_t *g, int x, int y, char ch, uint16_t color,
                      int scale) {
    if (ch < 32 || ch > 'Z')
        ch = ' ';
    const uint8_t *glyph = ui_font5x7[(unsigned)ch - 32U];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (glyph[col] & (1U << row))
                fill(g, x + col * scale, y + row * scale, scale, scale, color);
}

int ui_render_text(const ui_gfx_t *g, int x, int y, const char *s,
                   uint16_t color, int scale) {
    if (scale < 1)
        scale = 1;
    for (; s && *s; s++) {
        draw_char(g, x, y, *s, color, scale);
        x += 6 * scale;
    }
    return x;
}

static int utf8_decode(const char *s, uint32_t *codepoint) {
    const uint8_t *p = (const uint8_t *)s;

    if (p[0] < 0x80U) {
        *codepoint = p[0];
        return 1;
    }
    if ((p[0] & 0xE0U) == 0xC0U && p[1] != 0U &&
        (p[1] & 0xC0U) == 0x80U) {
        *codepoint = ((uint32_t)(p[0] & 0x1FU) << 6) | (p[1] & 0x3FU);
        return 2;
    }
    if ((p[0] & 0xF0U) == 0xE0U && p[1] != 0U && p[2] != 0U &&
        (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U) {
        *codepoint = ((uint32_t)(p[0] & 0x0FU) << 12) |
                     ((uint32_t)(p[1] & 0x3FU) << 6) | (p[2] & 0x3FU);
        return 3;
    }
    if ((p[0] & 0xF8U) == 0xF0U && p[1] != 0U && p[2] != 0U &&
        p[3] != 0U && (p[1] & 0xC0U) == 0x80U &&
        (p[2] & 0xC0U) == 0x80U && (p[3] & 0xC0U) == 0x80U) {
        *codepoint = ((uint32_t)(p[0] & 0x07U) << 18) |
                     ((uint32_t)(p[1] & 0x3FU) << 12) |
                     ((uint32_t)(p[2] & 0x3FU) << 6) | (p[3] & 0x3FU);
        return 4;
    }
    *codepoint = 0xFFFDU;
    return 1;
}

static void draw_cjk16(const ui_gfx_t *g, int x, int y,
                       const uint8_t *glyph, uint16_t color) {
    for (int row = 0; row < 16; row++) {
        uint16_t bits = (uint16_t)((uint16_t)glyph[row * 2] << 8) |
                        glyph[row * 2 + 1];
        for (int col = 0; col < 16; col++)
            if (bits & (uint16_t)(0x8000U >> col))
                fill(g, x + col, y + row, 1, 1, color);
    }
}

/* Speech is UTF-8. ASCII uses the 5x7 font; CJK prefers an SD-resident 16x16
 * glyph and falls back to a visible placeholder when the font is unavailable. */
static void draw_speech_line(const ui_gfx_t *g, int x, int y, const char *s,
                              uint16_t color, int scale) {
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80u) {
            draw_char(g, x, y, (char)c, color, scale);
            x += 6 * scale;
            s++;
        } else {
            uint32_t codepoint;
            int bytes = utf8_decode(s, &codepoint);
            const uint8_t *glyph = cjk_lookup ? cjk_lookup(codepoint) : NULL;
            if (glyph)
                draw_cjk16(g, x, y, glyph, color);
            else
                fill(g, x + scale, y, 4 * scale, 7 * scale, color);
            x += 18; /* 16px glyph + 2px gap, three speech budget units */
            s += bytes;
        }
    }
}

static const ui_rect_t *hit_rect(const ui_home_model_t *m, ui_action_t a) {
    for (unsigned i = 0; i < m->hit_count; i++)
        if (m->hits[i].action == a)
            return &m->hits[i].rect;
    return 0;
}

/* Card: panel fill, accent bar, label, big value + unit, optional sub-line. */
static void draw_card(const ui_gfx_t *g, const ui_home_model_t *m,
                      const ui_card_t *c) {
    const ui_rect_t *r = &c->rect;
    fill(g, r->x, r->y, r->w, r->h, m->color_panel);
    fill(g, r->x, r->y, 6, r->h, c->accent); /* left accent bar */
    ui_render_text(g, r->x + 14, r->y + 10, c->label, m->color_muted, 2);
    int vx = ui_render_text(g, r->x + 14, r->y + 34, c->value, c->accent, 5);
    ui_render_text(g, vx + 4, r->y + 48, c->unit, m->color_muted, 2);
    if (c->sub[0])
        ui_render_text(g, r->x + 14, r->y + 78, c->sub, m->color_muted, 1);
}

static void draw_button(const ui_gfx_t *g, const ui_home_model_t *m,
                        const ui_rect_t *r, const char *label, uint16_t accent,
                        int on) {
    if (!r)
        return;
    fill(g, r->x, r->y, r->w, r->h, on ? accent : m->color_panel);
    fill(g, r->x, r->y, r->w, 3, accent);
    uint16_t tc = on ? m->color_bg : m->color_text;
    int tw = 0;
    for (const char *s = label; *s; s++)
        tw += 12;
    ui_render_text(g, r->x + (r->w - tw) / 2, r->y + (r->h - 14) / 2, label, tc,
                   2);
}

/* Procedural placeholder catgirl (used when no SD sprite frame is supplied). */
static void draw_cat_placeholder(const ui_gfx_t *g, const ui_home_model_t *m,
                                 const ui_rect_t *r, int mood) {
    fill(g, r->x, r->y, r->w, r->h, m->color_panel);
    int cx = r->x + r->w / 2;
    int hy = r->y + r->h / 3;
    int hs = r->w / 3; /* head half-size */
    uint16_t fur = 0xFFDF;
    uint16_t ink = m->color_bg;
    /* ears */
    fill(g, cx - hs, hy - hs, hs / 2, hs / 2, fur);
    fill(g, cx + hs - hs / 2, hy - hs, hs / 2, hs / 2, fur);
    /* head */
    fill(g, cx - hs, hy - hs / 2, 2 * hs, 2 * hs, fur);
    /* eyes: WARN squints, HAPPY closes to happy arcs, others open */
    int eyw = hs / 3, eyh = mood == LIVE2D_HAPPY ? 3 : hs / 2;
    uint16_t eyec = (mood == LIVE2D_WARN) ? 0xF800 : ink;
    fill(g, cx - hs / 2 - eyw / 2, hy, eyw, eyh, eyec);
    fill(g, cx + hs / 2 - eyw / 2, hy, eyw, eyh, eyec);
    /* mouth */
    if (mood == LIVE2D_HAPPY)
        fill(g, cx - hs / 4, hy + hs, hs / 2, 3, ink);
    else if (mood == LIVE2D_TALK)
        fill(g, cx - 4, hy + hs, 8, 8, ink);
    else
        fill(g, cx - hs / 6, hy + hs, hs / 3, 2, ink);
    /* HAPPY blush */
    if (mood == LIVE2D_HAPPY) {
        fill(g, cx - hs, hy + hs / 2, hs / 3, hs / 4, 0xFC10);
        fill(g, cx + hs - hs / 3, hy + hs / 2, hs / 3, hs / 4, 0xFC10);
    }
}

/* Speech panel + its wrapped lines. */
static void draw_dialog(const ui_gfx_t *g, const ui_home_model_t *m) {
    const ui_rect_t *d = &m->dialog;

    if (!d->w || !d->h)
        return;
    fill(g, d->x, d->y, d->w, d->h, m->color_panel);
    fill(g, d->x, d->y, d->w, 3, m->color_accent);
    for (unsigned i = 0; i < m->speech_lines; i++)
        draw_speech_line(g, d->x + 8, d->y + 12 + (int)i * 22, m->speech[i],
                         m->color_text, 1);
}

void ui_render_home_ex(const ui_gfx_t *g, const ui_home_model_t *m,
                       const live2d_t *cat, const uint16_t *cat_frame,
                       int cat_w, int cat_h, unsigned flags) {
    if (!g || !m)
        return;

    if (!(flags & UI_RENDER_KEEP_BACKGROUND))
        fill(g, 0, 0, UI_RENDER_W, UI_RENDER_H, m->color_bg);

    /* header: clock (left), net + alert (right) */
    int header_h = m->card_count ? (int)m->cards[0].rect.y : 60;
    fill(g, 0, 0, UI_RENDER_W, header_h, m->color_panel);
    fill(g, 0, header_h - 3, UI_RENDER_W, 3, m->color_accent);
    ui_render_text(g, 14, 20, m->clock, m->color_text, 4);
    ui_render_text(g, UI_RENDER_W - 14 - 5 * 12, 14, m->net_label,
                   m->color_accent, 2);
    if (m->alert_label && m->alert_label[0]) {
        int aw = 0;
        for (const char *s = m->alert_label; *s; s++)
            aw += 12;
        fill(g, UI_RENDER_W - 14 - aw - 8, 40, aw + 8, 20, m->alert_color);
        ui_render_text(g, UI_RENDER_W - 14 - aw - 4, 44, m->alert_label,
                       m->color_bg, 2);
    }

    /* data cards */
    for (unsigned i = 0; i < m->card_count; i++)
        draw_card(g, m, &m->cards[i]);

    /* pump / theme / menu buttons */
    draw_button(g, m, hit_rect(m, UI_ACTION_PUMP_TOGGLE), "PUMP",
                m->color_accent, 0);
    draw_button(g, m, hit_rect(m, UI_ACTION_THEME_CYCLE), "THEME",
                m->color_accent, 0);
    draw_button(g, m, hit_rect(m, UI_ACTION_MENU), "MENU", m->color_muted, 0);

    /* catgirl sprite region (the first TALK hit is the sprite, not the box) */
    const ui_rect_t *cr = hit_rect(m, UI_ACTION_TALK);
    if (cr) {
        if (cat_frame && cat_w > 0 && cat_h > 0)
            g->blit(g->ctx, cr->x, cr->y, cat_w, cat_h, cat_frame);
        else
            draw_cat_placeholder(g, m, cr, cat ? (int)cat->state : m->cat_mood);
    }

    /*
     * Dialog box, left of the cat. Always painted, even with nothing to say:
     * an empty panel reads as "she has no line yet", whereas appearing and
     * vanishing with each cloud reply reads as a glitch. The text itself is
     * only ever the LLM's (ui_home wraps it; never fabricated locally).
     */
    draw_dialog(g, m);
}

void ui_render_home(const ui_gfx_t *g, const ui_home_model_t *m,
                    const live2d_t *cat, const uint16_t *cat_frame, int cat_w,
                    int cat_h) {
    ui_render_home_ex(g, m, cat, cat_frame, cat_w, cat_h, 0);
}

void ui_render_cat(const ui_gfx_t *g, const ui_home_model_t *m,
                   const live2d_t *cat) {
    const ui_rect_t *r;

    if (!g || !m)
        return;
    r = hit_rect(m, UI_ACTION_TALK);
    if (r)
        draw_cat_placeholder(g, m, r,
                             cat ? (int)cat->state : m->cat_mood);
}

void ui_render_press(const ui_gfx_t *g, const ui_home_model_t *m, uint16_t x,
                     uint16_t y) {
    if (!g || !m)
        return;
    for (unsigned i = 0; i < m->hit_count; i++) {
        const ui_rect_t *r = &m->hits[i].rect;
        if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h)
            continue;
        /* Inset outline, so the release repaint of the whole screen erases it
         * without needing to remember what was underneath. */
        fill(g, r->x, r->y, r->w, 2, m->color_accent);
        fill(g, r->x, r->y + r->h - 2, r->w, 2, m->color_accent);
        fill(g, r->x, r->y, 2, r->h, m->color_accent);
        fill(g, r->x + r->w - 2, r->y, 2, r->h, m->color_accent);
        return;
    }
}

void ui_render_diag(const ui_gfx_t *g, const ui_diag_model_t *d,
                    const ui_home_model_t *m) {
    static const char *const labels[UI_DIAG_MAX_HITS] = {
        "BACK", "CAL", "SCAN", "BT",
    };

    if (!g || !d || !m)
        return;

    fill(g, 0, 0, UI_RENDER_W, UI_RENDER_H, m->color_bg);
    fill(g, 0, 0, UI_RENDER_W, 60, m->color_panel);
    fill(g, 0, 57, UI_RENDER_W, 3, m->color_accent);
    ui_render_text(g, 14, 18, "DIAGNOSTICS", m->color_text, 3);

    for (unsigned i = 0; i < d->row_count; i++) {
        const ui_diag_row_t *row = &d->rows[i];
        int y = (int)DIAG_ROW0_Y + (int)i * (int)DIAG_ROW_PITCH;
        uint16_t tone = row->level == UI_DIAG_OK     ? RGB565_OK
                        : row->level == UI_DIAG_WARN ? RGB565_WARN
                                                     : m->color_accent;
        fill(g, DIAG_ROW_X, y, DIAG_ROW_W, DIAG_ROW_H, m->color_panel);
        fill(g, DIAG_ROW_X, y, 5, DIAG_ROW_H, tone);
        ui_render_text(g, DIAG_ROW_X + 14, y + 10, row->label, m->color_muted,
                       2);
        ui_render_text(g, DIAG_ROW_X + 140, y + 10, row->value, tone, 2);
    }

    for (unsigned i = 0; i < d->hit_count && i < UI_DIAG_MAX_HITS; i++)
        draw_button(g, m, &d->hits[i].rect, labels[i], m->color_accent, 0);
}

void ui_render_cal_target(const ui_gfx_t *g, int index, int total) {
    if (!g)
        return;
    static const int inset = UI_RENDER_CAL_INSET;
    int xs[4] = {inset, UI_RENDER_W - inset, inset, UI_RENDER_W - inset};
    int ys[4] = {inset, inset, UI_RENDER_H - inset, UI_RENDER_H - inset};
    int i = index < 0 ? 0 : index > 3 ? 3 : index;

    fill(g, 0, 0, UI_RENDER_W, UI_RENDER_H, 0x0000);
    int cx = xs[i], cy = ys[i];
    uint16_t fg = 0x07E0; /* green crosshair */
    fill(g, cx - 20, cy - 1, 41, 3, fg);
    fill(g, cx - 1, cy - 20, 3, 41, fg);
    fill(g, cx - 5, cy - 5, 10, 10, 0xF800); /* red centre dot */

    /* progress "N/T" near the middle */
    char msg[8];
    msg[0] = (char)('0' + (index + 1) % 10);
    msg[1] = '/';
    msg[2] = (char)('0' + total % 10);
    msg[3] = '\0';
    ui_render_text(g, UI_RENDER_W / 2 - 18, UI_RENDER_H / 2 - 8, msg, 0xFFFF, 3);
}

#endif /* CONFIG_BOARD_STM32F103 */
