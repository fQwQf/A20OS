#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include "desktop_apps.h"
#include "desktop_terminal.h"
#include "desktop_utils.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"

enum {
    TOPBAR_HEIGHT = 50,
    SIDEBAR_WIDTH = 74,
    CURSOR_WIDTH = 12,
    CURSOR_HEIGHT = 20,
};

static const desktop_app_t *app_registry[] = {
    &desktop_app_dashboard,
    &desktop_app_terminal,
    &desktop_app_monitor,
    &desktop_app_processes,
    &desktop_app_files,
    &desktop_app_network,
    &desktop_app_system,
};

static uint32_t cursor_pixels[CURSOR_WIDTH * CURSOR_HEIGHT];
static lv_obj_t *topbar_clock_label;
static lv_obj_t *topbar_date_label;
static lv_obj_t *topbar_mem_label;
static lv_obj_t *topbar_uptime_label;
static lv_obj_t *sidebar;
static lv_obj_t *content_area;
static lv_obj_t *nav_buttons[sizeof(app_registry) / sizeof(app_registry[0])];
static lv_obj_t *current_app_view;
static int current_app_index = -1;

static const lv_image_dsc_t cursor_image = {
    .header = {
        .cf = LV_COLOR_FORMAT_ARGB8888,
        .w = CURSOR_WIDTH,
        .h = CURSOR_HEIGHT,
        .stride = CURSOR_WIDTH * sizeof(uint32_t),
    },
    .data_size = sizeof(cursor_pixels),
    .data = (const uint8_t *)cursor_pixels,
};

static uint32_t desktop_tick_get(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint32_t)((uint64_t)now.tv_sec * 1000U +
                      (uint64_t)now.tv_nsec / 1000000U);
}

static void init_cursor_image(void)
{
    memset(cursor_pixels, 0, sizeof(cursor_pixels));
    for (int32_t y = 0; y < CURSOR_HEIGHT; y++) {
        int32_t row_width = 1 + (y * (CURSOR_WIDTH - 1)) / (CURSOR_HEIGHT - 1);
        for (int32_t x = 0; x < row_width; x++) {
            bool edge = x == 0 || x == row_width - 1 || y == CURSOR_HEIGHT - 1;
            cursor_pixels[y * CURSOR_WIDTH + x] = edge ? 0xff111619U : 0xfff9fbfcU;
        }
    }
}

static void format_uptime(char *buffer, size_t size, unsigned long seconds)
{
    unsigned long hours = seconds / 3600;
    unsigned long minutes = (seconds % 3600) / 60;
    if (hours > 0)
        snprintf(buffer, size, "%luh %02lum", hours, minutes);
    else
        snprintf(buffer, size, "%lum", minutes);
}

static void update_status_bar(void)
{
    time_t now = time(NULL);
    struct tm local;
    char text[48];
    if (localtime_r(&now, &local)) {
        strftime(text, sizeof(text), "%H:%M", &local);
        lv_label_set_text(topbar_clock_label, text);
        strftime(text, sizeof(text), "%b %d", &local);
        lv_label_set_text(topbar_date_label, text);
    }

    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        unsigned long unit = info.mem_unit ? info.mem_unit : 1;
        unsigned long total_kb = info.totalram * unit / 1024;
        unsigned long free_kb = info.freeram * unit / 1024;
        unsigned long used_kb = total_kb > free_kb ? total_kb - free_kb : 0;
        int percent = total_kb ? (int)(used_kb * 100 / total_kb) : 0;
        lv_label_set_text_fmt(topbar_mem_label, LV_SYMBOL_CHARGE " %d%%", percent);
        format_uptime(text, sizeof(text), info.uptime);
        lv_label_set_text_fmt(topbar_uptime_label, LV_SYMBOL_REFRESH " %s", text);
    }
}

static void status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_status_bar();
}

static void switch_app(int index);

static void nav_button_cb(lv_event_t *event)
{
    lv_obj_t *btn = lv_event_get_target(event);
    for (size_t i = 0; i < sizeof(nav_buttons) / sizeof(nav_buttons[0]); i++) {
        if (nav_buttons[i] == btn) {
            switch_app((int)i);
            break;
        }
    }
}

static lv_obj_t *create_nav_button(lv_obj_t *parent, const desktop_app_t *app, bool active)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 54, 54);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, active ? 2 : 0, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(DESKTOP_COLOR_TEAL), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(active ? DESKTOP_COLOR_TEAL_SOFT : DESKTOP_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xE2E8EA), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(active ? DESKTOP_COLOR_TEAL : DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_outline_width(button, 0, LV_STATE_FOCUSED);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, app->symbol);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_center(label);

    return button;
}

static void create_topbar(lv_obj_t *screen)
{
    lv_obj_t *topbar = lv_obj_create(screen);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, lv_pct(100), TOPBAR_HEIGHT);
    lv_obj_set_style_radius(topbar, 0, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_border_width(topbar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(topbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(topbar, lv_color_hex(DESKTOP_COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(topbar, lv_color_hex(DESKTOP_COLOR_SURFACE), 0);
    lv_obj_set_style_pad_hor(topbar, 18, 0);
    lv_obj_set_style_pad_ver(topbar, 0, 0);
    lv_obj_set_scrollable(topbar, false);

    lv_obj_t *mark = lv_obj_create(topbar);
    lv_obj_set_size(mark, 27, 27);
    lv_obj_set_style_radius(mark, 6, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_bg_color(mark, lv_color_hex(DESKTOP_COLOR_TEAL), 0);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_scrollable(mark, false);

    lv_obj_t *letter = lv_label_create(mark);
    lv_label_set_text(letter, "A");
    lv_obj_set_style_text_color(letter, lv_color_white(), 0);
    lv_obj_center(letter);

    lv_obj_t *product = lv_label_create(topbar);
    lv_label_set_text(product, "A20OS");
    lv_obj_set_style_text_color(product, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(product, &lv_font_montserrat_16, 0);
    lv_obj_align(product, LV_ALIGN_LEFT_MID, 38, -7);

    lv_obj_t *context = lv_label_create(topbar);
    lv_label_set_text(context, "Mission Control");
    lv_obj_set_style_text_color(context, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_align(context, LV_ALIGN_LEFT_MID, 38, 10);

    topbar_uptime_label = lv_label_create(topbar);
    lv_label_set_text(topbar_uptime_label, LV_SYMBOL_REFRESH " --");
    lv_obj_set_style_text_color(topbar_uptime_label, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_align(topbar_uptime_label, LV_ALIGN_RIGHT_MID, -155, 0);

    topbar_mem_label = lv_label_create(topbar);
    lv_label_set_text(topbar_mem_label, LV_SYMBOL_CHARGE " --");
    lv_obj_set_style_text_color(topbar_mem_label, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_align(topbar_mem_label, LV_ALIGN_RIGHT_MID, -80, 0);

    lv_obj_t *clock_box = lv_obj_create(topbar);
    lv_obj_set_size(clock_box, 70, 34);
    lv_obj_set_style_radius(clock_box, 6, 0);
    lv_obj_set_style_border_width(clock_box, 1, 0);
    lv_obj_set_style_border_color(clock_box, lv_color_hex(DESKTOP_COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(clock_box, lv_color_hex(DESKTOP_COLOR_SURFACE_MUTED), 0);
    lv_obj_align(clock_box, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_scrollable(clock_box, false);

    topbar_clock_label = lv_label_create(clock_box);
    lv_label_set_text(topbar_clock_label, "--:--");
    lv_obj_set_style_text_color(topbar_clock_label, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(topbar_clock_label, &lv_font_montserrat_16, 0);
    lv_obj_align(topbar_clock_label, LV_ALIGN_TOP_MID, 0, 2);

    topbar_date_label = lv_label_create(clock_box);
    lv_label_set_text(topbar_date_label, "--- --");
    lv_obj_set_style_text_color(topbar_date_label, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_align(topbar_date_label, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void create_sidebar(lv_obj_t *screen)
{
    int32_t height = lv_display_get_vertical_resolution(NULL);
    lv_obj_t *bar = lv_obj_create(screen);
    sidebar = bar;
    lv_obj_set_pos(bar, 0, TOPBAR_HEIGHT);
    lv_obj_set_size(bar, SIDEBAR_WIDTH, height - TOPBAR_HEIGHT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(DESKTOP_COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(DESKTOP_COLOR_SURFACE), 0);
    lv_obj_set_style_pad_top(bar, 14, 0);
    lv_obj_set_style_pad_row(bar, 8, 0);
    lv_obj_set_scrollable(bar, false);

    for (size_t i = 0; i < sizeof(app_registry) / sizeof(app_registry[0]); i++) {
        lv_obj_t *btn = create_nav_button(bar, app_registry[i], i == 0);
        nav_buttons[i] = btn;
        lv_obj_add_event_cb(btn, nav_button_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void create_content_area(lv_obj_t *screen)
{
    int32_t width = lv_display_get_horizontal_resolution(NULL);
    int32_t height = lv_display_get_vertical_resolution(NULL);

    lv_obj_t *area = lv_obj_create(screen);
    content_area = area;
    lv_obj_set_pos(area, SIDEBAR_WIDTH, TOPBAR_HEIGHT);
    lv_obj_set_size(area, width - SIDEBAR_WIDTH, height - TOPBAR_HEIGHT);
    lv_obj_set_style_radius(area, 0, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_style_bg_color(area, lv_color_hex(DESKTOP_COLOR_CANVAS), 0);
    lv_obj_set_style_pad_all(area, 14, 0);
    lv_obj_set_scrollable(area, false);
}

static void switch_app(int index)
{
    if (index < 0 || index >= (int)(sizeof(app_registry) / sizeof(app_registry[0])))
        return;
    if (index == current_app_index)
        return;

    const desktop_app_t *app = app_registry[index];

    if (current_app_index == 1)
        desktop_terminal_release_focus();

    if (current_app_view) {
        lv_obj_delete(current_app_view);
        current_app_view = NULL;
    }

    if (current_app_index >= 0) {
        lv_obj_t *prev = nav_buttons[current_app_index];
        lv_obj_set_style_border_width(prev, 0, 0);
        lv_obj_set_style_bg_color(prev, lv_color_hex(DESKTOP_COLOR_SURFACE), 0);
        lv_obj_set_style_text_color(prev, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    }

    lv_obj_t *next = nav_buttons[index];
    lv_obj_set_style_border_width(next, 2, 0);
    lv_obj_set_style_bg_color(next, lv_color_hex(DESKTOP_COLOR_TEAL_SOFT), 0);
    lv_obj_set_style_text_color(next, lv_color_hex(DESKTOP_COLOR_TEAL), 0);

    current_app_index = index;
    current_app_view = app->create(content_area);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("Starting A20OS Mission Control...\n");

    lv_init();
    lv_tick_set_cb(desktop_tick_get);
    lv_port_disp_init();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(DESKTOP_COLOR_CANVAS), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_scrollable(screen, false);

    lv_indev_t *mouse_indev = lv_port_indev_init();
    lv_port_keyboard_init();

    create_topbar(screen);
    create_sidebar(screen);
    create_content_area(screen);

    init_cursor_image();
    lv_obj_t *cursor_obj = lv_image_create(lv_layer_sys());
    lv_image_set_src(cursor_obj, &cursor_image);
    if (mouse_indev)
        lv_indev_set_cursor(mouse_indev, cursor_obj);

    update_status_bar();
    lv_timer_create(status_timer_cb, 1000, NULL);

    switch_app(0);

    printf("Mission Control initialized, entering loop...\n");
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }
}
