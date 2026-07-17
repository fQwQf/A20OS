#ifndef _STM32F103_BLUETOOTH_CMD_H
#define _STM32F103_BLUETOOTH_CMD_H

typedef enum bluetooth_cmd_kind {
    BLUETOOTH_CMD_INVALID = 0,
    BLUETOOTH_CMD_ACTION,
    BLUETOOTH_CMD_PING,
    BLUETOOTH_CMD_STATUS,
    BLUETOOTH_CMD_HELP,
    BLUETOOTH_CMD_FAN_SET,
    BLUETOOTH_CMD_PUMP_SET,
    BLUETOOTH_CMD_THEME_SET,
    BLUETOOTH_CMD_LIGHT_SET,
    BLUETOOTH_CMD_MUTE_SET,
    BLUETOOTH_CMD_AUTO,
} bluetooth_cmd_kind_t;

typedef struct bluetooth_cmd {
    bluetooth_cmd_kind_t kind;
    int value;
} bluetooth_cmd_t;

/* AUTO values deliberately match ui_home.h's manual-mask bits. */
#define BLUETOOTH_AUTO_FAN 0x01
#define BLUETOOTH_AUTO_PUMP 0x02
#define BLUETOOTH_AUTO_THEME 0x04
#define BLUETOOTH_AUTO_LIGHT 0x08
#define BLUETOOTH_AUTO_ALL 0x0f

/* Parse one CR/LF-terminated HC-05 SPP line. ACTION values are ir_action_t. */
int bluetooth_cmd_parse(const char *line, bluetooth_cmd_t *out);

#endif
