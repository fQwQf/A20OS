/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_SG2002_AUDIO_CLOCK_H
#define A20_SG2002_AUDIO_CLOCK_H
#include "core/types.h"
/* SDK cv181x_g2_pll_recalc_rate, read-only A0PLL. Never reprogram a CPU PLL. */
static inline unsigned sg2002_audio_divider(uint32_t mipi, uint32_t synth,
                                            uint32_t control, uint32_t audio) {
    uint64_t pre = mipi & 127, post = (mipi >> 8) & 127,
             mul = (mipi >> 17) & 127;
    if (!pre || !post || !mul || !synth)
        return 0;
    uint64_t parent = 25000000ULL * mul / (pre * post);
    uint64_t ref = (parent << ((control & 1) ? 26 : 25)) / synth;
    pre = audio & 127;
    post = (audio >> 8) & 127;
    mul = (audio >> 17) & 127;
    if (!pre || !post || !mul || ref > (uint64_t)-1 / mul)
        return 0;
    uint64_t rate = ref * mul / (pre * post);
    uint64_t div = (rate + 12288000) / 24576000;
    if (!div || div > 255)
        return 0;
    uint64_t target = div * 24576000;
    uint64_t error = rate > target ? rate - target : target - rate;
    return error <= target / 10000 ? (unsigned)div : 0; /* <=100ppm */
}
#endif
