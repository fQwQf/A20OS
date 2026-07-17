#ifdef CONFIG_BOARD_STM32F103

#include "peripherals.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "actuators.h"
#include "bluetooth.h"
#include "dht11.h"
#include "display.h"
#include "extsram.h"
#include "hub_config.h"
#include "hub_controller.h"
#include "hub_proto.h"
#include "hub_ui.h"
#include "ir.h"
#include "live2d.h"
#include "ui_home.h"
#include "keys.h"
#include "light_sensor.h"
#include "memory.h"
#include "sdcard.h"
#include "sdfs.h"
#include "touch.h"
#include "watchdog.h"
#include "touch_cal.h"
#include "wifi.h"

#define TOUCH_POLL_INTERVAL_MS 20U
#define KEY_POLL_INTERVAL_MS 20U
#define BLUETOOTH_POLL_INTERVAL_MS 20U
#define WIFI_POLL_INTERVAL_MS 20U
#define MEMORY_POLL_INTERVAL_MS 1000U
#define LIGHT_POLL_INTERVAL_MS 250U
#define SDCARD_CHECK_INTERVAL_MS 2000U
#define SERVICE_SLOW_THRESHOLD_MS 25U
#define CONTROL_TICK_INTERVAL_MS 2000U
#define BOOT_HOUR 12U /* assumed wall-clock hour at boot until an RTC exists */

static stm32_peripheral_state_t peripherals;
static uint64_t last_touch_poll;
static uint64_t last_key_poll;
static uint64_t last_bluetooth_poll;
static uint64_t last_wifi_poll;
static uint64_t last_memory_poll;
static uint64_t last_light_poll;
static uint64_t last_sdcard_check;
static uint64_t last_control_tick;
static int touch_pressed;
static char bluetooth_line[STM32_BLUETOOTH_LINE_MAX];

/* Smart-hub control state. */
static hub_controller_t hub_ctl;
static int dht_present;
static int16_t last_temp_c;
static uint8_t last_humidity;
static int have_env;
static uint64_t buzzer_off_at;
static int buzzer_active;
static uint8_t net_seq; /* SNAPSHOT frame sequence number       */
static int ir_present;

/* --- cloud control over the non-blocking wifi.c TCP socket --- */
#define CLOUD_REPLY_TIMEOUT_MS 1500U    /* wait for a CONTROL after a SNAPSHOT */
#define CLOUD_FRESH_MS 8000U            /* how long a cloud decision overrides */
#define CLOUD_CONNECT_BACKOFF_MS 5000U  /* retry cadence for CIPSTART          */
static char cloud_proxy_ip[16] = HUB_PROXY_IP;
static uint16_t cloud_proxy_port = HUB_PROXY_PORT;
static uint8_t cloud_rx[HUB_OVERHEAD + HUB_MAX_PAYLOAD]; /* frame reassembly    */
static size_t cloud_rx_len;
static uint64_t cloud_connect_retry_at;
static int cloud_awaiting;           /* SNAPSHOT sent, CONTROL not yet parsed  */
static uint64_t cloud_reply_deadline;
static hub_control_t cloud_last;     /* most recent CONTROL from the proxy     */
static int cloud_last_valid;
static uint64_t cloud_last_ms;

/* Assembled UI/Live2D state for the on-board renderer (wiring to display.c is
 * the on-board step; kept current here so it's ready). */
static ui_home_state_t g_ui_state;
static live2d_t g_cat;
static char g_last_speech[HUB_SPEECH_MAX + 1];

/* Config loaded from /CFG/WIFI.TXT at boot (SSID used for a one-shot join). */
static hub_cfg_t boot_cfg;
static int cfg_join_attempted;

/* Buzzer feedback durations (ms). */
#define BUZZ_BOOT_MS 120U  /* "hub is alive" chime at startup       */
#define BUZZ_INPUT_MS 30U  /* short chirp on key/touch/IR input     */

/*
 * Non-blocking buzzer: drive PB8 high now and schedule the off in the service
 * loop. Only ever extends an in-progress beep (so a short input chirp can't
 * cut an alert beep short). Buzzer = active buzzer on PB8 (docs/pz §8).
 */
static void hub_buzz(uint64_t now, uint32_t ms) {
    stm32_buzzer_set(1);
    uint64_t until = now + ms;
    if (!buzzer_active || until > buzzer_off_at)
        buzzer_off_at = until;
    buzzer_active = 1;
}

static void update_wifi_display(void) {
    const stm32_wifi_info_t *wifi = stm32_wifi_info();
    stm32_display_set_wifi(
        wifi->active, wifi->detected, wifi->at_responsive,
        wifi->configured, wifi->connecting, wifi->joined,
        wifi->got_ip, wifi->socket_connected, wifi->ssid,
        wifi->ip_address, wifi->mac_address, wifi->scan_ssid,
        wifi->access_points, wifi->baud_rate, wifi->received_bytes,
        wifi->transmitted_bytes, wifi->dropped_bytes,
        wifi->last_event);
}

static void update_memory_display(void) {
    stm32_memory_refresh();
    stm32_display_set_memory(stm32_memory_info());
}

static void update_bluetooth_display(void) {
    const stm32_bluetooth_info_t *bt = stm32_bluetooth_info();
    stm32_display_set_bluetooth(
        peripherals.bluetooth_ready, bt->detected, bt->at_responsive,
        bt->connected, bt->waiting, bt->configured,
        bt->slave_mode, bt->uuid_supported, bt->uuid_configured,
        bt->device_name, bt->pin,
        bt->service_uuid, bt->baud_rate,
        bt->received_bytes, bt->transmitted_bytes, bt->dropped_bytes);
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
           bt->pin[0] ? bt->pin : "(unread)");
}

static void update_light_display(void) {
    const stm32_light_sensor_info_t *light = stm32_light_sensor_info();
    stm32_display_set_light(
        peripherals.light_sensor_ready, light->raw_adc,
        light->intensity_percent, light->backlight_percent,
        light->auto_brightness);
}

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

static void update_display_status(void) {
    const stm32_sdcard_info_t *sd = stm32_sdcard_info();
    stm32_display_set_peripherals(
        peripherals.external_sram_ready, peripherals.external_sram_bytes,
        peripherals.sdcard_ready, sd->sectors, sd->fat32,
        sd->bus_width, sd->volume_label,
        peripherals.touch_armed, peripherals.keys_ready);
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
        stm32_sdfs_load_touch_cal() == FAT32LITE_OK)
        printf("[BOOT] touch calibration=loaded (/CFG/TOUCH.CAL)\n");

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
           bt->pin[0] ? bt->pin : "(unread)",
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
    dht_present = stm32_dht11_init() == 0;
    stm32_actuators_init();
    hub_controller_init(&hub_ctl);
    live2d_init(&g_cat);
    g_last_speech[0] = '\0';
    have_env = 0;
    buzzer_active = 0;
    printf("[BOOT] smart-hub controller ready dht11=%s (PG11)"
           " actuators=fan(TIM3_CH1/PA6),pump(PA7),buzzer(PB8)\n",
           dht_present ? "armed" : "absent");
    ir_present = stm32_ir_init() == 0;
    printf("[BOOT] IR receiver=%s (PB9/EXTI9, NEC)\n",
           ir_present ? "armed" : "absent");

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
    for (;;) {
        uint8_t chunk[64];
        int n = stm32_wifi_read(chunk, sizeof(chunk));
        if (n <= 0)
            break;
        for (int i = 0; i < n; i++)
            if (cloud_rx_len < sizeof(cloud_rx))
                cloud_rx[cloud_rx_len++] = chunk[i];
    }

    /* Extract every complete CONTROL frame; resync past bad/leading bytes. */
    size_t off = 0;
    while (off < cloud_rx_len) {
        hub_frame_t f;
        int r = hub_proto_parse(cloud_rx + off, cloud_rx_len - off, &f);
        if (r == 0)
            break; /* need more bytes */
        if (r < 0) {
            off++; /* drop one byte and re-scan for the magic */
            continue;
        }
        hub_control_t cc;
        if (hub_proto_decode_control(&f, &cc) == 0) {
            cloud_last = cc;
            cloud_last_valid = 1;
            cloud_last_ms = now;
            cloud_awaiting = 0;
            printf("[NET] cloud fan=%u pump=%u theme=%u mood=%u speech=%s\n",
                   cc.fan_level, cc.pump_on, cc.theme_id, cc.mood, cc.speech);
        }
        off += (size_t)r;
    }
    if (off > 0) { /* compact consumed bytes */
        size_t rem = cloud_rx_len - off;
        for (size_t i = 0; i < rem; i++)
            cloud_rx[i] = cloud_rx[off + i];
        cloud_rx_len = rem;
    } else if (cloud_rx_len == sizeof(cloud_rx)) {
        cloud_rx_len = 0; /* full of unparseable garbage -> drop it */
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

/* Latest assembled home-screen state + catgirl, for the on-board renderer. */
const ui_home_state_t *stm32_peripherals_ui_state(void) { return &g_ui_state; }
const live2d_t *stm32_peripherals_cat(void) { return &g_cat; }

/* Build an environment snapshot and drive the actuators from the decision. */
static void run_control_tick(uint64_t now) {
    env_snapshot_t s;
    int16_t t = last_temp_c;
    uint8_t h = last_humidity;

    if (dht_present && stm32_dht11_read(&t, &h) == 0) {
        last_temp_c = t;
        last_humidity = h;
        have_env = 1;
        s.valid = 1;
    } else {
        /* No fresh reading: fail-safe path (rule engine holds + SENSOR). */
        s.valid = have_env ? 0 : 0;
    }
    s.temp_c = t;
    s.humidity = h;
    s.light = stm32_light_sensor_info()->intensity_percent;
    s.hour = (uint8_t)((BOOT_HOUR + now / 3600000ULL) % 24U);

    hub_action_t a;
    hub_controller_step(&hub_ctl, (uint32_t)now, &s, &a);

    /*
     * Cloud-preferred: send the snapshot to the proxy and apply its CONTROL if
     * it answers in time; otherwise keep the local rule-engine decision. The
     * controller's own hysteresis state stays based on the local decision, so
     * the fallback remains coherent when the network drops.
     */
    int cloud = 0;
    const stm32_wifi_info_t *w = stm32_wifi_info();
    /*
     * Send this snapshot to the proxy when the socket is up and no request is
     * already in flight. The reply (a CONTROL frame) arrives asynchronously and
     * is parsed in cloud_net_service(); we apply the freshest one below.
     */
    if (w->socket_connected && !w->command_busy && !cloud_awaiting) {
        uint8_t txf[HUB_OVERHEAD + 8];
        int tn = hub_proto_encode_snapshot(net_seq++, &s, txf, sizeof(txf));
        if (tn > 0 && stm32_wifi_send(txf, (size_t)tn) == 0) {
            cloud_awaiting = 1;
            cloud_reply_deadline = now + CLOUD_REPLY_TIMEOUT_MS;
        }
    }
    /* Cloud-preferred, local fail-safe: override with a fresh cloud decision. */
    if (cloud_last_valid && now - cloud_last_ms <= CLOUD_FRESH_MS) {
        a.decision.fan_level = cloud_last.fan_level;
        a.decision.pump_on = cloud_last.pump_on;
        a.decision.theme = (env_theme_t)cloud_last.theme_id;
        cloud = 1;
    }

    /*
     * Assemble the home-screen + catgirl state for the renderer. Speech is the
     * cloud (LLM) line only; a new line makes the catgirl talk, otherwise she
     * follows the derived mood (without interrupting an active TALK).
     */
    hub_ui_input_t uin;
    uin.snap = s;
    uin.decision = a.decision;
    uin.minute = (uint8_t)((now / 60000ULL) % 60ULL);
    uin.net_cloud = (uint8_t)cloud;
    uin.cloud_valid = (uint8_t)cloud;
    uin.cloud_mood = cloud ? cloud_last.mood : 0;
    uin.cloud_speech = cloud ? cloud_last.speech : (const char *)0;
    hub_ui_build(&uin, &g_ui_state);

    if (cloud && cloud_last.speech[0] &&
        strncmp(g_last_speech, cloud_last.speech, sizeof(g_last_speech)) != 0) {
        live2d_speak(&g_cat, cloud_last.speech, now); /* new line -> TALK */
        strncpy(g_last_speech, cloud_last.speech, sizeof(g_last_speech) - 1);
        g_last_speech[sizeof(g_last_speech) - 1] = '\0';
    } else if (!(g_cat.state == LIVE2D_TALK && g_cat.talk_deadline_ms != 0)) {
        live2d_set_state(&g_cat, hub_ui_state(&uin), now);
    }

    stm32_fan_set_level(a.decision.fan_level);
    stm32_pump_set(a.decision.pump_on);
    if (a.alert_started)
        hub_buzz(now, HUB_BUZZER_MS); /* alert tone (longer than input chirp) */

    if (a.fan_changed || a.pump_changed || a.alert_started || a.alert_cleared ||
        cloud) {
        printf("[CTRL] src=%s T=%dC H=%u%% L=%u h=%02u v=%u -> fan=%u pump=%u"
               " bl=%u%% theme=%s alert=%s%s\n",
               cloud ? "cloud" : "local", (int)s.temp_c, s.humidity, s.light,
               s.hour, s.valid, a.decision.fan_level, a.decision.pump_on,
               a.decision.backlight, env_theme_name(a.decision.theme),
               env_alert_name(a.decision.alert),
               a.alert_started ? " (NEW)" : a.alert_cleared ? " (clr)" : "");
    }
}

void stm32_peripherals_service(uint64_t now) {
    uint64_t service_start = timer_get_ticks();
    stm32_watchdog_feed(); /* main loop is alive -> stave off the reset */
    stm32_display_update_ticks(now);

    /* Non-blocking buzzer off-timer (finer than the control tick). */
    if (buzzer_active && now >= buzzer_off_at) {
        stm32_buzzer_set(0);
        buzzer_active = 0;
    }

    if (now - last_control_tick >= CONTROL_TICK_INTERVAL_MS) {
        last_control_tick = now;
        run_control_tick(now);
    }

    live2d_tick(&g_cat, now); /* advance the catgirl animation/TALK timeout */

    if (ir_present) {
        uint32_t code;
        if (stm32_ir_poll(&code)) {
            printf("[IR] code=0x%x\n", (unsigned)code);
            hub_buzz(now, BUZZ_INPUT_MS); /* feedback chirp */
            /* TODO: map remote keys to actions once the codes are known —
             * press keys, read the printed codes, then dispatch here
             * (e.g. manual fan level / theme toggle / mute buzzer). */
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
            hub_buzz(now, BUZZ_INPUT_MS); /* feedback chirp */
            stm32_display_handle_key(key);
            handle_display_actions();
        }
    }

    if (peripherals.touch_armed &&
        now - last_touch_poll >= TOUCH_POLL_INTERVAL_MS) {
        uint16_t x = 0;
        uint16_t y = 0;
        last_touch_poll = now;
        int pressed = stm32_touch_poll(&x, &y);
        stm32_display_show_touch(x, y, pressed);
        handle_display_actions();
        if (pressed && !touch_pressed) {
            printf("[TOUCH] down x=%u y=%u\n", (unsigned)x, (unsigned)y);
            hub_buzz(now, BUZZ_INPUT_MS); /* feedback chirp */
        }
        else if (!pressed && touch_pressed)
            printf("[TOUCH] up\n");
        touch_pressed = pressed;
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
            printf("[BT] rx: %s", bluetooth_line);
            stm32_display_show_bluetooth_line(bluetooth_line);
            update_bluetooth_display();
        }
    }

    if (peripherals.wifi_ready &&
        now - last_wifi_poll >= WIFI_POLL_INTERVAL_MS) {
        last_wifi_poll = now;
        const stm32_wifi_info_t before = *stm32_wifi_info();
        stm32_wifi_service(now);
        const stm32_wifi_info_t *after = stm32_wifi_info();
        if (before.phase != after->phase ||
            before.joined != after->joined ||
            before.got_ip != after->got_ip ||
            before.socket_connected != after->socket_connected ||
            before.access_points != after->access_points ||
            before.received_bytes != after->received_bytes ||
            before.transmitted_bytes != after->transmitted_bytes ||
            before.dropped_bytes != after->dropped_bytes)
            update_wifi_display();
        /* One-shot join with credentials from the card, once the module is
         * responsive and idle (build-time SSID is empty by default). */
        if (boot_cfg.have_ssid && !cfg_join_attempted && after->at_responsive &&
            !after->joined && !after->connecting && !after->command_busy) {
            if (stm32_wifi_join(boot_cfg.ssid, boot_cfg.pass) == 0) {
                cfg_join_attempted = 1;
                printf("[NET] joining ssid=%s (from /CFG/WIFI.TXT)\n",
                       boot_cfg.ssid);
            }
        }
        cloud_net_service(now); /* keep the proxy socket + parse CONTROL */
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

    /*
     * Full SDIO initialization has long command timeouts. Never run it
     * periodically for an absent optional card; use the UART retry command.
     */
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

    peripherals.service_calls++;
    peripherals.service_last_ms = elapsed_ms(service_start);
    record_max(&peripherals.service_max_ms, peripherals.service_last_ms);
    if (peripherals.service_last_ms >= SERVICE_SLOW_THRESHOLD_MS)
        peripherals.service_slow_calls++;
}

int stm32_peripherals_retry_sdcard(void) {
    uint64_t start = timer_get_ticks();
    int was_ready = peripherals.sdcard_ready;

    peripherals.sdcard_ready = stm32_sdcard_init() == 0;
    last_sdcard_check = timer_get_ticks();
    record_max(&peripherals.sdcard_max_ms, elapsed_ms(start));
    if (peripherals.sdcard_ready != was_ready || peripherals.sdcard_ready) {
        report_sdcard();
        update_display_status();
    }
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
