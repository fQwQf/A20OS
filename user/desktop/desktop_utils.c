#include "desktop_utils.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int desktop_read_file(const char *path, char *buf, size_t size)
{
    if (size == 0)
        return -1;
    buf[0] = '\0';

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    size_t n = fread(buf, 1, size - 1, f);
    if (n < size - 1 && ferror(f)) {
        fclose(f);
        return -1;
    }
    buf[n] = '\0';
    fclose(f);
    return (int)n;
}

bool desktop_parse_key_kb(const char *text, const char *key, unsigned long *out_kb)
{
    const char *p = strstr(text, key);
    if (!p)
        return false;

    p += strlen(key);
    while (*p && !isdigit((unsigned char)*p))
        p++;
    if (!*p)
        return false;

    char *end = NULL;
    unsigned long val = strtoul(p, &end, 10);
    if (p == end)
        return false;

    while (*end && isspace((unsigned char)*end))
        end++;
    if (strncmp(end, "kB", 2) != 0 && strncmp(end, "KB", 2) != 0)
        return false;

    *out_kb = val;
    return true;
}

lv_obj_t *desktop_card_create(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_radius(card, 7, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(DESKTOP_COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(DESKTOP_COLOR_SURFACE), 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    return card;
}

lv_obj_t *desktop_card_title_create(lv_obj_t *parent, const char *title, const char *subtitle)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_set_size(block, lv_pct(100), subtitle ? 38 : 20);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_scrollable(block, false);

    lv_obj_t *title_label = lv_label_create(block);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    if (subtitle) {
        lv_obj_t *caption_label = lv_label_create(block);
        lv_label_set_text(caption_label, subtitle);
        lv_obj_set_style_text_color(caption_label, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
        lv_obj_align(caption_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    return block;
}

lv_obj_t *desktop_metric_card_create(lv_obj_t *parent, const char *symbol,
                                     const char *label, const char *value,
                                     uint32_t color)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_height(card, 78);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_radius(card, 7, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xE0E5E7), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(DESKTOP_COLOR_SURFACE), 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_scrollable(card, false);

    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *val_label = lv_label_create(card);
    lv_label_set_text(val_label, value ? value : "--");
    lv_obj_set_style_text_color(val_label, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(val_label, &lv_font_montserrat_16, 0);
    lv_obj_align(val_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *name_label = lv_label_create(card);
    lv_label_set_text(name_label, label);
    lv_obj_set_style_text_color(name_label, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_align(name_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    return card;
}

lv_obj_t *desktop_button_create(lv_obj_t *parent, const char *symbol,
                                const char *text, bool primary)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_height(button, 32);
    lv_obj_set_style_radius(button, 5, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, primary ? 0 : 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? DESKTOP_COLOR_TEAL : DESKTOP_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? 0x1E6E64 : 0xE2E8EA), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(primary ? 0xFFFFFF : DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_pad_hor(button, 11, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text_fmt(label, "%s  %s", symbol, text);
    lv_obj_center(label);
    return button;
}

lv_obj_t *desktop_badge_create(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, 22);
    lv_obj_set_style_radius(badge, 4, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(color), 0);
    lv_obj_set_style_pad_hor(badge, 8, 0);
    lv_obj_set_style_pad_ver(badge, 2, 0);
    lv_obj_set_scrollable(badge, false);

    lv_obj_t *label = lv_label_create(badge);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return badge;
}

lv_obj_t *desktop_label_pair_create(lv_obj_t *parent, const char *name, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 5, 0);
    lv_obj_set_scrollable(row, false);

    lv_obj_t *name_label = lv_label_create(row);
    lv_label_set_text(name_label, name);
    lv_obj_set_style_text_color(name_label, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, value ? value : "--");
    lv_obj_set_style_text_color(value_label, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_align(value_label, LV_ALIGN_RIGHT_MID, 0, 0);

    return row;
}
