 /*
  * A20OS context switch for RISC-V 64-bit
  *
  * TaskContext layout (128 bytes):
  *   offset 0:   ra
  *   offset 8:   tp
  *   offset 16:  s[0]  (stored at (0+2)*8)
  *   offset 24:  s[1]  (at (1+2)*8)
  *   ...
  *   offset 104: s[11] (at (11+2)*8)
  *   offset 112: satp   (at 14*8)
  *   offset 120: sstatus (at 15*8)
  *   Total: 16*8 = 128 bytes
  *
  * __switch(next_kstack):
  *   a0 = pointer to next task's saved TaskContext on its kernel stack
  *   tp always points to current task_t struct (kstack at offset 0)
  *   sd sp, 0(tp) saves current sp into current task's kstack field
  *
  * Ported directly from RocketOS switch.S.
  */

.altmacro
.macro SAVE_CALLEE n
    sd s\n, (\n+2)*8(sp)
.endm
.macro LOAD_CALLEE n
    ld s\n, (\n+2)*8(a0)
.endm

    .section .text
    .globl __switch
    .align 2

__switch:
    /*
     * a0 -> next task's saved TaskContext pointer
     * (i.e., next_task->kstack, which points to the TaskContext
     *  previously saved on that task's kernel stack)
     */

    /* Allocate TaskContext on current kernel stack */
    addi    sp, sp, -16*8

    /* Save callee-saved registers */
    sd      ra, 0(sp)
    sd      tp, 8(sp)
    .set n, 0
    .rept 12
        SAVE_CALLEE %n
        .set n, n+1
    .endr

    /* Save satp and sstatus CSRs */
    csrr    t0, satp
    sd      t0, 14*8(sp)
    csrr    t0, sstatus
    sd      t0, 15*8(sp)

    /*
     * Save current sp into current task's kstack field.
     * tp points to current task_t struct.
     * kstack is the first field at offset 0.
     * This is critical: sd sp, 0(tp) writes current sp
     * into the task's kstack, so it can be restored later.
     */
    sd      sp, 0(tp)

    /*
     * Now restore from next task's saved context.
     * a0 points to the TaskContext on next task's kernel stack.
     */
    ld      ra, 0(a0)
    ld      tp, 8(a0)
    /* tp now points to next task_t struct */

    .set n, 0
    .rept 12
        LOAD_CALLEE %n
        .set n, n+1
    .endr

    /* Restore satp and sstatus (ensure SIE is enabled so timer ints fire) */
    ld      t0, 14*8(a0)
    csrr    t1, satp
    beq     t0, t1, .Lskip_switch_satp
    csrw    satp, t0
    sfence.vma
.Lskip_switch_satp:
    ld      t0, 15*8(a0)
    csrw    sstatus, t0

    /* Set sp past the TaskContext area on next task's stack */
    addi    a0, a0, 16*8
    mv      sp, a0

    /* Return to next task's execution point */
    ret
