/* Physical, MMU-off ARMv7 HYP monitor. No host virtual pointer is dereferenced.
 * One HYP stack per CPU; r0-r12 are captured before using any scratch register. */
#include "hyp_state.h"
.syntax unified
.arch armv7-a
.arch_extension virt
.fpu neon
.section .text.entry
.align 5
.global arm32_hyp_vectors
arm32_hyp_vectors:
    b .Lbad
    b .Lbad
    b .Lbad
    b .Lbad
    b .Lbad
    b .Lsync
    b .Lirq
    b .Lbad

/* Boot entry: caller is still at a physical PC. Return in SVC with DAIF set. */
.global arm32_hyp_boot
arm32_hyp_boot:
    mrs r3,cpsr
    and r3,r3,#31
    cmp r3,#0x1a
    bxne lr
    mrc p15,0,r3,c0,c0,5
    and r3,r3,#255
    cmp r3,#CONFIG_NR_CPUS
    bhs .Lpark
    ldr r0,=arm32_hyp_boot_cpus
    mov r1,#1
    str r1,[r0,r3,lsl #2]
    ldr sp,=arm32_hyp_stacks
    add sp,sp,r3,lsl #12
    add sp,sp,#4096
    sub sp,sp,#16
    mov r0,#0
    str r0,[sp]
    mcr p15,4,r0,c1,c1,0 /* HCR: host physical IRQ routing normal */
    mcr p15,4,r0,c1,c1,2 /* HCPTR */
    mcr p15,4,r0,c1,c1,3 /* HSTR */
    mcr p15,4,r0,c1,c1,1 /* HDCR */
    ldr r1,=arm32_hyp_vectors
    mcr p15,4,r1,c12,c0,0
    mov r1,#3
    mcr p15,4,r1,c14,c1,0 /* CNTHCTL: PL1 physical timer access */
    mcrr p15,4,r0,r0,c14 /* CNTVOFF */
    mov r0,#0xd3
    msr spsr_cxsf,r0
    msr elr_hyp,lr
    isb
    eret
.Lpark:
    wfi
    b .Lpark

.Lsync:
    sub sp,sp,#64
    stmia sp,{r0-r12}
    str lr,[sp,#60]
    mrs r0,elr_hyp
    str r0,[sp,#52]
    mrs r0,spsr
    str r0,[sp,#56]
    ldr r4,[sp,#64]
    cmp r4,#0
    bne .Lguest_exit
    /* Only the private HVC immediate may operate the monitor. */
    mrc p15,4,r5,c5,c2,0
    ldr r6,=0xfc00ffff
    and r5,r5,r6
    ldr r6,=0x48000a20
    cmp r5,r6
    bne .Lreject
    ldr r0,[sp]
    cmp r0,#2
    beq .Lflush_return
    cmp r0,#1
    bne .Lreject
    ldr r4,[sp,#4]
    cmp r4,#0
    beq .Lreject
    str r4,[sp,#64]
    add r4,r4,#A32_HOST
    mov r5,sp
    mov r6,r4
    mov r7,#15
.Lhost_gpr:
    ldr r8,[r5],#4
    str r8,[r6],#4
    subs r7,r7,#1
    bne .Lhost_gpr
    bl .Lsave_bank
    sub r4,r4,#A32_HOST
    ldr r5,[r4,#A32_VTCR]
    mcr p15,4,r5,c2,c1,2
    add r6,r4,#A32_VTTBR
    ldrd r0,r1,[r6]
    mcrr p15,6,r0,r1,c2
    mov r5,#0
    mcr p15,4,r5,c14,c1,0 /* guest cannot alter physical timer */
    ldr r5,[r4,#A32_MPIDR]
    mcr p15,4,r5,c0,c0,5
    ldr r5,=0xe40
    mcr p15,4,r5,c1,c1,1 /* trap guest PMU accesses */
    ldr r5,=((1<<9)|(1<<15))
    mcr p15,4,r5,c1,c1,3
    mov r5,#0x4000
    mcr p15,4,r5,c1,c1,2 /* trap guest CP14 debug coprocessor */
    ldr r5,[r4,#A32_HCR]
    mcr p15,4,r5,c1,c1,0
    bl .Lload_bank
    clrex
    dsb sy
    mov r5,#0
    mcr p15,4,r5,c8,c7,4 /* TLBIALLNSNH: flush guest S1+S2 */
    dsb sy
    isb
    ldr r5,[r4,#A32_PC]
    msr elr_hyp,r5
    ldr r5,[r4,#A32_CPSR]
    msr spsr_cxsf,r5
    ldr r5,[r4,#68]
    mov lr,r5
    ldmia r4,{r0-r12}
    add sp,sp,#64
    eret
.Lirq:
    sub sp,sp,#64
    stmia sp,{r0-r12}
    str lr,[sp,#60]
    mov r0,#1
    b .Lasync
.Lbad:
    sub sp,sp,#64
    stmia sp,{r0-r12}
    str lr,[sp,#60]
    mov r0,#2
.Lasync:
    ldr r4,[sp,#64]
    cmp r4,#0
    beq .Lpark
    str r0,[r4,#A32_KIND]
    mrs r0,elr_hyp
    str r0,[sp,#52]
    mrs r0,spsr
    str r0,[sp,#56]
    b .Lcapture
.Lguest_exit:
    mov r0,#0
    str r0,[r4,#A32_KIND]
.Lcapture:
    mov r5,sp
    mov r6,r4
    mov r7,#15
.Lguest_gpr:
    ldr r8,[r5],#4
    str r8,[r6],#4
    subs r7,r7,#1
    bne .Lguest_gpr
    bl .Lsave_bank
    mrc p15,4,r5,c5,c2,0
    str r5,[r4,#A32_HSR]
    mrc p15,4,r5,c6,c0,0
    str r5,[r4,#A32_HDFAR]
    mrc p15,4,r5,c6,c0,2
    str r5,[r4,#A32_HIFAR]
    mrc p15,4,r5,c6,c0,4
    str r5,[r4,#A32_HPFAR]
    mov r5,#0
    mcr p15,4,r5,c1,c1,0
    mcr p15,4,r5,c1,c1,2
    mcr p15,4,r5,c1,c1,1
    mcr p15,4,r5,c1,c1,3
    mcrr p15,6,r5,r5,c2
    mov r5,#3
    mcr p15,4,r5,c14,c1,0
    add r4,r4,#A32_HOST
    bl .Lload_bank
    clrex
    dsb sy
    mov r5,#0
    mcr p15,4,r5,c8,c7,4
    dsb sy
    isb
    ldr r5,[r4,#A32_PC]
    msr elr_hyp,r5
    ldr r5,[r4,#A32_CPSR]
    msr spsr_cxsf,r5
    mov r5,#0
    str r5,[sp,#64]
    ldr r5,[r4,#68]
    mov lr,r5
    ldmia r4,{r0-r12}
    mov r0,#0
    add sp,sp,#64
    eret
.Lflush_return:
    dsb sy
    mov r0,#0
    mcr p15,4,r0,c8,c7,4
    dsb sy
    isb
    b .Lreturn
.Lreject:
    mvn r0,#0
.Lreturn:
    str r0,[sp]
    ldr r0,[sp,#52]
    msr elr_hyp,r0
    ldr r0,[sp,#56]
    msr spsr_cxsf,r0
    ldr lr,[sp,#60]
    ldmia sp,{r0-r12}
    add sp,sp,#64
    eret

.Lsave_bank:
    mrs r5,sp_usr
    str r5,[r4,#64]
    ldr r5,[sp,#60]
    str r5,[r4,#68]
    mrs r5,sp_svc
    str r5,[r4,#72]
    mrs r5,lr_svc
    str r5,[r4,#76]
    mrs r5,spsr_svc
    str r5,[r4,#80]
    mrs r5,sp_abt
    str r5,[r4,#84]
    mrs r5,lr_abt
    str r5,[r4,#88]
    mrs r5,spsr_abt
    str r5,[r4,#92]
    mrs r5,sp_und
    str r5,[r4,#96]
    mrs r5,lr_und
    str r5,[r4,#100]
    mrs r5,spsr_und
    str r5,[r4,#104]
    mrs r5,sp_irq
    str r5,[r4,#108]
    mrs r5,lr_irq
    str r5,[r4,#112]
    mrs r5,spsr_irq
    str r5,[r4,#116]
    mrs r5,r8_fiq
    str r5,[r4,#120]
    mrs r5,r9_fiq
    str r5,[r4,#124]
    mrs r5,r10_fiq
    str r5,[r4,#128]
    mrs r5,r11_fiq
    str r5,[r4,#132]
    mrs r5,r12_fiq
    str r5,[r4,#136]
    mrs r5,sp_fiq
    str r5,[r4,#140]
    mrs r5,lr_fiq
    str r5,[r4,#144]
    mrs r5,spsr_fiq
    str r5,[r4,#148]
    mrc p15,0,r5,c1,c0,0
    str r5,[r4,#160]
    mrc p15,0,r5,c1,c0,1
    str r5,[r4,#164]
    mrc p15,0,r5,c1,c0,2
    str r5,[r4,#168]
    mrc p15,0,r5,c2,c0,2
    str r5,[r4,#172]
    mrc p15,0,r5,c3,c0,0
    str r5,[r4,#176]
    mrc p15,0,r5,c12,c0,0
    str r5,[r4,#180]
    mrc p15,0,r5,c13,c0,1
    str r5,[r4,#184]
    mrc p15,0,r5,c13,c0,2
    str r5,[r4,#188]
    mrc p15,0,r5,c13,c0,3
    str r5,[r4,#192]
    mrc p15,0,r5,c13,c0,4
    str r5,[r4,#196]
    mrc p15,2,r5,c0,c0,0
    str r5,[r4,#200]
    mrc p15,0,r5,c5,c0,0
    str r5,[r4,#204]
    mrc p15,0,r5,c5,c0,1
    str r5,[r4,#208]
    mrc p15,0,r5,c5,c1,0
    str r5,[r4,#212]
    mrc p15,0,r5,c5,c1,1
    str r5,[r4,#216]
    mrc p15,0,r5,c6,c0,0
    str r5,[r4,#220]
    mrc p15,0,r5,c6,c0,2
    str r5,[r4,#224]
    mrc p15,0,r5,c14,c1,0
    str r5,[r4,#228]
    mrc p15,0,r5,c14,c3,1
    str r5,[r4,#232]
    mrrc p15,0,r0,r1,c2
    add r6,r4,#240
    strd r0,r1,[r6]
    mrrc p15,1,r0,r1,c2
    add r6,r4,#248
    strd r0,r1,[r6]
    mrrc p15,0,r0,r1,c7
    add r6,r4,#256
    strd r0,r1,[r6]
    mrrc p15,3,r0,r1,c14
    add r6,r4,#264
    strd r0,r1,[r6]
    mrc p15,0,r5,c13,c0,0
    str r5,[r4,#236]
    mrc p15,0,r5,c10,c2,0
    str r5,[r4,#272]
    mrc p15,0,r5,c10,c2,1
    str r5,[r4,#276]
    mrc p15,0,r5,c10,c3,0
    str r5,[r4,#280]
    mrc p15,0,r5,c10,c3,1
    str r5,[r4,#284]
    ldr r5,=0xf00000
    mcr p15,0,r5,c1,c0,2
    isb
    vmrs r5,fpexc
    str r5,[r4,#A32_FPEXC]
    orr r5,r5,#0x40000000
    vmsr fpexc,r5
    vmrs r5,fpscr
    str r5,[r4,#A32_FPSCR]
    add r6,r4,#A32_FP
    vstmia r6!,{d0-d15}
    vstmia r6!,{d16-d31}
    bx lr

.Lload_bank:
    ldr r5,[r4,#64]
    msr sp_usr,r5
    ldr r5,[r4,#72]
    msr sp_svc,r5
    ldr r5,[r4,#76]
    msr lr_svc,r5
    ldr r5,[r4,#80]
    msr spsr_svc,r5
    ldr r5,[r4,#84]
    msr sp_abt,r5
    ldr r5,[r4,#88]
    msr lr_abt,r5
    ldr r5,[r4,#92]
    msr spsr_abt,r5
    ldr r5,[r4,#96]
    msr sp_und,r5
    ldr r5,[r4,#100]
    msr lr_und,r5
    ldr r5,[r4,#104]
    msr spsr_und,r5
    ldr r5,[r4,#108]
    msr sp_irq,r5
    ldr r5,[r4,#112]
    msr lr_irq,r5
    ldr r5,[r4,#116]
    msr spsr_irq,r5
    ldr r5,[r4,#120]
    msr r8_fiq,r5
    ldr r5,[r4,#124]
    msr r9_fiq,r5
    ldr r5,[r4,#128]
    msr r10_fiq,r5
    ldr r5,[r4,#132]
    msr r11_fiq,r5
    ldr r5,[r4,#136]
    msr r12_fiq,r5
    ldr r5,[r4,#140]
    msr sp_fiq,r5
    ldr r5,[r4,#144]
    msr lr_fiq,r5
    ldr r5,[r4,#148]
    msr spsr_fiq,r5
    ldr r5,[r4,#160]
    mcr p15,0,r5,c1,c0,0
    ldr r5,[r4,#164]
    mcr p15,0,r5,c1,c0,1
    ldr r5,[r4,#168]
    mcr p15,0,r5,c1,c0,2
    ldr r5,[r4,#172]
    mcr p15,0,r5,c2,c0,2
    ldr r5,[r4,#176]
    mcr p15,0,r5,c3,c0,0
    ldr r5,[r4,#180]
    mcr p15,0,r5,c12,c0,0
    ldr r5,[r4,#184]
    mcr p15,0,r5,c13,c0,1
    ldr r5,[r4,#188]
    mcr p15,0,r5,c13,c0,2
    ldr r5,[r4,#192]
    mcr p15,0,r5,c13,c0,3
    ldr r5,[r4,#196]
    mcr p15,0,r5,c13,c0,4
    ldr r5,[r4,#200]
    mcr p15,2,r5,c0,c0,0
    ldr r5,[r4,#204]
    mcr p15,0,r5,c5,c0,0
    ldr r5,[r4,#208]
    mcr p15,0,r5,c5,c0,1
    ldr r5,[r4,#212]
    mcr p15,0,r5,c5,c1,0
    ldr r5,[r4,#216]
    mcr p15,0,r5,c5,c1,1
    ldr r5,[r4,#220]
    mcr p15,0,r5,c6,c0,0
    ldr r5,[r4,#224]
    mcr p15,0,r5,c6,c0,2
    ldr r5,[r4,#228]
    mcr p15,0,r5,c14,c1,0
    ldr r5,[r4,#232]
    mcr p15,0,r5,c14,c3,1
    add r6,r4,#240
    ldrd r0,r1,[r6]
    mcrr p15,0,r0,r1,c2
    add r6,r4,#248
    ldrd r0,r1,[r6]
    mcrr p15,1,r0,r1,c2
    add r6,r4,#256
    ldrd r0,r1,[r6]
    mcrr p15,0,r0,r1,c7
    add r6,r4,#264
    ldrd r0,r1,[r6]
    mcrr p15,3,r0,r1,c14
    ldr r5,[r4,#236]
    mcr p15,0,r5,c13,c0,0
    ldr r5,[r4,#272]
    mcr p15,0,r5,c10,c2,0
    ldr r5,[r4,#276]
    mcr p15,0,r5,c10,c2,1
    ldr r5,[r4,#280]
    mcr p15,0,r5,c10,c3,0
    ldr r5,[r4,#284]
    mcr p15,0,r5,c10,c3,1
    ldr r5,=0xf00000
    mcr p15,0,r5,c1,c0,2
    isb
    mov r5,#0x40000000
    vmsr fpexc,r5
    add r6,r4,#A32_FP
    vldmia r6!,{d0-d15}
    vldmia r6!,{d16-d31}
    ldr r5,[r4,#A32_FPSCR]
    vmsr fpscr,r5
    ldr r5,[r4,#A32_FPEXC]
    vmsr fpexc,r5
    ldr r5,[r4,#168]
    mcr p15,0,r5,c1,c0,2
    isb
    bx lr

.ltorg
.align 3
.global arm32_hyp_boot_cpus
arm32_hyp_boot_cpus:
    .space CONFIG_NR_CPUS * 4
.align 12
arm32_hyp_stacks:
    .space CONFIG_NR_CPUS * 4096
