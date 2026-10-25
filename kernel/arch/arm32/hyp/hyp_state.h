#ifndef A20_ARM32_HYP_STATE_H
#define A20_ARM32_HYP_STATE_H
#define A32_BANK_SIZE 592
#define A32_PC 52
#define A32_CPSR 56
#define A32_BANKED 64
#define A32_SYS 160
#define A32_FP 320
#define A32_FPSCR 576
#define A32_FPEXC 580
#define A32_HOST 592
#define A32_VTTBR 1184
#define A32_VTCR 1192
#define A32_HCR 1196
#define A32_HSR 1200
#define A32_HDFAR 1204
#define A32_HIFAR 1208
#define A32_HPFAR 1212
#define A32_KIND 1216
#define A32_MPIDR 1220
#ifndef __ASSEMBLER__
#include "core/types.h"
struct arm32_hyp_bank {
    uint32_t r[13], pc, cpsr, pad;
    uint32_t banked[22], pad2[2];
    uint32_t sys[32], pad3[8];
    uint64_t fp[32];
    uint32_t fpscr, fpexc, pad4[2];
};
struct arm32_hyp_state {
    struct arm32_hyp_bank guest, host;
    uint64_t vttbr;
    uint32_t vtcr, hcr, hsr, hdfar, hifar, hpfar, kind, mpidr;
};
_Static_assert(sizeof(struct arm32_hyp_bank) == A32_BANK_SIZE, "HYP bank size");
_Static_assert(__builtin_offsetof(struct arm32_hyp_bank, fp) == A32_FP, "HYP FP offset");
_Static_assert(__builtin_offsetof(struct arm32_hyp_state, vttbr) == A32_VTTBR, "HYP metadata");
#endif
#endif
