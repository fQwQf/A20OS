/*
 * Smart home hub — control orchestration (hardware-independent).
 *
 * Wraps the env rule engine with the stateful bits the main loop needs:
 * change detection (so we only re-drive actuators when a value changes) and
 * alert edge detection (so the buzzer beeps once when an alert appears rather
 * than screaming continuously). Pure and deterministic — `now_ms` is passed
 * in — so it is covered by the QEMU self-test and the host unit test.
 *
 * The platform layer (peripherals.c) samples DHT11 + light + hour into an
 * env_snapshot_t, calls hub_controller_step(), and applies the resulting
 * hub_action_t to the real actuators. See docs/stm32-big-exp.md §4.2, §5.5.
 */
#ifndef _STM32F103_HUB_CONTROLLER_H
#define _STM32F103_HUB_CONTROLLER_H

#include "core/types.h"
#include "env_rule.h"

#define HUB_BUZZER_MS 800u /* how long the buzzer sounds per alert edge */

typedef struct hub_action {
    env_decision_t decision;     /* full decision from the rule engine     */
    uint8_t        fan_changed;  /* fan_level differs from last step        */
    uint8_t        pump_changed; /* pump_on differs from last step          */
    uint8_t        buzzer_on;    /* desired buzzer state this step          */
    uint8_t        alert_started;/* rising edge into a (new) alert          */
    uint8_t        alert_cleared;/* falling edge back to NONE               */
} hub_action_t;

typedef struct hub_controller {
    env_rule_config_t cfg;
    env_decision_t    last;
    int               have_last;
    uint32_t          buzzer_until_ms;
} hub_controller_t;

/* Initialise with default thresholds and no prior state. */
void hub_controller_init(hub_controller_t *c);

/* Evaluate one snapshot at time now_ms; fills `act`. On the first call
 * fan_changed/pump_changed are set so the caller applies the initial state. */
void hub_controller_step(hub_controller_t *c, uint32_t now_ms,
                         const env_snapshot_t *in, hub_action_t *act);

#endif /* _STM32F103_HUB_CONTROLLER_H */
