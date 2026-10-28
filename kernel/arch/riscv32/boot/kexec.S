/*
 * RISC-V32 kexec handoff trampoline (Sv32, qemu-virt-riscv32).
 *
 * The kernel runs identity-mapped (PAGE_OFFSET == 0), so every pointer in
 * the handoff control block — source chain nodes, destinations, the block
 * itself — is already a physical address.  The 64-bit control fields carry
 * 32-bit values; only their low words are read.  Chain nodes follow the
 * 32-bit layout: next pointer at +0, payload at +4, 4092 bytes per node.
 */

    .section .text
    .globl a20_rv64_kexec_trampoline
    .globl a20_rv64_kexec_trampoline_end
    .globl a20_rv64_kexec_enter

a20_rv64_kexec_enter:
    mv      t0, a0
    mv      t1, a1
    csrw    satp, a2
    sfence.vma
    .word   0x0000100f  /* fence.i */
    mv      a0, t1
    jr      t0

a20_rv64_kexec_trampoline:
    csrw    satp, zero
    sfence.vma
    mv      s0, a0
    addi    s1, s0, 40
    lw      s2, 24(s0)
.Lsegment:
    beqz    s2, .Ljump
    lw      t0, 0(s1)
    lw      s3, 8(s1)
    lw      s4, 16(s1)
    lw      s5, 24(s1)
    li      s6, 0
    li      t3, 0
.Lbyte:
    bgeu    s6, s5, .Lnext_segment
    li      t4, 4092
    bltu    t3, t4, .Lsource
    beqz    t0, .Lfail
    lw      t0, 0(t0)           /* next node; VA == PA in this layout */
    li      t3, 0
.Lsource:
    beqz    t0, .Lfail
    add     t4, t0, t3
    addi    t4, t4, 4           /* chain payload starts at +4 on RV32 */
    bgeu    s6, s4, .Lzero
    lbu     t5, 0(t4)
    j       .Lstore
.Lzero:
    li      t5, 0
.Lstore:
    sb      t5, 0(s3)
    addi    s3, s3, 1
    addi    s6, s6, 1
    addi    t3, t3, 1
    j       .Lbyte
.Lnext_segment:
    addi    s1, s1, 32
    addi    s2, s2, -1
    j       .Lsegment
.Ljump:
    fence   rw, rw
    csrw    satp, zero
    sfence.vma
    .word   0x0000100f  /* fence.i */
    lw      a0, 8(s0)           /* boot_arg0: hart id */
    lw      a1, 16(s0)          /* boot_arg1: DTB physical address */
    lw      t0, 0(s0)
    jr      t0
.Lfail:
    wfi
    j       .Lfail
a20_rv64_kexec_trampoline_end:
