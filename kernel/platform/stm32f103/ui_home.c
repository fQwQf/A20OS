/*
 * Home-screen UI model. Pure integer/string logic (no register or display
 * access) so it runs under the host unit test and QEMU self-test. The target
 * renderer walks this model and calls the display.c primitives.
 * See ui_home.h for the contract.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "ui_home.h"
#include "env_rule.h" /* env_theme_t / env_alert_t enum values */

#define RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8U) << 8) | (((g) & 0xFCU) << 3) | ((b) >> 3))

/* --- 320x480 layout -------------------------------------------------------
 * header | 2x2 data cards | pump + theme buttons | cat sprite + speech bubble
 */
#define SCREEN_W 320U
#define CARD_MARGIN 12U
#define CARD_GAP 12U
#define CARD_W 142U
#define CARD_H 95U
#define CARD_ROW0_Y 72U
#define CARD_ROW1_Y 179U
#define CARD_COL0_X 12U
#define CARD_COL1_X 166U
#define BTN_Y 286U
#define BTN_H 46U
#define CAT_X 176U
#define CAT_Y 340U
#define CAT_W 144U
#define CAT_H 140U

/* Signed decimal into a small buffer; returns the string (always NUL term). */
static void fmt_int(int value, char *out, unsigned cap) {
    char tmp[12];
    unsigned n = 0;
    unsigned neg = value < 0;
    unsigned v = neg ? (unsigned)(-value) : (unsigned)value;
    do {
        tmp[n++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v && n < sizeof(tmp));
    unsigned pos = 0;
    if (neg && pos + 1U < cap)
        out[pos++] = '-';
    while (n > 0 && pos + 1U < cap)
        out[pos++] = tmp[--n];
    out[pos] = '\0';
}

static void fmt_clock(uint8_t hour, uint8_t minute, char out[6]) {
    if (hour > 23)
        hour = 23;
    if (minute > 59)
        minute = 59;
    out[0] = (char)('0' + hour / 10U);
    out[1] = (char)('0' + hour % 10U);
    out[2] = ':';
    out[3] = (char)('0' + minute / 10U);
    out[4] = (char)('0' + minute % 10U);
    out[5] = '\0';
}

static void resolve_palette(const ui_home_state_t *st, ui_home_model_t *m) {
    switch (st->theme) {
    case ENV_THEME_NIGHT:
        m->color_bg = RGB565(6, 10, 22);
        m->color_panel = RGB565(18, 24, 44);
        m->color_accent = RGB565(126, 132, 224);
        break;
    case ENV_THEME_COZY:
        m->color_bg = RGB565(26, 16, 12);
        m->color_panel = RGB565(46, 30, 22);
        m->color_accent = RGB565(240, 160, 72);
        break;
    case ENV_THEME_DAY:
    default:
        m->color_bg = RGB565(12, 20, 34);
        m->color_panel = RGB565(24, 38, 58);
        m->color_accent = RGB565(35, 202, 222);
        break;
    }
    m->color_text = RGB565(245, 248, 252);
    m->color_muted = RGB565(150, 168, 190);
}

/* Alert colour + label; also nudges the accent when something is wrong. */
static void resolve_alert(const ui_home_state_t *st, ui_home_model_t *m) {
    switch (st->alert) {
    case ENV_ALERT_HEAT:
        m->alert_label = "HEAT";
        m->alert_color = RGB565(230, 52, 62);
        break;
    case ENV_ALERT_DRY:
        m->alert_label = "DRY";
        m->alert_color = RGB565(246, 196, 57);
        break;
    case ENV_ALERT_SENSOR:
        m->alert_label = "SENSOR";
        m->alert_color = RGB565(238, 120, 40);
        break;
    case ENV_ALERT_NONE:
    default:
        m->alert_label = "";
        m->alert_color = m->color_muted;
        break;
    }
}

static uint16_t temp_accent(const ui_home_model_t *m, int8_t t) {
    if (t >= 32)
        return RGB565(230, 52, 62);
    if (t >= 28)
        return RGB565(246, 196, 57);
    return m->color_accent;
}

static uint16_t humidity_accent(uint8_t h) {
    if (h < 40U)
        return RGB565(246, 196, 57); /* dry */
    if (h > 70U)
        return RGB565(35, 160, 235); /* damp */
    return RGB565(44, 190, 112);     /* comfortable */
}

static void set_card(ui_card_t *c, const char *label, int value,
                     const char *unit, uint16_t accent, uint16_t x,
                     uint16_t y) {
    c->label = label;
    fmt_int(value, c->value, sizeof(c->value));
    c->unit = unit;
    c->accent = accent;
    c->rect.x = x;
    c->rect.y = y;
    c->rect.w = CARD_W;
    c->rect.h = CARD_H;
}

static void add_hit(ui_home_model_t *m, uint16_t x, uint16_t y, uint16_t w,
                    uint16_t h, ui_action_t action) {
    if (m->hit_count >= UI_HOME_MAX_HITS)
        return;
    ui_hit_region_t *r = &m->hits[m->hit_count++];
    r->rect.x = x;
    r->rect.y = y;
    r->rect.w = w;
    r->rect.h = h;
    r->action = action;
}

static void wrap_speech(const char *s, ui_home_model_t *m) {
    for (unsigned i = 0; i < UI_HOME_SPEECH_LINES; i++)
        m->speech[i][0] = '\0';
    m->speech_lines = 0;
    if (!s)
        return;

    unsigned li = 0, col = 0, pos = 0;
    while (*s && li < UI_HOME_SPEECH_LINES) {
        unsigned char c = (unsigned char)*s;
        unsigned bytes = 1, cols = 1;
        if (c >= 0xF0u) {
            bytes = 4;
            cols = 2;
        } else if (c >= 0xE0u) {
            bytes = 3;
            cols = 2;
        } else if (c >= 0xC0u) {
            bytes = 2;
            cols = 2;
        }
        if (c == '\n') {
            m->speech[li][pos] = '\0';
            li++;
            col = 0;
            pos = 0;
            s++;
            continue;
        }
        if (col + cols > UI_HOME_SPEECH_COLS) {
            m->speech[li][pos] = '\0';
            li++;
            col = 0;
            pos = 0;
            if (li >= UI_HOME_SPEECH_LINES)
                break;
        }
        if (c == ' ' && col == 0) { /* drop leading space after a wrap */
            s++;
            continue;
        }
        for (unsigned b = 0; b < bytes && *s; b++) {
            if (pos + 1U < sizeof(m->speech[li]))
                m->speech[li][pos++] = *s;
            s++;
        }
        col += cols;
    }
    if (li < UI_HOME_SPEECH_LINES)
        m->speech[li][pos] = '\0';
    /* Count populated lines. */
    m->speech_lines = 0;
    for (unsigned i = 0; i < UI_HOME_SPEECH_LINES; i++)
        if (m->speech[i][0] != '\0')
            m->speech_lines = i + 1U;
}

void ui_home_build(const ui_home_state_t *st, ui_home_model_t *m) {
    if (!st || !m)
        return;

    for (unsigned i = 0; i < sizeof(*m); i++)
        ((uint8_t *)m)[i] = 0;

    resolve_palette(st, m);
    resolve_alert(st, m);

    fmt_clock(st->hour, st->minute, m->clock);
    m->net_label = st->net_cloud ? "CLOUD" : "LOCAL";
    m->cat_mood = st->mood > 3 ? 3 : st->mood;

    /* 2x2 data cards. */
    set_card(&m->cards[0], "TEMP", st->temp_c, "C", temp_accent(m, st->temp_c),
             CARD_COL0_X, CARD_ROW0_Y);
    set_card(&m->cards[1], "HUMI", st->humidity, "%",
             humidity_accent(st->humidity), CARD_COL1_X, CARD_ROW0_Y);
    set_card(&m->cards[2], "LIGHT", st->light, "LV", m->color_accent,
             CARD_COL0_X, CARD_ROW1_Y);
    {
        static const uint16_t fan_hue_r[4] = {150, 44, 246, 230};
        static const uint16_t fan_hue_g[4] = {168, 190, 196, 52};
        static const uint16_t fan_hue_b[4] = {190, 112, 57, 62};
        uint8_t lvl = st->fan_level > 3 ? 3 : st->fan_level;
        uint16_t accent =
            RGB565(fan_hue_r[lvl], fan_hue_g[lvl], fan_hue_b[lvl]);
        set_card(&m->cards[3], "FAN", lvl, "LV", accent, CARD_COL1_X,
                 CARD_ROW1_Y);
    }
    m->card_count = 4;

    wrap_speech(st->speech, m);

    /* Hit regions: fan up/down (top/bottom of the fan card), pump + theme
     * buttons, and the cat sprite. */
    add_hit(m, CARD_COL1_X, CARD_ROW1_Y, CARD_W, CARD_H / 2U, UI_ACTION_FAN_UP);
    add_hit(m, CARD_COL1_X, CARD_ROW1_Y + CARD_H / 2U, CARD_W,
            CARD_H - CARD_H / 2U, UI_ACTION_FAN_DOWN);
    add_hit(m, CARD_MARGIN, BTN_Y, CARD_W, BTN_H, UI_ACTION_PUMP_TOGGLE);
    add_hit(m, CARD_COL1_X, BTN_Y, CARD_W, BTN_H, UI_ACTION_THEME_CYCLE);
    add_hit(m, CAT_X, CAT_Y, CAT_W, CAT_H, UI_ACTION_TALK);
}

ui_action_t ui_home_hit_test(const ui_home_model_t *m, uint16_t x, uint16_t y) {
    if (!m)
        return UI_ACTION_NONE;
    for (unsigned i = 0; i < m->hit_count; i++) {
        const ui_rect_t *r = &m->hits[i].rect;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return m->hits[i].action;
    }
    return UI_ACTION_NONE;
}

#endif /* CONFIG_BOARD_STM32F103 */
