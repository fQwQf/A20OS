/*
 * riscv64 vcpu enter/exit, plus the prelude a guest trap needs before the
 * kernel's ordinary trap entry can be used for it.
 *
 * Why the prelude exists at all: VS-mode and HS-mode share the physical
 * register file, so a trap out of the guest arrives with the GUEST's sp and
 * tp live.  trap.S __trap_from_kernel assumes it is a host kernel trap -- it
 * stores through tp (the sp guard) before any C runs, and it builds its frame
 * on the current stack -- so both have to be the host's.  The prelude banks
 * those two guest registers in vcpu->regs[] (their natural home) and switches
 * to the host stack and tp, then joins the standard entry.  hyp_arch_guest_trap
 * writes the banked values back into the frame, and trap.S returns on the
 * frame's x[2] (the same rule __return_to_user uses for user mode), so a
 * resumed guest comes back on its own stack.
 *
 * Everything else about a guest trap is the kernel's: the frame layout, the
 * C dispatcher, and the sret that follows (hstatus.SPV is 1 from the trap, and
 * sstatus.SPP is 1 because VS-mode's privilege is S, which is exactly the pair
 * helper_sret turns back into VS-mode).
 *
 * Two routines leave normal C code and are the only ones that do:
 *   hyp_arch_vcpu_enter  park the host callee-saved set, build the guest trap
 *                        frame from vcpu->regs/pc, sret into VS-mode
 *   hyp_arch_vcpu_exit   terminal exit: drop the guest view of hstatus, put
 *                        stvec back, restore the host set from vcpu->arch[]
 *                        and return to enter's caller
 */

#include "hyp_asm_offsets.h"

    .set HYP_FRAME_SIZE, 70*8

    .set HYP_VCPU_REGS_OFF, HYP_ASM_VCPU_REGS_OFF
    .set HYP_VCPU_PC_OFF,   HYP_ASM_VCPU_PC_OFF
    .set HYP_VCPU_ARCH_OFF, HYP_ASM_VCPU_ARCH_OFF

/*
 * hstatus field positions (QEMU target/riscv cpu_bits.h HSTATUS_*, which the
 * tree's assembler does not carry; Linux's asm/csr.h agrees on all of them).
 */
    .set HSTATUS_SPV,  (1 << 7)
    .set HSTATUS_SPVP, (1 << 8)
    .set HSTATUS_HU,   (1 << 9)
    .set HSTATUS_VTVM, (1 << 20)
    .set HSTATUS_GUEST_ON,  (HSTATUS_SPV | HSTATUS_SPVP | HSTATUS_VTVM)
    .set HSTATUS_GUEST_OFF, (HSTATUS_SPV | HSTATUS_SPVP | HSTATUS_VTVM | HSTATUS_HU)

    .equ SSTATUS_SIE,  (1 << 1)
    .equ SSTATUS_SPIE, (1 << 5)
    .equ SSTATUS_SPP,  (1 << 8)

.altmacro
.macro LOAD_GP n
    ld x\n, \n*8(sp)
.endm

    .section .text

/* ------------------------------------------------------------------ */
/* Guest trap prelude.  Installed in stvec by the enter path; the run    */
/* loop's exit path puts __trap_from_kernel back.                        */
/* ------------------------------------------------------------------ */
    .align 2
    .globl hyp_guest_trap_entry
    .type hyp_guest_trap_entry, @function
hyp_guest_trap_entry:
    /* Only a trap taken out of a running guest reaches here, so the live
     * registers are the guest's and g_hyp_arch_vcpu is set.  The NULL test is
     * belt and braces for the window where stvec is still this vector but
     * the guest has already been torn down. */
    la      t0, g_hyp_arch_vcpu
    ld      t0, 0(t0)
    beqz    t0, .Lhyp_guest_into_kernel
    sd      sp, HYP_VCPU_REGS_OFF + 2*8(t0)
    sd      tp, HYP_VCPU_REGS_OFF + 4*8(t0)
    ld      sp, HYP_VCPU_ARCH_OFF + 1*8(t0)
    ld      tp, HYP_VCPU_ARCH_OFF + 3*8(t0)
.Lhyp_guest_into_kernel:
    j       __trap_from_kernel
    .size hyp_guest_trap_entry, . - hyp_guest_trap_entry

/* ------------------------------------------------------------------ */
/* Initial entry: host context into vcpu->arch[], guest context out of  */
/* vcpu->regs/pc, then sret.  Returns only through hyp_arch_vcpu_exit.   */
/* ------------------------------------------------------------------ */
    .align 2
    .globl hyp_arch_vcpu_enter
    .type hyp_arch_vcpu_enter, @function
hyp_arch_vcpu_enter:
    /* a0 = vcpu.  Save the host callee-saved set first: every register is
     * scratch from here on.  arch[] is the frozen layout from
     * hyp_vcpu.h -- [0]=ra [1]=sp [2]=gp [3]=tp [4..10]=t0-t6 [11..22]=s0-s11. */
    sd      ra, HYP_VCPU_ARCH_OFF + 0*8(a0)
    sd      sp, HYP_VCPU_ARCH_OFF + 1*8(a0)
    sd      gp, HYP_VCPU_ARCH_OFF + 2*8(a0)
    sd      tp, HYP_VCPU_ARCH_OFF + 3*8(a0)
    sd      t0, HYP_VCPU_ARCH_OFF + 4*8(a0)
    sd      t1, HYP_VCPU_ARCH_OFF + 5*8(a0)
    sd      t2, HYP_VCPU_ARCH_OFF + 6*8(a0)
    sd      t3, HYP_VCPU_ARCH_OFF + 7*8(a0)
    sd      t4, HYP_VCPU_ARCH_OFF + 8*8(a0)
    sd      t5, HYP_VCPU_ARCH_OFF + 9*8(a0)
    sd      t6, HYP_VCPU_ARCH_OFF + 10*8(a0)
    sd      s0, HYP_VCPU_ARCH_OFF + 11*8(a0)
    sd      s1, HYP_VCPU_ARCH_OFF + 12*8(a0)
    sd      s2, HYP_VCPU_ARCH_OFF + 13*8(a0)
    sd      s3, HYP_VCPU_ARCH_OFF + 14*8(a0)
    sd      s4, HYP_VCPU_ARCH_OFF + 15*8(a0)
    sd      s5, HYP_VCPU_ARCH_OFF + 16*8(a0)
    sd      s6, HYP_VCPU_ARCH_OFF + 17*8(a0)
    sd      s7, HYP_VCPU_ARCH_OFF + 18*8(a0)
    sd      s8, HYP_VCPU_ARCH_OFF + 19*8(a0)
    sd      s9, HYP_VCPU_ARCH_OFF + 20*8(a0)
    sd      s10, HYP_VCPU_ARCH_OFF + 21*8(a0)
    sd      s11, HYP_VCPU_ARCH_OFF + 22*8(a0)

    /* Publish the vcpu for the guest trap prelude, which has to reach it
     * before any C runs.  Cleared by hyp_arch_guest_trap() on the way out. */
    la      t0, g_hyp_arch_vcpu
    sd      a0, 0(t0)

    /* The guest image goes into a real trap frame on the kernel stack, so the
     * run loop's fixups (sepc past an ecall, a rewritten GPR) happen in the
     * structure the rest of the kernel already knows. */
    addi    sp, sp, -HYP_FRAME_SIZE
    mv      t1, sp
    li      t2, 70
.Lhyp_zero_frame:
    sd      zero, 0(t1)
    addi    t1, t1, 8
    addi    t2, t2, -1
    bnez    t2, .Lhyp_zero_frame

    /* x[1..31] = vcpu->regs[1..31]; x[0] is hardwired zero. */
    addi    t3, a0, HYP_VCPU_REGS_OFF + 8
    addi    t4, sp, 8
    li      t5, 31
.Lhyp_copy_gpr:
    ld      t6, 0(t3)
    sd      t6, 0(t4)
    addi    t3, t3, 8
    addi    t4, t4, 8
    addi    t5, t5, -1
    bnez    t5, .Lhyp_copy_gpr
    sd      zero, 0(sp)

    ld      t0, HYP_VCPU_PC_OFF(a0)
    sd      t0, 33*8(sp)

    /* Guest sstatus: SPP=1 because VS-mode's privilege is S (sret reads SPP
     * for the privilege and hstatus.SPV for the virtual side), FS=Off because
     * the vcpu carries no guest FP image so a guest FP instruction must fault
     * instead of quietly sharing the host's FPRs, and SPIE|SIE so the host
     * timer interrupt reaches the guest as a trap to HS rather than sitting
     * pending behind it (sret copies SPIE into SIE, so SPIE is the load-bearing
     * half). */
    li      t0, SSTATUS_SPP | SSTATUS_SPIE | SSTATUS_SIE
    sd      t0, 32*8(sp)

    /* kernel_tp: the host task pointer, which is what the trap entry expects
     * to find in tp when the guest traps. */
    sd      tp, 35*8(sp)

    /* hstatus for the guest.  Only the VS-facing permission bits are
     * touched; hstatus.SIE and everything else the host owns is left alone,
     * because sret does not restore SPV (hardware cleared it on the last
     * trap-return) and that is the one field enter has to re-assert. */
    csrr    t0, hstatus
    li      t1, HSTATUS_GUEST_ON
    or      t0, t0, t1
    li      t1, HSTATUS_HU
    not     t1, t1
    and     t0, t0, t1
    csrw    hstatus, t0

    /* Guest exceptions and host interrupts both land on the prelude, which
     * joins the ordinary kernel entry.  A host interrupt is delivered to HS
     * because hideleg is zero, so every guest-external interrupt traps here
     * instead of being handled by the guest; the frame's sstatus carries
     * SIE, which is what lets the guest's own interrupts pending be a
     * separate question (nothing is delegated either way).  No hstatus bit
     * gates this on the platform this slice is measured on: QEMU's
     * VS-to-HS path takes hsie = 1 unconditionally while V=1
     * (target/riscv/cpu_helper.c, riscv_cpu_pending_to_irq) and has no
     * interrupt-enable field in hstatus at all -- bit 5 is VSBE there.  An
     * earlier version of this comment claimed "hstatus.VIE set above"; no
     * such field was ever programmed here, and none is, so do not read this
     * as depending on one.
     *
     * From here to the sret there is no window for a host trap: hstatus.SPV is
     * already set, so anything that arrived now would be taken for a guest trap
     * and this function's own sp/tp banked as the guest's.  Interrupts are off
     * across the window and the sret re-arms them from the frame's sstatus,
     * which carries SPIE|SIE for the guest. */
    csrc    sstatus, SSTATUS_SIE

    la      t0, hyp_guest_trap_entry
    csrw    stvec, t0

    /* Nothing to set up for the guest's own translation: VS-stage-1 is
     * vsatp, which this slice leaves at MODE=Bare, and hgatp is already
     * armed from hyp_arch_vcpu_setup().  satp keeps the host's table across
     * the sret -- the trap-return into the guest parks it and promotes vsatp
     * in its place, then hands satp back on the way out -- so the host
     * fetches its own trap vector exactly as it does normally. */
    ld      t0, 33*8(sp)
    csrw    sepc, t0
    ld      t0, 32*8(sp)
    csrw    sstatus, t0

    ld      x1, 1*8(sp)
    ld      x3, 3*8(sp)
    ld      x4, 4*8(sp)
    .set n, 5
    .rept 27
    LOAD_GP %n
    .set n, n+1
    .endr

    /* sp last: nothing may read through the frame after this. */
    ld      t0, 2*8(sp)
    mv      sp, t0
    sret
    .size hyp_arch_vcpu_enter, . - hyp_arch_vcpu_enter

/* ------------------------------------------------------------------ */
/* Terminal exit.  a0 = vcpu.  Never returns: it returns TO enter's      */
/* caller, discarding everything the trap path built on the stack.       */
/* ------------------------------------------------------------------ */
    .align 2
    .globl hyp_arch_vcpu_exit
    .type hyp_arch_vcpu_exit, @function
hyp_arch_vcpu_exit:
    /* The guest view of hstatus goes away with the guest: HS-mode must not
     * keep SPV/SPVP/VTVM/HU set behind a running host.  There is no
     * translation state to put back: satp never moved, and vsatp is still at
     * the reset value. */
    csrr    t0, hstatus
    li      t1, HSTATUS_GUEST_OFF
    not     t1, t1
    and     t0, t0, t1
    csrw    hstatus, t0

    /* Back to the host's own vector before any host trap can be taken. */
    la      t0, __trap_from_kernel
    csrw    stvec, t0

    /* And unpublish the vcpu before anything downstream can look at it.
     * hyp_arch_guest_trap() already cleared it on the way into C, so this
     * store is normally writing NULL over NULL -- the point is that the exit
     * path does not DEPEND on that having happened.  A pointer left
     * published here outlives this function: hyp_vcpu_run() drops the claim
     * and hyp_vcpu_put() frees the object, so anything that read the pointer
     * afterwards would be reading a freed vcpu through the guest trap prelude.
     * Clearing it here makes "no vcpu is published once the guest is gone" a
     * property of this function instead of an invariant about a caller's
     * earlier behaviour. */
    la      t0, g_hyp_arch_vcpu
    sd      zero, 0(t0)

    /* arch[1] is the kernel sp this function's caller was standing on, so
     * restoring it discards the guest frame and the whole C trap path below it
     * in one step. */
    ld      sp, HYP_VCPU_ARCH_OFF + 1*8(a0)
    ld      ra, HYP_VCPU_ARCH_OFF + 0*8(a0)
    ld      gp, HYP_VCPU_ARCH_OFF + 2*8(a0)
    ld      tp, HYP_VCPU_ARCH_OFF + 3*8(a0)
    ld      t0, HYP_VCPU_ARCH_OFF + 4*8(a0)
    ld      t1, HYP_VCPU_ARCH_OFF + 5*8(a0)
    ld      t2, HYP_VCPU_ARCH_OFF + 6*8(a0)
    ld      t3, HYP_VCPU_ARCH_OFF + 7*8(a0)
    ld      t4, HYP_VCPU_ARCH_OFF + 8*8(a0)
    ld      t5, HYP_VCPU_ARCH_OFF + 9*8(a0)
    ld      t6, HYP_VCPU_ARCH_OFF + 10*8(a0)
    ld      s0, HYP_VCPU_ARCH_OFF + 11*8(a0)
    ld      s1, HYP_VCPU_ARCH_OFF + 12*8(a0)
    ld      s2, HYP_VCPU_ARCH_OFF + 13*8(a0)
    ld      s3, HYP_VCPU_ARCH_OFF + 14*8(a0)
    ld      s4, HYP_VCPU_ARCH_OFF + 15*8(a0)
    ld      s5, HYP_VCPU_ARCH_OFF + 16*8(a0)
    ld      s6, HYP_VCPU_ARCH_OFF + 17*8(a0)
    ld      s7, HYP_VCPU_ARCH_OFF + 18*8(a0)
    ld      s8, HYP_VCPU_ARCH_OFF + 19*8(a0)
    ld      s9, HYP_VCPU_ARCH_OFF + 20*8(a0)
    ld      s10, HYP_VCPU_ARCH_OFF + 21*8(a0)
    ld      s11, HYP_VCPU_ARCH_OFF + 22*8(a0)
    ret
    .size hyp_arch_vcpu_exit, . - hyp_arch_vcpu_exit