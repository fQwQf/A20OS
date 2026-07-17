/*
 * Smart home hub — QEMU self-test.
 *
 * QEMU's stm32vldiscovery machine does not model the FSMC LCD, SDIO, ADC,
 * SPI touch, DHT11, PWM or the ESP8266/HC-05 UART peers, so the peripheral
 * drivers are compiled out under CONFIG_STM32_QEMU. This self-test exercises
 * the parts that ARE hardware-independent — the environment rule engine and
 * the STM32<->proxy frame protocol — so the QEMU run demonstrates real logic
 * instead of an empty tick loop. It is also the on-target mirror of the host
 * unit test in tools/smarthub-test/.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "smarthub.h"
#include "env_rule.h"
#include "hub_controller.h"
#include "hub_proto.h"
#include "touch_cal.h"
#include "ui_home.h"
#include "live2d.h"
#include "drivers/stm32f1/rgb_matrix.h"
#include "core/stdio.h"

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* --- Environment rule engine demo over a scripted day. --- */
static void run_rule_demo(void) {
    env_rule_config_t cfg;
    env_rule_default_config(&cfg);

    static const env_snapshot_t script[] = {
        /* temp humi light hour valid */
        {22, 55, 70, 9, 1},  /* comfortable morning         */
        {27, 50, 80, 13, 1}, /* warming up -> fan 1         */
        {30, 45, 85, 14, 1}, /* hotter -> fan 2             */
        {33, 42, 85, 15, 1}, /* hot -> fan 3                */
        {30, 38, 60, 16, 1}, /* cooling + dry -> pump on    */
        {36, 20, 50, 17, 1}, /* heat + very dry -> alerts   */
        {24, 60, 10, 23, 1}, /* night, dim -> low backlight */
        {24, 60, 90, 3, 1},  /* deep night stays capped     */
        {0, 0, 0, 3, 0},     /* sensor failure -> hold+alert*/
    };

    env_decision_t prev;
    prev.fan_level = 0;
    prev.pump_on = 0;
    prev.backlight = 60;
    prev.alert = ENV_ALERT_NONE;
    prev.theme = ENV_THEME_DAY;

    printf("[HUB] rule-engine demo (scripted day):\n");
    for (unsigned i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
        env_decision_t d;
        env_rule_eval(&cfg, &prev, &script[i], &d);
        printf("  in{T=%dC H=%u%% L=%u h=%02u v=%u} -> "
               "fan=%u pump=%u bl=%u%% theme=%s alert=%s\n",
               (int)script[i].temp_c, script[i].humidity, script[i].light,
               script[i].hour, script[i].valid, d.fan_level, d.pump_on,
               d.backlight, env_theme_name(d.theme), env_alert_name(d.alert));
        prev = d;
    }
}

/* --- Closed-loop controller demo: sense -> decide -> (would) act. --- */
static void run_controller_demo(void) {
    hub_controller_t ctl;
    hub_controller_init(&ctl);

    /* {ms_offset, snapshot}: a warming, then drying, then recovering room. */
    struct step {
        uint32_t ms;
        env_snapshot_t s;
    };
    static const struct step steps[] = {
        {0, {23, 55, 70, 10, 1}},     /* start comfortable            */
        {2000, {27, 50, 75, 10, 1}},  /* fan -> 1                      */
        {4000, {33, 45, 80, 11, 1}},  /* fan -> 3                      */
        {6000, {36, 22, 60, 11, 1}},  /* HEAT alert (buzzer beeps)     */
        {6400, {36, 22, 60, 11, 1}},  /* 400ms later: still beeping    */
        {7200, {36, 22, 60, 11, 1}},  /* >800ms later: buzzer off      */
        {9000, {24, 30, 50, 12, 1}},  /* cools; DRY alert (new beep)   */
        {11000, {24, 55, 50, 12, 1}}, /* humidity recovers; alert clear*/
    };

    printf("[HUB] controller demo (sense->decide->act):\n");
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        hub_action_t a;
        hub_controller_step(&ctl, steps[i].ms, &steps[i].s, &a);
        printf("  t=%5ums T=%dC H=%u%% -> fan=%u%s pump=%u%s buzzer=%s"
               " alert=%s%s\n",
               steps[i].ms, (int)steps[i].s.temp_c, steps[i].s.humidity,
               a.decision.fan_level, a.fan_changed ? "*" : " ",
               a.decision.pump_on, a.pump_changed ? "*" : " ",
               a.buzzer_on ? "ON" : "--", env_alert_name(a.decision.alert),
               a.alert_started ? " (NEW)" : a.alert_cleared ? " (clr)" : "");
    }
}

/* --- Protocol round-trip + corruption checks. --- */
static int run_proto_tests(void) {
    int fails = 0;
    uint8_t buf[HUB_OVERHEAD + HUB_MAX_PAYLOAD];

    /* Snapshot encode -> parse. */
    env_snapshot_t s = {28, 41, 77, 14, 1};
    int n = hub_proto_encode_snapshot(7, &s, buf, sizeof(buf));
    hub_frame_t f;
    if (n <= 0 || hub_proto_parse(buf, (size_t)n, &f) != n ||
        f.type != HUB_MSG_SNAPSHOT || f.seq != 7 || f.len != 5) {
        printf("[HUB] FAIL snapshot round-trip (n=%d)\n", n);
        fails++;
    }

    /* Control encode -> parse -> decode. */
    hub_control_t c = {2, 1, ENV_THEME_COZY, 3, "有点闷,风扇开到二档~"};
    n = hub_proto_encode_control(9, &c, buf, sizeof(buf));
    hub_control_t got;
    if (n <= 0 || hub_proto_parse(buf, (size_t)n, &f) != n ||
        hub_proto_decode_control(&f, &got) != 0 || got.fan_level != 2 ||
        got.pump_on != 1 || got.theme_id != ENV_THEME_COZY || got.mood != 3 ||
        !str_eq(got.speech, c.speech)) {
        printf("[HUB] FAIL control round-trip (n=%d speech='%s')\n", n,
               n > 0 ? got.speech : "");
        fails++;
    }

    /* Corrupt one payload byte -> CRC must reject. */
    if (n > 8) {
        buf[6] ^= 0xFFu;
        if (hub_proto_parse(buf, (size_t)n, &f) != -1) {
            printf("[HUB] FAIL corruption not detected\n");
            fails++;
        }
        buf[6] ^= 0xFFu;
    }

    /* Truncated frame -> parser reports "need more" (0), not a false accept. */
    if (hub_proto_parse(buf, (size_t)(n - 1), &f) != 0) {
        printf("[HUB] FAIL truncation handling\n");
        fails++;
    }

    /* Bad magic -> reject. */
    uint8_t bad[8] = {0x00, 0x00, 0, 0, 0, 0, 0, 0};
    if (hub_proto_parse(bad, sizeof(bad), &f) != -1) {
        printf("[HUB] FAIL bad-magic handling\n");
        fails++;
    }

    return fails;
}

/* --- Touch calibration: four-corner solve + blob round-trip. --- */
static int run_touch_cal_tests(void) {
    int fails = 0;
    const uint16_t W = 320, H = 480;

    /* Non-swapped, non-inverted panel sampled at the four corners. */
    touch_cal_point_t pts[4] = {
        {300, 300, 0, 0},
        {3700, 300, 319, 0},
        {300, 3700, 0, 479},
        {3700, 3700, 319, 479},
    };
    stm32_touch_calibration_t cal;
    if (touch_cal_solve(pts, 4, W, H, &cal) != 0 || cal.swap_xy != 0 ||
        cal.invert_x != 0 || cal.invert_y != 0) {
        printf("[HUB] FAIL touch-cal solve\n");
        fails++;
    }

    uint8_t blob[TOUCH_CAL_BLOB_SIZE];
    stm32_touch_calibration_t rt;
    if (touch_cal_serialize(&cal, blob, sizeof(blob)) != TOUCH_CAL_BLOB_SIZE ||
        touch_cal_deserialize(blob, sizeof(blob), &rt) != 0 ||
        rt.x_min != cal.x_min || rt.x_max != cal.x_max) {
        printf("[HUB] FAIL touch-cal blob round-trip\n");
        fails++;
    }
    blob[6] ^= 0xFFu; /* corrupt payload -> CRC must reject */
    if (touch_cal_deserialize(blob, sizeof(blob), &rt) != -1) {
        printf("[HUB] FAIL touch-cal blob corruption not detected\n");
        fails++;
    }
    printf("[HUB] touch-cal: solved swap=%u invx=%u invy=%u x[%u,%u] y[%u,%u]\n",
           cal.swap_xy, cal.invert_x, cal.invert_y, cal.x_min, cal.x_max,
           cal.y_min, cal.y_max);
    return fails;
}

/* --- Home-screen UI model: build + hit-test + speech wrap. --- */
static int run_ui_home_tests(void) {
    int fails = 0;
    /* Speech text is LLM-generated at runtime — never hardcoded. This is a
     * synthetic mixed ASCII/CJK vector purely to exercise column wrapping.
     * Designated so adding a ui_home_state_t field can't silently shift the
     * values onto the wrong members. */
    ui_home_state_t st = {
        .temp_c = 29, .humidity = 38, .light = 55, .fan_level = 2,
        .pump_on = 1, .backlight = 60, .hour = 14, .minute = 30,
        .theme = ENV_THEME_COZY, .mood = 2, .alert = ENV_ALERT_DRY,
        .net_cloud = 1, .manual_mask = 0,
        .speech = "WRAP0123换行测试示例文本",
    };
    ui_home_model_t m;
    ui_home_build(&st, &m);

    if (m.card_count != 4 || !str_eq(m.cards[0].value, "29") ||
        !str_eq(m.cards[1].value, "38") || !str_eq(m.clock, "14:30")) {
        printf("[HUB] FAIL ui-home build\n");
        fails++;
    }
    /* Centre of the pump button must hit-test to PUMP_TOGGLE. */
    if (ui_home_hit_test(&m, 60, 309) != UI_ACTION_PUMP_TOGGLE ||
        ui_home_hit_test(&m, 200, 190) != UI_ACTION_FAN_UP ||
        ui_home_hit_test(&m, 10, 10) != UI_ACTION_NONE) {
        printf("[HUB] FAIL ui-home hit-test\n");
        fails++;
    }
    printf("[HUB] ui-home: clock=%s net=%s alert=%s cards=[%sC %s%% L%s F%s]"
           " speech_lines=%u\n",
           m.clock, m.net_label, m.alert_label, m.cards[0].value,
           m.cards[1].value, m.cards[2].value, m.cards[3].value,
           m.speech_lines);
    for (unsigned i = 0; i < m.speech_lines; i++)
        printf("[HUB]   speech[%u]=\"%s\"\n", i, m.speech[i]);
    return fails;
}

/* --- Live2D sprite state machine: mood mapping + frame path + TALK timeout. */
static int run_live2d_tests(void) {
    int fails = 0;
    live2d_t l;
    live2d_init(&l);

    if (live2d_mood_to_state(2, 0) != LIVE2D_HAPPY ||
        live2d_mood_to_state(2, 1) != LIVE2D_WARN) {
        printf("[HUB] FAIL live2d mood mapping\n");
        fails++;
    }

    char path[LIVE2D_PATH_MAX];
    live2d_set_state(&l, LIVE2D_TALK, 0);
    l.frame = 7;
    if (live2d_frame_path(&l, path, sizeof(path)) <= 0 ||
        !str_eq(path, "/live2d/talk/07.raw")) {
        printf("[HUB] FAIL live2d frame path (%s)\n", path);
        fails++;
    }

    /* speak() then run past the deadline -> reverts to IDLE. The string is a
     * synthetic length vector; real speech is LLM-generated. */
    live2d_init(&l);
    live2d_speak(&l, "SPEAKLEN示例文本测试", 1000);
    uint32_t deadline = l.talk_deadline_ms;
    live2d_tick(&l, deadline + 1);
    if (l.state != LIVE2D_IDLE) {
        printf("[HUB] FAIL live2d TALK timeout\n");
        fails++;
    }
    printf("[HUB] live2d: talk window=%ums -> path=%s\n", deadline - 1000,
           path);
    return fails;
}

/* --- RGB matrix framebuffer: bounds, color layout, fill and brightness. --- */
static int run_rgb_matrix_tests(void) {
    int fails = 0;
    uint8_t saved_brightness = stm32_rgb_matrix_brightness();

    stm32_rgb_matrix_clear();
    if (stm32_rgb_matrix_set_pixel(2, 3, 0x123456U) != 0 ||
        stm32_rgb_matrix_get_pixel(2, 3) != 0x123456U ||
        stm32_rgb_matrix_set_pixel(5, 0, STM32_RGB_COLOR_RED) != -1 ||
        stm32_rgb_matrix_set_pixel(0, 5, STM32_RGB_COLOR_RED) != -1) {
        printf("[HUB] FAIL rgb-matrix pixel/bounds\n");
        fails++;
    }

    stm32_rgb_matrix_fill(STM32_RGB_COLOR_YELLOW);
    for (uint8_t y = 0; y < STM32_RGB_MATRIX_HEIGHT; y++) {
        for (uint8_t x = 0; x < STM32_RGB_MATRIX_WIDTH; x++) {
            if (stm32_rgb_matrix_get_pixel(x, y) != STM32_RGB_COLOR_YELLOW)
                fails++;
        }
    }
    stm32_rgb_matrix_set_brightness(73);
    if (stm32_rgb_matrix_brightness() != 73U) {
        printf("[HUB] FAIL rgb-matrix brightness\n");
        fails++;
    }
    stm32_rgb_matrix_clear();
    stm32_rgb_matrix_set_brightness(saved_brightness);
    printf("[HUB] rgb-matrix: 5x5 framebuffer %s\n",
           fails == 0 ? "PASS" : "FAIL");
    return fails;
}

int smarthub_selftest(void) {
    printf("\n[HUB] ===== smart-hub core self-test =====\n");
    run_rule_demo();
    run_controller_demo();
    int fails = run_proto_tests();
    fails += run_touch_cal_tests();
    fails += run_ui_home_tests();
    fails += run_live2d_tests();
    fails += run_rgb_matrix_tests();
    printf("[HUB] protocol self-test: %s (%d failure%s)\n",
           fails == 0 ? "PASS" : "FAIL", fails, fails == 1 ? "" : "s");
    printf("[HUB] ===== self-test done =====\n\n");
    return fails;
}

#endif /* CONFIG_BOARD_STM32F103 */
