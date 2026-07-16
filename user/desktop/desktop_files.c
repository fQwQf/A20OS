#include "desktop_apps.h"
#include "desktop_utils.h"
#include "lvgl.h"

#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct {
    char name[NAME_MAX + 1];
    char path[PATH_MAX];
    bool is_dir;
    off_t size;
} file_entry_t;

typedef enum {
    FILE_ACTION_UP,
    FILE_ACTION_DIR,
    FILE_ACTION_FILE,
} file_action_kind_t;

typedef struct {
    file_action_kind_t kind;
    char name[NAME_MAX + 1];
    char path[PATH_MAX];
    off_t size;
} file_action_t;

typedef struct {
    struct file_manager_state *state;
    file_action_t action;
} file_item_ctx_t;

typedef struct file_manager_state {
    lv_obj_t *view;
    lv_obj_t *path_label;
    lv_obj_t *list;
    char current_path[PATH_MAX];
} file_manager_state_t;

static void format_size(char *buffer, size_t size, off_t value)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double scaled = (double)value;
    size_t unit = 0;

    while (scaled >= 1024.0 && unit < (sizeof(units) / sizeof(units[0])) - 1) {
        scaled /= 1024.0;
        unit++;
    }

    if (unit == 0)
        snprintf(buffer, size, "%lld %s", (long long)value, units[unit]);
    else
        snprintf(buffer, size, "%.1f %s", scaled, units[unit]);
}

static void build_child_path(char *buffer, size_t size, const char *base, const char *name)
{
    if (strcmp(base, "/") == 0)
        snprintf(buffer, size, "/%s", name);
    else
        snprintf(buffer, size, "%s/%s", base, name);
}

static void path_parent(char *buffer, size_t size, const char *path)
{
    if (strcmp(path, "/") == 0) {
        snprintf(buffer, size, "/");
        return;
    }

    snprintf(buffer, size, "%s", path);

    char *slash = strrchr(buffer, '/');
    if (!slash || slash == buffer) {
        snprintf(buffer, size, "/");
        return;
    }

    *slash = '\0';
}

static int compare_entries(const void *lhs, const void *rhs)
{
    const file_entry_t *a = (const file_entry_t *)lhs;
    const file_entry_t *b = (const file_entry_t *)rhs;

    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;

    return strcmp(a->name, b->name);
}

static void action_delete_cb(lv_event_t *event)
{
    void *user_data = lv_event_get_user_data(event);
    free(user_data);
}

static void show_file_dialog(file_manager_state_t *state, const file_action_t *action)
{
    (void)state;

    char size_text[32];
    char text[PATH_MAX + 96];
    format_size(size_text, sizeof(size_text), action->size);
    snprintf(text, sizeof(text), "Name: %s\nPath: %s\nSize: %s", action->name,
             action->path, size_text);

    lv_obj_t *msgbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(msgbox, "File Details");
    lv_msgbox_add_text(msgbox, text);
    lv_msgbox_add_close_button(msgbox);
    lv_obj_center(msgbox);
}

static void refresh_list(file_manager_state_t *state);

static void list_entry_click_cb(lv_event_t *event)
{
    file_item_ctx_t *ctx = (file_item_ctx_t *)lv_event_get_user_data(event);
    file_manager_state_t *state;
    file_action_t *action;

    if (!ctx || !ctx->state)
        return;

    state = ctx->state;
    action = &ctx->action;

    switch (action->kind) {
    case FILE_ACTION_UP:
        path_parent(state->current_path, sizeof(state->current_path), state->current_path);
        refresh_list(state);
        break;
    case FILE_ACTION_DIR:
        snprintf(state->current_path, sizeof(state->current_path), "%s", action->path);
        refresh_list(state);
        break;
    case FILE_ACTION_FILE:
        show_file_dialog(state, action);
        break;
    }
}

static void add_list_button(file_manager_state_t *state, file_action_kind_t kind,
                            const char *icon, const char *name,
                            const char *detail, const char *path, off_t size)
{
    char text[NAME_MAX + 64];
    file_item_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx)
        return;

    memset(ctx, 0, sizeof(*ctx));
    ctx->state = state;
    ctx->action.kind = kind;
    ctx->action.size = size;
    snprintf(ctx->action.name, sizeof(ctx->action.name), "%s", name);
    snprintf(ctx->action.path, sizeof(ctx->action.path), "%s", path);

    snprintf(text, sizeof(text), "%s\n%s", name, detail);

    lv_obj_t *button = lv_list_add_button(state->list, icon, text);
    lv_obj_set_height(button, 56);
    lv_obj_set_style_bg_color(button, lv_color_hex(DESKTOP_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(DESKTOP_COLOR_SURFACE_MUTED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_text_color(button, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_pad_ver(button, 8, 0);
    lv_obj_add_event_cb(button, list_entry_click_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(button, action_delete_cb, LV_EVENT_DELETE, ctx);
}

static void add_info_message(file_manager_state_t *state, const char *message)
{
    lv_obj_t *label = lv_label_create(state->list);
    lv_label_set_text(label, message);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_color(label, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_pad_top(label, 12, 0);
    lv_obj_set_style_pad_bottom(label, 4, 0);
}

static void refresh_list(file_manager_state_t *state)
{
    DIR *dir;
    struct dirent *entry;
    file_entry_t *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;

    lv_label_set_text(state->path_label, state->current_path);
    lv_obj_clean(state->list);

    dir = opendir(state->current_path);
    if (!dir) {
        add_info_message(state, "Cannot open directory");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        file_entry_t item;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        memset(&item, 0, sizeof(item));
        snprintf(item.name, sizeof(item.name), "%s", entry->d_name);
        build_child_path(item.path, sizeof(item.path), state->current_path, entry->d_name);

        if (stat(item.path, &st) != 0)
            continue;

        item.is_dir = S_ISDIR(st.st_mode);
        item.size = st.st_size;

        if (count == capacity) {
            size_t next_capacity = capacity == 0 ? 16 : capacity * 2;
            file_entry_t *next_entries = realloc(entries, next_capacity * sizeof(*entries));
            if (!next_entries)
                break;
            entries = next_entries;
            capacity = next_capacity;
        }

        entries[count++] = item;
    }
    closedir(dir);

    qsort(entries, count, sizeof(*entries), compare_entries);

    if (strcmp(state->current_path, "/") != 0) {
        char parent_path[PATH_MAX];
        path_parent(parent_path, sizeof(parent_path), state->current_path);
        add_list_button(state, FILE_ACTION_UP, LV_SYMBOL_LEFT, "..", "Parent directory",
                        parent_path, 0);
    }

    for (size_t i = 0; i < count; i++) {
        char detail[32];
        const char *icon = entries[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
        file_action_kind_t kind = entries[i].is_dir ? FILE_ACTION_DIR : FILE_ACTION_FILE;

        if (entries[i].is_dir)
            snprintf(detail, sizeof(detail), "Directory");
        else
            format_size(detail, sizeof(detail), entries[i].size);

        add_list_button(state, kind, icon, entries[i].name, detail, entries[i].path,
                        entries[i].size);
    }

    if (count == 0)
        add_info_message(state, "This directory is empty");

    free(entries);
}

static void refresh_button_cb(lv_event_t *event)
{
    file_manager_state_t *state = (file_manager_state_t *)lv_event_get_user_data(event);
    refresh_list(state);
}

static void view_delete_cb(lv_event_t *event)
{
    file_manager_state_t *state = (file_manager_state_t *)lv_event_get_user_data(event);
    free(state);
}

static lv_obj_t *create(lv_obj_t *parent)
{
    file_manager_state_t *state = malloc(sizeof(*state));
    if (!state)
        return NULL;

    memset(state, 0, sizeof(*state));
    snprintf(state->current_path, sizeof(state->current_path), "/");

    lv_obj_t *view = lv_obj_create(parent);
    lv_obj_set_size(view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view, 0, 0);
    lv_obj_set_style_pad_all(view, 0, 0);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(view, false);
    lv_obj_add_event_cb(view, view_delete_cb, LV_EVENT_DELETE, state);

    state->view = view;

    lv_obj_t *card = desktop_card_create(view);
    lv_obj_set_size(card, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);

    lv_obj_t *header = lv_obj_create(card);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, 42);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_scrollable(header, false);

    lv_obj_t *title = desktop_card_title_create(header, "Files", "Browse filesystem");
    lv_obj_set_width(title, 220);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    state->path_label = lv_label_create(header);
    lv_label_set_text(state->path_label, "/");
    lv_obj_set_width(state->path_label, 220);
    lv_obj_set_style_text_color(state->path_label, lv_color_hex(DESKTOP_COLOR_TEXT_MUTED), 0);
    lv_label_set_long_mode(state->path_label, LV_LABEL_LONG_DOT);
    lv_obj_align(state->path_label, LV_ALIGN_RIGHT_MID, -86, 0);

    lv_obj_t *refresh = desktop_button_create(header, LV_SYMBOL_REFRESH, "Refresh", false);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(refresh, refresh_button_cb, LV_EVENT_CLICKED, state);

    state->list = lv_list_create(card);
    lv_obj_set_width(state->list, lv_pct(100));
    lv_obj_set_flex_grow(state->list, 1);
    lv_obj_set_style_border_width(state->list, 1, 0);
    lv_obj_set_style_border_color(state->list, lv_color_hex(DESKTOP_COLOR_BORDER), 0);
    lv_obj_set_style_radius(state->list, 6, 0);
    lv_obj_set_style_bg_color(state->list, lv_color_hex(DESKTOP_COLOR_SURFACE_MUTED), 0);
    lv_obj_set_style_pad_all(state->list, 8, 0);
    lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_AUTO);
    refresh_list(state);

    return view;
}

desktop_app_t desktop_app_files = {
    .name = "Files",
    .symbol = LV_SYMBOL_DIRECTORY,
    .desc = "Browse filesystem",
    .accent = DESKTOP_COLOR_PURPLE,
    .create = create,
};
