#include "bluetooth_cmd.h"
#include "drivers/stm32f1/ir.h"

static char upper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

static const char *skip_space(const char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int at_end(const char *s) {
    s = skip_space(s);
    while (*s == '\r' || *s == '\n')
        s++;
    return *s == '\0';
}

static int take_word(const char **input, const char *word) {
    const char *s = skip_space(*input);
    const char *w = word;
    while (*w && upper(*s) == *w) {
        s++;
        w++;
    }
    if (*w || (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n'))
        return 0;
    *input = s;
    return 1;
}

static int take_uint(const char **input, int *value) {
    const char *s = skip_space(*input);
    int n = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s++ - '0');
        digits++;
        if (n > 1000)
            return 0;
    }
    if (!digits)
        return 0;
    *input = s;
    *value = n;
    return 1;
}

static int exact(const char *line, const char *word) {
    return take_word(&line, word) && at_end(line);
}

static int action_alias(const char *line) {
    if (exact(line, "FAN_UP") || exact(line, "FAN+")) return IR_ACT_FAN_UP;
    if (exact(line, "FAN_DOWN") || exact(line, "FAN-")) return IR_ACT_FAN_DOWN;
    if (exact(line, "PUMP")) return IR_ACT_PUMP_TOGGLE;
    if (exact(line, "THEME")) return IR_ACT_THEME_CYCLE;
    if (exact(line, "TALK")) return IR_ACT_TALK;
    if (exact(line, "MUTE")) return IR_ACT_MUTE_BUZZER;
    if (exact(line, "MENU")) return IR_ACT_MENU;
    if (exact(line, "LIGHT_UP") || exact(line, "BL+")) return IR_ACT_LIGHT_UP;
    if (exact(line, "LIGHT_DOWN") || exact(line, "BL-")) return IR_ACT_LIGHT_DOWN;
    return IR_ACT_NONE;
}

int bluetooth_cmd_parse(const char *line, bluetooth_cmd_t *out) {
    static const int digit_actions[9] = {
        IR_ACT_FAN_UP, IR_ACT_FAN_DOWN, IR_ACT_PUMP_TOGGLE,
        IR_ACT_THEME_CYCLE, IR_ACT_TALK, IR_ACT_MUTE_BUZZER,
        IR_ACT_MENU, IR_ACT_LIGHT_UP, IR_ACT_LIGHT_DOWN,
    };
    const char *p;
    int value;

    if (!line || !out)
        return -1;
    out->kind = BLUETOOTH_CMD_INVALID;
    out->value = 0;
    p = skip_space(line);
    if (*p >= '1' && *p <= '9' && at_end(p + 1)) {
        out->kind = BLUETOOTH_CMD_ACTION;
        out->value = digit_actions[*p - '1'];
        return 0;
    }
    value = action_alias(line);
    if (value != IR_ACT_NONE) {
        out->kind = BLUETOOTH_CMD_ACTION;
        out->value = value;
        return 0;
    }
    if (exact(line, "PING")) out->kind = BLUETOOTH_CMD_PING;
    else if (exact(line, "STATUS")) out->kind = BLUETOOTH_CMD_STATUS;
    else if (exact(line, "HELP") || exact(line, "?")) out->kind = BLUETOOTH_CMD_HELP;
    else {
        p = line;
        if (take_word(&p, "FAN") && take_uint(&p, &value) && value <= 3 && at_end(p)) {
            out->kind = BLUETOOTH_CMD_FAN_SET; out->value = value;
        } else {
            p = line;
            if (take_word(&p, "LIGHT") && take_uint(&p, &value) && value <= 100 && at_end(p)) {
                out->kind = BLUETOOTH_CMD_LIGHT_SET; out->value = value;
            }
        }
    }
    if (out->kind != BLUETOOTH_CMD_INVALID)
        return 0;

#define PARSE_NAMED(group, name, kind_value, parsed_value) \
    do { p = line; if (take_word(&p, group) && take_word(&p, name) && at_end(p)) { \
        out->kind = kind_value; out->value = parsed_value; return 0; } } while (0)
    PARSE_NAMED("PUMP", "ON", BLUETOOTH_CMD_PUMP_SET, 1);
    PARSE_NAMED("PUMP", "OFF", BLUETOOTH_CMD_PUMP_SET, 0);
    PARSE_NAMED("MUTE", "ON", BLUETOOTH_CMD_MUTE_SET, 1);
    PARSE_NAMED("MUTE", "OFF", BLUETOOTH_CMD_MUTE_SET, 0);
    PARSE_NAMED("THEME", "DAY", BLUETOOTH_CMD_THEME_SET, 0);
    PARSE_NAMED("THEME", "NIGHT", BLUETOOTH_CMD_THEME_SET, 1);
    PARSE_NAMED("THEME", "COZY", BLUETOOTH_CMD_THEME_SET, 2);
    PARSE_NAMED("AUTO", "FAN", BLUETOOTH_CMD_AUTO, BLUETOOTH_AUTO_FAN);
    PARSE_NAMED("AUTO", "PUMP", BLUETOOTH_CMD_AUTO, BLUETOOTH_AUTO_PUMP);
    PARSE_NAMED("AUTO", "THEME", BLUETOOTH_CMD_AUTO, BLUETOOTH_AUTO_THEME);
    PARSE_NAMED("AUTO", "LIGHT", BLUETOOTH_CMD_AUTO, BLUETOOTH_AUTO_LIGHT);
    PARSE_NAMED("AUTO", "ALL", BLUETOOTH_CMD_AUTO, BLUETOOTH_AUTO_ALL);
#undef PARSE_NAMED
    return -1;
}
