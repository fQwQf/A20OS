#include "desktop_apps.h"
#include "desktop_utils.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    NETWORK_MAX_INTERFACES = 32,
    NETWORK_REFRESH_MS = 2000,
    NETWORK_BUFFER_SIZE = 8192,
};

typedef struct {
    char name[32];
    unsigned long long rx_bytes;
    unsigned long long rx_packets;
    unsigned long long tx_bytes;
    unsigned long long tx_packets;
    unsigned long long rx_rate;
    unsigned long long tx_rate;
    bool is_loopback;
    bool is_up;
} network_iface_t;

typedef struct {
    int count;
    network_iface_t items[NETWORK_MAX_INTERFACES];
} network_snapshot_t;

typedef struct {
    lv_obj_t *view;
    lv_obj_t *status_title;
    lv_obj_t *status_detail;
    lv_obj_t *metric_interfaces;
    lv_obj_t *metric_rx_total;
    lv_obj_t *metric_tx_total;
    lv_obj_t *list;
    lv_obj_t *empty_label;
    lv_timer_t *timer;
    network_snapshot_t previous;
    uint64_t previous_ms;
} network_state_t;

static uint64_t network_now_ms(void)
{
    return lv_tick_get();
}

static void network_trim(char *text)
{
    char *start = text;
    while (*start == ' ' || *start == '\t')
        start++;

    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end = '\0';

    if (start != text)
        memmove(text, start, (size_t)(end - start) + 1);
}

static const network_iface_t *network_find_previous(const network_snapshot_t *snapshot,
                                                    const char *name)
{
    int i;
    for (i = 0; i < snapshot->count; ++i) {
        if (strcmp(snapshot->items[i].name, name) == 0)
            return &snapshot->items[i];
    }
    return NULL;
}

static void network_format_bytes(unsigned long long bytes, char *buffer, size_t size)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit < (sizeof(units) / sizeof(units[0])) - 1) {
        value /= 1024.0;
        unit++;
    }

    if (unit == 0)
        snprintf(buffer, size, "%llu %s", bytes, units[unit]);
    else
        snprintf(buffer, size, "%.1f %s", value, units[unit]);
}

static void network_format_rate(unsigned long long bytes_per_sec, char *buffer, size_t size)
{
    char amount[32];
    network_format_bytes(bytes_per_sec, amount, sizeof(amount));
    snprintf(buffer, size, "%s/s", amount);
}

static int network_read_snapshot(network_snapshot_t *snapshot)
{
    char buffer[NETWORK_BUFFER_SIZE];
    int length = desktop_read_file("/proc/net/dev", buffer, sizeof(buffer));
    char *line;

    snapshot->count = 0;
    if (length <= 0)
        return -1;

    line = buffer;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next)
            *next = '\0';

        if (strchr(line, ':') != NULL) {
            char name[32] = {0};
            unsigned long long rx_bytes = 0, rx_packets = 0;
            unsigned long long tx_bytes = 0, tx_packets = 0;
            char *colon = strchr(line, ':');
            size_t name_len = (size_t)(colon - line);

            if (name_len >= sizeof(name))
                name_len = sizeof(name) - 1;
            memcpy(name, line, name_len);
            name[name_len] = '\0';
            network_trim(name);

            if (name[0] != '\0' &&
                sscanf(colon + 1,
                       " %llu %llu %*u %*u %*u %*u %*u %*u %llu %llu",
                       &rx_bytes, &rx_packets, &tx_bytes, &tx_packets) == 4 &&
                snapshot->count < NETWORK_MAX_INTERFACES) {
                network_iface_t *iface = &snapshot->items[snapshot->count++];
                memset(iface, 0, sizeof(*iface));
                snprintf(iface->name, sizeof(iface->name), "%s", name);
                iface->rx_bytes = rx_bytes;
                iface->rx_packets = rx_packets;
                iface->tx_bytes = tx_bytes;
                iface->tx_packets = tx_packets;
                iface->is_loopback = strcmp(name, "lo") == 0;
                iface->is_up = iface->is_loopback || rx_bytes > 0 || tx_bytes > 0;
            }
        }

        line = next ? next + 1 : NULL;
    }

    return snapshot->count;
}

static void network_compute_rates(network_state_t *state, network_snapshot_t *snapshot,
                                  uint64_t now_ms)
{
    int i;
    uint64_t delta_ms = state->previous_ms > 0 && now_ms > state->previous_ms
                      ? now_ms - state->previous_ms
                      : 0;

    for (i = 0; i < snapshot->count; ++i) {
        network_iface_t *iface = &snapshot->items[i];
        const network_iface_t *prev = network_find_previous(&state->previous, iface->name);
        if (!prev || delta_ms == 0)
            continue;

        if (iface->rx_bytes >= prev->rx_bytes) {
            unsigned long long delta = iface->rx_bytes - prev->rx_bytes;
            iface->rx_rate = (unsigned long long)((delta * 1000ULL) / delta_ms);
        }
        if (iface->tx_bytes >= prev->tx_bytes) {
            unsigned long long delta = iface->tx_bytes - prev->tx_bytes;
            iface->tx_rate = (unsigned long long)((delta * 1000ULL) / delta_ms);
        }
    }

    memcpy(&state->previous, snapshot, sizeof(*snapshot));
    state->previous_ms = now_ms;
}

static lv_obj_t *network_create_column(lv_obj_t *parent, const char *title, uint32_t accent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_width(panel, lv_pct(48));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(DESKTOP_COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(DESKTOP_COLOR_SURFACE_MUTED), 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 4, 0);
    lv_obj_set_scrollable(panel, false);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *label = lv_label_create(panel);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(accent), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    return panel;
}

static void network_populate_interface_card(lv_obj_t *parent, const network_iface_t *iface)
{
    char text[64];
    char badge_text[16];
    uint32_t badge_color;

    lv_obj_t *card = desktop_card_create(parent);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = lv_obj_create(card);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, 30);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_scrollable(header, false);

    lv_obj_t *icon = lv_label_create(header);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(icon, lv_color_hex(DESKTOP_COLOR_BLUE), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *name = lv_label_create(header);
    lv_label_set_text(name, iface->name);
    lv_obj_set_style_text_color(name, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 24, 0);

    if (iface->is_loopback) {
        snprintf(badge_text, sizeof(badge_text), "Loopback");
        badge_color = DESKTOP_COLOR_BLUE;
    } else if (iface->is_up) {
        snprintf(badge_text, sizeof(badge_text), "Up");
        badge_color = DESKTOP_COLOR_TEAL;
    } else {
        snprintf(badge_text, sizeof(badge_text), "Down");
        badge_color = DESKTOP_COLOR_AMBER;
    }

    lv_obj_t *badge = desktop_badge_create(header, badge_text, badge_color);
    lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *columns = lv_obj_create(card);
    lv_obj_set_width(columns, lv_pct(100));
    lv_obj_set_height(columns, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(columns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(columns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(columns, 0, 0);
    lv_obj_set_style_pad_all(columns, 0, 0);
    lv_obj_set_style_pad_column(columns, 10, 0);
    lv_obj_set_scrollable(columns, false);

    lv_obj_t *rx_panel = network_create_column(columns, "RX", DESKTOP_COLOR_TEAL);
    network_format_bytes(iface->rx_bytes, text, sizeof(text));
    desktop_label_pair_create(rx_panel, "Bytes", text);
    snprintf(text, sizeof(text), "%llu", iface->rx_packets);
    desktop_label_pair_create(rx_panel, "Packets", text);
    network_format_rate(iface->rx_rate, text, sizeof(text));
    desktop_label_pair_create(rx_panel, "Rate", text);

    lv_obj_t *tx_panel = network_create_column(columns, "TX", DESKTOP_COLOR_AMBER);
    network_format_bytes(iface->tx_bytes, text, sizeof(text));
    desktop_label_pair_create(tx_panel, "Bytes", text);
    snprintf(text, sizeof(text), "%llu", iface->tx_packets);
    desktop_label_pair_create(tx_panel, "Packets", text);
    network_format_rate(iface->tx_rate, text, sizeof(text));
    desktop_label_pair_create(tx_panel, "Rate", text);
}

static void network_clear_list(lv_obj_t *list)
{
    uint32_t count = lv_obj_get_child_count(list);
    while (count > 0) {
        lv_obj_delete(lv_obj_get_child(list, count - 1));
        count--;
    }
}

static void network_render(network_state_t *state)
{
    network_snapshot_t snapshot;
    unsigned long long total_rx = 0;
    unsigned long long total_tx = 0;
    int active = 0;
    int loopback = 0;
    int i;
    char text[96];

    if (network_read_snapshot(&snapshot) < 0) {
        lv_label_set_text(state->status_title, "Network data unavailable");
        lv_label_set_text(state->status_detail,
                          "Could not read /proc/net/dev. Check procfs availability.");
        lv_label_set_text(state->metric_interfaces, "0");
        lv_label_set_text(state->metric_rx_total, "--");
        lv_label_set_text(state->metric_tx_total, "--");
        lv_obj_add_flag(state->list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(state->empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(state->empty_label,
                          "Network interface details are unavailable right now.");
        return;
    }

    network_compute_rates(state, &snapshot, network_now_ms());
    network_clear_list(state->list);

    for (i = 0; i < snapshot.count; ++i) {
        total_rx += snapshot.items[i].rx_bytes;
        total_tx += snapshot.items[i].tx_bytes;
        if (snapshot.items[i].is_loopback)
            loopback++;
        else if (snapshot.items[i].is_up)
            active++;
        network_populate_interface_card(state->list, &snapshot.items[i]);
    }

    snprintf(text, sizeof(text), "%d", snapshot.count);
    lv_label_set_text(state->metric_interfaces, text);
    network_format_bytes(total_rx, text, sizeof(text));
    lv_label_set_text(state->metric_rx_total, text);
    network_format_bytes(total_tx, text, sizeof(text));
    lv_label_set_text(state->metric_tx_total, text);

    if (snapshot.count == 0) {
        lv_label_set_text(state->status_title, "No interfaces detected");
        lv_label_set_text(state->status_detail,
                          "procfs is present, but no network interfaces were reported.");
        lv_obj_add_flag(state->list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(state->empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(state->empty_label,
                          "No interface entries were found in /proc/net/dev.");
        return;
    }

    snprintf(text, sizeof(text), "%d interfaces, %d active, %d loopback",
             snapshot.count, active, loopback);
    lv_label_set_text(state->status_title, "Network status overview");
    lv_label_set_text(state->status_detail, text);
    lv_obj_remove_flag(state->list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(state->empty_label, LV_OBJ_FLAG_HIDDEN);
}

static void network_timer_cb(lv_timer_t *timer)
{
    network_state_t *state = (network_state_t *)lv_timer_get_user_data(timer);
    if (!state)
        return;
    network_render(state);
}

static void network_view_delete_cb(lv_event_t *event)
{
    network_state_t *state = (network_state_t *)lv_event_get_user_data(event);
    if (!state)
        return;

    if (state->timer) {
        lv_timer_delete(state->timer);
        state->timer = NULL;
    }
    lv_free(state);
}

static lv_obj_t *create(lv_obj_t *parent)
{
    network_state_t *state = lv_malloc(sizeof(*state));
    if (!state)
        return NULL;
    memset(state, 0, sizeof(*state));

    lv_obj_t *view = lv_obj_create(parent);
    state->view = view;
    lv_obj_set_size(view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view, 0, 0);
    lv_obj_set_style_pad_all(view, 0, 0);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(view, false);
    lv_obj_add_event_cb(view, network_view_delete_cb, LV_EVENT_DELETE, state);

    lv_obj_t *card = desktop_card_create(view);
    lv_obj_set_size(card, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    desktop_card_title_create(card, "Network", "Interfaces and live traffic totals");

    lv_obj_t *status_card = lv_obj_create(card);
    lv_obj_set_width(status_card, lv_pct(100));
    lv_obj_set_height(status_card, 88);
    lv_obj_set_style_radius(status_card, 7, 0);
    lv_obj_set_style_border_width(status_card, 0, 0);
    lv_obj_set_style_bg_color(status_card, lv_color_hex(DESKTOP_COLOR_BLUE_SOFT), 0);
    lv_obj_set_style_pad_hor(status_card, 16, 0);
    lv_obj_set_style_pad_ver(status_card, 12, 0);
    lv_obj_set_scrollable(status_card, false);

    lv_obj_t *mark = lv_obj_create(status_card);
    lv_obj_set_size(mark, 42, 42);
    lv_obj_set_style_radius(mark, 8, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_bg_color(mark, lv_color_hex(DESKTOP_COLOR_BLUE), 0);
    lv_obj_set_scrollable(mark, false);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *icon = lv_label_create(mark);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
    lv_obj_center(icon);

    state->status_title = lv_label_create(status_card);
    lv_label_set_text(state->status_title, "Scanning network interfaces");
    lv_obj_set_style_text_color(state->status_title, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(state->status_title, &lv_font_montserrat_16, 0);
    lv_obj_align(state->status_title, LV_ALIGN_TOP_LEFT, 58, 0);

    state->status_detail = lv_label_create(status_card);
    lv_label_set_text(state->status_detail, "Preparing interface summary from procfs");
    lv_obj_set_width(state->status_detail, lv_pct(72));
    lv_label_set_long_mode(state->status_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(state->status_detail, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_align(state->status_detail, LV_ALIGN_TOP_LEFT, 58, 24);

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

    lv_obj_t *interfaces_card = desktop_metric_card_create(metrics, "NET",
                                                           "Interfaces", "--",
                                                           DESKTOP_COLOR_BLUE);
    state->metric_interfaces = lv_obj_get_child(interfaces_card, 1);

    lv_obj_t *rx_card = desktop_metric_card_create(metrics, "RX",
                                                   "Received", "--",
                                                   DESKTOP_COLOR_TEAL);
    state->metric_rx_total = lv_obj_get_child(rx_card, 1);

    lv_obj_t *tx_card = desktop_metric_card_create(metrics, "TX",
                                                   "Transmitted", "--",
                                                   DESKTOP_COLOR_AMBER);
    state->metric_tx_total = lv_obj_get_child(tx_card, 1);

    lv_obj_t *list_card = desktop_card_create(card);
    lv_obj_set_width(list_card, lv_pct(100));
    lv_obj_set_flex_grow(list_card, 1);
    lv_obj_set_flex_flow(list_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_card, 8, 0);

    desktop_card_title_create(list_card, "Interface details", "RX and TX counters by adapter");

    state->empty_label = lv_label_create(list_card);
    lv_label_set_text(state->empty_label, "Loading interface details...");
    lv_obj_set_style_text_color(state->empty_label, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_pad_top(state->empty_label, 6, 0);

    state->list = lv_obj_create(list_card);
    lv_obj_set_width(state->list, lv_pct(100));
    lv_obj_set_flex_grow(state->list, 1);
    lv_obj_set_flex_flow(state->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(state->list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->list, 0, 0);
    lv_obj_set_style_pad_all(state->list, 0, 0);
    lv_obj_set_style_pad_row(state->list, 8, 0);
    lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_AUTO);

    network_render(state);
    state->timer = lv_timer_create(network_timer_cb, NETWORK_REFRESH_MS, state);

    return view;
}

desktop_app_t desktop_app_network = {
    .name = "Network",
    .symbol = LV_SYMBOL_WIFI,
    .desc = "Interfaces and tools",
    .accent = DESKTOP_COLOR_BLUE,
    .create = create,
};
