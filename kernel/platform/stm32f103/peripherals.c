#ifdef CONFIG_BOARD_STM32F103

#include "peripherals.h"
#include "core/arch.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "drivers/stm32f1/actuators.h"
#include "board.h"
#include "drivers/stm32f1/bluetooth.h"
#include "bluetooth_cmd.h"
#include "cjk_font.h"
#include "drivers/stm32f1/dht11.h"
#include "drivers/stm32f1/display.h"
#include "drivers/stm32f1/extsram.h"
#include "hub_config.h"
#include "hub_controller.h"
#include "hub_proto.h"
#include "hub_ui.h"
#include "drivers/stm32f1/ir.h"
#include "live2d.h"
#include "live2d_load.h"
#include "drivers/stm32f1/rtc.h"
#include "drivers/stm32f1/rgb_matrix.h"
#include "drivers/stm32f1/stm32_uart.h" /* stm32_hclk_hz() for the diagnostics page */
#include "ui_diag.h"
#include "ui_home.h"
#include "ui_render.h"
#include "drivers/stm32f1/keys.h"
#include "drivers/stm32f1/light_sensor.h"
#include "drivers/stm32f1/memory.h"
#include "drivers/stm32f1/sdcard.h"
#include "sdfs.h"
#include "drivers/stm32f1/touch.h"
#include "drivers/stm32f1/watchdog.h"
#include "touch_cal.h"
#include "drivers/stm32f1/wifi.h"

#define TOUCH_POLL_INTERVAL_MS 20U
#define KEY_POLL_INTERVAL_MS 20U
#define BLUETOOTH_POLL_INTERVAL_MS 20U
#define WIFI_POLL_INTERVAL_MS 20U
#define MEMORY_POLL_INTERVAL_MS 1000U
#define LIGHT_POLL_INTERVAL_MS 250U
#define SDCARD_CHECK_INTERVAL_MS 2000U
#define SERVICE_SLOW_THRESHOLD_MS 25U
#define CONTROL_TICK_INTERVAL_MS 2000U
#define MANUAL_OVERRIDE_MS 600000U
#define TOUCH_LONG_PRESS_MS 700U

static stm32_peripheral_state_t peripherals;
static uint64_t last_touch_poll;
static uint64_t last_key_poll;
static uint64_t last_bluetooth_poll;
static uint64_t last_wifi_poll;
static uint64_t last_memory_poll;
static uint64_t last_light_poll;
static uint64_t last_sdcard_check;
static uint64_t last_control_tick;
static uint64_t last_environment_poll;
static uint32_t last_live2d_clock;
static int touch_pressed;
static uint16_t touch_down_x;
static uint16_t touch_down_y;
static uint16_t touch_last_x;
static uint16_t touch_last_y;
static uint64_t touch_down_at;
static char bluetooth_line[STM32_BLUETOOTH_LINE_MAX];

/* Smart-hub control state. */
static hub_controller_t hub_ctl;
static int dht_present;
static int16_t last_temp_c;
static uint8_t last_humidity;
static int have_env;
static uint64_t buzzer_off_at;
static int buzzer_active;
static int buzzer_muted;
static uint8_t net_seq; /* SNAPSHOT frame sequence number       */
static int ir_present;

/* --- cloud control over the non-blocking wifi.c TCP socket --- */
/*
 * How long a SNAPSHOT waits for its CONTROL. This has to cover a real LLM
 * round-trip, not just the wire: measured on the bench, the proxy's live call
 * (deepseek-v4-flash) takes 2.5-2.6s, on top of ~140ms RTT and ~75ms to shift a
 * ~70-byte frame in over the ESP8266's 9600 baud link. At the old 1500ms every
 * live reply arrived after cloud_awaiting had already been cleared and was
 * dropped by the seq/deadline check below — no speech ever reached the bubble,
 * and the catgirl never saw a cloud mood. Keep it under CLOUD_FRESH_MS so a
 * reply that is accepted is still fresh enough to apply.
 */
#define CLOUD_REPLY_TIMEOUT_MS 6000U
/*
 * How long a cloud decision keeps overriding the local rules. Must outlast the
 * gap between asks (CLOUD_HEARTBEAT_MS) plus a reply, or the decision would
 * lapse back to local between heartbeats and the source would flap.
 */
#define CLOUD_FRESH_MS 40000U
#define CLOUD_CONNECT_BACKOFF_MS 5000U  /* retry cadence for CIPSTART          */
static char cloud_proxy_ip[16] = HUB_PROXY_IP;
static uint16_t cloud_proxy_port = HUB_PROXY_PORT;
static uint8_t cloud_rx[HUB_OVERHEAD + HUB_MAX_PAYLOAD]; /* frame reassembly    */
static size_t cloud_rx_len;
static uint64_t cloud_connect_retry_at;
static int cloud_awaiting;           /* SNAPSHOT sent, CONTROL not yet parsed  */
static uint8_t cloud_expected_seq;
static uint64_t cloud_reply_deadline;
static hub_control_t cloud_last;     /* most recent CONTROL from the proxy     */
static int cloud_last_valid;
static uint64_t cloud_last_ms;
/* The control task publishes snapshots here; only the network task touches
 * wifi.c and consumes them. IRQ masking makes the handoff atomic against the
 * 10 ms PendSV preemption on this single-core target. */
static env_snapshot_t cloud_snapshot_pending;
static int cloud_snapshot_pending_valid;

static void cloud_publish_control(const hub_control_t *control, uint64_t now) {
    uint32_t flags = arch_irq_save();
    cloud_last = *control;
    cloud_last_valid = 1;
    cloud_last_ms = now;
    arch_irq_restore(flags);
}

static int cloud_read_control(hub_control_t *control, uint64_t *updated_at) {
    uint32_t flags = arch_irq_save();
    int valid = cloud_last_valid;
    if (valid) {
        *control = cloud_last;
        *updated_at = cloud_last_ms;
    }
    arch_irq_restore(flags);
    return valid;
}

static void cloud_publish_snapshot(const env_snapshot_t *snapshot) {
    uint32_t flags = arch_irq_save();
    cloud_snapshot_pending = *snapshot;
    cloud_snapshot_pending_valid = 1;
    arch_irq_restore(flags);
}

static int cloud_take_snapshot(env_snapshot_t *snapshot) {
    uint32_t flags = arch_irq_save();
    int valid = cloud_snapshot_pending_valid;
    if (valid) {
        *snapshot = cloud_snapshot_pending;
        cloud_snapshot_pending_valid = 0;
    }
    arch_irq_restore(flags);
    return valid;
}

/*
 * When is it worth asking the cloud?
 *
 * Every control tick (2s) was wrong on three counts. It re-asked an LLM to
 * narrate a room that had not changed — ~900 calls an hour, real money for no
 * information. Each reply carried a new speech line, and a new line restarts
 * TALK for up to 6s; at one line every ~4s the catgirl never left TALK, so the
 * cloud mood was never applied (a live TALK outranks it) and IDLE never got a
 * chance to play normally.
 *
 * So: ask on a real change, on an explicit request (tapping the cat), or on a
 * slow heartbeat. Light is deliberately not a trigger — it swings tens of
 * percent as people move past the sensor and says nothing about comfort.
 */
#define CLOUD_HEARTBEAT_MS 30000U /* refresh even when nothing changed */
#define CLOUD_TEMP_DELTA 2        /* degC that counts as "something changed" */
#define CLOUD_HUMI_DELTA 5        /* %RH  */

static uint64_t cloud_last_ask_ms;
static env_snapshot_t cloud_asked_snap; /* what the last ask described */
static int cloud_ask_now;               /* set by a tap: ask on the next tick */

static int abs_diff(int a, int b) { return a > b ? a - b : b - a; }

static int cloud_should_ask(const env_snapshot_t *s, uint64_t now) {
    if (cloud_ask_now || cloud_last_ask_ms == 0)
        return 1;
    if (now - cloud_last_ask_ms >= CLOUD_HEARTBEAT_MS)
        return 1;
    if (s->valid != cloud_asked_snap.valid)
        return 1; /* the sensor came back or died — worth a word */
    if (!s->valid)
        return 0;
    if (abs_diff(s->temp_c, cloud_asked_snap.temp_c) >= CLOUD_TEMP_DELTA)
        return 1;
    if (abs_diff(s->humidity, cloud_asked_snap.humidity) >= CLOUD_HUMI_DELTA)
        return 1;
    if (s->hour != cloud_asked_snap.hour)
        return 1; /* the hour drives night/day behaviour */
    return 0;
}

static void cloud_parse_frames(uint64_t now) {
    size_t off = 0;

    while (off < cloud_rx_len) {
        hub_frame_t f;
        int r = hub_proto_parse(cloud_rx + off, cloud_rx_len - off, &f);
        if (r == 0)
            break;
        if (r < 0) {
            off++;
            continue;
        }
        hub_control_t cc;
        hub_time_t network_time;
        if (cloud_awaiting && now < cloud_reply_deadline &&
            f.seq == cloud_expected_seq &&
            hub_proto_decode_time(&f, &network_time) == 0) {
            if (stm32_rtc_set_network_time(
                    network_time.unix_utc,
                    network_time.utc_offset_minutes) == 0) {
                int hour, minute, second;
                stm32_rtc_get_hhmmss(&hour, &minute, &second);
                printf("[RTC] network sync %02d:%02d:%02d UTC%+d:%02d"
                       " epoch=%u count=%u\n",
                       hour, minute, second,
                       network_time.utc_offset_minutes / 60,
                       network_time.utc_offset_minutes < 0
                           ? -(network_time.utc_offset_minutes % 60)
                           : network_time.utc_offset_minutes % 60,
                       (unsigned)network_time.unix_utc,
                       (unsigned)stm32_rtc_sync_count());
            }
        }
        if (cloud_awaiting && now < cloud_reply_deadline &&
            f.seq == cloud_expected_seq &&
            hub_proto_decode_control(&f, &cc) == 0) {
            cloud_publish_control(&cc, now);
            cloud_awaiting = 0;
            printf("[NET] cloud fan=%u pump=%u theme=%u mood=%u speech=%s\n",
                   cc.fan_level, cc.pump_on, cc.theme_id, cc.mood, cc.speech);
        }
        off += (size_t)r;
    }
    if (off > 0) {
        size_t rem = cloud_rx_len - off;
        for (size_t i = 0; i < rem; i++)
            cloud_rx[i] = cloud_rx[off + i];
        cloud_rx_len = rem;
    } else if (cloud_rx_len == sizeof(cloud_rx)) {
        cloud_rx_len = 0;
    }
}

/* Assembled UI/Live2D state for the on-board renderer. */
static ui_home_state_t g_ui_state;
static live2d_t g_cat;
static hub_ui_input_t g_ui_pending;
static char g_ui_pending_speech[HUB_SPEECH_MAX + 1];
static int g_ui_pending_valid;
static char g_ui_speech[HUB_SPEECH_MAX + 1];
static char g_last_speech[HUB_SPEECH_MAX + 1];
static uint64_t g_last_speech_ms;
/* How long the newest LLM line stays in the dialog box without a refresh. */
#define SPEECH_STICKY_MS 60000U
static ui_home_model_t g_ui_model;
static ui_gfx_t g_ui_gfx;
static uint8_t g_manual_mask;
static uint8_t g_manual_fan;
static uint8_t g_manual_pump;
static uint8_t g_manual_theme;
static uint8_t g_manual_backlight;
static uint8_t g_backlight; /* backlight percent in effect, for the LIGHT card */
static uint64_t g_manual_until;
static int g_ui_ready;
static ui_home_state_t g_rendered_state;
static char g_rendered_speech[HUB_SPEECH_MAX + 1];
static int g_rendered_state_valid;

/* Which page the panel is showing. The SYS/MEM/TF/BT instrumentation lives on
 * PAGE_DIAG, one level below the hub UI (see ui_diag.h). */
#define PAGE_HOME 0
#define PAGE_DIAG 1
static int g_page;
static ui_diag_model_t g_diag_model;

/* Mirrors ui_home.h's UI_MANUAL_* bits; the mask is handed to the UI model. */
#define MANUAL_FAN  0x01U
#define MANUAL_PUMP 0x02U
#define MANUAL_THEME 0x04U
#define MANUAL_BACKLIGHT 0x08U
#define BACKLIGHT_STEP 10U

static touch_cal_point_t g_cal_points[4];
static unsigned g_cal_index;
static int g_cal_active;
static int g_cal_pressed;
static uint32_t g_cal_raw_x_sum;
static uint32_t g_cal_raw_y_sum;
static unsigned g_cal_sample_count;
static unsigned g_cal_release_count;

/* Config loaded from /CFG/WIFI.TXT at boot (SSID used for a one-shot join). */
static hub_cfg_t boot_cfg;
static int cfg_join_attempted;
static env_snapshot_t sensor_snapshot;
static int sensor_snapshot_valid;

static void run_control_tick(uint64_t now);

static void ui_publish_input(const hub_ui_input_t *input) {
    uint32_t flags = arch_irq_save();
    g_ui_pending = *input;
    if (input->cloud_speech) {
        strncpy(g_ui_pending_speech, input->cloud_speech,
                sizeof(g_ui_pending_speech) - 1U);
        g_ui_pending_speech[sizeof(g_ui_pending_speech) - 1U] = '\0';
    } else {
        g_ui_pending_speech[0] = '\0';
    }
    g_ui_pending.cloud_speech = NULL;
    g_ui_pending_valid = 1;
    arch_irq_restore(flags);
}

static int ui_take_input(hub_ui_input_t *input, char *speech) {
    uint32_t flags = arch_irq_save();
    int valid = g_ui_pending_valid;
    if (valid) {
        *input = g_ui_pending;
        strncpy(speech, g_ui_pending_speech, HUB_SPEECH_MAX);
        speech[HUB_SPEECH_MAX] = '\0';
        g_ui_pending_valid = 0;
    }
    arch_irq_restore(flags);
    if (valid)
        input->cloud_speech = speech[0] ? speech : NULL;
    return valid;
}

static void sensor_publish_snapshot(const env_snapshot_t *snapshot) {
    uint32_t flags = arch_irq_save();
    sensor_snapshot = *snapshot;
    sensor_snapshot_valid = 1;
    arch_irq_restore(flags);
}

static int sensor_read_snapshot(env_snapshot_t *snapshot) {
    uint32_t flags = arch_irq_save();
    int valid = sensor_snapshot_valid;
    if (valid)
        *snapshot = sensor_snapshot;
    arch_irq_restore(flags);
    return valid;
}

/* Buzzer feedback durations (ms). */
#define BUZZ_BOOT_MS 120U  /* "hub is alive" chime at startup       */
#define BUZZ_INPUT_MS 30U  /* short chirp on key/touch/IR input     */

/*
 * Non-blocking buzzer: drive PB8 high now and schedule the off in the service
 * loop. Only ever extends an in-progress beep (so a short input chirp can't
 * cut an alert beep short). Buzzer = active buzzer on PB8 (docs/pz §8).
 */
static void hub_buzz(uint64_t now, uint32_t ms) {
    if (buzzer_muted)
        return;
    stm32_buzzer_set(1);
    uint64_t until = now + ms;
    if (!buzzer_active || until > buzzer_off_at)
        buzzer_off_at = until;
    buzzer_active = 1;
}

/* Snapshot the driver status the diagnostics page formats. */
static void diag_state_build(ui_diag_state_t *d) {
    const stm32_memory_info_t *mem = stm32_memory_info();
    const stm32_sdcard_info_t *sd = stm32_sdcard_info();
    const stm32_bluetooth_info_t *bt = stm32_bluetooth_info();
    const stm32_wifi_info_t *w = stm32_wifi_info();

    d->hclk_hz = stm32_hclk_hz();
    d->uptime_s = (uint32_t)(timer_get_ticks() / 1000ULL);
    d->ram_used = mem->internal_ram_used;
    d->ram_total = mem->internal_ram_total;
    d->ext_used = mem->external_ram_used;
    d->ext_total = mem->external_ram_total;
    d->sd_ready = peripherals.sdcard_ready;
    d->sd_fat32 = sd->fat32;
    d->bt_ready = peripherals.bluetooth_ready;
    d->bt_connected = peripherals.bluetooth_connected;
    d->bt_waiting = bt->waiting;
    d->wifi_ready = w->at_responsive;
    d->wifi_joined = w->joined;
    d->wifi_socket = w->socket_connected;
    d->touch_ready = peripherals.touch_armed;
    d->ir_ready = ir_present;
    d->ir_bindings = ir_map_count();
}

static int ui_draw_cached_live2d(void) {
    int drawn;
    stm32_sdfs_lock();
    drawn = live2d_load_draw(&g_ui_gfx, &g_cat);
    stm32_sdfs_unlock();
    return drawn;
}

static void ui_render_full(void) {
    if (!peripherals.display_ready || g_cal_active)
        return;
    /*
     * Give the loader a turn before painting. A repaint triggered by a state
     * change (tapping the cat, a new cloud mood) would otherwise run before the
     * service loop had read a single frame of the new state, so it drew the
     * procedural placeholder — the sprite visibly vanished on every tap. The
     * loader bursts frame 0 on a state change, so one call is enough.
     */
    /* Built either way: the diagnostics page borrows this model's palette. */
    ui_home_build(&g_ui_state, &g_ui_model);
    if (g_page == PAGE_DIAG) {
        ui_diag_state_t ds;
        diag_state_build(&ds);
        ui_diag_build(&ds, &g_diag_model);
        ui_render_diag(&g_ui_gfx, &g_diag_model, &g_ui_model);
        g_ui_ready = 1;
        g_rendered_state_valid = 0;
        return;
    }
    ui_render_home_ex(&g_ui_gfx, &g_ui_model, &g_cat, NULL, 0, 0, 0);
    if (!ui_draw_cached_live2d())
        ui_render_cat(&g_ui_gfx, &g_ui_model, &g_cat);
    g_ui_ready = 1;
    g_rendered_state = g_ui_state;
    if (g_ui_state.speech) {
        strncpy(g_rendered_speech, g_ui_state.speech,
                sizeof(g_rendered_speech) - 1U);
        g_rendered_speech[sizeof(g_rendered_speech) - 1U] = '\0';
        g_rendered_state.speech = g_rendered_speech;
    } else {
        g_rendered_speech[0] = '\0';
        g_rendered_state.speech = NULL;
    }
    g_rendered_state_valid = 1;
}

static int ui_text_changed(const char *a, const char *b) {
    if (!a || !b)
        return a != b;
    return strcmp(a, b) != 0;
}

/* The 16-bit FSMC panel is written directly, so a full render is visible while
 * it is in progress. Periodic control ticks update only the regions whose
 * model inputs changed; theme/alert changes are rare and still repaint fully. */
static void ui_render_update(void) {
    unsigned regions = 0;

    if (!peripherals.display_ready || g_cal_active || g_page != PAGE_HOME)
        return;
    if (!g_ui_ready || !g_rendered_state_valid ||
        g_rendered_state.theme != g_ui_state.theme ||
        g_rendered_state.alert != g_ui_state.alert) {
        ui_render_full();
        return;
    }

    if (g_rendered_state.hour != g_ui_state.hour ||
        g_rendered_state.minute != g_ui_state.minute ||
        g_rendered_state.net_cloud != g_ui_state.net_cloud)
        regions |= UI_RENDER_REGION_HEADER;
    if (g_rendered_state.temp_c != g_ui_state.temp_c)
        regions |= UI_RENDER_REGION_CARD0;
    if (g_rendered_state.humidity != g_ui_state.humidity)
        regions |= UI_RENDER_REGION_CARD1;
    if (g_rendered_state.light != g_ui_state.light ||
        g_rendered_state.backlight != g_ui_state.backlight)
        regions |= UI_RENDER_REGION_CARD2;
    if (g_rendered_state.fan_level != g_ui_state.fan_level ||
        ((g_rendered_state.manual_mask ^ g_ui_state.manual_mask) & MANUAL_FAN))
        regions |= UI_RENDER_REGION_CARD3;
    if (ui_text_changed(g_rendered_state.speech, g_ui_state.speech))
        regions |= UI_RENDER_REGION_DIALOG;

    if (regions) {
        ui_home_build(&g_ui_state, &g_ui_model);
        ui_render_home_regions(&g_ui_gfx, &g_ui_model, regions);
    }
    g_rendered_state = g_ui_state;
    if (g_ui_state.speech) {
        strncpy(g_rendered_speech, g_ui_state.speech,
                sizeof(g_rendered_speech) - 1U);
        g_rendered_speech[sizeof(g_rendered_speech) - 1U] = '\0';
        g_rendered_state.speech = g_rendered_speech;
    } else {
        g_rendered_speech[0] = '\0';
        g_rendered_state.speech = NULL;
    }
}

static void ui_render_cat_frame(void) {
    if (!g_ui_ready || g_cal_active || g_page != PAGE_HOME ||
        !peripherals.display_ready)
        return;
    if (!ui_draw_cached_live2d())
        ui_render_cat(&g_ui_gfx, &g_ui_model, &g_cat);
}

static void ui_manual_arm(uint8_t mask, uint64_t now) {
    g_manual_mask |= mask;
    g_manual_until = now + MANUAL_OVERRIDE_MS;
}

static void ui_apply_action(ui_action_t action, uint64_t now) {
    if (action == UI_ACTION_NONE)
        return;
    uint32_t flags = arch_irq_save();
    if (action == UI_ACTION_FAN_UP || action == UI_ACTION_FAN_DOWN) {
        int level = g_ui_state.fan_level;
        level += action == UI_ACTION_FAN_UP ? 1 : -1;
        if (level < 0)
            level = 0;
        if (level > 3)
            level = 3;
        g_manual_fan = (uint8_t)level;
        g_ui_state.fan_level = g_manual_fan;
        stm32_fan_set_level(g_manual_fan);
        ui_manual_arm(MANUAL_FAN, now);
    } else if (action == UI_ACTION_PUMP_TOGGLE) {
        g_manual_pump = g_ui_state.pump_on ? 0U : 1U;
        g_ui_state.pump_on = g_manual_pump;
        stm32_pump_set(g_manual_pump);
        ui_manual_arm(MANUAL_PUMP, now);
    } else if (action == UI_ACTION_THEME_CYCLE) {
        g_manual_theme = (uint8_t)((g_ui_state.theme + 1U) % 3U);
        g_ui_state.theme = g_manual_theme;
        ui_manual_arm(MANUAL_THEME, now);
    } else if (action == UI_ACTION_LIGHT_UP || action == UI_ACTION_LIGHT_DOWN) {
        int pct = (int)g_backlight;
        pct += action == UI_ACTION_LIGHT_UP ? (int)BACKLIGHT_STEP
                                            : -(int)BACKLIGHT_STEP;
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        g_manual_backlight = (uint8_t)pct;
        g_backlight = g_manual_backlight;
        g_ui_state.backlight = g_manual_backlight;
        stm32_light_sensor_set_manual_brightness(g_manual_backlight);
        ui_manual_arm(MANUAL_BACKLIGHT, now);
    } else if (action == UI_ACTION_MENU) {
        g_page = g_page == PAGE_HOME ? PAGE_DIAG : PAGE_HOME;
        g_ui_ready = 0;
    } else if (action == UI_ACTION_TALK) {
        /*
         * Replay the current line, and ask the proxy for a new one. The cloud
         * is otherwise only asked on a real change or a slow heartbeat
         * (cloud_should_ask), so this is what makes tapping her feel like
         * asking a question. Speech is never invented here.
         */
        live2d_speak(&g_cat, g_ui_state.speech, (uint32_t)now);
        cloud_ask_now = 1;
        last_control_tick = 0;
    }
    g_ui_state.manual_mask = g_manual_mask;
    arch_irq_restore(flags);
    hub_buzz(now, BUZZ_INPUT_MS);
    ui_render_full();
}

/*
 * Long-press a control -> hand it straight back to the rule engine. Tapping a
 * card takes manual ownership for MANUAL_OVERRIDE_MS (10 min); without this
 * there is no way to give it back early and the card just sits on MANU. Returns
 * 1 when it released one, 0 to let the normal tap action run instead.
 */
static int ui_release_manual(ui_action_t action, uint64_t now) {
    uint8_t bit = 0;

    if (action == UI_ACTION_FAN_UP || action == UI_ACTION_FAN_DOWN)
        bit = MANUAL_FAN;
    else if (action == UI_ACTION_LIGHT_UP || action == UI_ACTION_LIGHT_DOWN)
        bit = MANUAL_BACKLIGHT;
    else if (action == UI_ACTION_PUMP_TOGGLE)
        bit = MANUAL_PUMP;
    else if (action == UI_ACTION_THEME_CYCLE)
        bit = MANUAL_THEME;
    uint32_t flags = arch_irq_save();
    if (!bit || !(g_manual_mask & bit)) {
        arch_irq_restore(flags);
        return 0;
    }
    g_manual_mask &= (uint8_t)~bit;
    g_ui_state.manual_mask = g_manual_mask;
    last_control_tick = 0; /* let the rule engine retake it now, not in 2s */
    arch_irq_restore(flags);
    hub_buzz(now, BUZZ_INPUT_MS);
    printf("[TOUCH] long-press -> auto (manual mask=0x%x)\n",
           (unsigned)g_manual_mask);
    ui_render_full();
    return 1;
}

/* Diagnostics-page buttons. CAL and the two radio pokes are here rather than
 * on the home screen because they interrupt what the hub is doing. */
static void apply_diag_action(ui_diag_action_t action, uint64_t now) {
    if (action == UI_DIAG_ACTION_NONE)
        return;
    hub_buzz(now, BUZZ_INPUT_MS);
    switch (action) {
    case UI_DIAG_ACTION_BACK:
        g_page = PAGE_HOME;
        g_ui_ready = 0;
        ui_render_full();
        break;
    case UI_DIAG_ACTION_CALIBRATE:
        /* Calibration owns the whole panel and returns to the home screen. */
        g_page = PAGE_HOME;
        g_ui_ready = 0;
        (void)stm32_peripherals_start_touch_calibration();
        break;
    case UI_DIAG_ACTION_WIFI_SCAN:
        if (stm32_wifi_scan() == 0)
            printf("[WIFI] AP scan started\n");
        ui_render_full();
        break;
    case UI_DIAG_ACTION_BT_TEST: {
        static const char message[] = "A20OS HC05 TEST\r\n";
        if (peripherals.bluetooth_ready &&
            stm32_bluetooth_send(message, sizeof(message) - 1U) == 0)
            printf("[BT] sent test message\n");
        ui_render_full();
        break;
    }
    default:
        break;
    }
}

static ui_action_t ui_action_from_ir(int action) {
    if (action == IR_ACT_FAN_UP)
        return UI_ACTION_FAN_UP;
    if (action == IR_ACT_FAN_DOWN)
        return UI_ACTION_FAN_DOWN;
    if (action == IR_ACT_PUMP_TOGGLE)
        return UI_ACTION_PUMP_TOGGLE;
    if (action == IR_ACT_THEME_CYCLE)
        return UI_ACTION_THEME_CYCLE;
    if (action == IR_ACT_TALK)
        return UI_ACTION_TALK;
    if (action == IR_ACT_MENU)
        return UI_ACTION_MENU;
    if (action == IR_ACT_LIGHT_UP)
        return UI_ACTION_LIGHT_UP;
    if (action == IR_ACT_LIGHT_DOWN)
        return UI_ACTION_LIGHT_DOWN;
    return UI_ACTION_NONE;
}

static ui_action_t ui_action_from_key(stm32_key_t key) {
    if (key == STM32_KEY_UP)
        return UI_ACTION_FAN_UP;
    if (key == STM32_KEY_DOWN)
        return UI_ACTION_FAN_DOWN;
    if (key == STM32_KEY_LEFT)
        return UI_ACTION_PUMP_TOGGLE;
    if (key == STM32_KEY_RIGHT)
        return UI_ACTION_THEME_CYCLE;
    return UI_ACTION_NONE;
}

/* One dispatcher for every remote input (IR, Bluetooth, direction keys), so
 * they cannot drift apart. MUTE is handled here because it drives the buzzer
 * rather than the UI model. */
static void apply_remote_action(int action, uint64_t now) {
    if (action == IR_ACT_NONE)
        return;
    if (action == IR_ACT_MUTE_BUZZER) {
        buzzer_muted = !buzzer_muted;
        if (buzzer_muted) {
            stm32_buzzer_set(0);
            buzzer_active = 0;
        } else {
            hub_buzz(now, BUZZ_INPUT_MS);
        }
        printf("[IN] buzzer=%s\n", buzzer_muted ? "muted" : "on");
        return;
    }
    ui_apply_action(ui_action_from_ir(action), now);
}

static void ui_handle_bluetooth_command(const char *line, uint64_t now) {
    bluetooth_cmd_t cmd;
    int redraw = 1;
    static const char help[] =
        "HELP 1..9 STATUS PING | FAN 0..3 | PUMP ON/OFF | "
        "THEME DAY/NIGHT/COZY | LIGHT 0..100 | MUTE ON/OFF | "
        "AUTO FAN/PUMP/THEME/LIGHT/ALL\r\n";

    if (!stm32_bluetooth_connected())
        return;
    if (bluetooth_cmd_parse(line, &cmd) != 0) {
        static const char error[] = "ERR unknown command; send HELP\r\n";
        (void)stm32_bluetooth_send(error, sizeof(error) - 1U);
        return;
    }
    if (cmd.kind == BLUETOOTH_CMD_HELP) {
        (void)stm32_bluetooth_send(help, sizeof(help) - 1U);
        return;
    }
    if (cmd.kind == BLUETOOTH_CMD_PING) {
        static const char pong[] = "PONG A20OS\r\n";
        (void)stm32_bluetooth_send(pong, sizeof(pong) - 1U);
        return;
    }
    if (cmd.kind == BLUETOOTH_CMD_STATUS) {
        char status[128];
        int n = snprintf(status, sizeof(status),
            "STATUS T=%d H=%u L=%u FAN=%u PUMP=%u BL=%u THEME=%s "
            "MODE=%02x MUTE=%u NET=%s\r\n",
            (int)g_ui_state.temp_c, (unsigned)g_ui_state.humidity,
            (unsigned)g_ui_state.light, (unsigned)g_ui_state.fan_level,
            (unsigned)g_ui_state.pump_on, (unsigned)g_ui_state.backlight,
            env_theme_name((env_theme_t)g_ui_state.theme),
            (unsigned)g_manual_mask, (unsigned)buzzer_muted,
            g_ui_state.net_cloud ? "CLOUD" : "LOCAL");
        if (n > 0)
            (void)stm32_bluetooth_send(status,
                (size_t)n < sizeof(status) ? (size_t)n : sizeof(status) - 1U);
        return;
    }
    if (cmd.kind == BLUETOOTH_CMD_ACTION) {
        apply_remote_action(cmd.value, now);
        redraw = 0; /* UI actions already repaint; MUTE has nothing to repaint. */
    } else {
        uint32_t flags = arch_irq_save();
        if (cmd.kind == BLUETOOTH_CMD_FAN_SET) {
        g_manual_fan = g_ui_state.fan_level = (uint8_t)cmd.value;
        stm32_fan_set_level(g_manual_fan);
        ui_manual_arm(MANUAL_FAN, now);
        } else if (cmd.kind == BLUETOOTH_CMD_PUMP_SET) {
        g_manual_pump = g_ui_state.pump_on = (uint8_t)cmd.value;
        stm32_pump_set(g_manual_pump);
        ui_manual_arm(MANUAL_PUMP, now);
        } else if (cmd.kind == BLUETOOTH_CMD_THEME_SET) {
        g_manual_theme = g_ui_state.theme = (uint8_t)cmd.value;
        ui_manual_arm(MANUAL_THEME, now);
        } else if (cmd.kind == BLUETOOTH_CMD_LIGHT_SET) {
        g_manual_backlight = g_backlight = g_ui_state.backlight =
            (uint8_t)cmd.value;
        stm32_light_sensor_set_manual_brightness(g_manual_backlight);
        ui_manual_arm(MANUAL_BACKLIGHT, now);
        } else if (cmd.kind == BLUETOOTH_CMD_MUTE_SET) {
        buzzer_muted = cmd.value;
        if (buzzer_muted) {
            stm32_buzzer_set(0);
            buzzer_active = 0;
        }
        } else if (cmd.kind == BLUETOOTH_CMD_AUTO) {
        g_manual_mask &= (uint8_t)~cmd.value;
        g_ui_state.manual_mask = g_manual_mask;
        last_control_tick = 0;
        }
        arch_irq_restore(flags);
    }
    g_ui_state.manual_mask = g_manual_mask;
    if (redraw)
        ui_render_full();
    {
        static const char ok[] = "OK\r\n";
        (void)stm32_bluetooth_send(ok, sizeof(ok) - 1U);
    }
}

static void touch_calibration_render(void) {
    ui_render_cal_target(&g_ui_gfx, (int)g_cal_index, 4);
}

int stm32_peripherals_start_touch_calibration(void) {
    if (!peripherals.display_ready || !peripherals.touch_armed)
        return -1;
    g_cal_index = 0;
    g_cal_active = 1;
    g_cal_pressed = 0;
    touch_pressed = 0;
    g_cal_raw_x_sum = 0;
    g_cal_raw_y_sum = 0;
    g_cal_sample_count = 0;
    g_cal_release_count = 0;
    touch_calibration_render();
    return 0;
}

static void touch_calibration_service(void) {
    static const uint16_t sx[4] = {
        UI_RENDER_CAL_INSET, UI_RENDER_W - UI_RENDER_CAL_INSET,
        UI_RENDER_CAL_INSET, UI_RENDER_W - UI_RENDER_CAL_INSET,
    };
    static const uint16_t sy[4] = {
        UI_RENDER_CAL_INSET, UI_RENDER_CAL_INSET,
        UI_RENDER_H - UI_RENDER_CAL_INSET,
        UI_RENDER_H - UI_RENDER_CAL_INSET,
    };
    uint16_t raw_x = 0;
    uint16_t raw_y = 0;
    int sample_valid = stm32_touch_poll_raw(&raw_x, &raw_y);
    int pressed = stm32_touch_pressed();

    if (sample_valid) {
        g_cal_release_count = 0;
        if (!g_cal_pressed) {
            g_cal_raw_x_sum = 0;
            g_cal_raw_y_sum = 0;
            g_cal_sample_count = 0;
        }
        if (g_cal_sample_count < 64U) {
            g_cal_raw_x_sum += raw_x;
            g_cal_raw_y_sum += raw_y;
            g_cal_sample_count++;
        }
    } else if (!pressed && g_cal_pressed && g_cal_release_count < 3U) {
        g_cal_release_count++;
        if (g_cal_release_count < 3U)
            return;
    }
    if (!pressed && g_cal_pressed) {
        if (g_cal_sample_count >= 5U) {
            g_cal_points[g_cal_index].raw_x =
                (uint16_t)(g_cal_raw_x_sum / g_cal_sample_count);
            g_cal_points[g_cal_index].raw_y =
                (uint16_t)(g_cal_raw_y_sum / g_cal_sample_count);
            g_cal_points[g_cal_index].screen_x = sx[g_cal_index];
            g_cal_points[g_cal_index].screen_y = sy[g_cal_index];
            g_cal_index++;
            hub_buzz(timer_get_ticks(), BUZZ_INPUT_MS);
        }
        if (g_cal_index < 4U) {
            touch_calibration_render();
        } else {
            stm32_touch_calibration_t cal;
            int r = touch_cal_solve(g_cal_points, 4, UI_RENDER_W,
                                    UI_RENDER_H, &cal);
            if (r == 0) {
                stm32_touch_set_calibration(&cal);
                if (stm32_sdfs_ready()) {
                    stm32_sdfs_lock();
                    r = stm32_sdfs_save_touch_cal(&cal);
                    stm32_sdfs_unlock();
                }
            }
            printf("[TOUCH] calibration %s save=%d\n",
                   r == 0 ? "complete" : "failed", r);
            g_cal_active = 0;
            touch_pressed = 0;
            g_cal_index = 0;
            g_ui_ready = 0;
            ui_render_full();
        }
    }
    g_cal_pressed = pressed;
}

static void update_wifi_display(void) {
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
    const stm32_wifi_info_t *wifi = stm32_wifi_info();
    stm32_display_set_wifi(
        wifi->active, wifi->detected, wifi->at_responsive,
        wifi->configured, wifi->connecting, wifi->joined,
        wifi->got_ip, wifi->socket_connected, wifi->ssid,
        wifi->ip_address, wifi->mac_address, wifi->scan_ssid,
        wifi->access_points, wifi->baud_rate, wifi->received_bytes,
        wifi->transmitted_bytes, wifi->dropped_bytes,
        wifi->last_event);
#endif
}

static void update_memory_display(void) {
    stm32_memory_refresh();
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
    stm32_display_set_memory(stm32_memory_info());
#endif
}

static void update_bluetooth_display(void) {
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
    const stm32_bluetooth_info_t *bt = stm32_bluetooth_info();
    stm32_display_set_bluetooth(
        peripherals.bluetooth_ready, bt->detected, bt->at_responsive,
        bt->connected, bt->waiting, bt->configured,
        bt->slave_mode, bt->uuid_supported, bt->uuid_configured,
        bt->device_name, bt->pin[0] ? "configured" : "(unread)",
        bt->service_uuid, bt->baud_rate,
        bt->received_bytes, bt->transmitted_bytes, bt->dropped_bytes);
#endif
}

static void report_bluetooth(void) {
    const stm32_bluetooth_info_t *bt = stm32_bluetooth_info();
    static const char *const rx_states[] = {
        "unknown", "driven-high", "floating", "driven-low",
    };
    unsigned rx_state = (unsigned)bt->rx_line_state;
    if (rx_state >= sizeof(rx_states) / sizeof(rx_states[0]))
        rx_state = 0;

    printf("[BT] interface=%s detected=%s rx=%s at=%s"
           " role=%s state=%s name=%s pin=%s\n",
           bt->ready ? "armed" : "disabled",
           bt->detected ? "yes" : "no",
           rx_states[rx_state],
           bt->at_responsive ? "responsive" : "no-response",
           bt->slave_mode ? "slave" : "unknown",
           bt->waiting ? "slave-waiting" : "not-waiting",
            bt->device_name[0] ? bt->device_name : "(unread)",
            bt->pin[0] ? "configured" : "(unread)");
}

static void update_light_display(void) {
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
    const stm32_light_sensor_info_t *light = stm32_light_sensor_info();
    stm32_display_set_light(
        peripherals.light_sensor_ready, light->raw_adc,
        light->intensity_percent, light->backlight_percent,
        light->auto_brightness);
#endif
}

#ifdef CONFIG_STM32_LEGACY_DASHBOARD
static void handle_display_actions(void) {
    stm32_display_action_t action = stm32_display_take_action();
    if (action == STM32_DISPLAY_ACTION_WIFI_SCAN) {
        if (stm32_wifi_scan() == 0)
            printf("[WIFI] AP scan started\n");
        update_wifi_display();
        return;
    }
    if (action != STM32_DISPLAY_ACTION_BLUETOOTH_TEST ||
        !peripherals.bluetooth_ready)
        return;
    static const char message[] = "A20OS HC05 TEST\r\n";
    if (stm32_bluetooth_send(message, sizeof(message) - 1U) == 0)
        printf("[BT] sent test message\n");
    update_bluetooth_display();
}
#endif

static void update_display_status(void) {
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
    const stm32_sdcard_info_t *sd = stm32_sdcard_info();
    stm32_display_set_peripherals(
        peripherals.external_sram_ready, peripherals.external_sram_bytes,
        peripherals.sdcard_ready, sd->sectors, sd->fat32,
        sd->bus_width, sd->volume_label,
        peripherals.touch_armed, peripherals.keys_ready);
#endif
}

static uint32_t elapsed_ms(uint64_t start) {
    uint64_t elapsed = timer_get_ticks() - start;
    return elapsed > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)elapsed;
}

static void record_max(uint32_t *maximum, uint32_t value) {
    if (value > *maximum)
        *maximum = value;
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
#ifndef CONFIG_STM32_LEGACY_DASHBOARD
    int touch_cal_loaded = 0;
#endif
    int display_id = stm32_display_init();
    peripherals.display_ready = display_id >= 0;
    peripherals.display_id =
        peripherals.display_ready ? (uint16_t)display_id : 0;
    printf("[BOOT] display=%s",
           peripherals.display_ready ? "ready" : "absent");
    if (peripherals.display_ready)
        printf(" id=0x%x", (unsigned)peripherals.display_id);
    printf(" (optional)\n");
    stm32_display_gfx(&g_ui_gfx);
    stm32_display_show_boot();

    peripherals.external_sram_ready = stm32_extsram_init() == 0;
    peripherals.external_sram_bytes = peripherals.external_sram_ready
                                          ? stm32_extsram_available()
                                          : 0;
    ext_sram_smoke_test();
    printf("[BOOT] external SRAM=%s bytes=%u\n",
           peripherals.external_sram_ready ? "ready" : "absent",
           (unsigned)stm32_extsram_capacity());
    stm32_memory_init();

    peripherals.sdcard_ready = stm32_sdcard_init() == 0;
    report_sdcard();

    /* Mount FAT32 so assets/config/logs on the card are reachable as files. */
    if (peripherals.sdcard_ready) {
        int fs = stm32_sdfs_mount();
        printf("[BOOT] TF filesystem=%s\n",
               fs == FAT32LITE_OK ? "fat32 mounted" : "mount failed");
        /* Apply /CFG/WIFI.TXT: retarget the proxy, and stash creds for join. */
        if (fs == FAT32LITE_OK && stm32_sdfs_load_config(&boot_cfg) > 0) {
            if (boot_cfg.have_proxy)
                stm32_peripherals_set_proxy(boot_cfg.proxy_ip,
                                            boot_cfg.proxy_port);
            printf("[BOOT] config=/CFG/WIFI.TXT ssid=%s proxy=%s\n",
                   boot_cfg.have_ssid ? boot_cfg.ssid : "(none)",
                   boot_cfg.have_proxy ? "set" : "(none)");
        }
    }

    peripherals.touch_armed = stm32_touch_init() == 0;
    printf("[BOOT] touch interface=%s (optional)\n",
           peripherals.touch_armed ? "armed" : "disabled");
    /* Apply a saved four-corner calibration if the card carries one. */
    if (peripherals.touch_armed && stm32_sdfs_ready() &&
        stm32_sdfs_load_touch_cal() == FAT32LITE_OK) {
#ifndef CONFIG_STM32_LEGACY_DASHBOARD
        touch_cal_loaded = 1;
#endif
        printf("[BOOT] touch calibration=loaded (/CFG/TOUCH.CAL)\n");
    }
    if (stm32_sdfs_ready() && peripherals.external_sram_ready) {
        int font = cjk_font_load();
        printf("[BOOT] CJK font=%s glyphs=%u (/FONT/CJK16.FNT)\n",
               font == FAT32LITE_OK ? "loaded" : "placeholder",
               cjk_font_glyph_count());
    }

    peripherals.keys_ready = stm32_keys_init() == 0;
    printf("[BOOT] direction keys=%s\n",
           peripherals.keys_ready ? "ready" : "disabled");

    peripherals.light_sensor_ready = stm32_light_sensor_init() == 0;
    const stm32_light_sensor_info_t *light = stm32_light_sensor_info();
    printf("[BOOT] light sensor=%s adc=ADC3/CH6/PF8 raw=%u"
           " level=%u/100 backlight=fixed-on\n",
           peripherals.light_sensor_ready ? "ready" : "disabled",
           (unsigned)light->raw_adc,
           (unsigned)light->intensity_percent);

    (void)stm32_bluetooth_init();
    peripherals.bluetooth_ready = stm32_bluetooth_info()->ready;
    peripherals.bluetooth_connected = stm32_bluetooth_connected();
    const stm32_bluetooth_info_t *bt = stm32_bluetooth_info();
    static const char *const rx_states[] = {
        "unknown", "driven-high", "floating", "driven-low",
    };
    unsigned rx_state = (unsigned)bt->rx_line_state;
    if (rx_state >= sizeof(rx_states) / sizeof(rx_states[0]))
        rx_state = 0;
    printf("[BOOT] HC-05 interface=%s detected=%s"
           " uart=USART3/PB10/PB11 key=PA4 state=PA15"
           " data-baud=%u at-baud=%u at-mode=%s"
           " rx=%s at=%s config=%s state=%s role=%s name=%s pin=%s"
           " uuid=0x%x requested=0x%x source=%s reset=%s\n",
           peripherals.bluetooth_ready ? "armed" : "disabled",
           bt->detected ? "yes" : "no",
           (unsigned)bt->baud_rate,
           (unsigned)bt->at_baud_rate,
           bt->at_key_mode == STM32_BLUETOOTH_AT_KEY_PULSE ?
               "key-pulse" :
           bt->at_key_mode == STM32_BLUETOOTH_AT_KEY_HIGH ?
               "key-high" : "key-low",
           rx_states[rx_state],
           bt->at_responsive ? "responsive" : "no-response",
           bt->configured ? "verified" : "unverified",
           bt->waiting ? "slave-waiting" : "not-waiting",
           bt->slave_mode ? "slave" : "unknown",
           bt->device_name[0] ? bt->device_name : "(unread)",
            bt->pin[0] ? "configured" : "(unread)",
           (unsigned)bt->service_uuid,
           (unsigned)bt->requested_uuid,
           bt->uuid_configured ? "module-configured" :
           bt->uuid_supported ? "module-mismatch" : "fixed-spp",
           bt->reset_performed ? "yes" : "no");

    peripherals.wifi_ready = stm32_wifi_init() == 0;
    printf("[BOOT] ESP8266 interface=%s uart=USART2/PA2/PA3"
           " enable=PC6 reset=PC7 baud=auto(115200-first)"
           " probe=deferred nonblocking\n",
           peripherals.wifi_ready ? "armed" : "disabled");

    const stm32_memory_info_t *memory = stm32_memory_info();
    peripherals.memory_capacity_from_silicon =
        memory->silicon_capacity_valid;
    printf("[BOOT] memory source=ram:%s flash:%s dev=0x%x rev=0x%x"
           " ram=%u/%u flash=%u/%u ext=%u/%u bytes\n",
           memory->ram_capacity_from_silicon ? "silicon" : "linked-layout",
           memory->flash_capacity_from_silicon ?
               "silicon" : "linked-layout",
           (unsigned)memory->device_id, (unsigned)memory->revision_id,
           (unsigned)memory->internal_ram_used,
           (unsigned)memory->internal_ram_total,
           (unsigned)memory->flash_used, (unsigned)memory->flash_total,
           (unsigned)memory->external_ram_used,
           (unsigned)memory->external_ram_total);

    /* Smart-hub sense/decide/act pipeline. */
    stm32_rtc_init();
    printf("[BOOT] RTC=%s (LSE preferred, LSI fallback)\n",
           stm32_rtc_available() ? "ready" : "fallback-time");
    dht_present = stm32_dht11_init() == 0;
    stm32_actuators_init();
    peripherals.rgb_matrix_ready = stm32_rgb_matrix_init() == 0;
    printf("[BOOT] RGB matrix=%s (WS2812B 5x5/PE5 brightness=%u/255)\n",
           peripherals.rgb_matrix_ready ? "ready" : "disabled",
           (unsigned)stm32_rgb_matrix_brightness());
    hub_controller_init(&hub_ctl);
    live2d_init(&g_cat);
    last_live2d_clock = stm32_live2d_frame_clock();
    g_last_speech[0] = '\0';
    have_env = 0;
    buzzer_active = 0;
    printf("[BOOT] smart-hub controller ready dht11=%s (PG11)"
           " actuators=fan(TIM3_CH1/PA6),pump(PA7),buzzer(PB8)\n",
           dht_present ? "armed" : "absent");
    ir_present = stm32_ir_init() == 0;
    ir_map_load_default();
    if (stm32_sdfs_ready()) {
        int bindings = ir_map_load_fs(stm32_sdfs(), "/CFG/IR.CFG");
        if (bindings >= 0)
            printf("[BOOT] IR map=/CFG/IR.CFG bindings=%d\n", bindings);
    }
    printf("[BOOT] IR receiver=%s (PB9/EXTI9, NEC) bindings=%u (%s)\n",
           ir_present ? "armed" : "absent", ir_map_count(),
           ir_map_count() ? "keys 1-9 live" : "NO KEYS BOUND");

    /*
     * wifi.c already owns USART2 and advances its AT probe from service().
     * Do not run the legacy synchronous esp8266.c probe here: it resets the
     * same UART and can hold boot before touch polling and buzzer shutdown.
     */
    printf("[BOOT] cloud control=nonblocking via wifi.c proxy=%s:%u"
           " (run `wifi join` then it auto-connects; `proxy <ip> <port>` to"
           " retarget)\n",
           cloud_proxy_ip, (unsigned)cloud_proxy_port);

    update_display_status();
    update_bluetooth_display();
    update_wifi_display();
    update_memory_display();
    update_light_display();
    hub_buzz(timer_get_ticks(), BUZZ_BOOT_MS); /* one startup chime */

    /*
     * Arm the watchdog only now — after the slow one-shot probes (bluetooth AT
     * scan, SD init) — so bring-up can't trip it. From here the service loop
     * must feed it within the timeout or the MCU resets.
     */
    stm32_watchdog_init(6000);
    printf("[BOOT] watchdog=%s (IWDG 6s)\n",
           stm32_watchdog_active() ? "armed" : "off");

    if (stm32_sdfs_ready()) /* one boot marker to /LOG/RUN.LOG */
        stm32_sdfs_log(timer_get_ticks(), "BOOT", "smart-hub up");

#ifndef CONFIG_STM32_LEGACY_DASHBOARD
    run_control_tick(timer_get_ticks());
    if (!touch_cal_loaded && peripherals.touch_armed)
        (void)stm32_peripherals_start_touch_calibration();
#endif
}

/*
 * Non-blocking cloud link over wifi.c: keep a TCP socket to the proxy open
 * (auto-connect once WiFi has an IP), drain incoming bytes, and reassemble
 * CONTROL frames into cloud_last. Runs every service tick.
 */
static void cloud_net_service(uint64_t now) {
    const stm32_wifi_info_t *w = stm32_wifi_info();
    if (!w->active || !w->got_ip) {
        cloud_rx_len = 0;
        cloud_awaiting = 0;
        return;
    }
    if (!w->socket_connected) {
        cloud_rx_len = 0;
        cloud_awaiting = 0;
        if (!w->command_busy && now >= cloud_connect_retry_at) {
            if (stm32_wifi_open("TCP", cloud_proxy_ip, cloud_proxy_port) == 0)
                printf("[NET] connecting proxy %s:%u\n", cloud_proxy_ip,
                       (unsigned)cloud_proxy_port);
            cloud_connect_retry_at = now + CLOUD_CONNECT_BACKOFF_MS;
        }
        return;
    }

    /* Drain the socket ring into the frame-reassembly buffer. */
    for (unsigned reads = 0; reads < 4U; reads++) {
        uint8_t chunk[64];
        int n = stm32_wifi_read(chunk, sizeof(chunk));
        if (n <= 0)
            break;
        for (int i = 0; i < n; i++) {
            if (cloud_rx_len == sizeof(cloud_rx))
                cloud_parse_frames(now);
            if (cloud_rx_len < sizeof(cloud_rx))
                cloud_rx[cloud_rx_len++] = chunk[i];
        }
        cloud_parse_frames(now);
    }

    if (cloud_awaiting && now >= cloud_reply_deadline)
        cloud_awaiting = 0; /* reply timed out; keep the socket, retry next tick */
}

/* Retarget the proxy at runtime (console `proxy <ip> <port>`); drops any open
 * socket so the next service reconnects to the new address. */
int stm32_peripherals_set_proxy(const char *ip, uint16_t port) {
    if (!ip || port == 0)
        return -1;
    size_t n = 0;
    while (ip[n] && n < sizeof(cloud_proxy_ip) - 1) {
        cloud_proxy_ip[n] = ip[n];
        n++;
    }
    if (ip[n]) /* address too long */
        return -1;
    cloud_proxy_ip[n] = '\0';
    cloud_proxy_port = port;
    cloud_last_valid = 0;
    cloud_awaiting = 0;
    cloud_rx_len = 0;
    cloud_connect_retry_at = 0;
    if (stm32_wifi_info()->socket_connected)
        stm32_wifi_close();
    printf("[NET] proxy set to %s:%u\n", cloud_proxy_ip,
           (unsigned)cloud_proxy_port);
    return 0;
}

void stm32_peripherals_request_time_sync(void) {
    cloud_ask_now = 1;
    last_control_tick = 0;
}

/* Latest assembled home-screen state + catgirl, for the on-board renderer. */
const ui_home_state_t *stm32_peripherals_ui_state(void) { return &g_ui_state; }
const live2d_t *stm32_peripherals_cat(void) { return &g_cat; }

/* Build an environment snapshot and drive the actuators from the decision. */
static void run_control_tick(uint64_t now) {
    env_snapshot_t s;
    int rtc_hour;
    int rtc_minute;
    int rtc_second;

    if (!sensor_read_snapshot(&s)) {
        s.temp_c = last_temp_c;
        s.humidity = last_humidity;
        s.light = 0;
        s.hour = 0;
        s.valid = 0;
    }
    stm32_rtc_get_hhmmss(&rtc_hour, &rtc_minute, &rtc_second);
    (void)rtc_second;
    s.hour = (uint8_t)rtc_hour;

    hub_action_t a;
    hub_controller_step(&hub_ctl, (uint32_t)now, &s, &a);

    /*
     * Cloud-preferred: send the snapshot to the proxy and apply its CONTROL if
     * it answers in time; otherwise keep the local rule-engine decision. The
     * controller's own hysteresis state stays based on the local decision, so
     * the fallback remains coherent when the network drops.
     */
    int cloud = 0;
    /*
     * Send this snapshot to the proxy when the socket is up, no request is in
     * flight, and there is actually something to ask about (see
     * cloud_should_ask). The reply (a CONTROL frame) arrives asynchronously and
     * is parsed in cloud_net_service(); we apply the freshest one below.
     */
    /* The network task owns request throttling and all wifi.c state. This task
     * only replaces the pending sample with the newest control snapshot. */
    cloud_publish_snapshot(&s);
    /* Cloud-preferred, local fail-safe: override with a fresh cloud decision. */
    hub_control_t cloud_control;
    uint64_t cloud_control_ms = 0;
    if (cloud_read_control(&cloud_control, &cloud_control_ms) &&
        now - cloud_control_ms <= CLOUD_FRESH_MS) {
        a.decision.fan_level = cloud_control.fan_level;
        a.decision.pump_on = cloud_control.pump_on;
        a.decision.theme = (env_theme_t)cloud_control.theme_id;
        cloud = 1;
    }
    uint32_t manual_flags = arch_irq_save();
    if (g_manual_mask && now >= g_manual_until)
        g_manual_mask = 0;
    uint8_t manual_mask = g_manual_mask;
    uint8_t manual_fan = g_manual_fan;
    uint8_t manual_pump = g_manual_pump;
    uint8_t manual_theme = g_manual_theme;
    uint8_t manual_backlight = g_manual_backlight;
    arch_irq_restore(manual_flags);
    if (manual_mask & MANUAL_FAN)
        a.decision.fan_level = manual_fan;
    if (manual_mask & MANUAL_PUMP)
        a.decision.pump_on = manual_pump;
    if (manual_mask & MANUAL_THEME)
        a.decision.theme = (env_theme_t)manual_theme;
    if (manual_mask & MANUAL_BACKLIGHT)
        a.decision.backlight = manual_backlight;
    manual_flags = arch_irq_save();
    g_backlight = a.decision.backlight;
    arch_irq_restore(manual_flags);

    /*
     * Assemble the home-screen + catgirl state for the renderer. Speech is the
     * cloud (LLM) line only; a new line makes the catgirl talk, otherwise she
     * follows the derived mood (without interrupting an active TALK).
     */
    hub_ui_input_t uin;
    uin.snap = s;
    uin.decision = a.decision;
    uin.minute = (uint8_t)rtc_minute;
    uin.net_cloud = (uint8_t)cloud;
    uin.cloud_valid = (uint8_t)cloud;
    uin.cloud_mood = cloud ? cloud_control.mood : 0;
    uin.cloud_speech = cloud ? cloud_control.speech : (const char *)0;
    uin.manual_mask = manual_mask;
    if (cloud && cloud_control.speech[0] &&
        strncmp(g_last_speech, cloud_control.speech,
                sizeof(g_last_speech)) != 0) {
        strncpy(g_last_speech, cloud_control.speech,
                sizeof(g_last_speech) - 1);
        g_last_speech[sizeof(g_last_speech) - 1] = '\0';
        g_last_speech_ms = now;
    }
    /*
     * Hold the newest line in the dialog box through ticks the proxy did not
     * answer. hub_ui_build only reports speech while a CONTROL is fresh
     * (CLOUD_FRESH_MS), so on a slow reply the box would blank and refill —
     * which reads as a fault, not as silence. This is still the LLM's own
     * text, just the most recent one, and it expires so a stale line cannot
     * outlive the conditions it described.
     */
    if (!uin.cloud_speech && g_last_speech[0] &&
        now - g_last_speech_ms <= SPEECH_STICKY_MS) {
        uin.cloud_speech = g_last_speech;
        uin.cloud_valid = 1;
    }
    ui_publish_input(&uin);

    stm32_fan_set_level(a.decision.fan_level);
    stm32_pump_set(a.decision.pump_on);
    stm32_light_sensor_set_manual_brightness(a.decision.backlight);
    if (a.alert_started)
        hub_buzz(now, HUB_BUZZER_MS); /* alert tone (longer than input chirp) */

    if (a.fan_changed || a.pump_changed || a.alert_started || a.alert_cleared ||
        cloud) {
        printf("[CTRL] src=%s T=%dC H=%u%% L=%u h=%02u v=%u -> fan=%u pump=%u"
               " bl=%u%% theme=%s alert=%s%s\n",
                manual_mask ? "manual" : cloud ? "cloud" : "local",
                (int)s.temp_c, s.humidity, s.light,
               s.hour, s.valid, a.decision.fan_level, a.decision.pump_on,
               a.decision.backlight, env_theme_name(a.decision.theme),
               env_alert_name(a.decision.alert),
                a.alert_started ? " (NEW)" : a.alert_cleared ? " (clr)" : "");
    }
}

void stm32_peripherals_control_service(uint64_t now) {
    uint64_t service_start = timer_get_ticks();
    stm32_watchdog_feed(); /* main loop is alive -> stave off the reset */
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
    stm32_display_update_ticks(now);
#endif

    /* Non-blocking buzzer off-timer (finer than the control tick). */
    if (buzzer_active && now >= buzzer_off_at) {
        stm32_buzzer_set(0);
        buzzer_active = 0;
    }

    if (now - last_control_tick >= CONTROL_TICK_INTERVAL_MS) {
        last_control_tick = now;
        run_control_tick(now);
    }

    peripherals.service_calls++;
    peripherals.service_last_ms = elapsed_ms(service_start);
    record_max(&peripherals.service_max_ms, peripherals.service_last_ms);
    if (peripherals.service_last_ms >= SERVICE_SLOW_THRESHOLD_MS)
        peripherals.service_slow_calls++;
}

void stm32_peripherals_ui_input_service(uint64_t now) {
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
    stm32_display_update_ticks(now);
#endif

    hub_ui_input_t input;
    char speech[HUB_SPEECH_MAX + 1];
    if (ui_take_input(&input, speech)) {
        ui_home_state_t next;
        int new_speech = input.cloud_speech &&
            (!g_ui_state.speech || strcmp(g_ui_state.speech,
                                          input.cloud_speech) != 0);
        hub_ui_build(&input, &next);
        if (next.speech) {
            strncpy(g_ui_speech, next.speech, sizeof(g_ui_speech) - 1U);
            g_ui_speech[sizeof(g_ui_speech) - 1U] = '\0';
            next.speech = g_ui_speech;
        } else {
            g_ui_speech[0] = '\0';
        }
        g_ui_state = next;
        if (new_speech)
            live2d_speak(&g_cat, g_ui_state.speech, (uint32_t)now);
        else if (!(g_cat.state == LIVE2D_TALK &&
                   g_cat.talk_deadline_ms != 0))
            live2d_set_state(&g_cat, hub_ui_state(&input), (uint32_t)now);
    }

    uint32_t live2d_clock = stm32_live2d_frame_clock();
    if (live2d_frame_clock_consume(&last_live2d_clock, live2d_clock)) {
        int changed = live2d_tick(&g_cat, (uint32_t)now);
#ifndef CONFIG_STM32_LEGACY_DASHBOARD
        if (changed)
            ui_render_cat_frame();
#else
        (void)changed;
#endif
    }

    if (ir_present) {
        uint32_t code;
        if (stm32_ir_poll(&code)) {
            int ir_action = ir_map_dispatch(code);
            printf("[IR] code=0x%08x action=%s\n", (unsigned)code,
                   ir_action_name(ir_action));
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
            (void)ir_action;
            hub_buzz(now, BUZZ_INPUT_MS);
#else
            apply_remote_action(ir_action, now);
#endif
        }
    }

    if (peripherals.keys_ready &&
        now - last_key_poll >= KEY_POLL_INTERVAL_MS) {
        last_key_poll = now;
        stm32_key_t key = stm32_keys_poll();
        if (key != STM32_KEY_NONE) {
            static const char *const names[] = {
                "none", "up", "down", "left", "right",
            };
            printf("[KEY] %s\n", names[key]);
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
            hub_buzz(now, BUZZ_INPUT_MS); /* feedback chirp */
            stm32_display_handle_key(key);
            handle_display_actions();
#else
            ui_apply_action(ui_action_from_key(key), now);
#endif
        }
    }

    if (peripherals.touch_armed &&
        now - last_touch_poll >= TOUCH_POLL_INTERVAL_MS) {
        uint16_t x = 0;
        uint16_t y = 0;
        last_touch_poll = now;
#ifdef CONFIG_STM32_LEGACY_DASHBOARD
        int pressed = stm32_touch_poll(&x, &y);
        stm32_display_show_touch(x, y, pressed);
        handle_display_actions();
#else
        if (g_cal_active) {
            touch_calibration_service();
            goto touch_done;
        }
        int pressed = stm32_touch_poll(&x, &y);
#endif
        if (pressed && !touch_pressed) {
            printf("[TOUCH] down x=%u y=%u\n", (unsigned)x, (unsigned)y);
#ifndef CONFIG_STM32_LEGACY_DASHBOARD
            touch_down_x = x;
            touch_down_y = y;
            touch_down_at = now;
            /* Light up whatever is under the finger right away; the repaint on
             * release clears it. Only the home page has hit outlines. */
            if (g_ui_ready && g_page == PAGE_HOME)
                ui_render_press(&g_ui_gfx, &g_ui_model, x, y);
#else
            hub_buzz(now, BUZZ_INPUT_MS); /* feedback chirp */
#endif
        }
        else if (!pressed && touch_pressed) {
            printf("[TOUCH] up\n");
#ifndef CONFIG_STM32_LEGACY_DASHBOARD
            int dx = (int)touch_last_x - (int)touch_down_x;
            if (g_page == PAGE_DIAG) {
                ui_diag_action_t da = ui_diag_hit_test(&g_diag_model,
                                                       touch_down_x,
                                                       touch_down_y);
                apply_diag_action(da, now);
            } else if (dx > 60 || dx < -60) {
                ui_apply_action(UI_ACTION_THEME_CYCLE, now);
            } else {
                /* A press that outlined a region always resolves to that same
                 * region's action, so the repaint below clears the outline. */
                ui_action_t act = ui_home_hit_test(&g_ui_model, touch_down_x,
                                                   touch_down_y);
                if (now - touch_down_at < TOUCH_LONG_PRESS_MS ||
                    !ui_release_manual(act, now))
                    ui_apply_action(act, now);
            }
#endif
        }
#ifndef CONFIG_STM32_LEGACY_DASHBOARD
        if (pressed) {
            touch_last_x = x;
            touch_last_y = y;
        }
#endif
        touch_pressed = pressed;
#ifndef CONFIG_STM32_LEGACY_DASHBOARD
touch_done:
        ;
#endif
    }

    if (peripherals.bluetooth_ready &&
        now - last_bluetooth_poll >= BLUETOOTH_POLL_INTERVAL_MS) {
        last_bluetooth_poll = now;
        int was_connected = peripherals.bluetooth_connected;
        stm32_bluetooth_service(now);
        peripherals.bluetooth_connected = stm32_bluetooth_connected();
        if (peripherals.bluetooth_connected != was_connected) {
            printf("[BT] link=%s\n",
                   peripherals.bluetooth_connected ? "connected" :
                                                     "disconnected");
            update_bluetooth_display();
        }

        int length;
        while ((length = stm32_bluetooth_read_line(
                    bluetooth_line, sizeof(bluetooth_line))) > 0) {
            (void)length;
            printf("[BT] authenticated command received (%u bytes)\n",
                   (unsigned)length);
#ifndef CONFIG_STM32_LEGACY_DASHBOARD
            ui_handle_bluetooth_command(bluetooth_line, now);
#endif
            stm32_display_show_bluetooth_line(bluetooth_line);
            update_bluetooth_display();
        }
    }
#ifndef CONFIG_STM32_LEGACY_DASHBOARD
    ui_render_update();
#endif
}

void stm32_peripherals_network_service(uint64_t now) {
    if (!peripherals.wifi_ready ||
        now - last_wifi_poll < WIFI_POLL_INTERVAL_MS)
        return;

    last_wifi_poll = now;
    const stm32_wifi_info_t before = *stm32_wifi_info();
    stm32_wifi_service(now);
    const stm32_wifi_info_t *after = stm32_wifi_info();
    if (before.phase != after->phase || before.joined != after->joined ||
        before.got_ip != after->got_ip ||
        before.socket_connected != after->socket_connected ||
        before.access_points != after->access_points ||
        before.received_bytes != after->received_bytes ||
        before.transmitted_bytes != after->transmitted_bytes ||
        before.dropped_bytes != after->dropped_bytes)
        update_wifi_display();

    if (boot_cfg.have_ssid && !cfg_join_attempted && after->at_responsive &&
        !after->got_ip && !after->connecting && !after->command_busy) {
        if (stm32_wifi_join(boot_cfg.ssid, boot_cfg.pass) == 0) {
            cfg_join_attempted = 1;
            printf("[NET] joining ssid=%s (from /CFG/WIFI.TXT)\n",
                   boot_cfg.ssid);
        }
    }
    cloud_net_service(now);

    env_snapshot_t snapshot;
    if (after->socket_connected && !after->command_busy && !cloud_awaiting &&
        cloud_take_snapshot(&snapshot) && cloud_should_ask(&snapshot, now)) {
        uint8_t txf[HUB_OVERHEAD + 8];
        uint8_t seq = net_seq++;
        int length = hub_proto_encode_snapshot(seq, &snapshot, txf,
                                               sizeof(txf));
        if (length > 0 && stm32_wifi_send(txf, (size_t)length) == 0) {
            cloud_awaiting = 1;
            cloud_expected_seq = seq;
            cloud_reply_deadline = now + CLOUD_REPLY_TIMEOUT_MS;
            cloud_last_ask_ms = now;
            cloud_asked_snap = snapshot;
            cloud_ask_now = 0;
        } else {
            cloud_publish_snapshot(&snapshot);
        }
    }
}

void stm32_peripherals_sensor_service(uint64_t now) {
    if (now - last_environment_poll >= CONTROL_TICK_INTERVAL_MS) {
        env_snapshot_t snapshot;
        int16_t temp = last_temp_c;
        uint8_t humidity = last_humidity;
        int hour;
        int minute;
        int second;

        last_environment_poll = now;
        if (dht_present && stm32_dht11_read(&temp, &humidity) == 0) {
            last_temp_c = temp;
            last_humidity = humidity;
            have_env = 1;
            snapshot.valid = 1;
        } else {
            snapshot.valid = 0;
        }
        snapshot.temp_c = temp;
        snapshot.humidity = humidity;
        snapshot.light = stm32_light_sensor_info()->intensity_percent;
        stm32_rtc_get_hhmmss(&hour, &minute, &second);
        (void)minute;
        (void)second;
        snapshot.hour = (uint8_t)hour;
        sensor_publish_snapshot(&snapshot);
    }

    if (now - last_memory_poll >= MEMORY_POLL_INTERVAL_MS) {
        last_memory_poll = now;
        update_memory_display();
    }

    if (peripherals.light_sensor_ready &&
        now - last_light_poll >= LIGHT_POLL_INTERVAL_MS) {
        uint64_t light_start = timer_get_ticks();
        last_light_poll = now;
        if (stm32_light_sensor_sample() != 0) {
            const stm32_light_sensor_info_t *light =
                stm32_light_sensor_info();
            printf("[LIGHT] ADC sample failed errors=%u\n",
                   (unsigned)light->errors);
        }
        update_light_display();
        record_max(&peripherals.light_max_ms, elapsed_ms(light_start));
    }

}

void stm32_peripherals_storage_service(uint64_t now) {
    /* This thread is the normal runtime owner of FAT32/SDIO resource I/O.
     * Console and calibration paths are serialized by the SDFS lock. */
    live2d_t cat;
    uint32_t flags = arch_irq_save();
    cat = g_cat;
    arch_irq_restore(flags);
    stm32_sdfs_lock();
    live2d_load_service(&cat);
    stm32_sdfs_unlock();

    if (peripherals.sdcard_ready &&
        now - last_sdcard_check >= SDCARD_CHECK_INTERVAL_MS) {
        uint64_t sd_start = timer_get_ticks();
        last_sdcard_check = now;
        if (stm32_sdcard_check() != 0) {
            peripherals.sdcard_ready = 0;
            printf("[TF] card removed or stopped responding\n");
            report_sdcard();
            update_display_status();
        }
        record_max(&peripherals.sdcard_max_ms, elapsed_ms(sd_start));
    }
}

void stm32_peripherals_service(uint64_t now) {
    stm32_peripherals_control_service(now);
    stm32_peripherals_ui_input_service(now);
    stm32_peripherals_network_service(now);
    stm32_peripherals_sensor_service(now);
    stm32_peripherals_storage_service(now);
}

int stm32_peripherals_retry_sdcard(void) {
    uint64_t start = timer_get_ticks();
    int was_ready = peripherals.sdcard_ready;

    stm32_sdfs_lock();
    peripherals.sdcard_ready = stm32_sdcard_init() == 0;
    last_sdcard_check = timer_get_ticks();
    record_max(&peripherals.sdcard_max_ms, elapsed_ms(start));
    if (peripherals.sdcard_ready != was_ready || peripherals.sdcard_ready) {
        report_sdcard();
        update_display_status();
    }
    if (peripherals.sdcard_ready && stm32_sdfs_mount() == FAT32LITE_OK) {
        (void)stm32_sdfs_load_touch_cal();
        (void)ir_map_load_fs(stm32_sdfs(), "/CFG/IR.CFG");
        if (peripherals.external_sram_ready)
            (void)cjk_font_load();
#ifndef CONFIG_STM32_LEGACY_DASHBOARD
        g_ui_ready = 0;
    ui_render_full();
#endif
    }
    stm32_sdfs_unlock();
    return peripherals.sdcard_ready ? 0 : -1;
}

int stm32_peripherals_retry_bluetooth(void) {
    uint64_t start = timer_get_ticks();
    int result = stm32_bluetooth_reprobe();

    record_max(&peripherals.bluetooth_retry_max_ms, elapsed_ms(start));
    peripherals.bluetooth_ready = stm32_bluetooth_info()->ready;
    peripherals.bluetooth_connected = stm32_bluetooth_connected();
    report_bluetooth();
    update_bluetooth_display();
    update_wifi_display();
    return result;
}

int stm32_peripherals_retry_wifi(void) {
    uint64_t start = timer_get_ticks();
    int result = stm32_wifi_reprobe();
    record_max(&peripherals.wifi_retry_max_ms, elapsed_ms(start));
    peripherals.wifi_ready = result == 0;
    update_bluetooth_display();
    update_wifi_display();
    return result;
}

const stm32_peripheral_state_t *stm32_peripherals_state(void) {
    return &peripherals;
}

#endif
