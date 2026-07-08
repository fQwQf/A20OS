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

#define PPC64_SLB_ESID_V            0x08000000
#define PPC64_SLB_VSID_KP           0x00000400
#define PPC64_SLB_VSID_L            0x00000100
#define PPC64_SLB_VSID_LP           0x00000030
#define PPC64_SLB_VSID_LP_01        0x00000010

#define PPC64_HPTE_V_VALID          0x1
#define PPC64_HPTE_V_LARGE          0x4

#define PPC64_HPTE_R_R              0x100
#define PPC64_HPTE_R_C              0x080
#define PPC64_HPTE_R_M              0x010
#define PPC64_HPTE_R_PP             0x002

#define PPC64_H_ENTER               0x08
#define PPC64_H_RESIZE_HPT_PREPARE  0x36C
#define PPC64_H_RESIZE_HPT_COMMIT   0x370
#define PPC64_H_SUCCESS             0
#define PPC64_H_LONG_BUSY_ORDER_1_MSEC   0x10
#define PPC64_H_LONG_BUSY_ORDER_10_MSEC  0x11
#define PPC64_H_LONG_BUSY_ORDER_100_MSEC 0x12
#define PPC64_H_LONG_BUSY_ORDER_1_SEC    0x13
#define PPC64_H_LONG_BUSY_ORDER_10_SEC   0x14
#define PPC64_H_LONG_BUSY_ORDER_100_SEC  0x15

#endif
