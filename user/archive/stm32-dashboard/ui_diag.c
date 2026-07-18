/*
 * Diagnostics page model (the secondary menu). Pure integer/string logic — no
 * register or display access — so the host unit test covers the formatting and
 * hit-testing. See ui_diag.h for why these panels moved off the home screen.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "ui_diag.h"

static void copy_str(char *out, unsigned cap, const char *s) {
    unsigned i = 0;
    while (s && s[i] && i + 1U < cap) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
}

/* Append an unsigned decimal; returns the new write position. */
static unsigned put_uint(char *out, unsigned cap, unsigned pos, uint32_t v) {
    char tmp[12];
    unsigned n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v && n < sizeof(tmp));
    while (n > 0 && pos + 1U < cap)
        out[pos++] = tmp[--n];
    out[pos] = '\0';
    return pos;
}

static unsigned put_str(char *out, unsigned cap, unsigned pos, const char *s) {
    while (s && *s && pos + 1U < cap)
        out[pos++] = *s++;
    out[pos] = '\0';
    return pos;
}

/* "used/total" in KiB, e.g. "12/64K". Rounds up so a live allocation never
 * reads as 0K. */
static void fmt_kib(char *out, unsigned cap, size_t used, size_t total) {
    unsigned pos = put_uint(out, cap, 0, (uint32_t)((used + 1023U) / 1024U));
    pos = put_str(out, cap, pos, "/");
    pos = put_uint(out, cap, pos, (uint32_t)(total / 1024U));
    put_str(out, cap, pos, "K");
}

static void fmt_hhmmss(char *out, unsigned cap, uint32_t seconds) {
    uint32_t h = seconds / 3600U;
    uint32_t m = (seconds / 60U) % 60U;
    uint32_t s = seconds % 60U;
    unsigned pos = 0;
    if (h < 10U)
        pos = put_str(out, cap, pos, "0");
    pos = put_uint(out, cap, pos, h);
    pos = put_str(out, cap, pos, ":");
    if (m < 10U)
        pos = put_str(out, cap, pos, "0");
    pos = put_uint(out, cap, pos, m);
    pos = put_str(out, cap, pos, ":");
    if (s < 10U)
        pos = put_str(out, cap, pos, "0");
    put_uint(out, cap, pos, s);
}

static ui_diag_row_t *add_row(ui_diag_model_t *m, const char *label,
                              ui_diag_level_t level) {
    static ui_diag_row_t sink; /* overflow guard: never write past the array */
    ui_diag_row_t *r;

    if (m->row_count >= UI_DIAG_MAX_ROWS)
        return &sink;
    r = &m->rows[m->row_count++];
    r->label = label;
    r->level = (uint8_t)level;
    r->value[0] = '\0';
    return r;
}

static void add_hit(ui_diag_model_t *m, uint16_t x, uint16_t y, uint16_t w,
                    uint16_t h, ui_diag_action_t action) {
    ui_diag_hit_t *hit;

    if (m->hit_count >= UI_DIAG_MAX_HITS)
        return;
    hit = &m->hits[m->hit_count++];
    hit->rect.x = x;
    hit->rect.y = y;
    hit->rect.w = w;
    hit->rect.h = h;
    hit->action = action;
}

void ui_diag_build(const ui_diag_state_t *st, ui_diag_model_t *m) {
    ui_diag_row_t *r;

    if (!st || !m)
        return;
    for (unsigned i = 0; i < sizeof(*m); i++)
        ((uint8_t *)m)[i] = 0;

    /* SYS: the core clock, in MHz. The xuanwu target tries HSE*9 -> 72MHz and
     * falls back to the 8MHz HSI, so this row is how you tell which happened. */
    r = add_row(m, "SYS", st->hclk_hz >= 72000000U ? UI_DIAG_OK
                                                   : UI_DIAG_WARN);
    {
        unsigned pos = put_uint(r->value, sizeof(r->value), 0,
                                st->hclk_hz / 1000000U);
        put_str(r->value, sizeof(r->value), pos, "MHZ");
    }

    r = add_row(m, "UPTIME", UI_DIAG_INFO);
    fmt_hhmmss(r->value, sizeof(r->value), st->uptime_s);

    r = add_row(m, "MEM", st->ram_total && st->ram_used * 4U > st->ram_total * 3U
                              ? UI_DIAG_WARN
                              : UI_DIAG_OK);
    fmt_kib(r->value, sizeof(r->value), st->ram_used, st->ram_total);

    r = add_row(m, "EXTMEM", st->ext_total ? UI_DIAG_OK : UI_DIAG_WARN);
    if (st->ext_total)
        fmt_kib(r->value, sizeof(r->value), st->ext_used, st->ext_total);
    else
        copy_str(r->value, sizeof(r->value), "ABSENT");

    r = add_row(m, "TF", st->sd_ready ? UI_DIAG_OK : UI_DIAG_WARN);
    copy_str(r->value, sizeof(r->value),
             !st->sd_ready ? "ABSENT" : st->sd_fat32 ? "FAT32" : "RAW");

    r = add_row(m, "BT", st->bt_connected ? UI_DIAG_OK
                         : st->bt_ready   ? UI_DIAG_INFO
                                          : UI_DIAG_WARN);
    copy_str(r->value, sizeof(r->value),
             st->bt_connected ? "LINKED"
             : st->bt_waiting ? "WAITING"
             : st->bt_ready   ? "READY"
                              : "OFF");

    r = add_row(m, "WIFI", st->wifi_socket  ? UI_DIAG_OK
                           : st->wifi_ready ? UI_DIAG_INFO
                                            : UI_DIAG_WARN);
    copy_str(r->value, sizeof(r->value),
             st->wifi_socket ? "PROXY"
             : st->wifi_joined ? "JOINED"
             : st->wifi_ready  ? "READY"
                               : "OFF");

    /* Touch and IR share a row: both are the input path, and eight rows is
     * what fits above the button strip. */
    r = add_row(m, "IN", st->touch_ready ? UI_DIAG_OK : UI_DIAG_WARN);
    {
        unsigned pos = put_str(r->value, sizeof(r->value), 0,
                               st->touch_ready ? "TOUCH+IR" : "IR");
        if (st->ir_ready) {
            pos = put_str(r->value, sizeof(r->value), pos, " ");
            put_uint(r->value, sizeof(r->value), pos, st->ir_bindings);
        }
    }

    add_hit(m, DIAG_BTN_X, DIAG_BTN_Y, DIAG_BTN_W, DIAG_BTN_H,
            UI_DIAG_ACTION_BACK);
    add_hit(m, DIAG_BTN_X + DIAG_BTN_PITCH, DIAG_BTN_Y, DIAG_BTN_W,
            DIAG_BTN_H, UI_DIAG_ACTION_CALIBRATE);
    add_hit(m, DIAG_BTN_X + 2U * DIAG_BTN_PITCH, DIAG_BTN_Y, DIAG_BTN_W,
            DIAG_BTN_H, UI_DIAG_ACTION_WIFI_SCAN);
    add_hit(m, DIAG_BTN_X + 3U * DIAG_BTN_PITCH, DIAG_BTN_Y, DIAG_BTN_W,
            DIAG_BTN_H, UI_DIAG_ACTION_BT_TEST);
}

ui_diag_action_t ui_diag_hit_test(const ui_diag_model_t *m, uint16_t x,
                                  uint16_t y) {
    if (!m)
        return UI_DIAG_ACTION_NONE;
    for (unsigned i = 0; i < m->hit_count; i++) {
        const ui_rect_t *r = &m->hits[i].rect;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return m->hits[i].action;
    }
    return UI_DIAG_ACTION_NONE;
}

#endif /* CONFIG_BOARD_STM32F103 */
