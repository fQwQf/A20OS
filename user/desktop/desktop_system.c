#include "desktop_apps.h"
#include "desktop_utils.h"
#include "lvgl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

static void add_info_section(lv_obj_t *parent, const char *title, const char *accent_symbol)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_set_width(block, lv_pct(100));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_pad_ver(block, 8, 0);
    lv_obj_set_scrollable(block, false);

    lv_obj_t *header = lv_obj_create(block);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, 20);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_scrollable(header, false);

    lv_obj_t *icon = lv_label_create(header);
    lv_label_set_text(icon, accent_symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(DESKTOP_COLOR_TEAL), 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *label = lv_label_create(header);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(DESKTOP_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);
}

static void trim_newline(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static lv_obj_t *create(lv_obj_t *parent)
{
    lv_obj_t *view = lv_obj_create(parent);
    lv_obj_set_size(view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view, 0, 0);
    lv_obj_set_style_pad_all(view, 0, 0);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(view, false);

    lv_obj_t *card = desktop_card_create(view);
    lv_obj_set_size(card, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, 0);

    desktop_card_title_create(card, "System", "Kernel and hardware information");

    lv_obj_t *scroll = lv_obj_create(card);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_set_style_pad_row(scroll, 4, 0);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);

    struct utsname un;
    if (uname(&un) == 0) {
        add_info_section(scroll, "Operating System", LV_SYMBOL_GPS);
        desktop_label_pair_create(scroll, "Kernel", un.sysname);
        desktop_label_pair_create(scroll, "Release", un.release);
        desktop_label_pair_create(scroll, "Version", un.version);
        desktop_label_pair_create(scroll, "Architecture", un.machine);

        char pretty[128] = {0};
        desktop_read_file("/etc/os-release", pretty, sizeof(pretty));
        const char *name = strstr(pretty, "PRETTY_NAME=\"");
        char value[96] = "A20OS";
        if (name) {
            name += 13;
            char *end = strchr(name, '"');
            if (end) {
                size_t len = (size_t)(end - name);
                if (len >= sizeof(value))
                    len = sizeof(value) - 1;
                memcpy(value, name, len);
                value[len] = '\0';
            }
        }
        desktop_label_pair_create(scroll, "Distribution", value);
    }

    add_info_section(scroll, "Processor", LV_SYMBOL_CHARGE);
    char cpuinfo[4096] = {0};
    if (desktop_read_file("/proc/cpuinfo", cpuinfo, sizeof(cpuinfo)) > 0) {
        char model[128] = "Unknown";
        int cores = 0;
        char *line = cpuinfo;
        while (line && *line) {
            char *next = strchr(line, '\n');
            if (strncmp(line, "model name\t:", 12) == 0) {
                char *val = line + 13;
                while (*val == ' ' || *val == '\t')
                    val++;
                size_t len = next ? (size_t)(next - val) : strlen(val);
                if (len >= sizeof(model))
                    len = sizeof(model) - 1;
                memcpy(model, val, len);
                model[len] = '\0';
                trim_newline(model);
            }
            if (strncmp(line, "processor\t:", 11) == 0)
                cores++;
            line = next ? next + 1 : NULL;
        }
        desktop_label_pair_create(scroll, "Model", model);
        char cores_text[32];
        snprintf(cores_text, sizeof(cores_text), "%d", cores ? cores : 1);
        desktop_label_pair_create(scroll, "Logical cores", cores_text);
    } else {
        desktop_label_pair_create(scroll, "Model", "Unknown");
        desktop_label_pair_create(scroll, "Logical cores", "1");
    }

    add_info_section(scroll, "Memory", LV_SYMBOL_DRIVE);
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        unsigned long unit = info.mem_unit ? info.mem_unit : 1;
        unsigned long total_kb = info.totalram * unit / 1024;
        char text[64];
        snprintf(text, sizeof(text), "%lu MiB", total_kb / 1024);
        desktop_label_pair_create(scroll, "Total RAM", text);
    } else {
        desktop_label_pair_create(scroll, "Total RAM", "Unknown");
    }

    add_info_section(scroll, "Mounts", LV_SYMBOL_DIRECTORY);
    char mounts[4096] = {0};
    if (desktop_read_file("/proc/mounts", mounts, sizeof(mounts)) > 0) {
        char *line = mounts;
        while (line && *line) {
            char *next = strchr(line, '\n');
            if (line[0] != ' ' && line[0] != '\t' && line[0] != '\0') {
                char device[64] = {0};
                char point[64] = {0};
                char fstype[32] = {0};
                if (sscanf(line, "%63s %63s %31s", device, point, fstype) == 3) {
                    char label[128];
                    snprintf(label, sizeof(label), "%s (%s)", point, fstype);
                    desktop_label_pair_create(scroll, label, device);
                }
            }
            line = next ? next + 1 : NULL;
        }
    } else {
        desktop_label_pair_create(scroll, "No mount info", "Unavailable");
    }

    return view;
}

desktop_app_t desktop_app_system = {
    .name = "System",
    .symbol = LV_SYMBOL_SETTINGS,
    .desc = "Kernel and hardware info",
    .accent = DESKTOP_COLOR_TEXT_MUTED,
    .create = create,
};
