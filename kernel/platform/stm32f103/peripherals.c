#ifdef CONFIG_BOARD_STM32F103

#include "peripherals.h"
#include "board.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/stm32f1/actuators.h"
#include "drivers/stm32f1/bluetooth.h"
#include "drivers/stm32f1/dht11.h"
#include "drivers/stm32f1/display.h"
#include "drivers/stm32f1/extsram.h"
#include "drivers/stm32f1/keys.h"
#include "drivers/stm32f1/light_sensor.h"
#include "drivers/stm32f1/memory.h"
#include "drivers/stm32f1/rtc.h"
#include "drivers/stm32f1/sdcard.h"
#include "drivers/stm32f1/touch.h"
#include "drivers/stm32f1/watchdog.h"
#include "drivers/stm32f1/wifi.h"
#include "drivers/stm32f1/rgb_matrix.h"
#include "sdcard_fs.h"

#define TOUCH_POLL_MS 20U
#define KEY_POLL_MS 20U
#define BT_POLL_MS 20U
#define WIFI_POLL_MS 20U
#define LIGHT_POLL_MS 250U
#define MEMORY_POLL_MS 1000U
#define SDCARD_CHECK_MS 2000U
#define SDCARD_RETRY_MS 5000U

static stm32_peripheral_state_t state;
static uint64_t last_touch, last_key, last_bt, last_wifi;
static uint64_t last_light, last_memory, last_sd_check, last_sd_retry;

static void publish_bluetooth_status(void) {
    const stm32_bluetooth_info_t *bt = stm32_bluetooth_info();
    stm32_display_set_bluetooth(
        state.bluetooth_ready, bt->detected, bt->at_responsive,
        bt->connected, bt->waiting, bt->configured, bt->slave_mode,
        bt->uuid_supported, bt->uuid_configured, bt->device_name,
        bt->pin[0] ? "configured" : "(unread)", bt->service_uuid,
        bt->baud_rate, bt->received_bytes, bt->transmitted_bytes,
        bt->dropped_bytes);
}

static void publish_wifi_status(void) {
    const stm32_wifi_info_t *wifi = stm32_wifi_info();
    stm32_display_set_wifi(
        wifi->active, wifi->detected, wifi->at_responsive, wifi->configured,
        wifi->connecting, wifi->joined, wifi->got_ip,
        wifi->socket_connected, wifi->ssid, wifi->ip_address,
        wifi->mac_address, wifi->scan_ssid, wifi->access_points,
        wifi->baud_rate, wifi->received_bytes, wifi->transmitted_bytes,
        wifi->dropped_bytes, wifi->last_event);
}

static void publish_light_status(void) {
    const stm32_light_sensor_info_t *light = stm32_light_sensor_info();
    stm32_display_set_light(state.light_sensor_ready, light->raw_adc,
                            light->intensity_percent,
                            light->backlight_percent,
                            light->auto_brightness);
}

static void publish_display_status(void) {
    const stm32_sdcard_info_t *sd = stm32_sdcard_info();
    stm32_display_set_peripherals(
        state.external_sram_ready, state.external_sram_bytes,
        state.sdcard_ready, sd->sectors, sd->fat32, sd->bus_width,
        sd->volume_label, state.touch_armed, state.keys_ready);
}

static void report_storage(void) {
    const stm32_sdcard_info_t *sd = stm32_sdcard_info();
    printf("[BOOT] TF card=%s", state.sdcard_ready ? "ready" : "absent");
    if (state.sdcard_ready)
        printf(" sectors=%lu bus=%u-bit fat32=%s", (unsigned long)sd->sectors,
               (unsigned)sd->bus_width, sd->fat32 ? "yes" : "no");
    printf("\n");
}

void stm32_peripherals_init(void) {
    int id = stm32_display_init();
    state.display_ready = id >= 0;
    state.display_id = state.display_ready ? (uint16_t)id : 0;
    printf("[BOOT] display=%s%s\n", state.display_ready ? "ready" : "absent",
           state.display_ready ? " (Xuanwu panel)" : "");
    stm32_display_show_boot();

    state.external_sram_ready = stm32_extsram_init() == 0;
    state.external_sram_bytes = state.external_sram_ready
                                    ? stm32_extsram_available() : 0;
    stm32_memory_init();
    printf("[BOOT] external SRAM=%s bytes=%u\n",
           state.external_sram_ready ? "ready" : "absent",
           (unsigned)state.external_sram_bytes);

    state.sdcard_ready = stm32_sdcard_init() == 0;
    report_storage();
    if (state.sdcard_ready)
        (void)stm32_sdcard_fs_mount();

    state.touch_armed = stm32_touch_init() == 0;
    if (state.touch_armed && stm32_sdcard_fs_load_touch_calibration() == 0)
        printf("[BOOT] touch calibration=loaded (/CFG/TOUCH.CAL)\n");
    state.keys_ready = stm32_keys_init() == 0;
    state.light_sensor_ready = stm32_light_sensor_init() == 0;
    stm32_rtc_init();
    (void)stm32_dht11_init();
    stm32_actuators_init();
    state.bluetooth_ready = stm32_bluetooth_init() == 0;
    state.bluetooth_connected = stm32_bluetooth_connected();
    state.wifi_ready = stm32_wifi_init() == 0;
    state.rgb_matrix_ready = stm32_rgb_matrix_init() == 0;
    publish_display_status();
    stm32_display_set_memory(stm32_memory_info());
    publish_light_status();
    publish_bluetooth_status();
    publish_wifi_status();
    stm32_watchdog_init(6000U);
    printf("[BOOT] touch=%s keys=%s light=%s bluetooth=%s wifi=%s\n",
           state.touch_armed ? "ready" : "absent",
           state.keys_ready ? "ready" : "absent",
           state.light_sensor_ready ? "ready" : "absent",
           state.bluetooth_ready ? "ready" : "absent",
           state.wifi_ready ? "ready" : "absent");
}

void stm32_peripherals_service(uint64_t now) {
    uint64_t start = timer_get_ticks();
    stm32_watchdog_feed();
    if (state.display_ready)
        stm32_display_update_ticks(now);

    if (state.touch_armed && now - last_touch >= TOUCH_POLL_MS) {
        uint16_t x, y;
        last_touch = now;
        if (stm32_touch_poll(&x, &y) >= 0)
            stm32_display_show_touch(x, y, stm32_touch_pressed());
    }
    if (state.keys_ready && now - last_key >= KEY_POLL_MS) {
        stm32_key_t key;
        last_key = now;
        key = stm32_keys_poll();
        if (key != STM32_KEY_NONE)
            stm32_display_handle_key(key);
    }
    stm32_display_action_t action = stm32_display_take_action();
    if (action == STM32_DISPLAY_ACTION_WIFI_SCAN)
        (void)stm32_wifi_scan();
    else if (action == STM32_DISPLAY_ACTION_BLUETOOTH_TEST &&
             state.bluetooth_ready)
        (void)stm32_bluetooth_send_text("A20OS HC05 TEST\r\n");
    if (state.bluetooth_ready && now - last_bt >= BT_POLL_MS) {
        last_bt = now;
        stm32_bluetooth_service(now);
        state.bluetooth_connected = stm32_bluetooth_connected();
        char line[STM32_BLUETOOTH_LINE_MAX];
        while (stm32_bluetooth_read_line(line, sizeof(line)) > 0)
            stm32_display_show_bluetooth_line(line);
        publish_bluetooth_status();
    }
    if (state.wifi_ready && now - last_wifi >= WIFI_POLL_MS) {
        last_wifi = now;
        stm32_wifi_service(now);
        publish_wifi_status();
    }
    if (state.light_sensor_ready && now - last_light >= LIGHT_POLL_MS) {
        last_light = now;
        (void)stm32_light_sensor_sample();
        publish_light_status();
    }
    if (now - last_memory >= MEMORY_POLL_MS) {
        last_memory = now;
        stm32_memory_refresh();
        stm32_display_set_memory(stm32_memory_info());
    }
    if (state.sdcard_ready && now - last_sd_check >= SDCARD_CHECK_MS) {
        last_sd_check = now;
        if (stm32_sdcard_check() != 0) {
            state.sdcard_ready = 0;
            stm32_sdcard_fs_unmount();
            report_storage();
            publish_display_status();
        }
    }
    if (!state.sdcard_ready && now - last_sd_retry >= SDCARD_RETRY_MS) {
        last_sd_retry = now;
        (void)stm32_peripherals_retry_sdcard();
    }
    state.service_calls++;
    state.service_last_ms = (uint32_t)(timer_get_ticks() - start);
    if (state.service_last_ms > state.service_max_ms)
        state.service_max_ms = state.service_last_ms;
}

int stm32_peripherals_retry_sdcard(void) {
    state.sdcard_ready = stm32_sdcard_recover() == 0;
    if (state.sdcard_ready)
        (void)stm32_sdcard_fs_mount();
    report_storage();
    publish_display_status();
    return state.sdcard_ready ? 0 : -1;
}

int stm32_peripherals_retry_bluetooth(void) {
    int r = stm32_bluetooth_reprobe();
    state.bluetooth_ready = r == 0;
    state.bluetooth_connected = stm32_bluetooth_connected();
    publish_bluetooth_status();
    return r;
}

int stm32_peripherals_retry_wifi(void) {
    int r = stm32_wifi_reprobe();
    state.wifi_ready = r == 0;
    publish_wifi_status();
    return r;
}

const stm32_peripheral_state_t *stm32_peripherals_state(void) { return &state; }

#endif
