#ifndef _STM32F103_BLUETOOTH_H
#define _STM32F103_BLUETOOTH_H

#include "core/types.h"

#define STM32_BLUETOOTH_LINE_MAX 96U

enum {
    STM32_BLUETOOTH_RX_UNKNOWN = 0,
    STM32_BLUETOOTH_RX_DRIVEN_HIGH,
    STM32_BLUETOOTH_RX_FLOATING,
    STM32_BLUETOOTH_RX_DRIVEN_LOW,
};

enum {
    STM32_BLUETOOTH_AT_KEY_LOW = 0,
    STM32_BLUETOOTH_AT_KEY_PULSE,
    STM32_BLUETOOTH_AT_KEY_HIGH,
};

#ifndef STM32_BLUETOOTH_DEVICE_NAME
#define STM32_BLUETOOTH_DEVICE_NAME "KasaneTeto"
#endif

#ifndef STM32_BLUETOOTH_PIN
#define STM32_BLUETOOTH_PIN "2233"
#endif

#ifndef STM32_BLUETOOTH_SERVICE_UUID
#define STM32_BLUETOOTH_SERVICE_UUID 0x1101U
#endif

#ifndef STM32_BLUETOOTH_SERVICE_UUID_TEXT
#define STM32_BLUETOOTH_SERVICE_UUID_TEXT "1101"
#endif

typedef struct stm32_bluetooth_info {
    int ready;
    int detected;
    int at_responsive;
    int at_pulse_mode;
    int at_power_on_mode;
    int at_key_mode;
    int configured;
    int connected;
    int waiting;
    int slave_mode;
    int discoverable;
    int name_configured;
    int pin_configured;
    int uuid_supported;
    int uuid_configured;
    int reset_performed;
    int rx_line_state;
    uint32_t baud_rate;
    uint32_t at_baud_rate;
    uint16_t service_uuid;
    uint16_t requested_uuid;
    uint32_t received_bytes;
    uint32_t transmitted_bytes;
    uint32_t dropped_bytes;
    uint32_t at_received_bytes;
    uint32_t at_error_bytes;
    uint32_t at_attempts;
    char device_name[33];
    char pin[17];
    char address[24];
} stm32_bluetooth_info_t;

int stm32_bluetooth_init(void);
int stm32_bluetooth_reprobe(void);
void stm32_bluetooth_early_key_init(void);
void stm32_bluetooth_irq(void);
void stm32_bluetooth_service(uint64_t now);
int stm32_bluetooth_connected(void);
int stm32_bluetooth_send(const void *data, size_t length);
int stm32_bluetooth_send_text(const char *text);
int stm32_bluetooth_read_line(char *buffer, size_t capacity);
const stm32_bluetooth_info_t *stm32_bluetooth_info(void);
void stm32_bluetooth_debug_status(void);
int stm32_bluetooth_debug_probe(void);
int stm32_bluetooth_debug_at(const char *command);

#endif
