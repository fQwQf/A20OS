/*
 * Smart home hub — environment rule engine implementation.
 * Hardware-independent; see env_rule.h.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "env_rule.h"

void env_rule_default_config(env_rule_config_t *cfg) {
    if (!cfg)
        return;
    /* Fan raises at 26/29/32 C, drops at 25/28/31 C (1 C hysteresis). */
    cfg->fan_up[0] = 26;
    cfg->fan_up[1] = 29;
    cfg->fan_up[2] = 32;
    cfg->fan_down[0] = 25;
    cfg->fan_down[1] = 28;
    cfg->fan_down[2] = 31;
    /* Pump (humidifier) on below 40 %RH, off at/above 45 %RH. */
    cfg->humid_on = 40;
    cfg->humid_off = 45;
    cfg->heat_alert_c = 35;
    cfg->dry_alert = 25;
    cfg->night_start = 22;
    cfg->night_end = 6;
    cfg->night_bl_max = 30;
}

static int is_night(const env_rule_config_t *cfg, uint8_t hour) {
    /* Night window may wrap midnight (e.g. 22..06). */
    if (cfg->night_start <= cfg->night_end)
        return hour >= cfg->night_start && hour < cfg->night_end;
    return hour >= cfg->night_start || hour < cfg->night_end;
}

static uint8_t clamp_u8(int v, int lo, int hi) {
    if (v < lo)
        return (uint8_t)lo;
    if (v > hi)
        return (uint8_t)hi;
    return (uint8_t)v;
}

void env_rule_eval(const env_rule_config_t *cfg, const env_decision_t *prev,
                   const env_snapshot_t *in, env_decision_t *out) {
    env_decision_t prev_local = *prev;
    int night = is_night(cfg, in->hour);

    /* --- Sensor failure: fail safe. Hold last actuation, raise SENSOR. --- */
    if (!in->valid) {
        out->fan_level = prev_local.fan_level;
        out->pump_on = prev_local.pump_on;
        out->alert = ENV_ALERT_SENSOR;
        /* Backlight still tracks the (last known) theme sensibly. */
        out->backlight = night ? cfg->night_bl_max : 60;
        out->theme = night ? ENV_THEME_NIGHT : ENV_THEME_DAY;
        return;
    }

    /* --- Fan level with hysteresis (start from previous level). --- */
    int level = prev_local.fan_level;
    if (level > 3)
        level = 3;
    while (level < 3 && in->temp_c >= cfg->fan_up[level])
        level++;
    while (level > 0 && in->temp_c < cfg->fan_down[level - 1])
        level--;
    out->fan_level = (uint8_t)level;

    /* --- Pump with hysteresis. --- */
    int pump = prev_local.pump_on ? 1 : 0;
    if (in->humidity < cfg->humid_on)
        pump = 1;
    else if (in->humidity >= cfg->humid_off)
        pump = 0;
    out->pump_on = (uint8_t)pump;

    /* --- Backlight: track ambient light; floor at 10, cap at night. --- */
    int bl = in->light;
    if (bl < 10)
        bl = 10;
    if (night && bl > cfg->night_bl_max)
        bl = cfg->night_bl_max;
    out->backlight = clamp_u8(bl, 0, 100);

    /* --- Theme. --- */
    if (night)
        out->theme = ENV_THEME_NIGHT;
    else if (in->light < 30)
        out->theme = ENV_THEME_COZY;
    else
        out->theme = ENV_THEME_DAY;

    /* --- Alert (priority: heat > dry > none). --- */
    if (in->temp_c >= cfg->heat_alert_c)
        out->alert = ENV_ALERT_HEAT;
    else if (in->humidity < cfg->dry_alert)
        out->alert = ENV_ALERT_DRY;
    else
        out->alert = ENV_ALERT_NONE;
}

const char *env_alert_name(env_alert_t a) {
    switch (a) {
    case ENV_ALERT_HEAT:
        return "HEAT";
    case ENV_ALERT_DRY:
        return "DRY";
    case ENV_ALERT_SENSOR:
        return "SENSOR";
    case ENV_ALERT_NONE:
    default:
        return "NONE";
    }
}

const char *env_theme_name(env_theme_t t) {
    switch (t) {
    case ENV_THEME_NIGHT:
        return "NIGHT";
    case ENV_THEME_COZY:
        return "COZY";
    case ENV_THEME_DAY:
    default:
        return "DAY";
    }
}

#endif /* CONFIG_BOARD_STM32F103 */
