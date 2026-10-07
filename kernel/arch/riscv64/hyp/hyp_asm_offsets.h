#ifndef _HYP_ASM_OFFSETS_H
#define _HYP_ASM_OFFSETS_H

/*
 * hyp_vcpu_t field offsets that hyp_vcpu_asm.S hardcodes.  The assembler
 * cannot lay out a C struct, so the numbers live here and the C half pins
 * every one of them with _Static_assert(offsetof() ... == ...): a struct
 * change that silently moved regs/pc/arch/tramp would corrupt the guest
 * register image at runtime instead of failing the build.  Included by both
 * halves.
 *
 * Derived from hyp_vcpu_t in kernel/include/hyp/hyp_vcpu.h:
 *   magic 0, running 4, vm 8, refcount 16, (4 pad) -> regs 24 (256 bytes)
 *   -> pc 280 -> arch 288 (24 slots) -> tramp 480.
 *
 * tramp[]'s slot indices are here too, not only the field offset: the
 * prelude writes them through sp (which IS &tramp[0] for the duration) and
 * hyp_arch.c reads the same slots back, so both halves must agree on one set
 * of numbers.  HYP_ASM_TRAMP_X0 is the base for "guest x[n]".
 */
#define HYP_ASM_VCPU_REGS_OFF  24
#define HYP_ASM_VCPU_PC_OFF    280
#define HYP_ASM_VCPU_ARCH_OFF  288
#define HYP_ASM_VCPU_TRAMP_OFF 480

#define HYP_ASM_TRAMP_SLOTS    35
#define HYP_ASM_TRAMP_HOST_SP  0
#define HYP_ASM_TRAMP_HOST_TP  1
#define HYP_ASM_TRAMP_GUEST_SP 2
#define HYP_ASM_TRAMP_X0       3   /* guest x[n] == tramp[HYP_ASM_TRAMP_X0 + n] */

#endif /* _HYP_ASM_OFFSETS_H */