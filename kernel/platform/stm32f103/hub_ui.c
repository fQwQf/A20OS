/*
 * Hub UI state assembly. Pure logic — see hub_ui.h. Shared by peripherals.c
 * (device) and tools/hub-sim (host simulator), covered by the host unit test.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "hub_ui.h"

live2d_state_t hub_ui_state(const hub_ui_input_t *in) {
    if (in->decision.alert != ENV_ALERT_NONE)
        return LIVE2D_WARN; /* something's wrong -> react regardless of cloud */
    if (in->cloud_valid)
        return live2d_mood_to_state(in->cloud_mood, 0);
    if (in->decision.fan_level == 0 && !in->decision.pump_on)
        return LIVE2D_HAPPY; /* calm, comfortable */
    return LIVE2D_TALK;      /* actively managing the room */
}

void hub_ui_build(const hub_ui_input_t *in, ui_home_state_t *out) {
    out->temp_c = in->snap.temp_c;
    out->humidity = in->snap.humidity;
    out->light = in->snap.light;
    out->fan_level = in->decision.fan_level;
    out->pump_on = in->decision.pump_on;
    out->hour = in->snap.hour;
    out->minute = in->minute;
    out->theme = (uint8_t)in->decision.theme;
    out->mood = (uint8_t)hub_ui_state(in);
    out->alert = (uint8_t)in->decision.alert;
    out->net_cloud = in->net_cloud;
    /* Bubble text is LLM-only: never fabricated locally. */
    out->speech = (in->cloud_valid && in->cloud_speech && in->cloud_speech[0])
                      ? in->cloud_speech
                      : (const char *)0;
}

#endif /* CONFIG_BOARD_STM32F103 */
