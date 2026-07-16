#include "desktop_apps.h"
#include "desktop_utils.h"
#include "lvgl.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum {
    MAX_QUICK_ACTIONS = 4,
};

typedef struct {
    lv_obj_t *uptime_value;
    lv_obj_t *mem_value;
    lv_obj_t *proc_value;
    lv_obj_t *display_value;
    lv_obj_t *welcome_detail;
    lv_timer_t *timer;
} dashboard_state_t;

static dashboard_state_t state;

static void format_uptime(char *buffer, size_t size, unsigned long seconds)
{
    unsigned long days = seconds / 86400;
    unsigned long hours = (seconds % 86400) / 3600;
    unsigned long minutes = (seconds % 3600) / 60;
    if (days > 0)
        snprintf(buffer, size, "%lud %luh %lum", days, hours, minutes);
    else if (hours > 0)
        snprintf(buffer, size, "%luh %lum", hours, minutes);
    else
        snprintf(buffer, size, "%lum", minutes);
}

static int count_processes(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9')
            count++;
    }
    closedir(d);
    return count;
}

static void update_dashboard(lv_timer_t *timer)
{
    (void)timer;
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        char text[64];
        format_uptime(text, sizeof(text), info.uptime);
        lv_label_set_text(state.uptime_value, text);

        unsigned long unit = info.mem_unit ? info.mem_unit : 1;
        unsigned long total_kb = info.totalram * unit / 1024;
        unsigned long free_kb = info.freeram * unit / 1024;
        unsigned long used_kb = total_kb > free_kb ? total_kb - free_kb : 0;
        snprintf(text, sizeof(text), "%lu / %lu MiB", used_kb / 1024, total_kb / 1024);
        lv_label_set_text(state.mem_value, text);
    }

    int procs = count_processes();
    lv_label_set_text_fmt(state.proc_value, "%d", procs);
}

static void quick_action_cb(lv_event_t *event)
{
    const char *command = (const char *)lv_event_get_user_data(event);
    (void)command;
}

static lv_obj_t *create_quick_action(lv_obj_t *parent, const char *symbol,
                                     const char *label, const char *command,
                                     uint32_t color)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, lv_pct(100), 48);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xE0E5E7), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(DESKTOP_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xE2E8EA), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(button, 12, 0);
    lv_obj_add_event_cb(button, quick_action_cb, LV_EVENT_CLICKED, (void *)command);

    lv_obj_t *icon = lv_label_create(button);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *text = lv_label_create(button);
    lv_label_set_text(text, label);
    lv_obj_set_style_text_color(text, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_align(text, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t *arrow = lv_label_create(button);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0xA8B2B7), 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);

    return button;
}

static void dashboard_view_delete_cb(lv_event_t *event)
{
    (void)event;
    if (state.timer) {
        lv_timer_delete(state.timer);
        state.timer = NULL;
    }
}

static lv_obj_t *create(lv_obj_t *parent)
{
    lv_obj_t *view = lv_obj_create(parent);
    lv_obj_add_event_cb(view, dashboard_view_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view, 0, 0);
    lv_obj_set_style_pad_all(view, 0, 0);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(view, false);

    lv_obj_t *card = desktop_card_create(view);
    lv_obj_set_size(card, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    desktop_card_title_create(card, "Dashboard", "A20OS system overview");

    lv_obj_t *welcome = lv_obj_create(card);
    lv_obj_set_size(welcome, lv_pct(100), 80);
    lv_obj_set_style_radius(welcome, 7, 0);
    lv_obj_set_style_border_width(welcome, 0, 0);
    lv_obj_set_style_bg_color(welcome, lv_color_hex(DESKTOP_COLOR_TEAL_SOFT), 0);
    lv_obj_set_style_pad_hor(welcome, 16, 0);
    lv_obj_set_style_pad_ver(welcome, 12, 0);
    lv_obj_set_scrollable(welcome, false);

    lv_obj_t *mark = lv_obj_create(welcome);
    lv_obj_set_size(mark, 42, 42);
    lv_obj_set_style_radius(mark, 8, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_bg_color(mark, lv_color_hex(DESKTOP_COLOR_TEAL), 0);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_scrollable(mark, false);

    lv_obj_t *letter = lv_label_create(mark);
    lv_label_set_text(letter, "A");
    lv_obj_set_style_text_color(letter, lv_color_white(), 0);
    lv_obj_set_style_text_font(letter, &lv_font_montserrat_16, 0);
    lv_obj_center(letter);

    lv_obj_t *title = lv_label_create(welcome);
    lv_label_set_text(title, "A20OS Mission Control");
    lv_obj_set_style_text_color(title, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 56, 0);

    state.welcome_detail = lv_label_create(welcome);
    lv_label_set_text(state.welcome_detail, "Hybrid kernel desktop environment");
    lv_obj_set_style_text_color(state.welcome_detail, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_align(state.welcome_detail, LV_ALIGN_TOP_LEFT, 56, 22);

    lv_obj_t *metrics = lv_obj_create(card);
    lv_obj_set_width(metrics, lv_pct(100));
    lv_obj_set_height(metrics, 78);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metrics, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(metrics, 10, 0);
    lv_obj_set_style_bg_opa(metrics, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(metrics, 0, 0);
    lv_obj_set_style_pad_all(metrics, 0, 0);
    lv_obj_set_scrollable(metrics, false);

    lv_obj_t *uptime_card = desktop_metric_card_create(metrics, LV_SYMBOL_REFRESH,
                                                       "Uptime", "--", DESKTOP_COLOR_TEAL);
    state.uptime_value = lv_obj_get_child(uptime_card, 1);

    lv_obj_t *mem_card = desktop_metric_card_create(metrics, LV_SYMBOL_CHARGE,
                                                    "Memory", "--", DESKTOP_COLOR_BLUE);
    state.mem_value = lv_obj_get_child(mem_card, 1);

    lv_obj_t *proc_card = desktop_metric_card_create(metrics, LV_SYMBOL_LIST,
                                                     "Processes", "--", DESKTOP_COLOR_AMBER);
    state.proc_value = lv_obj_get_child(proc_card, 1);

    lv_obj_t *display_card = desktop_metric_card_create(metrics, LV_SYMBOL_EYE_OPEN,
                                                        "Display", "--", DESKTOP_COLOR_PURPLE);
    state.display_value = lv_obj_get_child(display_card, 1);

    lv_label_set_text_fmt(state.display_value, "%d x %d",
                          (int)lv_display_get_horizontal_resolution(NULL),
                          (int)lv_display_get_vertical_resolution(NULL));

    lv_obj_t *quick_card = desktop_card_create(card);
    lv_obj_set_width(quick_card, lv_pct(100));
    lv_obj_set_flex_grow(quick_card, 1);
    lv_obj_set_flex_flow(quick_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(quick_card, 6, 0);

    desktop_card_title_create(quick_card, "Quick actions", "Launch common tasks");

    create_quick_action(quick_card, LV_SYMBOL_KEYBOARD, "Open Terminal",
                        "", DESKTOP_COLOR_DARK);
    create_quick_action(quick_card, LV_SYMBOL_LIST, "List processes",
                        "", DESKTOP_COLOR_AMBER);
    create_quick_action(quick_card, LV_SYMBOL_DIRECTORY, "Browse root",
                        "", DESKTOP_COLOR_PURPLE);
    create_quick_action(quick_card, LV_SYMBOL_WIFI, "Network status",
                        "", DESKTOP_COLOR_BLUE);

    update_dashboard(NULL);
    state.timer = lv_timer_create(update_dashboard, 1000, NULL);

    return view;
}

desktop_app_t desktop_app_dashboard = {
    .name = "Home",
    .symbol = LV_SYMBOL_HOME,
    .desc = "System overview",
    .accent = DESKTOP_COLOR_TEAL,
    .create = create,
};
