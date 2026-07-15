#ifndef _STM32F103_WIFI_H
#define _STM32F103_WIFI_H

#include "core/types.h"

#define STM32_WIFI_LINE_MAX 96U

typedef enum stm32_wifi_phase {
    STM32_WIFI_DISABLED = 0,
    STM32_WIFI_RESET_WAIT,
    STM32_WIFI_BOOT_WAIT,
    STM32_WIFI_AT_WAIT,
    STM32_WIFI_GMR_WAIT,
    STM32_WIFI_MODE_WAIT,
    STM32_WIFI_MUX_WAIT,
    STM32_WIFI_JOIN_WAIT,
    STM32_WIFI_IP_WAIT,
    STM32_WIFI_READY,
    STM32_WIFI_SCAN_WAIT,
    STM32_WIFI_SOCKET_WAIT,
    STM32_WIFI_SEND_PROMPT_WAIT,
    STM32_WIFI_SEND_RESULT_WAIT,
    STM32_WIFI_CLOSE_WAIT,
    STM32_WIFI_RAW_AT_WAIT,
} stm32_wifi_phase_t;

typedef struct stm32_wifi_info {
    int active;
    int detected;
    int at_responsive;
    int configured;
    int station_mode;
    int connecting;
    int joined;
    int got_ip;
    int socket_connected;
    int command_busy;
    stm32_wifi_phase_t phase;
    uint32_t baud_rate;
    uint32_t received_bytes;
    uint32_t transmitted_bytes;
    uint32_t dropped_bytes;
    uint32_t uart_errors;
    uint32_t command_timeouts;
    uint32_t access_points;
    char ssid[33];
    char ip_address[16];
    char mac_address[18];
    char scan_ssid[33];
    char last_event[STM32_WIFI_LINE_MAX];
} stm32_wifi_info_t;

int stm32_wifi_init(void);
void stm32_wifi_shutdown(void);
int stm32_wifi_reprobe(void);
void stm32_wifi_irq(void);
void stm32_wifi_service(uint64_t now);
int stm32_wifi_scan(void);
int stm32_wifi_join(const char *ssid, const char *password);
int stm32_wifi_open(const char *protocol, const char *host, uint16_t port);
int stm32_wifi_close(void);
int stm32_wifi_send(const void *data, size_t length);
int stm32_wifi_read(void *data, size_t capacity);
int stm32_wifi_debug_at(const char *command);
void stm32_wifi_debug_status(void);
const stm32_wifi_info_t *stm32_wifi_info(void);

#endif
