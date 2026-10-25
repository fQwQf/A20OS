/* RV32 layout with uint64 public slots; pinned by C static assertions. */
#ifndef _HYP_ASM_OFFSETS_H
#define _HYP_ASM_OFFSETS_H

#define HYP_ASM_VCPU_REGS_OFF  16
#define HYP_ASM_VCPU_PC_OFF    272
#define HYP_ASM_VCPU_ARCH_OFF  280
#define HYP_ASM_VCPU_TRAMP_OFF 472
#define HYP_ASM_VCPU_FPU_OFF   888
#define HYP_ASM_VCPU_FCSR_OFF  1144

#define HYP_ASM_TRAMP_SLOTS    35
#define HYP_ASM_TRAMP_HOST_SP  0
#define HYP_ASM_TRAMP_HOST_TP  1
#define HYP_ASM_TRAMP_GUEST_SP 2
#define HYP_ASM_TRAMP_X0       3

#endif
