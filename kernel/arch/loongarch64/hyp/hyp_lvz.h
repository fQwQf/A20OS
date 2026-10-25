#ifndef A20_HYP_LVZ_H
#define A20_HYP_LVZ_H
#define LVZ_GPR 0
#define LVZ_HOST 256
#define LVZ_ROOT 512
#define LVZ_PC 768
#define LVZ_ESTAT 776
#define LVZ_BADV 784
#define LVZ_BADI 792
#define LVZ_S2_ROOT 800
#define LVZ_GCFG 808
#define LVZ_GINTC 816
#ifndef __ASSEMBLER__
#include "core/types.h"
typedef struct hyp_lvz_state {
    uint64_t gpr[32], host[32], root[32];
    uint64_t pc, estat, badv, badi, s2_root, gcfg, gintc;
    uint64_t gcsr[0x184];
} hyp_lvz_state_t;
#define LVZ_OFFSET(field, off) _Static_assert(__builtin_offsetof(hyp_lvz_state_t, field) == off, "LVZ " #field " offset")
LVZ_OFFSET(gpr, LVZ_GPR); LVZ_OFFSET(host, LVZ_HOST); LVZ_OFFSET(root, LVZ_ROOT);
LVZ_OFFSET(pc, LVZ_PC); LVZ_OFFSET(estat, LVZ_ESTAT); LVZ_OFFSET(badv, LVZ_BADV);
LVZ_OFFSET(badi, LVZ_BADI); LVZ_OFFSET(s2_root, LVZ_S2_ROOT);
LVZ_OFFSET(gcfg, LVZ_GCFG); LVZ_OFFSET(gintc, LVZ_GINTC);
void hyp_lvz_switch(hyp_lvz_state_t *state);
#endif
#endif
