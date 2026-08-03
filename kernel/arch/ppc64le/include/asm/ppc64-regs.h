#ifndef _ASM_PPC64_REGS_H
#define _ASM_PPC64_REGS_H

#define PPC64_MSR_SF    (1UL << 63)
#define PPC64_MSR_ISF   (1UL << 61)
#define PPC64_MSR_HV    (1UL << 60)
#define PPC64_MSR_VEC   (1UL << 25)
#define PPC64_MSR_VSX   (1UL << 24)
#define PPC64_MSR_EE    (1UL << 15)
#define PPC64_MSR_PR    (1UL << 14)
#define PPC64_MSR_FP    (1UL << 13)
#define PPC64_MSR_ME    (1UL << 12)
#define PPC64_MSR_FE0   (1UL << 11)
#define PPC64_MSR_SE    (1UL << 10)
#define PPC64_MSR_BE    (1UL << 9)
#define PPC64_MSR_FE1   (1UL << 8)
#define PPC64_MSR_IR    (1UL << 5)
#define PPC64_MSR_DR    (1UL << 4)
#define PPC64_MSR_PMM   (1UL << 2)
#define PPC64_MSR_RI    (1UL << 1)
#define PPC64_MSR_LE    (1UL << 0)

#define PPC64_H_SET_MODE            0x31C
#define PPC64_H_SET_MODE_LE         1
#define PPC64_H_SET_MODE_RESOURCE_LE 4
#define PPC64_H_REGISTER_PROC_TBL    0x37C
#define PPC64_PROC_TABLE_NEW         0x18
#define PPC64_PROC_TABLE_RADIX       0x04
#define PPC64_H_SUCCESS             0
#define PPC64_TRAP_SCRATCH_PA        0x1000

#define PPC64_SPR_PID                0x030
#define PPC64_SPR_PIR                0x11e

#endif
