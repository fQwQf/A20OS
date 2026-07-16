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

int smarthub_selftest(void) {
    printf("\n[HUB] ===== smart-hub core self-test =====\n");
    run_rule_demo();
    run_controller_demo();
    int fails = run_proto_tests();
    printf("[HUB] protocol self-test: %s (%d failure%s)\n",
           fails == 0 ? "PASS" : "FAIL", fails, fails == 1 ? "" : "s");
    printf("[HUB] ===== self-test done =====\n\n");
    return fails;
}

#endif /* CONFIG_BOARD_STM32F103 */
