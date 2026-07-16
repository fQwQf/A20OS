#ifndef A20OS_DESKTOP_UTILS_H
#define A20OS_DESKTOP_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

enum {
    DESKTOP_COLOR_CANVAS        = 0xE9EEF0,
    DESKTOP_COLOR_SURFACE       = 0xFFFFFF,
    DESKTOP_COLOR_SURFACE_MUTED = 0xF5F7F8,
    DESKTOP_COLOR_BORDER        = 0xD4DCE0,
    DESKTOP_COLOR_TEXT          = 0x202B31,
    DESKTOP_COLOR_TEXT_MUTED    = 0x6D7A81,
    DESKTOP_COLOR_TEAL          = 0x257E72,
    DESKTOP_COLOR_TEAL_SOFT     = 0xDCECE8,
    DESKTOP_COLOR_BLUE          = 0x4C76A8,
    DESKTOP_COLOR_BLUE_SOFT     = 0xE2ECF6,
    DESKTOP_COLOR_AMBER         = 0xBC8A32,
    DESKTOP_COLOR_AMBER_SOFT    = 0xF5E8D5,
    DESKTOP_COLOR_RED           = 0xB45757,
    DESKTOP_COLOR_RED_SOFT      = 0xF5D5D5,
    DESKTOP_COLOR_PURPLE        = 0x7A5A8A,
    DESKTOP_COLOR_DARK          = 0x20272C,
};

int  desktop_read_file(const char *path, char *buf, size_t size);
bool desktop_parse_key_kb(const char *text, const char *key, unsigned long *out_kb);

lv_obj_t *desktop_card_create(lv_obj_t *parent);
lv_obj_t *desktop_card_title_create(lv_obj_t *parent, const char *title, const char *subtitle);
lv_obj_t *desktop_metric_card_create(lv_obj_t *parent, const char *symbol,
                                     const char *label, const char *value,
                                     uint32_t color);
lv_obj_t *desktop_button_create(lv_obj_t *parent, const char *symbol,
                                const char *text, bool primary);
lv_obj_t *desktop_badge_create(lv_obj_t *parent, const char *text, uint32_t color);
lv_obj_t *desktop_label_pair_create(lv_obj_t *parent, const char *name, const char *value);

#endif
