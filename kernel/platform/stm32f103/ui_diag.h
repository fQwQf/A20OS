#ifndef _STM32F103_UI_DIAG_H
#define _STM32F103_UI_DIAG_H

/*
 * Diagnostics page model — the secondary menu (hardware-independent).
 *
 * The SYS / MEM / TF / BT / WIFI panels used to be the whole screen: they were
 * the old A20OS bring-up dashboard (display.c, CONFIG_STM32_LEGACY_DASHBOARD),
 * six full pages of nav buttons that left no room for the hub UI. They are
 * developer instrumentation, not what a home hub should show at rest, so they
 * now live one level down: the home screen's MENU button opens this page, BACK
 * returns. The legacy dashboard build stays as-is for bring-up work.
 *
 * ui_diag_build() turns raw peripheral status into formatted rows + hit
 * regions; ui_diag_hit_test() maps a touch to an action. Pure integer/string
 * logic with no register access, so the host unit test covers it.
 */

#include "core/types.h"
#include "ui_home.h" /* ui_rect_t + the palette in ui_home_model_t */

#define UI_DIAG_MAX_ROWS 8U
#define UI_DIAG_MAX_HITS 4U
#define UI_DIAG_VALUE_MAX 14U

/* 320x480 layout: header | UI_DIAG_MAX_ROWS status rows | 4 action buttons.
 * Shared with the renderer, which walks rows by index. */
#define DIAG_ROW_X 14U
#define DIAG_ROW_W 292U
#define DIAG_ROW0_Y 76U
#define DIAG_ROW_H 34U
#define DIAG_ROW_PITCH 38U
#define DIAG_BTN_X 12U
#define DIAG_BTN_Y 400U
#define DIAG_BTN_H 46U
#define DIAG_BTN_W 68U
#define DIAG_BTN_PITCH 76U /* 12 + 4*68 + 3*8 = 308 */

typedef enum ui_diag_action {
    UI_DIAG_ACTION_NONE = 0,
    UI_DIAG_ACTION_BACK,
    UI_DIAG_ACTION_CALIBRATE, /* re-run the four-corner touch calibration */
    UI_DIAG_ACTION_WIFI_SCAN,
    UI_DIAG_ACTION_BT_TEST,
} ui_diag_action_t;

/* Row severity -> colour, resolved by the renderer against the live palette. */
typedef enum ui_diag_level {
    UI_DIAG_OK = 0,   /* green  */
    UI_DIAG_INFO = 1, /* accent */
    UI_DIAG_WARN = 2, /* yellow */
} ui_diag_level_t;

typedef struct ui_diag_row {
    const char *label; /* static, e.g. "SYS" */
    char value[UI_DIAG_VALUE_MAX];
    uint8_t level; /* ui_diag_level_t */
} ui_diag_row_t;

typedef struct ui_diag_hit {
    ui_rect_t rect;
    ui_diag_action_t action;
} ui_diag_hit_t;

typedef struct ui_diag_model {
    ui_diag_row_t rows[UI_DIAG_MAX_ROWS];
    unsigned row_count;
    ui_diag_hit_t hits[UI_DIAG_MAX_HITS];
    unsigned hit_count;
} ui_diag_model_t;

/* Raw status collected from the drivers by peripherals.c. */
typedef struct ui_diag_state {
    uint32_t hclk_hz;
    uint32_t uptime_s;
    size_t ram_used;
    size_t ram_total;
    size_t ext_used;
    size_t ext_total;
    int sd_ready;
    int sd_fat32;
    int bt_ready;
    int bt_connected;
    int bt_waiting;
    int wifi_ready;
    int wifi_joined;
    int wifi_socket;
    int touch_ready;
    int ir_ready;
    unsigned ir_bindings;
} ui_diag_state_t;

void ui_diag_build(const ui_diag_state_t *st, ui_diag_model_t *m);
ui_diag_action_t ui_diag_hit_test(const ui_diag_model_t *m, uint16_t x,
                                  uint16_t y);

#endif /* _STM32F103_UI_DIAG_H */
