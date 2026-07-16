#include "desktop_apps.h"
#include "desktop_utils.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    MONITOR_HISTORY_POINTS = 36,
    MONITOR_PROC_BUFFER    = 2048,
};

typedef struct {
    lv_obj_t *cpu_value;
    lv_obj_t *mem_value;
    lv_obj_t *free_mem_value;
    lv_obj_t *cpu_detail;
    lv_obj_t *mem_detail;
    lv_obj_t *cpu_chart;
    lv_obj_t *mem_chart;
    lv_chart_series_t *cpu_series;
    lv_chart_series_t *mem_series;
    lv_timer_t *timer;
    int32_t cpu_history[MONITOR_HISTORY_POINTS];
    int32_t mem_history[MONITOR_HISTORY_POINTS];
    unsigned long long prev_total;
    unsigned long long prev_idle;
    bool has_cpu_sample;
} monitor_state_t;

static monitor_state_t state;

static void monitor_set_metric(lv_obj_t *label, const char *text)
{
    if (label)
        lv_label_set_text(label, text ? text : "--");
}

static void format_percent(char *buffer, size_t size, unsigned value)
{
    snprintf(buffer, size, "%u%%", value);
}

static void format_mib(char *buffer, size_t size, unsigned long kb)
{
    unsigned long whole = kb / 1024;
    unsigned long frac = ((kb % 1024) * 10UL) / 1024UL;
    snprintf(buffer, size, "%lu.%lu MiB", whole, frac);
}

static void format_mem_detail(char *buffer, size_t size,
                              unsigned long used_kb, unsigned long total_kb)
{
    unsigned long used_mib = used_kb / 1024;
    unsigned long total_mib = total_kb / 1024;
    snprintf(buffer, size, "%lu / %lu MiB used", used_mib, total_mib);
}

static void push_history(int32_t *history, int32_t value)
{
    for (int i = 0; i < MONITOR_HISTORY_POINTS - 1; ++i)
        history[i] = history[i + 1];
    history[MONITOR_HISTORY_POINTS - 1] = value;
}

static bool read_cpu_usage(unsigned *usage_out)
{
    char buffer[MONITOR_PROC_BUFFER];
    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;

    if (desktop_read_file("/proc/stat", buffer, sizeof(buffer)) <= 0)
        return false;

    if (sscanf(buffer, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4)
        return false;

    unsigned long long idle_all = idle + iowait;
    unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;

    if (!state.has_cpu_sample || total <= state.prev_total || idle_all < state.prev_idle) {
        state.prev_total = total;
        state.prev_idle = idle_all;
        state.has_cpu_sample = true;
        *usage_out = 0;
        return true;
    }

    unsigned long long total_delta = total - state.prev_total;
    unsigned long long idle_delta = idle_all - state.prev_idle;
    unsigned long long busy_delta = total_delta > idle_delta ? total_delta - idle_delta : 0;
    unsigned usage = total_delta ? (unsigned)((busy_delta * 100ULL) / total_delta) : 0;

    state.prev_total = total;
    state.prev_idle = idle_all;
    *usage_out = usage > 100U ? 100U : usage;
    return true;
}

static bool read_memory_usage(unsigned *used_pct_out,
                              unsigned long *free_kb_out,
                              unsigned long *used_kb_out,
                              unsigned long *total_kb_out)
{
    char buffer[MONITOR_PROC_BUFFER];
    unsigned long total_kb = 0, available_kb = 0;

    if (desktop_read_file("/proc/meminfo", buffer, sizeof(buffer)) <= 0)
        return false;

    if (!desktop_parse_key_kb(buffer, "MemTotal:", &total_kb) || total_kb == 0)
        return false;

    if (!desktop_parse_key_kb(buffer, "MemAvailable:", &available_kb))
        desktop_parse_key_kb(buffer, "MemFree:", &available_kb);

    if (available_kb > total_kb)
        available_kb = total_kb;

    unsigned long used_kb = total_kb - available_kb;
    unsigned used_pct = (unsigned)((used_kb * 100UL) / total_kb);

    if (used_pct_out)
        *used_pct_out = used_pct > 100U ? 100U : used_pct;
    if (free_kb_out)
        *free_kb_out = available_kb;
    if (used_kb_out)
        *used_kb_out = used_kb;
    if (total_kb_out)
        *total_kb_out = total_kb;
    return true;
}

static void update_monitor(lv_timer_t *timer)
{
    (void)timer;

    unsigned cpu_pct = 0;
    if (read_cpu_usage(&cpu_pct)) {
        char text[32];
        format_percent(text, sizeof(text), cpu_pct);
        monitor_set_metric(state.cpu_value, text);
        monitor_set_metric(state.cpu_detail, text);
        push_history(state.cpu_history, (int32_t)cpu_pct);
    } else {
        monitor_set_metric(state.cpu_value, "--");
        monitor_set_metric(state.cpu_detail, "--");
        push_history(state.cpu_history, 0);
    }
    if (state.cpu_chart)
        lv_chart_refresh(state.cpu_chart);

    unsigned mem_pct = 0;
    unsigned long free_kb = 0, used_kb = 0, total_kb = 0;
    if (read_memory_usage(&mem_pct, &free_kb, &used_kb, &total_kb)) {
        char pct_text[32];
        char free_text[32];
        char detail_text[64];
        format_percent(pct_text, sizeof(pct_text), mem_pct);
        format_mib(free_text, sizeof(free_text), free_kb);
        format_mem_detail(detail_text, sizeof(detail_text), used_kb, total_kb);

        monitor_set_metric(state.mem_value, pct_text);
        monitor_set_metric(state.free_mem_value, free_text);
        monitor_set_metric(state.mem_detail, detail_text);
        push_history(state.mem_history, (int32_t)mem_pct);
    } else {
        monitor_set_metric(state.mem_value, "--");
        monitor_set_metric(state.free_mem_value, "--");
        monitor_set_metric(state.mem_detail, "--");
        push_history(state.mem_history, 0);
    }
    if (state.mem_chart)
        lv_chart_refresh(state.mem_chart);
}

static lv_obj_t *create_chart_card(lv_obj_t *parent, const char *title,
                                   const char *subtitle, uint32_t accent,
                                   lv_obj_t **detail_out, lv_obj_t **chart_out,
                                   lv_chart_series_t **series_out,
                                   int32_t *history)
{
    lv_obj_t *card = desktop_card_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    lv_obj_t *header = lv_obj_create(card);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, 38);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_scrollable(header, false);

    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *subtitle_label = lv_label_create(header);
    lv_label_set_text(subtitle_label, subtitle);
    lv_obj_set_style_text_color(subtitle_label, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_align(subtitle_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *detail = lv_label_create(header);
    lv_label_set_text(detail, "--");
    lv_obj_set_style_text_color(detail, lv_color_hex(accent), 0);
    lv_obj_align(detail, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *chart = lv_chart_create(card);
    lv_obj_set_width(chart, lv_pct(100));
    lv_obj_set_height(chart, 168);
    lv_obj_set_style_radius(chart, 6, 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(DESKTOP_COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(DESKTOP_COLOR_CANVAS), 0);
    lv_obj_set_style_pad_all(chart, 8, 0);
    lv_obj_set_style_line_width(chart, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, MONITOR_HISTORY_POINTS);
    lv_chart_set_axis_min_value(chart, LV_CHART_AXIS_PRIMARY_Y, 0);
    lv_chart_set_axis_max_value(chart, LV_CHART_AXIS_PRIMARY_Y, 100);
    lv_chart_set_div_line_count(chart, 5, 6);

    lv_chart_series_t *series = lv_chart_add_series(chart, lv_color_hex(accent), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_ext_y_array(chart, series, history);

    if (detail_out)
        *detail_out = detail;
    if (chart_out)
        *chart_out = chart;
    if (series_out)
        *series_out = series;
    return card;
}

static void monitor_view_delete_cb(lv_event_t *event)
{
    (void)event;
    if (state.timer) {
        lv_timer_delete(state.timer);
        state.timer = NULL;
    }
}

static lv_obj_t *create(lv_obj_t *parent)
{
    memset(&state, 0, sizeof(state));

    lv_obj_t *view = lv_obj_create(parent);
    lv_obj_add_event_cb(view, monitor_view_delete_cb, LV_EVENT_DELETE, NULL);
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

    desktop_card_title_create(card, "Monitor", "Live CPU and memory usage");

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

    lv_obj_t *cpu_card = desktop_metric_card_create(metrics, LV_SYMBOL_CHARGE,
                                                    "CPU", "--", DESKTOP_COLOR_BLUE);
    state.cpu_value = lv_obj_get_child(cpu_card, 1);

    lv_obj_t *mem_card = desktop_metric_card_create(metrics, LV_SYMBOL_BARS,
                                                    "Memory", "--", DESKTOP_COLOR_TEAL);
    state.mem_value = lv_obj_get_child(mem_card, 1);

    lv_obj_t *free_card = desktop_metric_card_create(metrics, LV_SYMBOL_DRIVE,
                                                     "Free Memory", "--", DESKTOP_COLOR_AMBER);
    state.free_mem_value = lv_obj_get_child(free_card, 1);

    lv_obj_t *charts = lv_obj_create(card);
    lv_obj_set_width(charts, lv_pct(100));
    lv_obj_set_flex_grow(charts, 1);
    lv_obj_set_flex_flow(charts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(charts, 0, 0);
    lv_obj_set_style_pad_row(charts, 10, 0);
    lv_obj_set_style_bg_opa(charts, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(charts, 0, 0);
    lv_obj_set_scrollable(charts, false);

    create_chart_card(charts, "CPU Usage", "Rolling 36 second activity",
                       DESKTOP_COLOR_BLUE, &state.cpu_detail,
                       &state.cpu_chart, &state.cpu_series, state.cpu_history);
    create_chart_card(charts, "Memory Usage", "Live pressure snapshot",
                       DESKTOP_COLOR_TEAL, &state.mem_detail,
                       &state.mem_chart, &state.mem_series, state.mem_history);

    state.timer = lv_timer_create(update_monitor, 1000, NULL);

    return view;
}

desktop_app_t desktop_app_monitor = {
    .name = "Monitor",
    .symbol = LV_SYMBOL_BARS,
    .desc = "Live system metrics",
    .accent = DESKTOP_COLOR_BLUE,
    .create = create,
};
