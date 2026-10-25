/* SPDX-License-Identifier: Apache-2.0
 * Small cyclic memory-to-peripheral program for an ARM PL330 DMA engine.
 * Instruction encodings and CCR fields: ARM DDI0424, Linux v6.17 pl330.c.
 * This generator contains no CPU instructions or platform addresses.
 */
#ifndef A20_PL330_AUDIO_PROGRAM_H
#define A20_PL330_AUDIO_PROGRAM_H
#include "core/types.h"
#define PL330_AUDIO_PERIODS 8U
#define PL330_AUDIO_PERIOD_BYTES 8192U
/* Eight 32-bit words per burst; nonsecure, incrementing uncached memory
 * source and non-incrementing peripheral destination. */
#define PL330_AUDIO_CCR                                                        \
    (1U | (2U << 1) | (7U << 4) | (1U << 9) | (2U << 15) | (7U << 18) |        \
     (1U << 23))
static inline void pl330_audio_word(uint8_t *p, uint32_t word) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(word >> (8 * i));
}
static inline unsigned pl330_audio_mov(uint8_t *p, unsigned reg,
                                       uint32_t value) {
    p[0] = 0xbc;
    p[1] = reg;
    pl330_audio_word(p + 2, value);
    return 6;
}
/* Returns program length, or 0 if a 32-bit DMA address/encoding is invalid.
 * Each period has DMAWMB before DMASEV so retirement observes completed writes.
 * The finite loop is exactly256 bursts*32B=8192B. Infinite back edge remains
 * below the instruction's 8-bit maximum jump distance. */
static inline unsigned pl330_audio_program(uint8_t *p, size_t capacity,
                                           uint64_t data, uint64_t fifo,
                                           unsigned request, unsigned event) {
    if (!p || capacity < 192 || request >= 32 || event >= 32 ||
        data >
            0x100000000ULL - PL330_AUDIO_PERIODS * PL330_AUDIO_PERIOD_BYTES ||
        fifo > 0xffffffffULL || (data & 3) || (fifo & 3))
        return 0;
    unsigned n = 0;
    n += pl330_audio_mov(p + n, 1, PL330_AUDIO_CCR); /* CCR */
    n += pl330_audio_mov(p + n, 2, (uint32_t)fifo);  /* DAR */
    p[n++] = 0x35;
    p[n++] = (uint8_t)(request << 3); /* DMAFLUSHP */
    unsigned first = n;
    for (unsigned i = 0; i < PL330_AUDIO_PERIODS; i++) {
        n += pl330_audio_mov(p + n, 0,
                             (uint32_t)(data + i * PL330_AUDIO_PERIOD_BYTES));
        p[n++] = 0x20;
        p[n++] = 255; /* DMALP LC0,256 */
        unsigned body = n;
        p[n++] = 0x32;
        p[n++] = (uint8_t)(request << 3); /* DMAWFP burst */
        p[n++] = 0x07;                    /* DMALD burst */
        p[n++] = 0x2b;
        p[n++] = (uint8_t)(request << 3); /* DMASTP burst */
        unsigned back = n - body;
        p[n++] = 0x38;
        p[n++] = (uint8_t)back; /* DMALPEND LC0,always */
        p[n++] = 0x13;          /* DMAWMB */
        p[n++] = 0x34;
        p[n++] = (uint8_t)(event << 3); /* DMASEV */
    }
    unsigned back = n - first;
    if (back > 255)
        return 0;
    p[n++] = 0x28;
    p[n++] = (uint8_t)back; /* DMALPFE */
    return n;
}
#endif
