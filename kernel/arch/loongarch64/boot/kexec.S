/*
 * LoongArch64 kexec handoff trampoline (QEMU virt).
 *
 * The board runs on an identity cached DMW (PAGE_OFFSET == 0), so the
 * trampoline addresses everything — source chain nodes, destination ranges,
 * the control block — at physical addresses directly.  The final CRMD write
 * drops PG and keeps DA set, which leaves the PC valid because the mapping
 * is already one-to-one, and the next kernel's _start re-installs its own
 * DMW pair exactly like after a firmware reset.
 */

    .section .text
    .globl a20_la64_kexec_enter
    .globl a20_la64_kexec_trampoline
    .globl a20_la64_kexec_park
    .globl a20_la64_kexec_trampoline_end

a20_la64_kexec_enter:
    move    $t0, $a0
    move    $a0, $a1
    jr      $t0

a20_la64_kexec_trampoline:
    move    $s0, $a0            /* control block physical address */
    addi.d  $s1, $s0, 40        /* seg[] */
    ld.w    $s2, $s0, 24        /* nr */
.Lsegment:
    beqz    $s2, .Ljump
    ld.d    $t0, $s1, 0         /* source chain head */
    ld.d    $s3, $s1, 8         /* destination cursor */
    ld.d    $s4, $s1, 16        /* bufsz */
    ld.d    $s5, $s1, 24        /* memsz */
    move    $s6, $zero          /* bytes emitted for this segment */
    move    $t3, $zero          /* data offset inside the chain node */
.Lbyte:
    bgeu    $s6, $s5, .Lnext_segment
    li.d    $t4, 4088
    bltu    $t3, $t4, .Lsource
    beqz    $t0, .Lfail
    ld.d    $t0, $t0, 0         /* next node; VA == PA in this layout */
    move    $t3, $zero
.Lsource:
    beqz    $t0, .Lfail
    add.d   $t4, $t0, $t3
    addi.d  $t4, $t4, 8         /* chain node payload starts at +8 */
    bgeu    $s6, $s4, .Lzero
    ld.bu   $t5, $t4, 0
    b       .Lstore
.Lzero:
    move    $t5, $zero
.Lstore:
    st.b    $t5, $s3, 0
    addi.d  $s3, $s3, 1
    addi.d  $s6, $s6, 1
    addi.d  $t3, $t3, 1
    b       .Lbyte
.Lnext_segment:
    addi.d  $s1, $s1, 32
    addi.d  $s2, $s2, -1
    b       .Lsegment
.Ljump:
    dbar    0
    ibar    0
    li.d    $t1, 0x8            /* CRMD: DA=1, PG=0, PLV0 */
    csrwr   $t1, 0x0
    ld.d    $a0, $s0, 8         /* boot_arg0 */
    ld.d    $a1, $s0, 16        /* boot_arg1 */
    ld.d    $a2, $s0, 32        /* boot_arg2 */
    ld.d    $t0, $s0, 0         /* entry */
    jr      $t0
.Lfail:
    b       .Lfail
a20_la64_kexec_trampoline_end:
