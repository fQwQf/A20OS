#ifdef CONFIG_BOARD_STM32F103

#include "peripherals.h"
#include "core/stdio.h"
#include "mm/slab.h"
#include "display.h"
#include "extsram.h"
#include "keys.h"
#include "sdcard.h"
#include "touch.h"

#define TOUCH_POLL_INTERVAL_MS 20U
#define KEY_POLL_INTERVAL_MS 20U
#define SDCARD_CHECK_INTERVAL_MS 2000U
#define SDCARD_REPROBE_INTERVAL_MS 5000U

static stm32_peripheral_state_t peripherals;
static uint64_t last_touch_poll;
static uint64_t last_key_poll;
static uint64_t last_sdcard_check;
static int touch_pressed;

static void update_display_status(void) {
    const stm32_sdcard_info_t *sd = stm32_sdcard_info();
    stm32_display_set_peripherals(
        peripherals.external_sram_ready, peripherals.external_sram_bytes,
        peripherals.sdcard_ready, sd->sectors, sd->fat32,
        sd->bus_width, sd->volume_label,
        peripherals.touch_armed, peripherals.keys_ready);
}

static void ext_sram_smoke_test(void) {
    if (!peripherals.external_sram_ready)
        return;

    uint8_t *probe = stm32_extsram_alloc(256U);
    if (!probe) {
        printf("[BOOT] external SRAM allocator=failed\n");
        stm32_extsram_shutdown();
        peripherals.external_sram_ready = 0;
        peripherals.external_sram_bytes = 0;
        return;
    }
    for (unsigned i = 0; i < 256U; i++)
        probe[i] = (uint8_t)(i ^ 0xA5U);
    for (unsigned i = 0; i < 256U; i++) {
        if (probe[i] == (uint8_t)(i ^ 0xA5U))
            continue;
        printf("[BOOT] external SRAM allocator=corrupt\n");
        stm32_extsram_free(probe);
        stm32_extsram_shutdown();
        peripherals.external_sram_ready = 0;
        peripherals.external_sram_bytes = 0;
        return;
    }
    stm32_extsram_free(probe);

    void *heap_probe = kmalloc(96U * 1024U);
    if (!heap_probe || !stm32_extsram_owns(heap_probe)) {
        printf("[BOOT] external SRAM heap fallback=failed\n");
        kfree(heap_probe);
        stm32_extsram_shutdown();
        peripherals.external_sram_ready = 0;
        peripherals.external_sram_bytes = 0;
        return;
    }
    kfree(heap_probe);
}

static void report_sdcard(void) {
    const stm32_sdcard_info_t *sd = stm32_sdcard_info();

    if (!peripherals.sdcard_ready) {
        printf("[BOOT] TF card=absent (optional)\n");
        return;
    }

    printf("[BOOT] TF card=ready sectors=%lu bus=%u-bit fat32=%s",
           (unsigned long)sd->sectors, (unsigned)sd->bus_width,
           sd->fat32 ? "yes" : "no");
    if (sd->fat32)
        printf(" partition=%u label=%s", (unsigned)sd->partition_lba,
               sd->volume_label[0] ? sd->volume_label : "(none)");
    printf("\n");
}

void stm32_peripherals_init(void) {
    int display_id = stm32_display_init();
    peripherals.display_ready = display_id >= 0;
    peripherals.display_id =
        peripherals.display_ready ? (uint16_t)display_id : 0;
    printf("[BOOT] display=%s",
           peripherals.display_ready ? "ready" : "absent");
    if (peripherals.display_ready)
        printf(" id=0x%x", (unsigned)peripherals.display_id);
    printf(" (optional)\n");
    stm32_display_show_boot();

    peripherals.external_sram_ready = stm32_extsram_init() == 0;
    peripherals.external_sram_bytes = peripherals.external_sram_ready
                                          ? stm32_extsram_available()
                                          : 0;
    ext_sram_smoke_test();
    printf("[BOOT] external SRAM=%s bytes=%u\n",
           peripherals.external_sram_ready ? "ready" : "absent",
           (unsigned)peripherals.external_sram_bytes);

    peripherals.sdcard_ready = stm32_sdcard_init() == 0;
    report_sdcard();

    peripherals.touch_armed = stm32_touch_init() == 0;
    printf("[BOOT] touch interface=%s (optional)\n",
           peripherals.touch_armed ? "armed" : "disabled");

    peripherals.keys_ready = stm32_keys_init() == 0;
    printf("[BOOT] direction keys=%s\n",
           peripherals.keys_ready ? "ready" : "disabled");

    update_display_status();
}

void stm32_peripherals_service(uint64_t now) {
    stm32_display_update_ticks(now);

    if (peripherals.keys_ready &&
        now - last_key_poll >= KEY_POLL_INTERVAL_MS) {
        last_key_poll = now;
        stm32_key_t key = stm32_keys_poll();
        if (key != STM32_KEY_NONE) {
            static const char *const names[] = {
                "none", "up", "down", "left", "right",
            };
            printf("[KEY] %s\n", names[key]);
            stm32_display_handle_key(key);
        }
    }

    if (peripherals.touch_armed &&
        now - last_touch_poll >= TOUCH_POLL_INTERVAL_MS) {
        uint16_t x = 0;
        uint16_t y = 0;
        last_touch_poll = now;
        int pressed = stm32_touch_poll(&x, &y);
        stm32_display_show_touch(x, y, pressed);
        if (pressed && !touch_pressed)
            printf("[TOUCH] down x=%u y=%u\n", (unsigned)x, (unsigned)y);
        else if (!pressed && touch_pressed)
            printf("[TOUCH] up\n");
        touch_pressed = pressed;
    }

    uint64_t interval = peripherals.sdcard_ready
                            ? SDCARD_CHECK_INTERVAL_MS
                            : SDCARD_REPROBE_INTERVAL_MS;
    if (now - last_sdcard_check < interval)
        return;
    last_sdcard_check = now;

    int was_ready = peripherals.sdcard_ready;
    if (was_ready)
        peripherals.sdcard_ready = stm32_sdcard_check() == 0;
    else
        peripherals.sdcard_ready = stm32_sdcard_init() == 0;

    if (peripherals.sdcard_ready == was_ready)
        return;
    if (peripherals.sdcard_ready)
        printf("[TF] card inserted and initialized\n");
    else
        printf("[TF] card removed or stopped responding\n");
    report_sdcard();
    update_display_status();
}

const stm32_peripheral_state_t *stm32_peripherals_state(void) {
    return &peripherals;
}

#endif
