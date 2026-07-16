#ifndef _STM32F103_UI_HOME_H
#define _STM32F103_UI_HOME_H

/*
 * Home-screen UI model (hardware-independent).
 *
 * ui_home_build() turns the current hub state (temperature, humidity, light,
 * fan/pump, clock, theme/mood/alert, cloud speech) into a fully-resolved
 * "what to draw" description: a theme/alert-driven RGB565 palette, formatted
 * data cards, a word-wrapped speech bubble and a set of touch hit regions.
 * ui_home_hit_test() maps a touch point to a UI action.
 *
 * This is pure integer/string logic with no register or display access, so it
 * is covered by the host unit test and the QEMU self-test. A thin renderer on
 * the target walks the model and calls the display.c primitives (that part is
 * hardware-only). Keeping layout and hit-testing here means the interactive
 * behaviour can be regression-tested without a panel.
 */

#include "core/types.h"

#define UI_HOME_MAX_CARDS 4U
#define UI_HOME_MAX_HITS 6U
#define UI_HOME_SPEECH_LINES 3U
#define UI_HOME_SPEECH_COLS 18U /* chars per bubble line (ASCII budget) */

typedef enum ui_action {
    UI_ACTION_NONE = 0,
    UI_ACTION_FAN_UP,
    UI_ACTION_FAN_DOWN,
    UI_ACTION_PUMP_TOGGLE,
    UI_ACTION_THEME_CYCLE,
    UI_ACTION_TALK, /* tapped the cat -> trigger an interaction */
} ui_action_t;

typedef struct ui_rect {
    uint16_t x, y, w, h;
} ui_rect_t;

typedef struct ui_hit_region {
    ui_rect_t rect;
    ui_action_t action;
} ui_hit_region_t;

typedef struct ui_card {
    const char *label;   /* static string, e.g. "TEMP" */
    char value[8];       /* formatted number, e.g. "-5" or "72" */
    const char *unit;    /* static string, e.g. "C", "%", "LV" */
    uint16_t accent;     /* RGB565 accent bar / value colour */
    ui_rect_t rect;      /* card bounds on the 320x480 panel */
} ui_card_t;

typedef struct ui_home_model {
    /* Palette (RGB565) resolved from theme + alert. */
    uint16_t color_bg;
    uint16_t color_panel;
    uint16_t color_text;
    uint16_t color_muted;
    uint16_t color_accent;

    ui_card_t cards[UI_HOME_MAX_CARDS];
    unsigned card_count;

    char clock[6]; /* "HH:MM" */
    const char *net_label; /* "CLOUD" or "LOCAL" */
    const char *alert_label; /* "" when none, else "HEAT"/"DRY"/"SENSOR" */
    uint16_t alert_color;

    /* Speech bubble, word-wrapped into fixed lines (ASCII-measured; multibyte
     * UTF-8 is kept intact but counts by byte for the width budget). */
    char speech[UI_HOME_SPEECH_LINES][UI_HOME_SPEECH_COLS * 3U + 1U];
    unsigned speech_lines;
    int cat_mood; /* 0..3, mirrors the Live2D state */

    ui_hit_region_t hits[UI_HOME_MAX_HITS];
    unsigned hit_count;
} ui_home_model_t;

typedef struct ui_home_state {
    int8_t temp_c;
    uint8_t humidity; /* %RH   */
    uint8_t light;    /* 0..100 */
    uint8_t fan_level; /* 0..3  */
    uint8_t pump_on;
    uint8_t hour;   /* 0..23 */
    uint8_t minute; /* 0..59 */
    uint8_t theme;  /* env_theme_t */
    uint8_t mood;   /* 0..3        */
    uint8_t alert;  /* env_alert_t */
    uint8_t net_cloud; /* 1 = cloud-controlled, 0 = local fallback */
    const char *speech; /* may be NULL */
} ui_home_state_t;

void ui_home_build(const ui_home_state_t *st, ui_home_model_t *m);
ui_action_t ui_home_hit_test(const ui_home_model_t *m, uint16_t x, uint16_t y);

#endif /* _STM32F103_UI_HOME_H */
