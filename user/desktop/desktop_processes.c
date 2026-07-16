#include "desktop_apps.h"
#include "desktop_utils.h"
#include "lvgl.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PROCESS_REFRESH_MS = 2000,
    PROCESS_NAME_MAX   = 64,
    PROCESS_STATE_MAX  = 64,
    PROCESS_CMD_MAX    = 512,
    PROCESS_STATUS_MAX = 2048,
    PROCESS_PATH_MAX   = 128,
    PROCESS_POPUP_MAX  = 1024,
};

typedef struct {
    int pid;
    char name[PROCESS_NAME_MAX];
    char state[PROCESS_STATE_MAX];
    unsigned long memory_kb;
    bool has_memory;
    char cmdline[PROCESS_CMD_MAX];
} process_info_t;

typedef struct {
    lv_obj_t *table;
    lv_obj_t *summary_value;
    lv_timer_t *timer;
    process_info_t *items;
    size_t count;
} process_manager_state_t;

static process_manager_state_t state;

static void process_manager_refresh(lv_timer_t *timer);

static bool is_numeric_name(const char *name)
{
    if (!name || !*name)
        return false;

    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (!isdigit(*p))
            return false;
    }
    return true;
}

static void copy_trimmed_value(char *dst, size_t dst_size, const char *value)
{
    if (!dst || dst_size == 0)
        return;

    dst[0] = '\0';
    if (!value)
        return;

    while (*value == ' ' || *value == '\t')
        value++;

    size_t len = strcspn(value, "\r\n");
    if (len >= dst_size)
        len = dst_size - 1;

    memcpy(dst, value, len);
    dst[len] = '\0';
}

static bool extract_status_value(const char *text, const char *key, char *dst, size_t dst_size)
{
    const char *pos = strstr(text, key);
    if (!pos)
        return false;

    pos += strlen(key);
    copy_trimmed_value(dst, dst_size, pos);
    return dst[0] != '\0';
}

static void read_cmdline_for_process(int pid, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0)
        return;

    char path[PROCESS_PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    FILE *file = fopen(path, "r");
    if (!file)
        return;

    dst[0] = '\0';

    size_t n = fread(dst, 1, dst_size - 1, file);
    fclose(file);

    if (n == 0)
        return;

    for (size_t i = 0; i < n; ++i) {
        if (dst[i] == '\0')
            dst[i] = ' ';
    }

    while (n > 0 && dst[n - 1] == ' ')
        n--;
    dst[n] = '\0';
}

static int compare_processes_by_pid(const void *lhs, const void *rhs)
{
    const process_info_t *a = (const process_info_t *)lhs;
    const process_info_t *b = (const process_info_t *)rhs;
    if (a->pid < b->pid)
        return -1;
    if (a->pid > b->pid)
        return 1;
    return 0;
}

static bool load_process_info(int pid, process_info_t *info)
{
    if (!info)
        return false;

    memset(info, 0, sizeof(*info));
    info->pid = pid;
    memcpy(info->name, "--", 3);
    memcpy(info->state, "--", 3);
    memcpy(info->cmdline, "N/A", 4);

    char path[PROCESS_PATH_MAX];
    char status[PROCESS_STATUS_MAX] = {0};
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    if (desktop_read_file(path, status, sizeof(status)) > 0) {
        extract_status_value(status, "Name:", info->name, sizeof(info->name));
        extract_status_value(status, "State:", info->state, sizeof(info->state));
        info->has_memory = desktop_parse_key_kb(status, "VmRSS:", &info->memory_kb);
    }

    read_cmdline_for_process(pid, info->cmdline, sizeof(info->cmdline));
    return true;
}

static void free_process_items(void)
{
    free(state.items);
    state.items = NULL;
    state.count = 0;
}

static void update_summary_label(void)
{
    if (!state.summary_value)
        return;
    lv_label_set_text_fmt(state.summary_value, "%u", (unsigned int)state.count);
}

static void populate_table(void)
{
    if (!state.table)
        return;

    int32_t scroll_y = lv_obj_get_scroll_y(state.table);

    lv_table_set_row_count(state.table, (uint32_t)state.count + 1);
    lv_table_set_column_count(state.table, 4);

    lv_table_set_cell_value(state.table, 0, 0, "PID");
    lv_table_set_cell_value(state.table, 0, 1, "Name");
    lv_table_set_cell_value(state.table, 0, 2, "State");
    lv_table_set_cell_value(state.table, 0, 3, "Memory (kB)");

    for (size_t i = 0; i < state.count; ++i) {
        char pid_text[16];
        char mem_text[32];
        const process_info_t *info = &state.items[i];

        snprintf(pid_text, sizeof(pid_text), "%d", info->pid);
        if (info->has_memory)
            snprintf(mem_text, sizeof(mem_text), "%lu", info->memory_kb);
        else
            snprintf(mem_text, sizeof(mem_text), "--");

        lv_table_set_cell_value(state.table, (uint32_t)i + 1, 0, pid_text);
        lv_table_set_cell_value(state.table, (uint32_t)i + 1, 1, info->name[0] ? info->name : "--");
        lv_table_set_cell_value(state.table, (uint32_t)i + 1, 2, info->state[0] ? info->state : "--");
        lv_table_set_cell_value(state.table, (uint32_t)i + 1, 3, mem_text);
    }

    update_summary_label();
    lv_obj_scroll_to_y(state.table, scroll_y, LV_ANIM_OFF);
}

static void scan_processes(void)
{
    DIR *dir = opendir("/proc");
    if (!dir) {
        free_process_items();
        populate_table();
        return;
    }

    process_info_t *items = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (!is_numeric_name(entry->d_name))
            continue;

        int pid = atoi(entry->d_name);
        if (pid <= 0)
            continue;

        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 32;
            process_info_t *new_items = realloc(items, new_capacity * sizeof(*items));
            if (!new_items)
                break;
            items = new_items;
            capacity = new_capacity;
        }

        if (load_process_info(pid, &items[count]))
            count++;
    }

    closedir(dir);

    qsort(items, count, sizeof(*items), compare_processes_by_pid);
    free_process_items();
    state.items = items;
    state.count = count;
    populate_table();
}

static void msgbox_close_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
        return;

    lv_obj_t *msgbox = (lv_obj_t *)lv_event_get_user_data(event);
    if (msgbox)
        lv_msgbox_close(msgbox);
}

static void show_process_popup(const process_info_t *info)
{
    if (!info)
        return;

    lv_obj_t *msgbox = lv_msgbox_create(NULL);
    char title[48];
    char body[PROCESS_POPUP_MAX];
    char memory_text[32];

    snprintf(title, sizeof(title), "Process %d", info->pid);
    if (info->has_memory)
        snprintf(memory_text, sizeof(memory_text), "%lu kB", info->memory_kb);
    else
        snprintf(memory_text, sizeof(memory_text), "N/A");

    snprintf(body, sizeof(body),
             "Name: %s\n"
             "State: %s\n"
             "VmRSS: %s\n"
             "Cmdline: %s",
             info->name[0] ? info->name : "--",
             info->state[0] ? info->state : "--",
             memory_text,
             info->cmdline[0] ? info->cmdline : "N/A");

    lv_msgbox_add_title(msgbox, title);
    lv_msgbox_add_close_button(msgbox);
    lv_msgbox_add_text(msgbox, body);
    lv_obj_t *close_btn = lv_msgbox_add_footer_button(msgbox, "Close");
    lv_obj_add_event_cb(close_btn, msgbox_close_cb, LV_EVENT_CLICKED, msgbox);

    lv_obj_set_width(msgbox, lv_pct(72));
    lv_obj_set_style_bg_color(msgbox, lv_color_hex(DESKTOP_COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(msgbox, lv_color_hex(DESKTOP_COLOR_BORDER), 0);
    lv_obj_center(msgbox);
}

static void process_table_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED)
        return;

    uint32_t row = 0;
    uint32_t col = 0;
    lv_obj_t *table = lv_event_get_target_obj(event);
    lv_table_get_selected_cell(table, &row, &col);
    (void)col;

    if (row == 0 || row > state.count)
        return;

    show_process_popup(&state.items[row - 1]);
}

static void process_view_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE)
        return;

    (void)event;
    if (state.timer) {
        lv_timer_delete(state.timer);
        state.timer = NULL;
    }

    state.table = NULL;
    state.summary_value = NULL;
    free_process_items();
}

static void process_manager_refresh(lv_timer_t *timer)
{
    (void)timer;
    scan_processes();
}

static lv_obj_t *create(lv_obj_t *parent)
{
    if (state.timer) {
        lv_timer_delete(state.timer);
        state.timer = NULL;
    }
    free_process_items();

    lv_obj_t *view = lv_obj_create(parent);
    lv_obj_set_size(view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view, 0, 0);
    lv_obj_set_style_pad_all(view, 0, 0);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(view, false);
    lv_obj_add_event_cb(view, process_view_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *card = desktop_card_create(view);
    lv_obj_set_size(card, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);

    desktop_card_title_create(card, "Processes", "Running tasks");

    lv_obj_t *summary = desktop_label_pair_create(card, "Visible processes", "--");
    state.summary_value = lv_obj_get_child(summary, 1);

    state.table = lv_table_create(card);
    lv_obj_set_width(state.table, lv_pct(100));
    lv_obj_set_flex_grow(state.table, 1);
    lv_obj_set_scrollbar_mode(state.table, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(state.table, lv_color_hex(DESKTOP_COLOR_SURFACE_MUTED), 0);
    lv_obj_set_style_border_width(state.table, 1, 0);
    lv_obj_set_style_border_color(state.table, lv_color_hex(DESKTOP_COLOR_BORDER), 0);
    lv_obj_set_style_radius(state.table, 6, 0);
    lv_obj_set_style_text_color(state.table, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_line_color(state.table, lv_color_hex(DESKTOP_COLOR_BORDER), 0);
    lv_obj_set_style_pad_all(state.table, 0, 0);
    lv_obj_add_event_cb(state.table, process_table_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_table_set_column_count(state.table, 4);
    lv_table_set_column_width(state.table, 0, 72);
    lv_table_set_column_width(state.table, 1, 180);
    lv_table_set_column_width(state.table, 2, 170);
    lv_table_set_column_width(state.table, 3, 120);

    scan_processes();
    state.timer = lv_timer_create(process_manager_refresh, PROCESS_REFRESH_MS, NULL);

    return view;
}

desktop_app_t desktop_app_processes = {
    .name = "Processes",
    .symbol = LV_SYMBOL_LIST,
    .desc = "Running tasks",
    .accent = DESKTOP_COLOR_AMBER,
    .create = create,
};
