/*
 * Live2D catgirl sprite state machine. Pure integer/string logic (no register
 * or display access) so it runs under the host unit test and QEMU self-test.
 * See live2d.h for the model.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "live2d.h"

/* Frame counts per state; TALK gets the most for a lively mouth flap. */
static const unsigned frame_counts[LIVE2D_STATE_COUNT] = {
    [LIVE2D_IDLE] = 8U,
    [LIVE2D_TALK] = 12U,
    [LIVE2D_HAPPY] = 10U,
    [LIVE2D_WARN] = 6U,
};

static const char *const state_names[LIVE2D_STATE_COUNT] = {
    [LIVE2D_IDLE] = "idle",
    [LIVE2D_TALK] = "talk",
    [LIVE2D_HAPPY] = "happy",
    [LIVE2D_WARN] = "warn",
};

/* Speech-on-screen window bounds, and how long each display column lingers. */
#define TALK_MIN_MS 1500U
#define TALK_MAX_MS 6000U
#define TALK_BASE_MS 400U
#define TALK_PER_COL_MS 180U

static int state_valid(live2d_state_t s) {
    /* enum may be an unsigned type; the cast also catches negative ids. */
    return (unsigned)s < (unsigned)LIVE2D_STATE_COUNT;
}

int live2d_state_loops(live2d_state_t s) { return state_valid(s); }

void live2d_init(live2d_t *l) {
    if (!l)
        return;
    l->state = LIVE2D_IDLE;
    l->frame = 0;
    l->last_advance_ms = 0;
    l->talk_deadline_ms = 0;
    l->dirty = 1; /* first render should paint frame 0 */
    l->stopped = 0;
}

unsigned live2d_frame_count(live2d_state_t s) {
    return state_valid(s) ? frame_counts[s] : 0U;
}

const char *live2d_state_name(live2d_state_t s) {
    return state_valid(s) ? state_names[s] : "idle";
}

live2d_state_t live2d_mood_to_state(uint8_t mood, uint8_t alert_active) {
    if (alert_active)
        return LIVE2D_WARN;
    if (mood >= LIVE2D_STATE_COUNT)
        return LIVE2D_IDLE;
    return (live2d_state_t)mood;
}

void live2d_set_state(live2d_t *l, live2d_state_t s, uint32_t now_ms) {
    if (!l || !state_valid(s))
        return;
    l->talk_deadline_ms = 0; /* an explicit state cancels a timed TALK */
    if (l->state == s)
        return; /* re-asserting the current state must not restart a parked one */
    l->state = s;
    l->frame = 0;
    l->last_advance_ms = now_ms;
    l->dirty = 1;
    l->stopped = 0;
}

/* Display-column length of a UTF-8 string: ASCII=1, CJK/lead byte>=0xC0 = 2. */
static unsigned speech_cols(const char *s) {
    unsigned cols = 0;
    while (s && *s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0xC0u)
            cols += 2;
        else if (c < 0x80u)
            cols += 1;
        /* continuation bytes (0x80..0xBF) add nothing */
        s++;
    }
    return cols;
}

void live2d_speak(live2d_t *l, const char *speech, uint32_t now_ms) {
    if (!l)
        return;
    uint32_t dur = TALK_BASE_MS + speech_cols(speech) * TALK_PER_COL_MS;
    if (dur < TALK_MIN_MS)
        dur = TALK_MIN_MS;
    if (dur > TALK_MAX_MS)
        dur = TALK_MAX_MS;

    if (l->state != LIVE2D_TALK) {
        l->state = LIVE2D_TALK;
        l->frame = 0;
        l->last_advance_ms = now_ms;
        l->dirty = 1;
        l->stopped = 0;
    }
    l->talk_deadline_ms = now_ms + dur;
    if (l->talk_deadline_ms == 0) /* avoid the "never" sentinel on wrap */
        l->talk_deadline_ms = 1;
}

int live2d_tick(live2d_t *l, uint32_t now_ms) {
    if (!l)
        return 0;
    int changed = 0;

    /* TALK auto-returns to IDLE once the speech window elapses. */
    if (l->state == LIVE2D_TALK && l->talk_deadline_ms != 0 &&
        (int32_t)(now_ms - l->talk_deadline_ms) >= 0) {
        l->talk_deadline_ms = 0;
        l->state = LIVE2D_IDLE;
        l->frame = 0;
        l->last_advance_ms = now_ms;
        l->dirty = 1;
        l->stopped = 0;
        changed = 1;
    }

    /* Keep animation phase tied to the millisecond clock. SD reads and LCD
     * blits can hold the main loop for more than one frame; advancing only one
     * frame and resetting the epoch to now made every such stall permanently
     * slow the animation and produced irregular motion. */
    uint32_t elapsed = now_ms - l->last_advance_ms;
    if (elapsed >= LIVE2D_FRAME_MS) {
        unsigned count = frame_counts[l->state];
        uint32_t steps = elapsed / LIVE2D_FRAME_MS;
        if (count == 0)
            count = 1;
        l->frame = (l->frame + steps) % count;
        l->last_advance_ms += steps * LIVE2D_FRAME_MS;
        l->dirty = 1;
        changed = 1;
    }

    if (l->dirty) {
        l->dirty = 0;
        changed = 1;
    }
    return changed;
}

int live2d_frame_clock_consume(uint32_t *observed, uint32_t current) {
    if (!observed || *observed == current)
        return 0;
    *observed = current;
    return 1;
}

static int append(char *buf, unsigned cap, unsigned *pos, const char *s) {
    while (*s) {
        if (*pos + 1U >= cap)
            return -1;
        buf[(*pos)++] = *s++;
    }
    return 0;
}

int live2d_frame_path(const live2d_t *l, char *buf, unsigned cap) {
    if (!l || !buf || cap == 0 || !state_valid(l->state))
        return -1;
    unsigned pos = 0;
    unsigned f = l->frame;
    if (append(buf, cap, &pos, "/live2d/") != 0 ||
        append(buf, cap, &pos, state_names[l->state]) != 0 ||
        append(buf, cap, &pos, "/") != 0)
        return -1;
    if (pos + 3U >= cap) /* "NN" + NUL */
        return -1;
    buf[pos++] = (char)('0' + (f / 10U) % 10U);
    buf[pos++] = (char)('0' + f % 10U);
    if (append(buf, cap, &pos, ".raw") != 0)
        return -1;
    buf[pos] = '\0';
    return (int)pos;
}

#endif /* CONFIG_BOARD_STM32F103 */
