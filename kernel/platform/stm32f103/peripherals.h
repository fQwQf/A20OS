#ifndef _STM32F103_PERIPHERALS_H
#define _STM32F103_PERIPHERALS_H

#include "core/types.h"
#include "ui_home.h"
#include "live2d.h"

typedef struct stm32_peripheral_state {
    int display_ready;
    uint16_t display_id;
    int external_sram_ready;
    size_t external_sram_bytes;
    int sdcard_ready;
    int touch_armed;
    int keys_ready;
    int bluetooth_ready;
    int bluetooth_connected;
    int wifi_ready;
    int light_sensor_ready;
    int rgb_matrix_ready;
    int memory_capacity_from_silicon;
    uint32_t service_calls;
    uint32_t service_last_ms;
    uint32_t service_max_ms;
    uint32_t service_slow_calls;
    uint32_t light_max_ms;
    uint32_t sdcard_max_ms;
    uint32_t bluetooth_retry_max_ms;
    uint32_t wifi_retry_max_ms;
} stm32_peripheral_state_t;

void stm32_peripherals_init(void);
void stm32_peripherals_service(uint64_t now);
int stm32_peripherals_retry_sdcard(void);
int stm32_peripherals_retry_bluetooth(void);
int stm32_peripherals_retry_wifi(void);
int stm32_peripherals_set_proxy(const char *ip, uint16_t port);
void stm32_peripherals_request_time_sync(void);
int stm32_peripherals_start_touch_calibration(void);
const stm32_peripheral_state_t *stm32_peripherals_state(void);

/* Latest assembled home-screen UI state and catgirl animation state, for the
 * on-board renderer (ui_render). Updated each control tick. */
const ui_home_state_t *stm32_peripherals_ui_state(void);
const live2d_t *stm32_peripherals_cat(void);

#endif
