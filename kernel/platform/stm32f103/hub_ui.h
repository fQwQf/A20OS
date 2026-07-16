#ifndef _STM32F103_HUB_UI_H
#define _STM32F103_HUB_UI_H

/*
 * Hub UI state assembly (hardware-independent).
 *
 * Turns "what the controller decided this tick" (+ the optional cloud
 * mood/speech) into the ui_home_state_t the renderer draws and the Live2D
 * state the catgirl plays. One place owns this mapping so the on-device path
 * (peripherals.c) and the host simulator (tools/hub-sim) stay identical.
 *
 * Speech rule: the bubble text is ONLY ever the cloud (LLM) speech — never
 * fabricated locally. Offline / no cloud reply -> no bubble. Mood is a state
 * (not text): alert forces WARN, else the cloud mood if present, else it is
 * derived from the decision.
 */

#include "core/types.h"
#include "env_rule.h"
#include "ui_home.h"
#include "live2d.h"

typedef struct hub_ui_input {
    env_snapshot_t snap;
    env_decision_t decision;
    uint8_t minute;       /* 0..59 (from RTC or uptime)               */
    uint8_t net_cloud;    /* 1 = this decision came from the cloud     */
    uint8_t cloud_valid;  /* 1 = cloud mood/speech below are present   */
    uint8_t cloud_mood;   /* Live2D mood id 0..3 (when cloud_valid)    */
    const char *cloud_speech; /* LLM bubble text (NULL/"" if none)     */
} hub_ui_input_t;

/* Live2D state for this input: alert -> WARN; else cloud mood if valid; else
 * derived from the decision (idle -> HAPPY, otherwise TALK). */
live2d_state_t hub_ui_state(const hub_ui_input_t *in);

/* Fill a ui_home_state_t. out->speech is the cloud speech only (or NULL). */
void hub_ui_build(const hub_ui_input_t *in, ui_home_state_t *out);

#endif /* _STM32F103_HUB_UI_H */
