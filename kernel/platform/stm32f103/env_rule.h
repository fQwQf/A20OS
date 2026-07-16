/*
 * Smart home hub — environment rule engine (hardware-independent).
 *
 * Maps an environment snapshot (temperature/humidity/light/hour) to a set of
 * actuator decisions (fan level, pump, backlight, alert, UI theme). Pure
 * logic with hysteresis; no register access, so it is testable both in QEMU
 * and with the host unit test (tools/smarthub-test).
 *
 * See docs/stm32-big-exp.md §5.5 (本地规则引擎) and §2.3.
 */
#ifndef _STM32F103_ENV_RULE_H
#define _STM32F103_ENV_RULE_H

#include "core/types.h"

/* Environment snapshot sampled from the sensors. */
typedef struct env_snapshot {
    int16_t temp_c;    /* whole degrees C (DHT11 returns an integer)         */
    uint8_t humidity;  /* relative humidity %RH, 0..100                      */
    uint8_t light;     /* ambient light 0..100 (light sensor / ADC3)         */
    uint8_t hour;      /* local hour 0..23 (RTC) for night quieting          */
    uint8_t valid;     /* 1 = snapshot usable, 0 = sensor read failed        */
} env_snapshot_t;

typedef enum env_alert {
    ENV_ALERT_NONE = 0,
    ENV_ALERT_HEAT,    /* too hot                                            */
    ENV_ALERT_DRY,     /* too dry                                            */
    ENV_ALERT_SENSOR,  /* sensor failure                                     */
} env_alert_t;

typedef enum env_theme {
    ENV_THEME_DAY = 0,
    ENV_THEME_NIGHT,
    ENV_THEME_COZY,
} env_theme_t;

/* Actuator/UI decision produced by the engine. */
typedef struct env_decision {
    uint8_t     fan_level;  /* 0..3                                          */
    uint8_t     pump_on;    /* 0/1                                           */
    uint8_t     backlight;  /* 0..100                                        */
    env_alert_t alert;
    env_theme_t theme;
} env_decision_t;

/* Tunable thresholds. Hysteresis prevents flapping around a boundary. */
typedef struct env_rule_config {
    int16_t fan_up[3];      /* temp at/above which to raise to level i+1     */
    int16_t fan_down[3];    /* temp below which to drop from level i+1       */
    uint8_t humid_on;       /* humidity below -> pump on                     */
    uint8_t humid_off;      /* humidity at/above -> pump off                 */
    int16_t heat_alert_c;   /* temp at/above -> heat alert                   */
    uint8_t dry_alert;      /* humidity below -> dry alert                   */
    uint8_t night_start;    /* hour night begins (e.g. 22)                   */
    uint8_t night_end;      /* hour night ends (e.g. 6)                      */
    uint8_t night_bl_max;   /* cap backlight at night                        */
} env_rule_config_t;

/* Fill cfg with sane defaults. */
void env_rule_default_config(env_rule_config_t *cfg);

/*
 * Pure evaluation. `prev` is the previous decision (drives fan/pump
 * hysteresis and the fail-safe hold on sensor failure); pass a zeroed
 * env_decision_t on the first call. Result is written to `out` (out may
 * alias prev safely — inputs are read before out is written).
 */
void env_rule_eval(const env_rule_config_t *cfg, const env_decision_t *prev,
                   const env_snapshot_t *in, env_decision_t *out);

const char *env_alert_name(env_alert_t a);
const char *env_theme_name(env_theme_t t);

#endif /* _STM32F103_ENV_RULE_H */
