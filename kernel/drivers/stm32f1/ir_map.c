/* Data-driven NEC remote binding parser. See ir.h. */
#include "drivers/stm32f1/ir.h"

static ir_map_binding_t ir_bindings[IR_MAP_MAX];
static unsigned ir_binding_count;

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int span_eq(const char *s, const char *e, const char *name) {
    while (s < e && *name) {
        if (upper(*s) != *name)
            return 0;
        s++;
        name++;
    }
    return s == e && *name == '\0';
}

static ir_action_t action_from_span(const char *s, const char *e) {
    if (span_eq(s, e, "FAN_UP"))
        return IR_ACT_FAN_UP;
    if (span_eq(s, e, "FAN_DOWN"))
        return IR_ACT_FAN_DOWN;
    if (span_eq(s, e, "PUMP_TOGGLE"))
        return IR_ACT_PUMP_TOGGLE;
    if (span_eq(s, e, "THEME_CYCLE"))
        return IR_ACT_THEME_CYCLE;
    if (span_eq(s, e, "TALK"))
        return IR_ACT_TALK;
    if (span_eq(s, e, "MUTE_BUZZER"))
        return IR_ACT_MUTE_BUZZER;
    if (span_eq(s, e, "MENU"))
        return IR_ACT_MENU;
    if (span_eq(s, e, "LIGHT_UP"))
        return IR_ACT_LIGHT_UP;
    if (span_eq(s, e, "LIGHT_DOWN"))
        return IR_ACT_LIGHT_DOWN;
    return IR_ACT_NONE;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_code(const char *s, const char *e, uint32_t *code) {
    unsigned digits = (unsigned)(e - s);
    if (digits < 3U || digits > 10U || s[0] != '0' ||
        (s[1] != 'x' && s[1] != 'X'))
        return 0;
    uint32_t value = 0;
    for (unsigned i = 2; i < digits; i++) {
        int digit = hex_digit(s[i]);
        if (digit < 0)
            return 0;
        value = (value << 4) | (uint32_t)digit;
    }
    *code = value;
    return 1;
}

int ir_map_parse(const char *text, unsigned len, ir_map_binding_t *table,
                 unsigned max) {
    if (!text || !table || max == 0)
        return -1;
    for (unsigned i = 0; i < max; i++) {
        table[i].code = 0;
        table[i].action = IR_ACT_NONE;
    }

    unsigned count = 0;
    unsigned pos = 0;
    while (pos < len) {
        unsigned line_start = pos;
        while (pos < len && text[pos] != '\n')
            pos++;
        const char *p = text + line_start;
        const char *end = text + pos;
        if (pos < len)
            pos++;

        while (p < end && is_space(*p))
            p++;
        if (p == end || *p == '#')
            continue;
        const char *code_start = p;
        while (p < end && !is_space(*p) && *p != '#')
            p++;
        const char *code_end = p;
        while (p < end && is_space(*p))
            p++;
        const char *action_start = p;
        while (p < end && !is_space(*p) && *p != '#')
            p++;
        const char *action_end = p;

        uint32_t code;
        ir_action_t action = action_from_span(action_start, action_end);
        if (!parse_code(code_start, code_end, &code) || action == IR_ACT_NONE)
            continue;
        unsigned slot = count;
        for (unsigned i = 0; i < count; i++) {
            if (table[i].code == code) {
                slot = i;
                break;
            }
        }
        if (slot == count) {
            if (count >= max)
                continue;
            count++;
        }
        table[slot].code = code;
        table[slot].action = action;
    }
    return (int)count;
}

int ir_map_load_default(void) {
    static const char defaults[] = IR_DEFAULT_MAP;
    int result = ir_map_parse(defaults, sizeof(defaults) - 1U, ir_bindings,
                              IR_MAP_MAX);
    ir_binding_count = result > 0 ? (unsigned)result : 0;
    return result;
}

const char *ir_action_name(int action) {
    switch (action) {
    case IR_ACT_FAN_UP:      return "FAN_UP";
    case IR_ACT_FAN_DOWN:    return "FAN_DOWN";
    case IR_ACT_PUMP_TOGGLE: return "PUMP_TOGGLE";
    case IR_ACT_THEME_CYCLE: return "THEME_CYCLE";
    case IR_ACT_TALK:        return "TALK";
    case IR_ACT_MUTE_BUZZER: return "MUTE_BUZZER";
    case IR_ACT_MENU:        return "MENU";
    case IR_ACT_LIGHT_UP:    return "LIGHT_UP";
    case IR_ACT_LIGHT_DOWN:  return "LIGHT_DOWN";
    default:                 return "unbound";
    }
}

int ir_map_dispatch(uint32_t code) {
    for (unsigned i = 0; i < ir_binding_count; i++) {
        if (ir_bindings[i].code == code)
            return ir_bindings[i].action;
    }
    return IR_ACT_NONE;
}

unsigned ir_map_count(void) { return ir_binding_count; }

#ifdef CONFIG_MCU
int ir_map_load_fs(fat32lite_fs_t *fs, const char *path) {
    char text[512];
    fat32lite_file_t file;
    uint32_t total;
    uint32_t size;
    int result;

    if (!fs || !path)
        return FAT32LITE_EINVAL;
    result = fat32lite_open(fs, path, &file);
    if (result != FAT32LITE_OK)
        return result;

    size = fat32lite_size(&file);
    if (size > sizeof(text)) {
        fat32lite_close(&file);
        return FAT32LITE_EINVAL;
    }
    total = 0;
    while (total < size) {
        int n = fat32lite_read(&file, text + total, size - total);
        if (n < 0) {
            fat32lite_close(&file);
            return n;
        }
        if (n == 0)
            break;
        total += (uint32_t)n;
    }
    result = fat32lite_close(&file);
    if (result != FAT32LITE_OK)
        return result;
    if (total != size)
        return FAT32LITE_EIO;
    result = ir_map_parse(text, total, ir_bindings, IR_MAP_MAX);
    ir_binding_count = result > 0 ? (unsigned)result : 0;
    return result;
}
#else
int ir_map_load_fs(fat32lite_fs_t *fs, const char *path) {
    (void)fs;
    (void)path;
    return FAT32LITE_EINVAL;
}
#endif
