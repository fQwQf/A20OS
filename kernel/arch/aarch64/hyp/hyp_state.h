#ifndef A20_AARCH64_HYP_STATE_H
#define A20_AARCH64_HYP_STATE_H
/* Shared with the physical, MMU-off EL2 monitor. Keep state in one page. */
#define A64_HYP_BANK_SIZE 1040
#define A64_HYP_PC 248
#define A64_HYP_PSTATE 256
#define A64_HYP_SP0 264
#define A64_HYP_SP1 272
#define A64_HYP_SYS 280
#define A64_HYP_SIMD 512
#define A64_HYP_FPCR 1024
#define A64_HYP_FPSR 1032
#define A64_HYP_HOST 1040
#define A64_HYP_VTTBR 2080
#define A64_HYP_VTCR 2088
#define A64_HYP_ESR 2096
#define A64_HYP_FAR 2104
#define A64_HYP_HPFAR 2112
#define A64_HYP_KIND 2120
#define A64_HYP_HCR 2128
#define A64_HYP_MPIDR 2136
#ifndef __ASSEMBLER__
#include "core/types.h"
struct aarch64_hyp_bank {
    uint64_t x[31], pc, pstate, sp0, sp1;
    uint64_t sys[29];
    uint64_t simd[32][2];
    uint64_t fpcr, fpsr;
};
struct aarch64_hyp_state {
    struct aarch64_hyp_bank guest, host;
    uint64_t vttbr, vtcr, esr, far, hpfar, kind, hcr, mpidr;
};
_Static_assert(sizeof(struct aarch64_hyp_bank) == A64_HYP_BANK_SIZE, "EL2 bank size");
_Static_assert(__builtin_offsetof(struct aarch64_hyp_bank, simd) == A64_HYP_SIMD, "EL2 SIMD bank");
_Static_assert(__builtin_offsetof(struct aarch64_hyp_state, vttbr) == A64_HYP_VTTBR, "EL2 metadata");
_Static_assert(sizeof(struct aarch64_hyp_state) < 4096, "EL2 state must fit one frame");
#endif
#endif
