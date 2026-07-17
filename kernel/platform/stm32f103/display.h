#ifndef _STM32F103_DISPLAY_H
#define _STM32F103_DISPLAY_H

#include "core/types.h"
#include "keys.h"
#include "memory.h"
#include "ui_render.h"

typedef enum stm32_display_action {
    STM32_DISPLAY_ACTION_NONE = 0,
    STM32_DISPLAY_ACTION_BLUETOOTH_TEST,
    STM32_DISPLAY_ACTION_WIFI_SCAN,
} stm32_display_action_t;

int stm32_display_init(void);
int stm32_display_ready(void);
/* Stream a w*h RGB565 pixel block to the panel at (x,y). Clips to 320x480. */
void stm32_display_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const uint16_t *px);
/* Fill a rect with a solid RGB565 color. Clips to 320x480. */
void stm32_display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             uint16_t color);
/* Fill *gfx with the on-board sink: ctx=NULL, fill_rect/blit backed above. */
void stm32_display_gfx(ui_gfx_t *gfx);
void stm32_display_show_boot(void);
void stm32_display_update_ticks(uint64_t ticks);
void stm32_display_set_peripherals(int sram_ready, size_t sram_bytes,
                                   int sd_ready, uint64_t sd_sectors,
                                   int sd_fat32, int sd_bus_width,
                                   const char *sd_volume_label,
                                   int touch_ready, int keys_ready);
void stm32_display_set_bluetooth(int ready, int detected, int at_responsive,
                                 int connected, int waiting, int configured,
                                 int slave_mode,
                                 int uuid_supported, int uuid_configured,
                                 const char *device_name,
                                 const char *pin, uint16_t service_uuid,
                                 uint32_t baud_rate, uint32_t received_bytes,
                                 uint32_t transmitted_bytes,
                                 uint32_t dropped_bytes);
void stm32_display_set_wifi(int active, int detected, int at_responsive,
                            int configured, int connecting, int joined,
                            int got_ip, int socket_connected,
                            const char *ssid, const char *ip_address,
                            const char *mac_address, const char *scan_ssid,
                            uint32_t access_points, uint32_t baud_rate,
                            uint32_t received_bytes,
                            uint32_t transmitted_bytes,
                            uint32_t dropped_bytes, const char *last_event);
void stm32_display_set_memory(const stm32_memory_info_t *info);
void stm32_display_set_light(int ready, uint16_t raw_adc,
                             uint8_t intensity_percent,
                             uint8_t backlight_percent,
                             int auto_brightness);
void stm32_display_show_bluetooth_line(const char *line);
void stm32_display_show_touch(uint16_t x, uint16_t y, int pressed);
void stm32_display_handle_key(stm32_key_t key);
stm32_display_action_t stm32_display_take_action(void);

#endif
