#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include "desktop_terminal.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"

enum {
    CURSOR_WIDTH = 12,
    CURSOR_HEIGHT = 20,
    TOPBAR_HEIGHT = 50,
    DOCK_WIDTH = 70,
    SIDEBAR_WIDTH = 248,
};

enum {
    COLOR_CANVAS = 0xE9EEF0,
    COLOR_SURFACE = 0xFFFFFF,
    COLOR_SURFACE_MUTED = 0xF5F7F8,
    COLOR_BORDER = 0xD4DCE0,
    COLOR_TEXT = 0x202B31,
    COLOR_TEXT_MUTED = 0x6D7A81,
    COLOR_TEAL = 0x257E72,
    COLOR_TEAL_SOFT = 0xDCECE8,
    COLOR_BLUE = 0x4C76A8,
    COLOR_AMBER = 0xBC8A32,
    COLOR_RED = 0xB45757,
    COLOR_DARK = 0x20272C,
};

typedef struct {
    const char * label;
    const char * detail;
    const char * symbol;
    const char * command;
    uint32_t accent;
} quick_command_t;

static const quick_command_t quick_commands[] = {
    {"System summary", "Kernel and architecture", LV_SYMBOL_CHARGE,
     "uname -a", COLOR_TEAL},
    {"Processes", "Inspect active tasks", LV_SYMBOL_LIST,
     "ps", COLOR_BLUE},
    {"Root filesystem", "Browse mounted root", LV_SYMBOL_DIRECTORY,
     "ls -la /", COLOR_AMBER},
    {"Network state", "Interfaces and sockets", LV_SYMBOL_WIFI,
     "netstat", 0x6C768F},
};

static uint32_t cursor_pixels[CURSOR_WIDTH * CURSOR_HEIGHT];
static lv_obj_t * clock_label;
static lv_obj_t * clock_date_label;
static lv_obj_t * shell_state_label;
static lv_obj_t * pty_label;
static lv_obj_t * session_indicator;
static lv_obj_t * uptime_label;
static lv_obj_t * memory_label;
static lv_obj_t * memory_bar;
static lv_obj_t * connection_label;

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
        int32_t row_width = 1 + (y * (CURSOR_WIDTH - 1)) /
                           (CURSOR_HEIGHT - 1);
        for (int32_t x = 0; x < row_width; x++) {
            bool edge = x == 0 || x == row_width - 1 ||
                        y == CURSOR_HEIGHT - 1;
            cursor_pixels[y * CURSOR_WIDTH + x] =
                edge ? 0xff111619U : 0xfff9fbfcU;
        }
    }
}

static void style_unframed(lv_obj_t * obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollable(obj, false);
}

static lv_obj_t * create_panel(lv_obj_t * parent)
{
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_style_radius(panel, 7, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_scrollable(panel, false);
    return panel;
}

static lv_obj_t * create_section_heading(lv_obj_t * parent,
                                         const char * title,
                                         const char * caption)
{
    lv_obj_t * block = lv_obj_create(parent);
    lv_obj_set_size(block, lv_pct(100), caption ? 38 : 20);
    style_unframed(block);

    lv_obj_t * title_label = lv_label_create(block);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    if (caption) {
        lv_obj_t * caption_label = lv_label_create(block);
        lv_label_set_text(caption_label, caption);
        lv_obj_set_style_text_color(caption_label,
                                    lv_color_hex(COLOR_TEXT_MUTED), 0);
        lv_obj_align(caption_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    return block;
}

static lv_obj_t * create_icon_button(lv_obj_t * parent, const char * icon,
                                     const char * name, bool active)
{
    lv_obj_t * button = lv_button_create(parent);
    lv_obj_set_size(button, 46, 46);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, active ? 1 : 0, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xB9D5CF), 0);
    lv_obj_set_style_bg_color(button,
                              active ? lv_color_hex(COLOR_TEAL_SOFT)
                                     : lv_color_hex(COLOR_SURFACE_MUTED),
                              0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xD6E1E3),
                              LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button,
                                active ? lv_color_hex(COLOR_TEAL)
                                       : lv_color_hex(0x55636B),
                                0);
    lv_obj_set_style_outline_width(button, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(button, lv_color_hex(0x86BDB4),
                                   LV_STATE_FOCUSED);

    lv_obj_t * label = lv_label_create(button);
    lv_label_set_text(label, icon);
    lv_obj_center(label);
    lv_obj_set_user_data(button, (void *)name);
    return button;
}

static lv_obj_t * create_tool_button(lv_obj_t * parent, const char * symbol,
                                     const char * text, bool primary)
{
    lv_obj_t * button = lv_button_create(parent);
    lv_obj_set_height(button, 32);
    lv_obj_set_style_radius(button, 5, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, primary ? 0 : 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x455159), 0);
    lv_obj_set_style_bg_color(button,
                              primary ? lv_color_hex(COLOR_TEAL)
                                      : lv_color_hex(0x303940),
                              0);
    lv_obj_set_style_bg_color(button,
                              primary ? lv_color_hex(0x1E6E64)
                                      : lv_color_hex(0x3C494F),
                              LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(0xF2F6F7), 0);
    lv_obj_set_style_pad_hor(button, 11, 0);

    lv_obj_t * label = lv_label_create(button);
    lv_label_set_text_fmt(label, "%s  %s", symbol, text);
    lv_obj_center(label);
    return button;
}

static lv_obj_t * create_metric_bar(lv_obj_t * parent, uint32_t color)
{
    lv_obj_t * bar = lv_bar_create(parent);
    lv_obj_set_size(bar, lv_pct(100), 7);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xE3E8EA), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    return bar;
}

static lv_obj_t * create_quick_action(lv_obj_t * parent,
                                      const quick_command_t * command)
{
    lv_obj_t * button = lv_button_create(parent);
    lv_obj_set_size(button, lv_pct(100), 49);
    lv_obj_set_style_radius(button, 5, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xDFE5E7), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xF8F9FA), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xE9EEF0),
                              LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_text_color(button, lv_color_hex(COLOR_TEXT), 0);

    lv_obj_t * accent = lv_obj_create(button);
    lv_obj_set_size(accent, 4, lv_pct(100));
    lv_obj_set_style_radius(accent, 0, 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(command->accent), 0);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_scrollable(accent, false);

    lv_obj_t * icon = lv_label_create(button);
    lv_label_set_text(icon, command->symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(command->accent), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_t * label = lv_label_create(button);
    lv_label_set_text(label, command->label);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 40, 6);

    lv_obj_t * detail = lv_label_create(button);
    lv_label_set_text(detail, command->detail);
    lv_obj_set_style_text_color(detail, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_align(detail, LV_ALIGN_BOTTOM_LEFT, 40, -5);

    lv_obj_t * arrow = lv_label_create(button);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x87939A), 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 0);
    return button;
}

static void quick_command_cb(lv_event_t * event)
{
    const quick_command_t * command =
        (const quick_command_t *)lv_event_get_user_data(event);
    desktop_terminal_send_command(command->command);
}

static void clear_terminal_cb(lv_event_t * event)
{
    (void)event;
    desktop_terminal_clear();
}

static void restart_terminal_cb(lv_event_t * event)
{
    (void)event;
    desktop_terminal_restart();
}

static void dock_terminal_cb(lv_event_t * event)
{
    (void)event;
    desktop_terminal_send_command("printf '\\nSession: %s\\n' \"$(tty)\"");
}

static void dock_files_cb(lv_event_t * event)
{
    (void)event;
    desktop_terminal_send_command("ls -la /");
}

static void dock_home_cb(lv_event_t * event)
{
    (void)event;
    desktop_terminal_send_command("cd / && printf 'Working directory: ' && pwd");
}

static void dock_settings_cb(lv_event_t * event)
{
    (void)event;
    desktop_terminal_send_command(
        "cat /bin/etc/os-release 2>/dev/null || "
        "cat /etc/os-release 2>/dev/null || uname -a");
}

static void format_uptime(char * buffer, size_t size, unsigned long seconds)
{
    unsigned long hours = seconds / 3600;
    unsigned long minutes = (seconds % 3600) / 60;

    if (hours > 0)
        snprintf(buffer, size, "%luh %02lum", hours, minutes);
    else
        snprintf(buffer, size, "%lum", minutes);
}

static void desktop_status_timer(lv_timer_t * timer)
{
    (void)timer;
    time_t now = time(NULL);
    struct tm local;
    char text[48];

    if (localtime_r(&now, &local)) {
        strftime(text, sizeof(text), "%H:%M", &local);
        lv_label_set_text(clock_label, text);
        strftime(text, sizeof(text), "%b %d", &local);
        lv_label_set_text(clock_date_label, text);
    }

    bool running = desktop_terminal_is_running();
    lv_label_set_text(shell_state_label, running ? "Ready" : "Stopped");
    lv_obj_set_style_text_color(shell_state_label,
                                running ? lv_color_hex(COLOR_TEAL)
                                        : lv_color_hex(COLOR_RED),
                                0);
    lv_label_set_text(connection_label,
                      running ? "PTY session connected"
                              : "Shell session unavailable");
    lv_label_set_text(pty_label, desktop_terminal_get_pty_name());
    lv_obj_set_style_bg_color(session_indicator,
                              running ? lv_color_hex(0x3F9B8D)
                                      : lv_color_hex(0xC46B6B),
                              0);

    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        unsigned long unit = info.mem_unit ? info.mem_unit : 1;
        unsigned long total_kb = info.totalram * unit / 1024;
        unsigned long free_kb = info.freeram * unit / 1024;
        unsigned long used_kb =
            total_kb > free_kb ? total_kb - free_kb : 0;
        int percent = total_kb ? (int)(used_kb * 100 / total_kb) : 0;

        format_uptime(text, sizeof(text), info.uptime);
        lv_label_set_text(uptime_label, text);
        lv_label_set_text_fmt(memory_label, "%lu / %lu MiB",
                              used_kb / 1024, total_kb / 1024);
        lv_bar_set_value(memory_bar, percent, true);
    }
}

static void create_topbar(lv_obj_t * screen)
{
    lv_obj_t * topbar = lv_obj_create(screen);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, lv_pct(100), TOPBAR_HEIGHT);
    lv_obj_set_style_radius(topbar, 0, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_border_width(topbar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(topbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(topbar, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(topbar, lv_color_hex(0xFAFBFB), 0);
    lv_obj_set_style_pad_hor(topbar, 18, 0);
    lv_obj_set_style_pad_ver(topbar, 0, 0);
    lv_obj_set_scrollable(topbar, false);

    lv_obj_t * mark = lv_obj_create(topbar);
    lv_obj_set_size(mark, 27, 27);
    lv_obj_set_style_radius(mark, 6, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_bg_color(mark, lv_color_hex(COLOR_TEAL), 0);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_scrollable(mark, false);

    lv_obj_t * letter = lv_label_create(mark);
    lv_label_set_text(letter, "A");
    lv_obj_set_style_text_color(letter, lv_color_white(), 0);
    lv_obj_center(letter);

    lv_obj_t * product = lv_label_create(topbar);
    lv_label_set_text(product, "A20OS");
    lv_obj_set_style_text_color(product, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(product, LV_ALIGN_LEFT_MID, 38, -7);

    lv_obj_t * context = lv_label_create(topbar);
    lv_label_set_text(context, "Development workspace");
    lv_obj_set_style_text_color(context, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_align(context, LV_ALIGN_LEFT_MID, 38, 10);

    lv_obj_t * environment = lv_obj_create(topbar);
    lv_obj_set_size(environment, 104, 27);
    lv_obj_set_style_radius(environment, 5, 0);
    lv_obj_set_style_border_width(environment, 1, 0);
    lv_obj_set_style_border_color(environment, lv_color_hex(0xCBD5D9), 0);
    lv_obj_set_style_bg_color(environment, lv_color_hex(0xF0F4F5), 0);
    lv_obj_align(environment, LV_ALIGN_RIGHT_MID, -96, 0);
    lv_obj_set_scrollable(environment, false);

    lv_obj_t * environment_label = lv_label_create(environment);
    lv_label_set_text(environment_label, LV_SYMBOL_CHARGE "  QEMU virt");
    lv_obj_set_style_text_color(environment_label,
                                lv_color_hex(0x526169), 0);
    lv_obj_center(environment_label);

    clock_label = lv_label_create(topbar);
    lv_label_set_text(clock_label, "--:--");
    lv_obj_set_style_text_color(clock_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, 0, -7);

    clock_date_label = lv_label_create(topbar);
    lv_label_set_text(clock_date_label, "--- --");
    lv_obj_set_style_text_color(clock_date_label,
                                lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_align(clock_date_label, LV_ALIGN_RIGHT_MID, 0, 10);
}

static void create_dock(lv_obj_t * screen)
{
    lv_obj_t * dock = lv_obj_create(screen);
    lv_obj_set_pos(dock, 0, TOPBAR_HEIGHT);
    lv_obj_set_size(dock, DOCK_WIDTH,
                    lv_display_get_vertical_resolution(NULL) - TOPBAR_HEIGHT);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(dock, 0, 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_border_side(dock, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(dock, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(dock, lv_color_hex(0xFAFBFB), 0);
    lv_obj_set_style_pad_top(dock, 15, 0);
    lv_obj_set_style_pad_row(dock, 10, 0);
    lv_obj_set_scrollable(dock, false);

    lv_obj_t * terminal_btn =
        create_icon_button(dock, LV_SYMBOL_KEYBOARD, "Terminal", true);
    lv_obj_add_event_cb(terminal_btn, dock_terminal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * files_btn =
        create_icon_button(dock, LV_SYMBOL_DIRECTORY, "Files", false);
    lv_obj_add_event_cb(files_btn, dock_files_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * home_btn =
        create_icon_button(dock, LV_SYMBOL_HOME, "Home", false);
    lv_obj_add_event_cb(home_btn, dock_home_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * monitor_btn =
        create_icon_button(dock, LV_SYMBOL_LIST, "Processes", false);
    lv_obj_add_event_cb(monitor_btn, quick_command_cb, LV_EVENT_CLICKED,
                        (void *)&quick_commands[1]);

    lv_obj_t * spacer = lv_obj_create(dock);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    style_unframed(spacer);

    lv_obj_t * settings_btn =
        create_icon_button(dock, LV_SYMBOL_SETTINGS, "System", false);
    lv_obj_add_event_cb(settings_btn, dock_settings_cb,
                        LV_EVENT_CLICKED, NULL);
}

static lv_obj_t * create_metric(lv_obj_t * parent, const char * name,
                                const char * symbol, uint32_t color,
                                lv_obj_t ** value_label)
{
    lv_obj_t * metric = lv_obj_create(parent);
    lv_obj_set_height(metric, 44);
    lv_obj_set_flex_grow(metric, 1);
    lv_obj_set_style_radius(metric, 5, 0);
    lv_obj_set_style_border_width(metric, 1, 0);
    lv_obj_set_style_border_color(metric, lv_color_hex(0xE0E5E7), 0);
    lv_obj_set_style_bg_color(metric, lv_color_hex(0xF8FAFA), 0);
    lv_obj_set_style_pad_hor(metric, 10, 0);
    lv_obj_set_style_pad_ver(metric, 6, 0);
    lv_obj_set_scrollable(metric, false);

    lv_obj_t * icon = lv_label_create(metric);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t * label = lv_label_create(metric);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 22, 0);

    *value_label = lv_label_create(metric);
    lv_label_set_text(*value_label, "--");
    lv_obj_set_style_text_color(*value_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(*value_label, LV_ALIGN_RIGHT_MID, 0, 0);
    return metric;
}

static void create_overview_strip(lv_obj_t * parent)
{
    lv_obj_t * strip = lv_obj_create(parent);
    lv_obj_set_size(strip, lv_pct(100), 70);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(strip, 7, 0);
    lv_obj_set_style_border_width(strip, 1, 0);
    lv_obj_set_style_border_color(strip, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_pad_all(strip, 12, 0);
    lv_obj_set_style_pad_column(strip, 9, 0);
    lv_obj_set_scrollable(strip, false);

    create_metric(strip, "Uptime", LV_SYMBOL_REFRESH, COLOR_TEAL,
                  &uptime_label);
    create_metric(strip, "Display", LV_SYMBOL_EYE_OPEN, COLOR_BLUE,
                  &connection_label);
    lv_label_set_text_fmt(connection_label, "%d x %d",
                          (int)lv_display_get_horizontal_resolution(NULL),
                          (int)lv_display_get_vertical_resolution(NULL));

    lv_obj_t * terminal_metric_value;
    create_metric(strip, "Terminal", LV_SYMBOL_KEYBOARD, COLOR_AMBER,
                  &terminal_metric_value);
    lv_label_set_text_fmt(terminal_metric_value, "%d x %d",
                          desktop_terminal_get_columns(),
                          desktop_terminal_get_rows());
}

static void create_system_panel(lv_obj_t * parent)
{
    lv_obj_t * overview = create_panel(parent);
    lv_obj_set_height(overview, 172);
    lv_obj_set_flex_flow(overview, LV_FLEX_FLOW_COLUMN);

    create_section_heading(overview, "Live session", "Interactive PTY");

    lv_obj_t * state_row = lv_obj_create(overview);
    lv_obj_set_size(state_row, lv_pct(100), 31);
    style_unframed(state_row);
    lv_obj_set_style_pad_right(state_row, 42, 0);

    session_indicator = lv_obj_create(state_row);
    lv_obj_set_size(session_indicator, 9, 9);
    lv_obj_set_style_radius(session_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(session_indicator, 0, 0);
    lv_obj_set_style_bg_color(session_indicator, lv_color_hex(COLOR_AMBER), 0);
    lv_obj_align(session_indicator, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_scrollable(session_indicator, false);

    connection_label = lv_label_create(state_row);
    lv_label_set_text(connection_label, "Connecting to shell");
    lv_obj_set_width(connection_label, 142);
    lv_label_set_long_mode(connection_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(connection_label,
                                lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_align(connection_label, LV_ALIGN_LEFT_MID, 17, 0);

    shell_state_label = lv_label_create(state_row);
    lv_label_set_text(shell_state_label, "Starting");
    lv_obj_align(shell_state_label, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t * divider = lv_obj_create(overview);
    lv_obj_set_size(divider, lv_pct(100), 1);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0xE4E8EA), 0);
    lv_obj_set_scrollable(divider, false);

    lv_obj_t * pty_name = lv_label_create(overview);
    lv_label_set_text(pty_name, "Pseudo terminal");
    lv_obj_set_style_text_color(pty_name, lv_color_hex(COLOR_TEXT_MUTED), 0);

    pty_label = lv_label_create(overview);
    lv_label_set_text(pty_label, "not connected");
    lv_obj_set_style_text_color(pty_label, lv_color_hex(COLOR_TEXT), 0);
}

static void create_resource_panel(lv_obj_t * parent)
{
    lv_obj_t * panel = create_panel(parent);
    lv_obj_set_height(panel, 104);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    create_section_heading(panel, "Resources", "Kernel memory");

    lv_obj_t * row = lv_obj_create(panel);
    lv_obj_set_size(row, lv_pct(100), 18);
    style_unframed(row);

    lv_obj_t * title = lv_label_create(row);
    lv_label_set_text(title, "Memory");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    memory_label = lv_label_create(row);
    lv_label_set_text(memory_label, "-- / -- MiB");
    lv_obj_set_style_text_color(memory_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(memory_label, LV_ALIGN_RIGHT_MID, 0, 0);

    memory_bar = create_metric_bar(panel, COLOR_BLUE);
    lv_bar_set_value(memory_bar, 0, false);
}

static void create_quick_panel(lv_obj_t * parent)
{
    lv_obj_t * quick = create_panel(parent);
    lv_obj_set_flex_grow(quick, 1);
    lv_obj_set_flex_flow(quick, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(quick, 11, 0);
    lv_obj_set_style_pad_row(quick, 5, 0);

    create_section_heading(quick, "Quick actions", "Run in terminal");

    for (size_t i = 0; i < sizeof(quick_commands) /
                            sizeof(quick_commands[0]); i++) {
        lv_obj_t * button = create_quick_action(quick, &quick_commands[i]);
        lv_obj_add_event_cb(button, quick_command_cb,
                            LV_EVENT_CLICKED, (void *)&quick_commands[i]);
    }
}

static void create_terminal_toolbar(lv_obj_t * parent)
{
    lv_obj_t * toolbar = lv_obj_create(parent);
    lv_obj_set_size(toolbar, lv_pct(100), 46);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(toolbar, 7, 0);
    lv_obj_set_style_border_width(toolbar, 0, 0);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(COLOR_DARK), 0);
    lv_obj_set_style_pad_hor(toolbar, 9, 0);
    lv_obj_set_style_pad_ver(toolbar, 7, 0);
    lv_obj_set_style_pad_column(toolbar, 7, 0);
    lv_obj_set_scrollable(toolbar, false);

    lv_obj_t * clear =
        create_tool_button(toolbar, LV_SYMBOL_TRASH, "Clear", false);
    lv_obj_add_event_cb(clear, clear_terminal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * restart =
        create_tool_button(toolbar, LV_SYMBOL_REFRESH, "Restart", true);
    lv_obj_add_event_cb(restart, restart_terminal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * spacer = lv_obj_create(toolbar);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    style_unframed(spacer);

    lv_obj_t * keyboard = lv_label_create(toolbar);
    lv_label_set_text(keyboard, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(0x72BEB2), 0);

    lv_obj_t * hint = lv_label_create(toolbar);
    lv_label_set_text(hint, "Terminal has keyboard focus");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xA5B0B5), 0);
}

static void create_workspace(lv_obj_t * screen)
{
    int32_t width = lv_display_get_horizontal_resolution(NULL);
    int32_t height = lv_display_get_vertical_resolution(NULL);

    lv_obj_t * workspace = lv_obj_create(screen);
    lv_obj_set_pos(workspace, DOCK_WIDTH, TOPBAR_HEIGHT);
    lv_obj_set_size(workspace, width - DOCK_WIDTH, height - TOPBAR_HEIGHT);
    lv_obj_set_flex_flow(workspace, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_radius(workspace, 0, 0);
    lv_obj_set_style_border_width(workspace, 0, 0);
    lv_obj_set_style_bg_color(workspace, lv_color_hex(COLOR_CANVAS), 0);
    lv_obj_set_style_pad_all(workspace, 14, 0);
    lv_obj_set_style_pad_row(workspace, 12, 0);
    lv_obj_set_scrollable(workspace, false);

    create_overview_strip(workspace);

    lv_obj_t * content = lv_obj_create(workspace);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(content, 12, 0);
    style_unframed(content);

    lv_obj_t * terminal_column = lv_obj_create(content);
    lv_obj_set_height(terminal_column, lv_pct(100));
    lv_obj_set_flex_grow(terminal_column, 1);
    lv_obj_set_flex_flow(terminal_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(terminal_column, 8, 0);
    style_unframed(terminal_column);

    create_terminal_toolbar(terminal_column);

    lv_obj_t * terminal_slot = lv_obj_create(terminal_column);
    lv_obj_set_width(terminal_slot, lv_pct(100));
    lv_obj_set_flex_grow(terminal_slot, 1);
    style_unframed(terminal_slot);
    desktop_terminal_create(terminal_slot);

    lv_obj_t * sidebar = lv_obj_create(content);
    lv_obj_set_size(sidebar, SIDEBAR_WIDTH, lv_pct(100));
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sidebar, 8, 0);
    style_unframed(sidebar);

    create_system_panel(sidebar);
    create_resource_panel(sidebar);
    create_quick_panel(sidebar);
}

int main(int argc, char ** argv)
{
    (void)argc;
    (void)argv;

    printf("Starting A20OS LVGL Desktop...\n");

    lv_init();
    lv_tick_set_cb(desktop_tick_get);
    lv_port_disp_init();

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_CANVAS), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_scrollable(screen, false);

    lv_indev_t * mouse_indev = lv_port_indev_init();
    lv_port_keyboard_init();

    create_topbar(screen);
    create_dock(screen);
    create_workspace(screen);

    init_cursor_image();
    lv_obj_t * cursor_obj = lv_image_create(lv_layer_sys());
    lv_image_set_src(cursor_obj, &cursor_image);
    if (mouse_indev)
        lv_indev_set_cursor(mouse_indev, cursor_obj);

    desktop_status_timer(NULL);
    lv_timer_create(desktop_status_timer, 1000, NULL);

    printf("Desktop and terminal initialized, entering loop...\n");
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }
}
