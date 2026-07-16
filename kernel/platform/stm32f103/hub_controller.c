/*
 * Smart home hub — control orchestration implementation. See hub_controller.h.
 * Hardware-independent.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "hub_controller.h"

void hub_controller_init(hub_controller_t *c) {
    env_rule_default_config(&c->cfg);
    c->have_last = 0;
    c->buzzer_until_ms = 0;
    /* last is only read when have_last is set; zero it for tidiness. */
    c->last.fan_level = 0;
    c->last.pump_on = 0;
    c->last.backlight = 60;
    c->last.alert = ENV_ALERT_NONE;
    c->last.theme = ENV_THEME_DAY;
}

void hub_controller_step(hub_controller_t *c, uint32_t now_ms,
                         const env_snapshot_t *in, hub_action_t *act) {
    /* Feed the previous decision as the hysteresis baseline. On the first
     * call use a neutral default (fan off, day). */
    env_decision_t prev;
    if (c->have_last) {
        prev = c->last;
    } else {
        prev.fan_level = 0;
        prev.pump_on = 0;
        prev.backlight = 60;
        prev.alert = ENV_ALERT_NONE;
        prev.theme = ENV_THEME_DAY;
    }

    env_rule_eval(&c->cfg, &prev, in, &act->decision);

    env_alert_t prev_alert = c->have_last ? c->last.alert : ENV_ALERT_NONE;
    act->fan_changed =
        (!c->have_last) || (act->decision.fan_level != c->last.fan_level);
    act->pump_changed =
        (!c->have_last) || (act->decision.pump_on != c->last.pump_on);

    /* Alert edges. A change of alert *type* (e.g. DRY -> HEAT) counts as a
     * new alert so the user is re-notified. */
    act->alert_started =
        (act->decision.alert != ENV_ALERT_NONE) &&
        (act->decision.alert != prev_alert);
    act->alert_cleared =
        (act->decision.alert == ENV_ALERT_NONE) && (prev_alert != ENV_ALERT_NONE);

    if (act->alert_started)
        c->buzzer_until_ms = now_ms + HUB_BUZZER_MS;
    act->buzzer_on = (now_ms < c->buzzer_until_ms) ? 1u : 0u;

    c->last = act->decision;
    c->have_last = 1;
}

#endif /* CONFIG_BOARD_STM32F103 */
