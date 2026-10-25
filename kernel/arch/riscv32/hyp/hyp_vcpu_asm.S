/* RV32 H entry and exit. Public uint64 register slots remain eight bytes;
 * every architectural GPR is stored zero extended. No guest stack access
 * occurs before every register has been banked in host memory. */
#include "hyp_asm_offsets.h"
.altmacro
.macro BANK n
 sw x\n, (3+\n)*8(sp)
 sw zero, (3+\n)*8+4(sp)
.endm
.macro SAVE_GP n
 sw x\n, \n*4(sp)
.endm
.macro LOAD_GP n
 lw x\n, \n*4(sp)
.endm
.macro SAVE_FP n
 fsd f\n, 144+\n*8(sp)
.endm
.macro LOAD_FP n
 fld f\n, 144+\n*8(sp)
.endm
.section .text
.align 2
.globl hyp_guest_trap_entry
hyp_guest_trap_entry:
 csrci sstatus, 2
 csrrw sp, sscratch, sp
 BANK 1
 .set n,3
 .rept 29
 BANK %n
 .set n,n+1
 .endr
 csrr t0,sscratch
 sw t0,16(sp)
 sw zero,20(sp)
 csrw sscratch,sp
 lw tp,8(sp)
 lw sp,0(sp)
 addi sp,sp,-416
 .set n,1
 .rept 31
 SAVE_GP %n
 .set n,n+1
 .endr
 csrr t0,sstatus
 sw t0,128(sp)
 csrr t0,sepc
 sw t0,132(sp)
 frcsr t0
 sw t0,400(sp)
 .set n,0
 .rept 32
 SAVE_FP %n
 .set n,n+1
 .endr
 la t0,__trap_from_kernel
 csrw stvec,t0
 mv a0,sp
 call hyp_arch_guest_trap
 j .Lrv32_guest_return

.align 2
.globl hyp_arch_vcpu_enter
hyp_arch_vcpu_enter:
 sw ra,HYP_ASM_VCPU_ARCH_OFF+0*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+0*8+4(a0)
 sw sp,HYP_ASM_VCPU_ARCH_OFF+1*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+1*8+4(a0)
 sw gp,HYP_ASM_VCPU_ARCH_OFF+2*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+2*8+4(a0)
 sw tp,HYP_ASM_VCPU_ARCH_OFF+3*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+3*8+4(a0)
 sw t0,HYP_ASM_VCPU_ARCH_OFF+4*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+4*8+4(a0)
 sw t1,HYP_ASM_VCPU_ARCH_OFF+5*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+5*8+4(a0)
 sw t2,HYP_ASM_VCPU_ARCH_OFF+6*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+6*8+4(a0)
 sw t3,HYP_ASM_VCPU_ARCH_OFF+7*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+7*8+4(a0)
 sw t4,HYP_ASM_VCPU_ARCH_OFF+8*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+8*8+4(a0)
 sw t5,HYP_ASM_VCPU_ARCH_OFF+9*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+9*8+4(a0)
 sw t6,HYP_ASM_VCPU_ARCH_OFF+10*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+10*8+4(a0)
 sw s0,HYP_ASM_VCPU_ARCH_OFF+11*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+11*8+4(a0)
 sw s1,HYP_ASM_VCPU_ARCH_OFF+12*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+12*8+4(a0)
 sw s2,HYP_ASM_VCPU_ARCH_OFF+13*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+13*8+4(a0)
 sw s3,HYP_ASM_VCPU_ARCH_OFF+14*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+14*8+4(a0)
 sw s4,HYP_ASM_VCPU_ARCH_OFF+15*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+15*8+4(a0)
 sw s5,HYP_ASM_VCPU_ARCH_OFF+16*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+16*8+4(a0)
 sw s6,HYP_ASM_VCPU_ARCH_OFF+17*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+17*8+4(a0)
 sw s7,HYP_ASM_VCPU_ARCH_OFF+18*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+18*8+4(a0)
 sw s8,HYP_ASM_VCPU_ARCH_OFF+19*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+19*8+4(a0)
 sw s9,HYP_ASM_VCPU_ARCH_OFF+20*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+20*8+4(a0)
 sw s10,HYP_ASM_VCPU_ARCH_OFF+21*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+21*8+4(a0)
 sw s11,HYP_ASM_VCPU_ARCH_OFF+22*8(a0)
 sw zero,HYP_ASM_VCPU_ARCH_OFF+22*8+4(a0)
 addi t0,a0,HYP_ASM_VCPU_TRAMP_OFF
 sw sp,0(t0)
 sw tp,8(t0)
 csrw sscratch,t0
 addi sp,sp,-416
 addi t0,a0,HYP_ASM_VCPU_REGS_OFF
 mv t1,sp
 li t2,32
.Lrv32_copy:
 lw t3,0(t0)
 sw t3,0(t1)
 addi t0,t0,8
 addi t1,t1,4
 addi t2,t2,-1
 bnez t2,.Lrv32_copy
 lw t0,HYP_ASM_VCPU_PC_OFF(a0)
 sw t0,132(sp)
 li t0,0x2020
 lw t1,HYP_ASM_VCPU_ARCH_OFF+23*8(a0)
 bnez t1,.Lrv32_vu
 ori t0,t0,256
.Lrv32_vu:
 sw t0,128(sp)
 li t0,0x2000
 csrs sstatus,t0
 fld f0,HYP_ASM_VCPU_FPU_OFF+0*8(a0)
 fsd f0,144+0*8(sp)
 fld f1,HYP_ASM_VCPU_FPU_OFF+1*8(a0)
 fsd f1,144+1*8(sp)
 fld f2,HYP_ASM_VCPU_FPU_OFF+2*8(a0)
 fsd f2,144+2*8(sp)
 fld f3,HYP_ASM_VCPU_FPU_OFF+3*8(a0)
 fsd f3,144+3*8(sp)
 fld f4,HYP_ASM_VCPU_FPU_OFF+4*8(a0)
 fsd f4,144+4*8(sp)
 fld f5,HYP_ASM_VCPU_FPU_OFF+5*8(a0)
 fsd f5,144+5*8(sp)
 fld f6,HYP_ASM_VCPU_FPU_OFF+6*8(a0)
 fsd f6,144+6*8(sp)
 fld f7,HYP_ASM_VCPU_FPU_OFF+7*8(a0)
 fsd f7,144+7*8(sp)
 fld f8,HYP_ASM_VCPU_FPU_OFF+8*8(a0)
 fsd f8,144+8*8(sp)
 fld f9,HYP_ASM_VCPU_FPU_OFF+9*8(a0)
 fsd f9,144+9*8(sp)
 fld f10,HYP_ASM_VCPU_FPU_OFF+10*8(a0)
 fsd f10,144+10*8(sp)
 fld f11,HYP_ASM_VCPU_FPU_OFF+11*8(a0)
 fsd f11,144+11*8(sp)
 fld f12,HYP_ASM_VCPU_FPU_OFF+12*8(a0)
 fsd f12,144+12*8(sp)
 fld f13,HYP_ASM_VCPU_FPU_OFF+13*8(a0)
 fsd f13,144+13*8(sp)
 fld f14,HYP_ASM_VCPU_FPU_OFF+14*8(a0)
 fsd f14,144+14*8(sp)
 fld f15,HYP_ASM_VCPU_FPU_OFF+15*8(a0)
 fsd f15,144+15*8(sp)
 fld f16,HYP_ASM_VCPU_FPU_OFF+16*8(a0)
 fsd f16,144+16*8(sp)
 fld f17,HYP_ASM_VCPU_FPU_OFF+17*8(a0)
 fsd f17,144+17*8(sp)
 fld f18,HYP_ASM_VCPU_FPU_OFF+18*8(a0)
 fsd f18,144+18*8(sp)
 fld f19,HYP_ASM_VCPU_FPU_OFF+19*8(a0)
 fsd f19,144+19*8(sp)
 fld f20,HYP_ASM_VCPU_FPU_OFF+20*8(a0)
 fsd f20,144+20*8(sp)
 fld f21,HYP_ASM_VCPU_FPU_OFF+21*8(a0)
 fsd f21,144+21*8(sp)
 fld f22,HYP_ASM_VCPU_FPU_OFF+22*8(a0)
 fsd f22,144+22*8(sp)
 fld f23,HYP_ASM_VCPU_FPU_OFF+23*8(a0)
 fsd f23,144+23*8(sp)
 fld f24,HYP_ASM_VCPU_FPU_OFF+24*8(a0)
 fsd f24,144+24*8(sp)
 fld f25,HYP_ASM_VCPU_FPU_OFF+25*8(a0)
 fsd f25,144+25*8(sp)
 fld f26,HYP_ASM_VCPU_FPU_OFF+26*8(a0)
 fsd f26,144+26*8(sp)
 fld f27,HYP_ASM_VCPU_FPU_OFF+27*8(a0)
 fsd f27,144+27*8(sp)
 fld f28,HYP_ASM_VCPU_FPU_OFF+28*8(a0)
 fsd f28,144+28*8(sp)
 fld f29,HYP_ASM_VCPU_FPU_OFF+29*8(a0)
 fsd f29,144+29*8(sp)
 fld f30,HYP_ASM_VCPU_FPU_OFF+30*8(a0)
 fsd f30,144+30*8(sp)
 fld f31,HYP_ASM_VCPU_FPU_OFF+31*8(a0)
 fsd f31,144+31*8(sp)
 lw t0,HYP_ASM_VCPU_FCSR_OFF(a0)
 sw t0,400(sp)
 csrr t0,hstatus
 li t1,0x300180
 or t0,t0,t1
 li t1,512
 not t1,t1
 and t0,t0,t1
 csrw hstatus,t0
 la t0,hyp_guest_trap_entry
 csrw stvec,t0
.Lrv32_guest_return:
 lw t0,400(sp)
 fscsr t0
 .set n,0
 .rept 32
 LOAD_FP %n
 .set n,n+1
 .endr
 lw t0,132(sp)
 csrw sepc,t0
 lw t0,128(sp)
 csrw sstatus,t0
 lw x1,4(sp)
 .set n,3
 .rept 29
 LOAD_GP %n
 .set n,n+1
 .endr
 lw sp,8(sp)
 sret

.align 2
.globl hyp_arch_vcpu_exit
hyp_arch_vcpu_exit:
 csrr t0,hstatus
 li t1,0x300380
 not t1,t1
 and t0,t0,t1
 csrw hstatus,t0
 la t0,__trap_from_kernel
 csrw stvec,t0
 lw ra,HYP_ASM_VCPU_ARCH_OFF+0*8(a0)
 lw sp,HYP_ASM_VCPU_ARCH_OFF+1*8(a0)
 lw gp,HYP_ASM_VCPU_ARCH_OFF+2*8(a0)
 lw tp,HYP_ASM_VCPU_ARCH_OFF+3*8(a0)
 lw t0,HYP_ASM_VCPU_ARCH_OFF+4*8(a0)
 lw t1,HYP_ASM_VCPU_ARCH_OFF+5*8(a0)
 lw t2,HYP_ASM_VCPU_ARCH_OFF+6*8(a0)
 lw t3,HYP_ASM_VCPU_ARCH_OFF+7*8(a0)
 lw t4,HYP_ASM_VCPU_ARCH_OFF+8*8(a0)
 lw t5,HYP_ASM_VCPU_ARCH_OFF+9*8(a0)
 lw t6,HYP_ASM_VCPU_ARCH_OFF+10*8(a0)
 lw s0,HYP_ASM_VCPU_ARCH_OFF+11*8(a0)
 lw s1,HYP_ASM_VCPU_ARCH_OFF+12*8(a0)
 lw s2,HYP_ASM_VCPU_ARCH_OFF+13*8(a0)
 lw s3,HYP_ASM_VCPU_ARCH_OFF+14*8(a0)
 lw s4,HYP_ASM_VCPU_ARCH_OFF+15*8(a0)
 lw s5,HYP_ASM_VCPU_ARCH_OFF+16*8(a0)
 lw s6,HYP_ASM_VCPU_ARCH_OFF+17*8(a0)
 lw s7,HYP_ASM_VCPU_ARCH_OFF+18*8(a0)
 lw s8,HYP_ASM_VCPU_ARCH_OFF+19*8(a0)
 lw s9,HYP_ASM_VCPU_ARCH_OFF+20*8(a0)
 lw s10,HYP_ASM_VCPU_ARCH_OFF+21*8(a0)
 lw s11,HYP_ASM_VCPU_ARCH_OFF+22*8(a0)
 ret
