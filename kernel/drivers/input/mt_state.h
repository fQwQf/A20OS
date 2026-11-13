/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_INPUT_MT_STATE_H
#define A20_INPUT_MT_STATE_H
#define INPUT_MT_SLOTS_MAX 16
#define INPUT_MT_AXIS_FIRST 0x30
#define INPUT_MT_AXES 14
struct input_mt_state {
    unsigned slot;
    int values[INPUT_MT_SLOTS_MAX][INPUT_MT_AXES];
};
static inline void input_mt_init(struct input_mt_state *state)
{
    state->slot = 0;
    for (unsigned i = 0; i < INPUT_MT_SLOTS_MAX; i++)
        for (unsigned a = 0; a < INPUT_MT_AXES; a++)
            state->values[i][a] = a == 9 ? -1 : 0; /* TRACKING_ID */
}
static inline void input_mt_update(struct input_mt_state *state, unsigned code, int value)
{
    if (code == 0x2f) {
        if (value >= 0 && value < INPUT_MT_SLOTS_MAX)
            state->slot = value;
    } else if (code >= INPUT_MT_AXIS_FIRST && code < INPUT_MT_AXIS_FIRST + INPUT_MT_AXES) {
        state->values[state->slot][code - INPUT_MT_AXIS_FIRST] = value;
    }
}
static inline int input_mt_snapshot(const struct input_mt_state *state, unsigned code,
                                     int *values, unsigned count)
{
    if (code < INPUT_MT_AXIS_FIRST || code >= INPUT_MT_AXIS_FIRST + INPUT_MT_AXES ||
        count > INPUT_MT_SLOTS_MAX)
        return -1;
    for (unsigned i = 0; i < count; i++)
        values[i] = state->values[i][code - INPUT_MT_AXIS_FIRST];
    return 0;
}
#endif
