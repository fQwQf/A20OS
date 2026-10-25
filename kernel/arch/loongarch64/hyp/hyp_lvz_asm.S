/* Experimental LVZ switch. No floating/vector instruction executes here.
 * Root EUEN=0 makes guest FPU/LSX/LASX/LBT attempts trap before host data is
 * accessed. Guest exceptions first bank GPRs through root KS6/KS7, never
 * through guest sp/tp. Host CSRs and ABI callee-saved registers are restored
 * before returning to C; all C code therefore sees the host PGDL/vector. */
#include "hyp_lvz.h"
.text
.p2align 6
.globl hyp_lvz_switch
.type hyp_lvz_switch,@function
hyp_lvz_switch:
 st.d $r1, $a0, LVZ_HOST+1*8
 st.d $r2, $a0, LVZ_HOST+2*8
 st.d $r3, $a0, LVZ_HOST+3*8
 st.d $r21, $a0, LVZ_HOST+21*8
 st.d $r22, $a0, LVZ_HOST+22*8
 st.d $r23, $a0, LVZ_HOST+23*8
 st.d $r24, $a0, LVZ_HOST+24*8
 st.d $r25, $a0, LVZ_HOST+25*8
 st.d $r26, $a0, LVZ_HOST+26*8
 st.d $r27, $a0, LVZ_HOST+27*8
 st.d $r28, $a0, LVZ_HOST+28*8
 st.d $r29, $a0, LVZ_HOST+29*8
 st.d $r30, $a0, LVZ_HOST+30*8
 st.d $r31, $a0, LVZ_HOST+31*8
 csrrd $t0, 0
 st.d $t0, $a0, LVZ_ROOT+0*8
 csrrd $t0, 1
 st.d $t0, $a0, LVZ_ROOT+1*8
 csrrd $t0, 2
 st.d $t0, $a0, LVZ_ROOT+2*8
 csrrd $t0, 4
 st.d $t0, $a0, LVZ_ROOT+3*8
 csrrd $t0, 6
 st.d $t0, $a0, LVZ_ROOT+4*8
 csrrd $t0, 12
 st.d $t0, $a0, LVZ_ROOT+5*8
 csrrd $t0, 25
 st.d $t0, $a0, LVZ_ROOT+6*8
 csrrd $t0, 80
 st.d $t0, $a0, LVZ_ROOT+7*8
 csrrd $t0, 81
 st.d $t0, $a0, LVZ_ROOT+8*8
 csrrd $t0, 21
 st.d $t0, $a0, LVZ_ROOT+9*8
 csrrd $t0, 82
 st.d $t0, $a0, LVZ_ROOT+10*8
 csrrd $t0, 83
 st.d $t0, $a0, LVZ_ROOT+11*8
 csrrd $t0, 54
 st.d $t0, $a0, LVZ_ROOT+12*8
 csrrd $t0, 55
 st.d $t0, $a0, LVZ_ROOT+13*8
 csrrd $t0, 96
 st.d $t0, $a0, LVZ_ROOT+14*8
 csrwr $a0, 0x36
 csrrd $a0, 0x36
 ld.d $t0, $a0, LVZ_ROOT+3*8
 li.d $t1, ~(7 << 16)
 and $t0, $t0, $t1
 csrwr $t0, 0x4
 la.local $t0, hyp_lvz_exception
 csrwr $t0, 0xc
 ld.d $t0, $a0, LVZ_S2_ROOT
 csrwr $t0, 0x19
 ld.d $t0, $a0, LVZ_GCFG
 csrwr $t0, 0x51
 ld.d $t0, $a0, LVZ_GINTC
 csrwr $t0, 0x52
 csrwr $zero, 0x53
 li.d $t0, (1 << 16)
 csrwr $t0, 0x50
 ld.d $t0, $a0, LVZ_ROOT+9*8
 li.d $t1, ~((255 << 16) | (1 << 13))
 and $t0, $t0, $t1
 li.d $t1, (1 << 16) | (1 << 12)
 or $t0, $t0, $t1
 csrwr $t0, 0x15
 invtlb 0x12, $zero, $zero
 dbar 0
 ibar 0
 ld.d $t0, $a0, LVZ_PC
 csrwr $t0, 0x6
 li.d $t0, 4
 csrwr $t0, 0x1
 li.d $t0, 2
 li.d $t1, 2
 csrxchg $t0, $t1, 0x50
 li.d $t0, 2
 csrwr $t0, 0x60
 csrwr $zero, 0x2
 ld.d $r1, $a0, LVZ_GPR+1*8
 ld.d $r2, $a0, LVZ_GPR+2*8
 ld.d $r3, $a0, LVZ_GPR+3*8
 ld.d $r5, $a0, LVZ_GPR+5*8
 ld.d $r6, $a0, LVZ_GPR+6*8
 ld.d $r7, $a0, LVZ_GPR+7*8
 ld.d $r8, $a0, LVZ_GPR+8*8
 ld.d $r9, $a0, LVZ_GPR+9*8
 ld.d $r10, $a0, LVZ_GPR+10*8
 ld.d $r11, $a0, LVZ_GPR+11*8
 ld.d $r12, $a0, LVZ_GPR+12*8
 ld.d $r13, $a0, LVZ_GPR+13*8
 ld.d $r14, $a0, LVZ_GPR+14*8
 ld.d $r15, $a0, LVZ_GPR+15*8
 ld.d $r16, $a0, LVZ_GPR+16*8
 ld.d $r17, $a0, LVZ_GPR+17*8
 ld.d $r18, $a0, LVZ_GPR+18*8
 ld.d $r19, $a0, LVZ_GPR+19*8
 ld.d $r20, $a0, LVZ_GPR+20*8
 ld.d $r21, $a0, LVZ_GPR+21*8
 ld.d $r22, $a0, LVZ_GPR+22*8
 ld.d $r23, $a0, LVZ_GPR+23*8
 ld.d $r24, $a0, LVZ_GPR+24*8
 ld.d $r25, $a0, LVZ_GPR+25*8
 ld.d $r26, $a0, LVZ_GPR+26*8
 ld.d $r27, $a0, LVZ_GPR+27*8
 ld.d $r28, $a0, LVZ_GPR+28*8
 ld.d $r29, $a0, LVZ_GPR+29*8
 ld.d $r30, $a0, LVZ_GPR+30*8
 ld.d $r31, $a0, LVZ_GPR+31*8
 ld.d $a0, $a0, LVZ_GPR+4*8
 ertn
.size hyp_lvz_switch,.-hyp_lvz_switch

/* All exceptions use EENTRY with ECFG.VS=0. */
.p2align 12
.globl hyp_lvz_exception
.type hyp_lvz_exception,@function
hyp_lvz_exception:
 csrwr $a0, 0x37
 csrrd $a0, 0x36
 st.d $r1, $a0, LVZ_GPR+1*8
 st.d $r2, $a0, LVZ_GPR+2*8
 st.d $r3, $a0, LVZ_GPR+3*8
 st.d $r5, $a0, LVZ_GPR+5*8
 st.d $r6, $a0, LVZ_GPR+6*8
 st.d $r7, $a0, LVZ_GPR+7*8
 st.d $r8, $a0, LVZ_GPR+8*8
 st.d $r9, $a0, LVZ_GPR+9*8
 st.d $r10, $a0, LVZ_GPR+10*8
 st.d $r11, $a0, LVZ_GPR+11*8
 st.d $r12, $a0, LVZ_GPR+12*8
 st.d $r13, $a0, LVZ_GPR+13*8
 st.d $r14, $a0, LVZ_GPR+14*8
 st.d $r15, $a0, LVZ_GPR+15*8
 st.d $r16, $a0, LVZ_GPR+16*8
 st.d $r17, $a0, LVZ_GPR+17*8
 st.d $r18, $a0, LVZ_GPR+18*8
 st.d $r19, $a0, LVZ_GPR+19*8
 st.d $r20, $a0, LVZ_GPR+20*8
 st.d $r21, $a0, LVZ_GPR+21*8
 st.d $r22, $a0, LVZ_GPR+22*8
 st.d $r23, $a0, LVZ_GPR+23*8
 st.d $r24, $a0, LVZ_GPR+24*8
 st.d $r25, $a0, LVZ_GPR+25*8
 st.d $r26, $a0, LVZ_GPR+26*8
 st.d $r27, $a0, LVZ_GPR+27*8
 st.d $r28, $a0, LVZ_GPR+28*8
 st.d $r29, $a0, LVZ_GPR+29*8
 st.d $r30, $a0, LVZ_GPR+30*8
 st.d $r31, $a0, LVZ_GPR+31*8
 csrrd $t0, 0x37
 st.d $t0, $a0, LVZ_GPR+4*8
 csrrd $t0, 0x6
 st.d $t0, $a0, LVZ_PC
 csrrd $t0, 0x5
 st.d $t0, $a0, LVZ_ESTAT
 csrrd $t0, 0x7
 st.d $t0, $a0, LVZ_BADV
 csrrd $t0, 0x8
 st.d $t0, $a0, LVZ_BADI
 csrrd $t0, 0x52
 st.d $t0, $a0, LVZ_GINTC
 invtlb 0x12, $zero, $zero
 ld.d $t0, $a0, LVZ_ROOT+1*8
 csrwr $t0, 1
 ld.d $t0, $a0, LVZ_ROOT+2*8
 csrwr $t0, 2
 ld.d $t0, $a0, LVZ_ROOT+3*8
 csrwr $t0, 4
 ld.d $t0, $a0, LVZ_ROOT+4*8
 csrwr $t0, 6
 ld.d $t0, $a0, LVZ_ROOT+5*8
 csrwr $t0, 12
 ld.d $t0, $a0, LVZ_ROOT+6*8
 csrwr $t0, 25
 ld.d $t0, $a0, LVZ_ROOT+7*8
 csrwr $t0, 80
 ld.d $t0, $a0, LVZ_ROOT+8*8
 csrwr $t0, 81
 ld.d $t0, $a0, LVZ_ROOT+9*8
 csrwr $t0, 21
 ld.d $t0, $a0, LVZ_ROOT+10*8
 csrwr $t0, 82
 ld.d $t0, $a0, LVZ_ROOT+11*8
 csrwr $t0, 83
 ld.d $t0, $a0, LVZ_ROOT+14*8
 csrwr $t0, 96
 ld.d $t0, $a0, LVZ_ROOT+13*8
 csrwr $t0, 55
 ld.d $t0, $a0, LVZ_ROOT+12*8
 csrwr $t0, 54
 ld.d $t0, $a0, LVZ_ROOT+0*8
 csrwr $t0, 0
 ld.d $r1, $a0, LVZ_HOST+1*8
 ld.d $r2, $a0, LVZ_HOST+2*8
 ld.d $r3, $a0, LVZ_HOST+3*8
 ld.d $r21, $a0, LVZ_HOST+21*8
 ld.d $r22, $a0, LVZ_HOST+22*8
 ld.d $r23, $a0, LVZ_HOST+23*8
 ld.d $r24, $a0, LVZ_HOST+24*8
 ld.d $r25, $a0, LVZ_HOST+25*8
 ld.d $r26, $a0, LVZ_HOST+26*8
 ld.d $r27, $a0, LVZ_HOST+27*8
 ld.d $r28, $a0, LVZ_HOST+28*8
 ld.d $r29, $a0, LVZ_HOST+29*8
 ld.d $r30, $a0, LVZ_HOST+30*8
 ld.d $r31, $a0, LVZ_HOST+31*8
 jirl $zero, $ra, 0
.size hyp_lvz_exception,.-hyp_lvz_exception
