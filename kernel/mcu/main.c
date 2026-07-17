#include "proc/proc.h"
#include "core/arch.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_core.h"
#include "mm/slab.h"
#include "backlight.h"
#include "bluetooth.h"
#include "console.h"
#include "dht11.h"
#include "fs/fat32lite.h"
#include "peripherals.h"
#include "rtc.h"
#include "rgb_matrix.h"
#include "heap.h"
#include "light_sensor.h"
#include "extsram.h"
#include "ir.h"
#include "live2d.h"
#include "live2d_load.h"
#include "sdcard.h"
#include "sdfs.h"
#include "smarthub.h"
#include "stm32_uart.h"
#include "wifi.h"

#ifdef CONFIG_STM32_QEMU
static volatile char diagnostic_line[64];
#else
static volatile char diagnostic_line[160];
#endif
static unsigned diagnostic_length;

static int text_equal(const char *left, const char *right) {
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int text_starts_with(const char *text, const char *prefix) {
    while (*prefix && *text == *prefix) {
        text++;
        prefix++;
    }
    return *prefix == '\0';
}

static void diagnostic_light_status(void) {
    const stm32_light_sensor_info_t *light = stm32_light_sensor_info();
    stm32_backlight_debug_t pwm;
    stm32_backlight_debug(&pwm);
    printf("[LIGHT] ready=%d raw=%u filtered=%u level=%u/100"
           " backlight=fixed-on samples=%u errors=%u\n",
           light->ready, (unsigned)light->raw_adc,
           (unsigned)light->filtered_adc,
           (unsigned)light->intensity_percent,
           (unsigned)light->samples, (unsigned)light->errors);
    printf("[BACKLIGHT] ready=%d percent=%u PB0-mode=0x%x"
           " IDR=%u ODR=%u pin=%u accumulator=%u"
           " writes=%u ticks=%u transitions=%u\n",
           pwm.initialized, (unsigned)pwm.percent,
           (unsigned)(pwm.gpio_crl & 0xFU),
           (unsigned)(pwm.gpio_idr & 1U),
           (unsigned)(pwm.gpio_odr & 1U),
           (unsigned)pwm.pin_high,
           (unsigned)pwm.accumulator,
           (unsigned)pwm.updates,
           (unsigned)pwm.modulation_ticks,
           (unsigned)pwm.pin_transitions);
}

static void diagnostic_prompt(void) {
    printf("a20> ");
}

static void diagnostic_performance(void) {
    const stm32_peripheral_state_t *state = stm32_peripherals_state();
    printf("[PERF] uptime=%lu ms hclk=%u pclk1=%u pclk2=%u"
           " service-calls=%u last=%u ms max=%u ms slow=%u"
           " light-max=%u ms sd-max=%u ms bt-retry-max=%u ms"
           " wifi-retry-max=%u ms\n",
           (unsigned long)timer_get_ticks(),
           (unsigned)stm32_hclk_hz(),
           (unsigned)stm32_pclk1_hz(),
           (unsigned)stm32_pclk2_hz(),
           (unsigned)state->service_calls,
           (unsigned)state->service_last_ms,
           (unsigned)state->service_max_ms,
           (unsigned)state->service_slow_calls,
           (unsigned)state->light_max_ms,
           (unsigned)state->sdcard_max_ms,
           (unsigned)state->bluetooth_retry_max_ms,
           (unsigned)state->wifi_retry_max_ms);
}

static void diagnostic_help(void) {
    printf("Commands:\n");
    printf("  uart          show all USART clock/baud/error state\n");
    printf("  perf          show clocks and main service latency\n");
    printf("  light         show ADC3 light sensor state\n");
    printf("  dht           one DHT11 read with failure-stage diagnostics\n");
    printf("  time [sync|set HH:MM[:SS]]  show, network-sync or set RTC\n");
    printf("  cal start     run the four-point touch calibration UI\n");
    printf("  rgb           show 5x5 matrix status\n");
    printf("  rgb clear | fill <RRGGBB> | pixel <x> <y> <RRGGBB>\n");
    printf("  rgb brightness <0-255> | test\n");
    printf("  sd retry      explicitly probe and initialize the TF card\n");
    printf("  sd            TF transfer diagnostics (SDIO_CK + error counts)\n");
    printf("  sd clk <mhz>  retune the SDIO data clock (0.1-24), no reflash\n");
    printf("  sd bus <1|4>  force the SD bus width, then `sd retry`\n");
    printf("  live2d        catgirl sprite state + frame cache + extsram\n");
    printf("  ir            IR receiver edge/frame/abort counters\n");
    printf("  fs [ls <dir>] | cat <path> | write <path> <text>"
           " | rm <path> | test\n");
    printf("  bt            show HC-05 detection/configuration state\n");
    printf("  bt retry      rerun HC-05 detection and slave configuration\n");
    printf("  bt probe      scan HC-05 runtime AT baud/key modes\n");
    printf("  bt at <cmd>   send an AT command, for example AT+ROLE?\n");
    printf("  wifi          show ESP8266 and network state\n");
    printf("  wifi retry    reprobe ESP8266 on USART2\n");
    printf("  wifi scan     asynchronously scan access points\n");
    printf("  wifi join <ssid> <password>  join an access point\n");
    printf("  wifi open <tcp|udp> <host> <port>  open a socket\n");
    printf("  wifi send <text> / wifi read / wifi close\n");
    printf("  wifi at <cmd> send a raw ESP8266 AT command\n");
    printf("  wifi debug <on|off>  per-command AT logging (default off)\n");
    printf("  proxy <ip> <port>  set the cloud proxy for auto cloud control\n");
    printf("  help          show this command list\n");
}

/*
 * TF transfer diagnostics. SDIOCLK is HCLK on the F103, so SDIO_CK rides the
 * core clock — this prints the rate actually in effect plus which SDIO error
 * the card is hitting, because rxoverr / dcrcfail / dtimeout need different
 * fixes and are indistinguishable from "the card stopped working".
 */
static void diagnostic_sd(void) {
    const stm32_sdcard_info_t *sd = stm32_sdcard_info();

    printf("[TF-DIAG] present=%d fat32=%d bus=%u-bit sectors=%lu\n",
           sd->present, sd->fat32, (unsigned)sd->bus_width,
           (unsigned long)sd->sectors);
    printf("[TF-DIAG] hclk=%u SDIO_CK=%u Hz (target=%u) \n",
           (unsigned)stm32_hclk_hz(), (unsigned)sd->sdio_ck_hz,
           (unsigned)sd->transfer_hz);
    printf("[TF-DIAG] err dcrc=%u dtimeout=%u rxovr=%u txudr=%u stbit=%u"
           " cmd=%u | retries=%u shutdowns=%u last_sta=0x%x\n",
           (unsigned)sd->err_dcrcfail, (unsigned)sd->err_dtimeout,
           (unsigned)sd->err_rxoverr, (unsigned)sd->err_txunderr,
           (unsigned)sd->err_stbiterr, (unsigned)sd->err_cmd,
           (unsigned)sd->retries, (unsigned)sd->shutdowns,
           (unsigned)sd->last_err_sta);
}

/*
 * Live2D sprite pipeline. "The catgirl doesn't animate" has several distinct
 * causes — the state never changes, the frames aren't on the card, the frame
 * set doesn't fit external SRAM, or the state flips back before one frame can
 * be read — and they look identical on the panel.
 */
static void diagnostic_live2d(void) {
    static const char *const names[] = {"IDLE", "TALK", "HAPPY", "WARN"};
    const live2d_t *cat = stm32_peripherals_cat();
    const ui_home_state_t *ui = stm32_peripherals_ui_state();
    live2d_load_stats_t st;
    unsigned s = (unsigned)cat->state;
    unsigned cs;

    live2d_load_get_stats(&st);
    cs = (unsigned)st.cached_state;
    printf("[L2D] state=%s frame=%u/%u stopped=%d talk_deadline=%u now=%u\n",
           s < 4 ? names[s] : "?", cat->frame,
           live2d_frame_count(cat->state), cat->stopped,
           (unsigned)cat->talk_deadline_ms, (unsigned)timer_get_ticks());
    printf("[L2D] cache state=%s loaded=%u/%u row=%u ready=%d failed=%d"
           " bytes=%u\n",
           cs < 4 ? names[cs] : "?", st.loaded, st.count, st.row,
           st.ready, st.failed, (unsigned)st.bytes);
    printf("[L2D] extsram free=%u/%u | ui mood=%u alert=%u fan=%u pump=%u"
           " speech=%s\n",
           (unsigned)stm32_extsram_available(),
           (unsigned)stm32_extsram_capacity(), ui->mood, ui->alert,
           ui->fan_level, ui->pump_on, ui->speech ? "yes" : "none");
}

/* Shared scratch for the fs command — keep it off the (small) console stack. */
static uint8_t fs_buf[256];

static uint8_t fs_pattern(uint32_t i) { return (uint8_t)((i * 7u + 3u) & 0xFF); }

static void diagnostic_fs_ls(fat32lite_fs_t *fs, const char *path) {
    fat32lite_dir_t d;
    int r = fat32lite_opendir(fs, path, &d);
    if (r) {
        printf("[FS] opendir %s err=%d\n", path, r);
        return;
    }
    printf("[FS] ls %s\n", path);
    fat32lite_dirent_t e;
    unsigned n = 0;
    while ((r = fat32lite_readdir(&d, &e)) == FAT32LITE_OK) {
        printf("  %s\t%s\t%u\n", e.name, e.is_dir ? "<DIR>" : "file",
               (unsigned)e.size);
        n++;
    }
    printf("[FS] %u entries (end=%d)\n", n, r);
}

static void diagnostic_fs_cat(fat32lite_fs_t *fs, const char *path) {
    fat32lite_file_t f;
    int r = fat32lite_open(fs, path, &f);
    if (r) {
        printf("[FS] open %s err=%d\n", path, r);
        return;
    }
    printf("[FS] cat %s size=%u\n", path, (unsigned)fat32lite_size(&f));
    uint32_t shown = 0;
    int n;
    while (shown < 1024u && (n = fat32lite_read(&f, fs_buf, sizeof(fs_buf))) > 0) {
        for (int i = 0; i < n && shown < 1024u; i++, shown++) {
            char c = (char)fs_buf[i];
            printf("%c", (c == '\n' || (c >= 32 && c <= 126)) ? c : '.');
        }
    }
    printf("\n[FS] shown=%u bytes\n", (unsigned)shown);
    fat32lite_close(&f);
}

static void diagnostic_fs_test(fat32lite_fs_t *fs) {
    const char *path = "/FSTEST.BIN";
    const uint32_t N = 40000u; /* > one cluster: exercises chain allocation */
    fat32lite_file_t f;
    int r = fat32lite_create(fs, path, &f);
    if (r) {
        printf("[FS] test: create err=%d\n", r);
        return;
    }
    int ok = 1;
    uint32_t off = 0;
    while (off < N) {
        uint32_t chunk = N - off;
        if (chunk > sizeof(fs_buf))
            chunk = sizeof(fs_buf);
        for (uint32_t i = 0; i < chunk; i++)
            fs_buf[i] = fs_pattern(off + i);
        int w = fat32lite_write(&f, fs_buf, chunk);
        if (w != (int)chunk) {
            printf("[FS] test: write@%u got=%d\n", (unsigned)off, w);
            ok = 0;
            break;
        }
        off += chunk;
    }
    uint32_t wrote = fat32lite_size(&f);
    fat32lite_close(&f);

    uint32_t got = 0, bad = 0;
    if (ok && (r = fat32lite_open(fs, path, &f)) != FAT32LITE_OK) {
        printf("[FS] test: reopen err=%d\n", r);
        ok = 0;
    }
    if (ok) {
        int n;
        while ((n = fat32lite_read(&f, fs_buf, sizeof(fs_buf))) > 0) {
            for (int i = 0; i < n; i++)
                if (fs_buf[i] != fs_pattern(got + (uint32_t)i))
                    bad++;
            got += (uint32_t)n;
        }
        fat32lite_close(&f);
    }
    fat32lite_unlink(fs, path);
    printf("[FS] test %s wrote=%u size=%u readback=%u mismatches=%u\n",
           (ok && bad == 0 && got == N) ? "PASS" : "FAIL", (unsigned)N,
           (unsigned)wrote, (unsigned)got, (unsigned)bad);
}

static void diagnostic_fs(char *args) {
    if (!stm32_sdfs_ready()) {
        int m = stm32_sdfs_mount();
        if (m != FAT32LITE_OK) {
            printf("[FS] not-mounted err=%d (try 'sd retry' first)\n", m);
            return;
        }
    }
    fat32lite_fs_t *fs = stm32_sdfs();
    if (!fs) {
        printf("[FS] no filesystem\n");
        return;
    }
    if (!args[0] || text_equal(args, "ls")) {
        diagnostic_fs_ls(fs, "/");
    } else if (text_starts_with(args, "ls ")) {
        diagnostic_fs_ls(fs, args + 3);
    } else if (text_starts_with(args, "cat ")) {
        diagnostic_fs_cat(fs, args + 4);
    } else if (text_starts_with(args, "write ")) {
        char *rest = args + 6;
        char *sp = strchr(rest, ' ');
        if (!sp) {
            printf("[FS] usage: fs write <path> <text>\n");
            return;
        }
        *sp++ = '\0';
        fat32lite_file_t f;
        int r = fat32lite_create(fs, rest, &f);
        if (r) {
            printf("[FS] write: create %s err=%d\n", rest, r);
            return;
        }
        int w = fat32lite_write(&f, sp, (uint32_t)strlen(sp));
        r = fat32lite_close(&f);
        printf("[FS] write %s bytes=%d close=%d\n", rest, w, r);
    } else if (text_starts_with(args, "rm ")) {
        const char *path = args + 3;
        int r = fat32lite_unlink(fs, path);
        printf("[FS] rm %s %s (err=%d)\n", path, r == FAT32LITE_OK ? "ok" : "fail",
               r);
    } else if (text_equal(args, "test")) {
        diagnostic_fs_test(fs);
    } else {
        printf("[FS] usage: fs [ls <dir>] | cat <path>"
               " | write <path> <text> | rm <path> | test\n");
    }
}

static void diagnostic_time(const char *args) {
    int hour;
    int minute;
    int second;

    if (args && text_equal(args, "sync")) {
        stm32_peripherals_request_time_sync();
        printf("[RTC] network sync queued\n");
    } else if (args && text_starts_with(args, "set ")) {
        const char *p = args + 4;
        if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' ||
            p[2] != ':' || p[3] < '0' || p[3] > '9' ||
            p[4] < '0' || p[4] > '9') {
            printf("[RTC] usage: time set HH:MM[:SS]\n");
            return;
        }
        hour = (p[0] - '0') * 10 + p[1] - '0';
        minute = (p[3] - '0') * 10 + p[4] - '0';
        second = 0;
        if (p[5] == ':') {
            if (p[6] < '0' || p[6] > '9' || p[7] < '0' || p[7] > '9' ||
                p[8] != '\0') {
                printf("[RTC] usage: time set HH:MM[:SS]\n");
                return;
            }
            second = (p[6] - '0') * 10 + p[7] - '0';
        } else if (p[5] != '\0') {
            printf("[RTC] usage: time set HH:MM[:SS]\n");
            return;
        }
        if (stm32_rtc_set_hhmmss(hour, minute, second) != 0) {
            printf("[RTC] invalid/unavailable\n");
            return;
        }
    }
    stm32_rtc_get_hhmmss(&hour, &minute, &second);
    printf("[RTC] %02d:%02d:%02d available=%d source=%s syncs=%u"
           " utc-offset=%d last-epoch=%u\n",
           hour, minute, second, stm32_rtc_available(),
           stm32_rtc_network_synced() ? "network" : "rtc/manual",
           (unsigned)stm32_rtc_sync_count(),
           stm32_rtc_utc_offset_minutes(),
           (unsigned)stm32_rtc_last_unix_utc());
}

static int parse_hex_color(const char *text, uint32_t *color) {
    uint32_t value = 0;
    unsigned digits = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    while (*text) {
        unsigned nibble;
        if (*text >= '0' && *text <= '9')
            nibble = (unsigned)(*text - '0');
        else if (*text >= 'a' && *text <= 'f')
            nibble = (unsigned)(*text - 'a') + 10U;
        else if (*text >= 'A' && *text <= 'F')
            nibble = (unsigned)(*text - 'A') + 10U;
        else
            return -1;
        if (++digits > 6U)
            return -1;
        value = (value << 4) | nibble;
        text++;
    }
    if (digits != 6U)
        return -1;
    *color = value;
    return 0;
}

static int parse_uint_range(const char *text, unsigned maximum,
                            unsigned *value) {
    unsigned parsed = 0;
    unsigned digits = 0;
    while (*text >= '0' && *text <= '9') {
        unsigned digit = (unsigned)(*text - '0');
        if (parsed > maximum / 10U ||
            (parsed == maximum / 10U && digit > maximum % 10U))
            return -1;
        parsed = parsed * 10U + digit;
        digits++;
        text++;
    }
    if (digits == 0 || *text != '\0')
        return -1;
    *value = parsed;
    return 0;
}

static void diagnostic_rgb(char *args) {
    if (!stm32_rgb_matrix_ready()) {
        printf("[RGB] unavailable (requires Xuanwu PE5 and HCLK >= 48MHz)\n");
        return;
    }
    if (!args[0]) {
        printf("[RGB] ready=1 size=5x5 pin=PE5 brightness=%u/255\n",
               (unsigned)stm32_rgb_matrix_brightness());
        return;
    }
    if (text_equal(args, "clear")) {
        stm32_rgb_matrix_clear();
    } else if (text_starts_with(args, "fill ")) {
        uint32_t color;
        if (parse_hex_color(args + 5, &color) != 0) {
            printf("[RGB] usage: rgb fill <RRGGBB>\n");
            return;
        }
        stm32_rgb_matrix_fill(color);
    } else if (text_starts_with(args, "brightness ")) {
        unsigned brightness;
        if (parse_uint_range(args + 11, 255U, &brightness) != 0) {
            printf("[RGB] brightness must be 0..255\n");
            return;
        }
        stm32_rgb_matrix_set_brightness((uint8_t)brightness);
    } else if (text_starts_with(args, "pixel ")) {
        char *save = NULL;
        char *x_text = strtok_r(args + 6, " ", &save);
        char *y_text = strtok_r(NULL, " ", &save);
        char *color_text = strtok_r(NULL, " ", &save);
        char *extra = strtok_r(NULL, " ", &save);
        uint32_t color;
        unsigned x;
        unsigned y;
        if (!x_text || !y_text || !color_text || extra ||
            parse_uint_range(x_text, 4U, &x) != 0 ||
            parse_uint_range(y_text, 4U, &y) != 0 ||
            parse_hex_color(color_text, &color) != 0 ||
            stm32_rgb_matrix_set_pixel((uint8_t)x, (uint8_t)y, color) != 0) {
            printf("[RGB] usage: rgb pixel <0-4> <0-4> <RRGGBB>\n");
            return;
        }
    } else if (text_equal(args, "test")) {
        stm32_rgb_matrix_clear();
        for (uint8_t i = 0; i < 5; i++) {
            (void)stm32_rgb_matrix_set_pixel(i, i, STM32_RGB_COLOR_RED);
            (void)stm32_rgb_matrix_set_pixel((uint8_t)(4U - i), i,
                                             STM32_RGB_COLOR_BLUE);
        }
        (void)stm32_rgb_matrix_set_pixel(2, 2, STM32_RGB_COLOR_WHITE);
    } else {
        printf("[RGB] usage: rgb [clear|fill <RRGGBB>|pixel <x> <y> <RRGGBB>"
               "|brightness <0-255>|test]\n");
        return;
    }
    printf("[RGB] update=%s brightness=%u/255\n",
           stm32_rgb_matrix_show() == 0 ? "ok" : "failed",
           (unsigned)stm32_rgb_matrix_brightness());
}

static void diagnostic_execute(char *line) {
    if (!line[0])
        return;
    if (text_equal(line, "help") || text_equal(line, "?")) {
        diagnostic_help();
    } else if (text_equal(line, "uart")) {
        printf("[UART] USART1 PA9/PA10 clock=%u requested=%u"
               " actual=%u BRR=0x%x 8N1 errors=%u\n",
               (unsigned)arch_uart_clock_hz(),
               (unsigned)arch_uart_baud_rate(),
               (unsigned)arch_uart_actual_baud_rate(),
               (unsigned)arch_uart_divider(),
               (unsigned)arch_uart_error_count());
        const stm32_uart_info_t *uart2 =
            stm32_uart_info(STM32_UART_USART2);
        printf("[UART] USART2 PA2/PA3 initialized=%d clock=%u"
               " requested=%u actual=%u BRR=0x%x 8N1 rx-irq=%d"
               " errors=%u rx=%u tx=%u edges=%u pin=%d last=0x%x\n",
               uart2->initialized, (unsigned)uart2->clock_hz,
               (unsigned)uart2->requested_baud,
               (unsigned)uart2->actual_baud,
               (unsigned)uart2->divider, uart2->rx_irq_enabled,
               (unsigned)uart2->error_count,
               (unsigned)uart2->rx_bytes,
               (unsigned)uart2->tx_bytes,
               (unsigned)uart2->rx_transitions,
               stm32_uart_rx_pin_level(STM32_UART_USART2),
               (unsigned)uart2->last_rx_byte);
        const stm32_uart_info_t *uart3 =
            stm32_uart_info(STM32_UART_USART3);
        printf("[UART] USART3 PB10/PB11 initialized=%d clock=%u"
               " requested=%u actual=%u BRR=0x%x 8N1 rx-irq=%d"
               " errors=%u rx=%u tx=%u edges=%u pin=%d last=0x%x\n",
               uart3->initialized, (unsigned)uart3->clock_hz,
               (unsigned)uart3->requested_baud,
               (unsigned)uart3->actual_baud,
               (unsigned)uart3->divider, uart3->rx_irq_enabled,
               (unsigned)uart3->error_count,
               (unsigned)uart3->rx_bytes,
               (unsigned)uart3->tx_bytes,
               (unsigned)uart3->rx_transitions,
               stm32_uart_rx_pin_level(STM32_UART_USART3),
               (unsigned)uart3->last_rx_byte);
    } else if (text_equal(line, "perf")) {
        diagnostic_performance();
    } else if (text_equal(line, "light")) {
        diagnostic_light_status();
    } else if (text_equal(line, "dht")) {
        stm32_dht11_debug_t d;
        int r = stm32_dht11_read_debug(&d);
        printf("[DHT] result=%s rest_level=%d failed_stage=%d bytes=%d"
               " raw=%02x,%02x,%02x,%02x,%02x sum=%02x/%02x -> T=%dC H=%u%%\n",
               r == 0 ? "OK" : "FAIL", d.rest_level, d.failed_stage,
               d.bytes_read, d.raw[0], d.raw[1], d.raw[2], d.raw[3], d.raw[4],
               d.checksum_calc, d.checksum_recv, (int)d.temp_c,
               (unsigned)d.humidity);
        printf("[DHT] resp_us=%d,%d,%d bit_us=%d,%d,%d,%d,%d,%d,%d,%d\n",
               d.resp_us[0], d.resp_us[1], d.resp_us[2], d.bit_us[0],
               d.bit_us[1], d.bit_us[2], d.bit_us[3], d.bit_us[4], d.bit_us[5],
               d.bit_us[6], d.bit_us[7]);
    } else if (text_equal(line, "time")) {
        diagnostic_time(NULL);
    } else if (text_starts_with(line, "time ")) {
        diagnostic_time(line + 5);
    } else if (text_equal(line, "cal start")) {
        printf("[TOUCH] calibration=%s\n",
               stm32_peripherals_start_touch_calibration() == 0 ?
                    "started" : "unavailable");
    } else if (text_equal(line, "rgb")) {
        diagnostic_rgb("");
    } else if (text_starts_with(line, "rgb ")) {
        diagnostic_rgb(line + 4);
    } else if (text_equal(line, "sd retry")) {
        int result = stm32_peripherals_retry_sdcard();
        printf("[TF-DIAG] retry=%s\n",
               result == 0 ? "ready" : "absent-or-failed");
        diagnostic_performance();
    } else if (text_equal(line, "sd")) {
        diagnostic_sd();
    } else if (text_equal(line, "live2d")) {
        diagnostic_live2d();
    } else if (text_equal(line, "ir")) {
        uint32_t edges = 0, frames = 0, aborts = 0;
        stm32_ir_stats(&edges, &frames, &aborts);
        printf("[IR] edges=%u frames=%u aborts=%u bindings=%u\n",
               (unsigned)edges, (unsigned)frames, (unsigned)aborts,
               ir_map_count());
        printf("[IR] press a remote key; a decoded code prints as"
               " '[IR] code=0x... action=...'\n");
    } else if (text_starts_with(line, "sd clk ")) {
        int mhz = atoi(line + 7);
        int r = stm32_sdcard_set_transfer_hz((uint32_t)mhz * 1000000U);
        printf("[TF-DIAG] clk=%s target=%dMHz -> SDIO_CK=%u Hz\n",
               r == 0 ? "set" : "rejected(0.1-24MHz)", mhz,
               (unsigned)stm32_sdcard_info()->sdio_ck_hz);
    } else if (text_starts_with(line, "sd bus ")) {
        int width = atoi(line + 7);
        int r = stm32_sdcard_set_bus_width(width);
        printf("[TF-DIAG] bus=%s width=%d (run `sd retry` to apply)\n",
               r == 0 ? "set" : "rejected(1|4)", width);
    } else if (text_equal(line, "fs")) {
        diagnostic_fs("");
    } else if (text_starts_with(line, "fs ")) {
        diagnostic_fs(line + 3);
    } else if (text_equal(line, "bt")) {
        stm32_bluetooth_debug_status();
    } else if (text_equal(line, "bt retry")) {
        int result = stm32_peripherals_retry_bluetooth();
        printf("[BT-DIAG] retry=%s\n",
               result == 0 ? "module-present" : "failed");
        stm32_bluetooth_debug_status();
        diagnostic_performance();
    } else if (text_equal(line, "bt probe")) {
        (void)stm32_bluetooth_debug_probe();
    } else if (text_starts_with(line, "bt at ")) {
        (void)stm32_bluetooth_debug_at(line + 6);
    } else if (text_equal(line, "wifi")) {
        stm32_wifi_debug_status();
    } else if (text_starts_with(line, "wifi debug ")) {
        int on = text_equal(line + 11, "on");
        stm32_wifi_set_verbose(on);
        printf("[WIFI-DIAG] per-command logging=%s\n", on ? "on" : "off");
    } else if (text_equal(line, "wifi retry")) {
        int result = stm32_peripherals_retry_wifi();
        printf("[WIFI-DIAG] retry=%s\n",
               result == 0 ? "scheduled" : "failed");
    } else if (text_equal(line, "wifi scan")) {
        printf("[WIFI-DIAG] scan=%s\n",
               stm32_wifi_scan() == 0 ? "scheduled" : "busy-or-unavailable");
    } else if (text_starts_with(line, "wifi join ")) {
        char *args = line + 10;
        char *space = strchr(args, ' ');
        if (!space) {
            printf("Usage: wifi join <ssid> <password>\n");
        } else {
            *space++ = '\0';
            printf("[WIFI-DIAG] join ssid=%s result=%s\n", args,
                   stm32_wifi_join(args, space) == 0 ?
                       "scheduled" : "invalid-or-busy");
        }
    } else if (text_starts_with(line, "wifi open ")) {
        char *save = NULL;
        char *protocol = strtok_r(line + 10, " ", &save);
        char *host = strtok_r(NULL, " ", &save);
        char *port_text = strtok_r(NULL, " ", &save);
        int port = port_text ? atoi(port_text) : 0;
        printf("[WIFI-DIAG] open=%s\n",
               protocol && host && port > 0 && port <= 65535 &&
               stm32_wifi_open(protocol, host, (uint16_t)port) == 0 ?
                   "scheduled" : "invalid-or-busy");
    } else if (text_starts_with(line, "wifi send ")) {
        const char *data = line + 10;
        printf("[WIFI-DIAG] send=%s\n",
               stm32_wifi_send(data, strlen(data)) == 0 ?
                   "scheduled" : "socket-unavailable-or-busy");
    } else if (text_equal(line, "wifi read")) {
        char data[97];
        int length = stm32_wifi_read(data, sizeof(data) - 1U);
        if (length > 0) {
            data[length] = '\0';
            printf("[WIFI-RX] bytes=%u data=", (unsigned)length);
            for (int i = 0; i < length; i++)
                printf("%c", data[i] >= 32 && data[i] <= 126 ? data[i] : '.');
            printf("\n");
        } else {
            printf("[WIFI-RX] empty\n");
        }
    } else if (text_equal(line, "wifi close")) {
        printf("[WIFI-DIAG] close=%s\n",
               stm32_wifi_close() == 0 ? "scheduled" : "busy");
    } else if (text_starts_with(line, "wifi at ")) {
        printf("[WIFI-DIAG] raw-at=%s\n",
               stm32_wifi_debug_at(line + 8) == 0 ?
                   "scheduled" : "invalid-or-busy");
    } else if (text_starts_with(line, "proxy ")) {
        char *save = NULL;
        char *ip = strtok_r(line + 6, " ", &save);
        char *port_text = strtok_r(NULL, " ", &save);
        int port = port_text ? atoi(port_text) : 0;
        printf("[NET-DIAG] proxy=%s\n",
               ip && port > 0 && port <= 65535 &&
               stm32_peripherals_set_proxy(ip, (uint16_t)port) == 0 ?
                   "set" : "invalid");
    } else {
        printf("Unknown command: %s\n", line);
        printf("Type help for available commands.\n");
    }
}

#ifndef CONFIG_STM32_QEMU
static void stm32_peripheral_thread(void) {
    for (;;) {
        uint64_t now = timer_get_ticks();
        stm32_peripherals_service(now);
        proc_yield();
    }
}
#endif

#ifdef CONFIG_STM32_QEMU
static void scheduler_probe_thread(void) {
    printf("[SCHED] probe task online, round-trip switching active\n");
    for (;;)
        proc_yield();
}
#endif

static void diagnostic_thread(void) {
    int c;
    for (;;) {
        while ((c = uart_try_getc()) >= 0) {
            if (c == '\r' || c == '\n') {
                if (diagnostic_length == 0)
                    continue;
                uart_putc('\n');
                diagnostic_line[diagnostic_length] = '\0';
                diagnostic_execute((char *)diagnostic_line);
                diagnostic_length = 0;
                diagnostic_prompt();
                continue;
            }
            if (c == 8 || c == 127) {
                if (diagnostic_length != 0) {
                    diagnostic_length--;
                    uart_puts("\b \b");
                }
                continue;
            }
            if (c < 32 || c > 126)
                continue;
            if (diagnostic_length + 1U >= sizeof(diagnostic_line)) {
                uart_putc('\a');
                continue;
            }
            diagnostic_line[diagnostic_length++] = (char)c;
            diagnostic_line[diagnostic_length] = '\0';
            if (!text_starts_with((char *)diagnostic_line, "wifi join ") ||
                !strchr((char *)diagnostic_line + 10, ' '))
                uart_putc((char)c);
        }
        proc_yield();
    }
}

void kernel_main(void) {
    arch_local_irq_disable();
    if (current_board && current_board->early_init)
        current_board->early_init();
    uart_init();
    mcu_heap_init();
    diagnostic_length = 0;

    printf("\n======================================\n");
    printf(" A20OS ARMv7-M STM32F103 bringup\n");
    printf("======================================\n");
    printf("[BOOT] board=%s arch=%s\n",
           current_board ? current_board->name : "unknown", ARCH_NAME);
    printf("[BOOT] USART1=PA9/PA10 clock=%u requested=%u"
           " actual=%u BRR=0x%x 8N1 RXNE-IRQ\n",
           (unsigned)arch_uart_clock_hz(),
           (unsigned)arch_uart_baud_rate(),
           (unsigned)arch_uart_actual_baud_rate(),
           (unsigned)arch_uart_divider());
    printf("[BOOT] heap=%u bytes\n", (unsigned)mcu_heap_available());

    void *probe = kmalloc(64);
    printf("[BOOT] allocator=%s ptr=%p\n", probe ? "ok" : "failed", probe);
    kfree(probe);

    timer_init();
#ifndef CONFIG_STM32_QEMU
    stm32_peripherals_init();
#else
    /* On QEMU the peripherals are stubbed out; exercise the hardware-
     * independent smart-hub core (rule engine, frame protocol, touch
     * calibration, UI model, Live2D state machine) here — before the
     * scheduler is brought up — so the run demonstrates real logic even while
     * the 8KB QEMU RAM model can't yet fit the scheduler task stacks. */
    smarthub_selftest();
#endif

    proc_init();
#ifdef CONFIG_STM32_QEMU
    int probe_pid = proc_alloc(scheduler_probe_thread);
    if (probe_pid < 0)
        panic("cannot create scheduler probe task");
#else
    int peripheral_pid = proc_alloc(stm32_peripheral_thread);
    if (peripheral_pid < 0)
        panic("cannot create peripheral task");
#endif
    int diagnostic_pid = proc_alloc(diagnostic_thread);
    if (diagnostic_pid < 0)
        panic("cannot create diagnostic task");
    printf("[BOOT] scheduler initialized, tasks created\n");

    arch_local_irq_enable();
    printf("[BOOT] SysTick=1000Hz source=HCLK=%u, entering scheduler\n",
           (unsigned)stm32_hclk_hz());
    diagnostic_help();
    diagnostic_prompt();

    sched();
    idle_loop();
}
