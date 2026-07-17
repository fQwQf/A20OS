#ifndef _STM32F103_LIVE2D_H
#define _STM32F103_LIVE2D_H

/*
 * Live2D catgirl sprite state machine (hardware-independent).
 *
 * A real Cubism runtime can't fit on an F103 (§2.3 of the hub manual), so the
 * catgirl is played back as pre-rendered RGB565 sprite frames stored on the TF
 * card under /live2d/<state>/<NN>.raw. This module owns the animation state:
 * which of IDLE/TALK/HAPPY/WARN is active (driven by the cloud `mood` byte or a
 * local alert), advancing frames at a fixed fps, auto-returning from TALK to
 * IDLE once the speech has been on screen long enough, and resolving the
 * current frame's card path. It reports a "dirty" edge so the renderer only
 * redraws the sprite region when the frame actually changes.
 *
 * The mood ids match hub_proto's control frame and the proxy's MOOD table
 * (IDLE=0, TALK=1, HAPPY=2, WARN=3), so a decoded control.mood is a state id.
 *
 * Pure integer/string logic — covered by the host unit test and QEMU
 * self-test; the frame blitting itself is the hardware step.
 */

#include "core/types.h"

typedef enum live2d_state {
    LIVE2D_IDLE = 0,
    LIVE2D_TALK = 1,
    LIVE2D_HAPPY = 2,
    LIVE2D_WARN = 3,
    LIVE2D_STATE_COUNT = 4
} live2d_state_t;

/* Frame cadence and per-state frame counts (see live2d.c frame_counts[]). */
#define LIVE2D_FRAME_MS 66U /* ~15 fps */
#define LIVE2D_PATH_MAX 32U

typedef struct live2d {
    live2d_state_t state;
    unsigned frame;          /* current frame index within the state */
    uint32_t last_advance_ms;
    uint32_t talk_deadline_ms; /* when TALK auto-reverts to IDLE (0 = never) */
    int dirty;               /* set when the visible frame changed */
} live2d_t;

void live2d_init(live2d_t *l);

/* Number of frames in a state, and its lowercase name ("idle"/"talk"/...). */
unsigned live2d_frame_count(live2d_state_t s);
const char *live2d_state_name(live2d_state_t s);

/*
 * Choose a state from a cloud mood id (0..3) and a local alert flag. A live
 * local alert (non-zero) forces WARN so the catgirl reacts even when offline /
 * the cloud said something calmer.
 */
live2d_state_t live2d_mood_to_state(uint8_t mood, uint8_t alert_active);

/* Switch to a state immediately (resets the frame if the state changed). A
 * timed TALK is set up via live2d_speak() instead. */
void live2d_set_state(live2d_t *l, live2d_state_t s, uint32_t now_ms);

/*
 * Enter TALK for a duration derived from the speech length (longer text stays
 * up longer, clamped to a sane window), then it auto-returns to IDLE. Passing
 * NULL/empty speech uses the minimum window.
 */
void live2d_speak(live2d_t *l, const char *speech, uint32_t now_ms);

/*
 * Advance the animation to now_ms. Handles the TALK->IDLE timeout and frame
 * stepping. Returns 1 if the visible frame/state changed (renderer should
 * redraw the sprite region), 0 otherwise.
 */
int live2d_tick(live2d_t *l, uint32_t now_ms);

/* Consume a clock-interrupt frame sequence once. Repeated service calls with
 * the same sequence return 0 and must not advance the animation. */
int live2d_frame_clock_consume(uint32_t *observed, uint32_t current);

/*
 * Resolve the current frame to its TF-card path, e.g. "/live2d/talk/07.raw".
 * Returns the string length written, or -1 on bad args / insufficient buffer.
 */
int live2d_frame_path(const live2d_t *l, char *buf, unsigned cap);

#endif /* _STM32F103_LIVE2D_H */
