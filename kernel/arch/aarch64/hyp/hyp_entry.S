/* Non-VHE EL2 monitor. Runs at its physical load address with MMU off.
 * No C, host virtual addresses or guest-controlled stack is used at EL2.
 * TPIDR_EL2 is zero in host mode and holds the state PA during guest mode.
 */
#include "hyp_state.h"
    .section .text
    .align 11
    .global aarch64_hyp_vectors
aarch64_hyp_vectors:
    .macro vector target
        b \target
        .space 124
    .endm
    .rept 8
    vector .Lfatal
    .endr
    vector .Lsync
    vector .Lirq
    vector .Lirq
    vector .Lserror
    /* A guest can enter AArch32 EL0 even though its kernel is AArch64.
     * Such an exit must return to the host, never wedge the EL2 monitor. */
    vector .Lsync
    vector .Lirq
    vector .Lirq
    vector .Lserror
.Lfatal:
    wfe
    b .Lfatal
.Lsync:
    stp x0, x1, [sp, #-16]!
    mov x1, #0
    b .Ldispatch
.Lirq:
    stp x0, x1, [sp, #-16]!
    mov x1, #1
    b .Ldispatch
.Lserror:
    stp x0, x1, [sp, #-16]!
    mov x1, #2
.Ldispatch:
    mrs x0, tpidr_el2
    cbnz x0, .Lguest_exit
    /* Host calls are synchronous, HVC64 with our private immediate. */
    mrs x0, esr_el2
    ubfx x1, x0, #26, #6
    cmp x1, #0x16
    b.ne .Lbad_host
    and x0, x0, #0xffff
    cmp x0, #0xa20
    b.ne .Lbad_host
    ldr x0, [sp]
    cmp x0, #2
    b.eq .Lflush
    cmp x0, #1
    b.ne .Lbad_host
    ldr x0, [sp, #8]
    msr tpidr_el2, x0
    add x0, x0, #A64_HYP_HOST
    stp x2, x3, [x0, #16]
    stp x4, x5, [x0, #32]
    stp x6, x7, [x0, #48]
    stp x8, x9, [x0, #64]
    stp x10, x11, [x0, #80]
    stp x12, x13, [x0, #96]
    stp x14, x15, [x0, #112]
    stp x16, x17, [x0, #128]
    stp x18, x19, [x0, #144]
    stp x20, x21, [x0, #160]
    stp x22, x23, [x0, #176]
    stp x24, x25, [x0, #192]
    stp x26, x27, [x0, #208]
    stp x28, x29, [x0, #224]
    str x30, [x0, #240]
    mrs x1, elr_el2
    str x1, [x0, #A64_HYP_PC]
    mrs x1, spsr_el2
    str x1, [x0, #A64_HYP_PSTATE]
    mrs x1, sp_el0
    str x1, [x0, #A64_HYP_SP0]
    mrs x1, sp_el1
    str x1, [x0, #A64_HYP_SP1]
    mrs x1, sctlr_el1
    str x1, [x0, #280]
    mrs x1, actlr_el1
    str x1, [x0, #288]
    mrs x1, cpacr_el1
    str x1, [x0, #296]
    mrs x1, tcr_el1
    str x1, [x0, #304]
    mrs x1, ttbr0_el1
    str x1, [x0, #312]
    mrs x1, ttbr1_el1
    str x1, [x0, #320]
    mrs x1, mair_el1
    str x1, [x0, #328]
    mrs x1, amair_el1
    str x1, [x0, #336]
    mrs x1, vbar_el1
    str x1, [x0, #344]
    mrs x1, contextidr_el1
    str x1, [x0, #352]
    mrs x1, tpidr_el0
    str x1, [x0, #360]
    mrs x1, tpidrro_el0
    str x1, [x0, #368]
    mrs x1, tpidr_el1
    str x1, [x0, #376]
    mrs x1, csselr_el1
    str x1, [x0, #384]
    mrs x1, cntkctl_el1
    str x1, [x0, #392]
    mrs x1, cntv_ctl_el0
    str x1, [x0, #400]
    mrs x1, cntv_cval_el0
    str x1, [x0, #408]
    mrs x1, elr_el1
    str x1, [x0, #416]
    mrs x1, spsr_el1
    str x1, [x0, #424]
    mrs x1, par_el1
    str x1, [x0, #432]
    mrs x1, esr_el1
    str x1, [x0, #440]
    mrs x1, far_el1
    str x1, [x0, #448]
    mrs x1, afsr0_el1
    str x1, [x0, #456]
    mrs x1, afsr1_el1
    str x1, [x0, #464]
    str q0, [x0, #512]
    str q1, [x0, #528]
    str q2, [x0, #544]
    str q3, [x0, #560]
    str q4, [x0, #576]
    str q5, [x0, #592]
    str q6, [x0, #608]
    str q7, [x0, #624]
    str q8, [x0, #640]
    str q9, [x0, #656]
    str q10, [x0, #672]
    str q11, [x0, #688]
    str q12, [x0, #704]
    str q13, [x0, #720]
    str q14, [x0, #736]
    str q15, [x0, #752]
    str q16, [x0, #768]
    str q17, [x0, #784]
    str q18, [x0, #800]
    str q19, [x0, #816]
    str q20, [x0, #832]
    str q21, [x0, #848]
    str q22, [x0, #864]
    str q23, [x0, #880]
    str q24, [x0, #896]
    str q25, [x0, #912]
    str q26, [x0, #928]
    str q27, [x0, #944]
    str q28, [x0, #960]
    str q29, [x0, #976]
    str q30, [x0, #992]
    str q31, [x0, #1008]
    mrs x1, fpcr
    str x1, [x0, #A64_HYP_FPCR]
    mrs x1, fpsr
    str x1, [x0, #A64_HYP_FPSR]
    /* Remove the host translations before the guest EL1 register bank. */
    mrs x0, tpidr_el2
    ldr x1, [x0, #A64_HYP_VTTBR]
    msr vttbr_el2, x1
    ldr x1, [x0, #A64_HYP_VTCR]
    msr vtcr_el2, x1
    dsb sy
    isb
    tlbi vmalls12e1
    ic iallu
    dsb sy
    isb
    ldr x1, [x0, #A64_HYP_MPIDR]
    msr vmpidr_el2, x1
    mrs x1, midr_el1
    msr vpidr_el2, x1
    /* Guest physical timer accesses trap; host CNTP remains untouched. */
    mov x1, #1 /* Allow counter reads, trap physical timer programming. */
    msr cnthctl_el2, x1
    msr cntvoff_el2, xzr
    ldr x1, =0x1033ff /* CPTR: trap trace/SVE, allow FP/SIMD. */
    msr cptr_el2, x1
    mov x1, #0xe60 /* Trap debug/PMU; neither is virtualized. */
    msr mdcr_el2, x1
    ldr x1, [x0, #A64_HYP_HCR]
    msr hcr_el2, x1
    isb
    ldr x1, [x0, #288]
    msr actlr_el1, x1
    ldr x1, [x0, #296]
    msr cpacr_el1, x1
    ldr x1, [x0, #304]
    msr tcr_el1, x1
    ldr x1, [x0, #312]
    msr ttbr0_el1, x1
    ldr x1, [x0, #320]
    msr ttbr1_el1, x1
    ldr x1, [x0, #328]
    msr mair_el1, x1
    ldr x1, [x0, #336]
    msr amair_el1, x1
    ldr x1, [x0, #344]
    msr vbar_el1, x1
    ldr x1, [x0, #352]
    msr contextidr_el1, x1
    ldr x1, [x0, #360]
    msr tpidr_el0, x1
    ldr x1, [x0, #368]
    msr tpidrro_el0, x1
    ldr x1, [x0, #376]
    msr tpidr_el1, x1
    ldr x1, [x0, #384]
    msr csselr_el1, x1
    ldr x1, [x0, #392]
    msr cntkctl_el1, x1
    ldr x1, [x0, #400]
    msr cntv_ctl_el0, x1
    ldr x1, [x0, #408]
    msr cntv_cval_el0, x1
    ldr x1, [x0, #416]
    msr elr_el1, x1
    ldr x1, [x0, #424]
    msr spsr_el1, x1
    ldr x1, [x0, #432]
    msr par_el1, x1
    ldr x1, [x0, #440]
    msr esr_el1, x1
    ldr x1, [x0, #448]
    msr far_el1, x1
    ldr x1, [x0, #456]
    msr afsr0_el1, x1
    ldr x1, [x0, #464]
    msr afsr1_el1, x1
    ldr x1, [x0, #280]
    msr sctlr_el1, x1
    ldr q0, [x0, #512]
    ldr q1, [x0, #528]
    ldr q2, [x0, #544]
    ldr q3, [x0, #560]
    ldr q4, [x0, #576]
    ldr q5, [x0, #592]
    ldr q6, [x0, #608]
    ldr q7, [x0, #624]
    ldr q8, [x0, #640]
    ldr q9, [x0, #656]
    ldr q10, [x0, #672]
    ldr q11, [x0, #688]
    ldr q12, [x0, #704]
    ldr q13, [x0, #720]
    ldr q14, [x0, #736]
    ldr q15, [x0, #752]
    ldr q16, [x0, #768]
    ldr q17, [x0, #784]
    ldr q18, [x0, #800]
    ldr q19, [x0, #816]
    ldr q20, [x0, #832]
    ldr q21, [x0, #848]
    ldr q22, [x0, #864]
    ldr q23, [x0, #880]
    ldr q24, [x0, #896]
    ldr q25, [x0, #912]
    ldr q26, [x0, #928]
    ldr q27, [x0, #944]
    ldr q28, [x0, #960]
    ldr q29, [x0, #976]
    ldr q30, [x0, #992]
    ldr q31, [x0, #1008]
    ldr x1, [x0, #A64_HYP_FPCR]
    msr fpcr, x1
    ldr x1, [x0, #A64_HYP_FPSR]
    msr fpsr, x1
    ldr x1, [x0, #A64_HYP_SP0]
    msr sp_el0, x1
    ldr x1, [x0, #A64_HYP_SP1]
    msr sp_el1, x1
    ldr x1, [x0, #A64_HYP_PC]
    msr elr_el2, x1
    ldr x1, [x0, #A64_HYP_PSTATE]
    msr spsr_el2, x1
    dsb sy
    isb
    add sp, sp, #16
    ldp x2, x3, [x0, #16]
    ldp x4, x5, [x0, #32]
    ldp x6, x7, [x0, #48]
    ldp x8, x9, [x0, #64]
    ldp x10, x11, [x0, #80]
    ldp x12, x13, [x0, #96]
    ldp x14, x15, [x0, #112]
    ldp x16, x17, [x0, #128]
    ldp x18, x19, [x0, #144]
    ldp x20, x21, [x0, #160]
    ldp x22, x23, [x0, #176]
    ldp x24, x25, [x0, #192]
    ldp x26, x27, [x0, #208]
    ldp x28, x29, [x0, #224]
    ldr x30, [x0, #240]
    ldp x0, x1, [x0]
    eret
.Lguest_exit:
    str x1, [x0, #A64_HYP_KIND]
    stp x2, x3, [x0, #16]
    stp x4, x5, [x0, #32]
    stp x6, x7, [x0, #48]
    stp x8, x9, [x0, #64]
    stp x10, x11, [x0, #80]
    stp x12, x13, [x0, #96]
    stp x14, x15, [x0, #112]
    stp x16, x17, [x0, #128]
    stp x18, x19, [x0, #144]
    stp x20, x21, [x0, #160]
    stp x22, x23, [x0, #176]
    stp x24, x25, [x0, #192]
    stp x26, x27, [x0, #208]
    stp x28, x29, [x0, #224]
    str x30, [x0, #240]
    ldp x2, x3, [sp]
    stp x2, x3, [x0]
    mrs x1, elr_el2
    str x1, [x0, #A64_HYP_PC]
    mrs x1, spsr_el2
    str x1, [x0, #A64_HYP_PSTATE]
    mrs x1, sp_el0
    str x1, [x0, #A64_HYP_SP0]
    mrs x1, sp_el1
    str x1, [x0, #A64_HYP_SP1]
    mrs x1, sctlr_el1
    str x1, [x0, #280]
    mrs x1, actlr_el1
    str x1, [x0, #288]
    mrs x1, cpacr_el1
    str x1, [x0, #296]
    mrs x1, tcr_el1
    str x1, [x0, #304]
    mrs x1, ttbr0_el1
    str x1, [x0, #312]
    mrs x1, ttbr1_el1
    str x1, [x0, #320]
    mrs x1, mair_el1
    str x1, [x0, #328]
    mrs x1, amair_el1
    str x1, [x0, #336]
    mrs x1, vbar_el1
    str x1, [x0, #344]
    mrs x1, contextidr_el1
    str x1, [x0, #352]
    mrs x1, tpidr_el0
    str x1, [x0, #360]
    mrs x1, tpidrro_el0
    str x1, [x0, #368]
    mrs x1, tpidr_el1
    str x1, [x0, #376]
    mrs x1, csselr_el1
    str x1, [x0, #384]
    mrs x1, cntkctl_el1
    str x1, [x0, #392]
    mrs x1, cntv_ctl_el0
    str x1, [x0, #400]
    mrs x1, cntv_cval_el0
    str x1, [x0, #408]
    mrs x1, elr_el1
    str x1, [x0, #416]
    mrs x1, spsr_el1
    str x1, [x0, #424]
    mrs x1, par_el1
    str x1, [x0, #432]
    mrs x1, esr_el1
    str x1, [x0, #440]
    mrs x1, far_el1
    str x1, [x0, #448]
    mrs x1, afsr0_el1
    str x1, [x0, #456]
    mrs x1, afsr1_el1
    str x1, [x0, #464]
    str q0, [x0, #512]
    str q1, [x0, #528]
    str q2, [x0, #544]
    str q3, [x0, #560]
    str q4, [x0, #576]
    str q5, [x0, #592]
    str q6, [x0, #608]
    str q7, [x0, #624]
    str q8, [x0, #640]
    str q9, [x0, #656]
    str q10, [x0, #672]
    str q11, [x0, #688]
    str q12, [x0, #704]
    str q13, [x0, #720]
    str q14, [x0, #736]
    str q15, [x0, #752]
    str q16, [x0, #768]
    str q17, [x0, #784]
    str q18, [x0, #800]
    str q19, [x0, #816]
    str q20, [x0, #832]
    str q21, [x0, #848]
    str q22, [x0, #864]
    str q23, [x0, #880]
    str q24, [x0, #896]
    str q25, [x0, #912]
    str q26, [x0, #928]
    str q27, [x0, #944]
    str q28, [x0, #960]
    str q29, [x0, #976]
    str q30, [x0, #992]
    str q31, [x0, #1008]
    mrs x1, fpcr
    str x1, [x0, #A64_HYP_FPCR]
    mrs x1, fpsr
    str x1, [x0, #A64_HYP_FPSR]
    mrs x1, esr_el2
    str x1, [x0, #A64_HYP_ESR]
    mrs x1, far_el2
    str x1, [x0, #A64_HYP_FAR]
    mrs x1, hpfar_el2
    str x1, [x0, #A64_HYP_HPFAR]
    /* Disable guest timer before restoring the host timer's bank. */
    msr cntv_ctl_el0, xzr
    mov x1, #(1 << 31)
    msr hcr_el2, x1
    isb
    dsb sy
    tlbi vmalls12e1
    dsb sy
    isb
    msr vttbr_el2, xzr
    mov x1, #3
    msr cnthctl_el2, x1
    mov x1, #0x33ff /* Host CPTR RES1, FP/SIMD enabled. */
    msr cptr_el2, x1
    msr mdcr_el2, xzr
    add x0, x0, #A64_HYP_HOST
    ldr x1, [x0, #288]
    msr actlr_el1, x1
    ldr x1, [x0, #296]
    msr cpacr_el1, x1
    ldr x1, [x0, #304]
    msr tcr_el1, x1
    ldr x1, [x0, #312]
    msr ttbr0_el1, x1
    ldr x1, [x0, #320]
    msr ttbr1_el1, x1
    ldr x1, [x0, #328]
    msr mair_el1, x1
    ldr x1, [x0, #336]
    msr amair_el1, x1
    ldr x1, [x0, #344]
    msr vbar_el1, x1
    ldr x1, [x0, #352]
    msr contextidr_el1, x1
    ldr x1, [x0, #360]
    msr tpidr_el0, x1
    ldr x1, [x0, #368]
    msr tpidrro_el0, x1
    ldr x1, [x0, #376]
    msr tpidr_el1, x1
    ldr x1, [x0, #384]
    msr csselr_el1, x1
    ldr x1, [x0, #392]
    msr cntkctl_el1, x1
    ldr x1, [x0, #400]
    msr cntv_ctl_el0, x1
    ldr x1, [x0, #408]
    msr cntv_cval_el0, x1
    ldr x1, [x0, #416]
    msr elr_el1, x1
    ldr x1, [x0, #424]
    msr spsr_el1, x1
    ldr x1, [x0, #432]
    msr par_el1, x1
    ldr x1, [x0, #440]
    msr esr_el1, x1
    ldr x1, [x0, #448]
    msr far_el1, x1
    ldr x1, [x0, #456]
    msr afsr0_el1, x1
    ldr x1, [x0, #464]
    msr afsr1_el1, x1
    ldr x1, [x0, #280]
    msr sctlr_el1, x1
    ldr q0, [x0, #512]
    ldr q1, [x0, #528]
    ldr q2, [x0, #544]
    ldr q3, [x0, #560]
    ldr q4, [x0, #576]
    ldr q5, [x0, #592]
    ldr q6, [x0, #608]
    ldr q7, [x0, #624]
    ldr q8, [x0, #640]
    ldr q9, [x0, #656]
    ldr q10, [x0, #672]
    ldr q11, [x0, #688]
    ldr q12, [x0, #704]
    ldr q13, [x0, #720]
    ldr q14, [x0, #736]
    ldr q15, [x0, #752]
    ldr q16, [x0, #768]
    ldr q17, [x0, #784]
    ldr q18, [x0, #800]
    ldr q19, [x0, #816]
    ldr q20, [x0, #832]
    ldr q21, [x0, #848]
    ldr q22, [x0, #864]
    ldr q23, [x0, #880]
    ldr q24, [x0, #896]
    ldr q25, [x0, #912]
    ldr q26, [x0, #928]
    ldr q27, [x0, #944]
    ldr q28, [x0, #960]
    ldr q29, [x0, #976]
    ldr q30, [x0, #992]
    ldr q31, [x0, #1008]
    ldr x1, [x0, #A64_HYP_FPCR]
    msr fpcr, x1
    ldr x1, [x0, #A64_HYP_FPSR]
    msr fpsr, x1
    ldr x1, [x0, #A64_HYP_SP0]
    msr sp_el0, x1
    ldr x1, [x0, #A64_HYP_SP1]
    msr sp_el1, x1
    ldr x1, [x0, #A64_HYP_PC]
    msr elr_el2, x1
    ldr x1, [x0, #A64_HYP_PSTATE]
    msr spsr_el2, x1
    msr tpidr_el2, xzr
    dsb sy
    isb
    add sp, sp, #16
    ldp x2, x3, [x0, #16]
    ldp x4, x5, [x0, #32]
    ldp x6, x7, [x0, #48]
    ldp x8, x9, [x0, #64]
    ldp x10, x11, [x0, #80]
    ldp x12, x13, [x0, #96]
    ldp x14, x15, [x0, #112]
    ldp x16, x17, [x0, #128]
    ldp x18, x19, [x0, #144]
    ldp x20, x21, [x0, #160]
    ldp x22, x23, [x0, #176]
    ldp x24, x25, [x0, #192]
    ldp x26, x27, [x0, #208]
    ldp x28, x29, [x0, #224]
    ldr x30, [x0, #240]
    mov x0, #0
    eret
.Lflush:
    /* All VMs use VMID zero and are fully invalidated on each activation.
     * A map change occurs with no guest currently executing on this CPU. */
    dsb sy
    tlbi vmalls12e1
    dsb sy
    isb
    ldp x0, x1, [sp], #16
    mov x0, #0
    eret
.Lbad_host:
    ldp x0, x1, [sp], #16
    mov x0, #-1
    eret
    .ltorg
