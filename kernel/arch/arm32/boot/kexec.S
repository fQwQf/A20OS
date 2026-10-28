/*
 * ARM32 kexec handoff trampoline (short-descriptor translation).
 *
 * a20_arm32_kexec_enter() runs once at its linked high-half VA, points TTBR0
 * at the dedicated kexec L1 (identity sections over the RAM window plus the
 * high-half alias), then branches to the trampoline's physical address.  The
 * trampoline copies each staged segment, cleans the destinations to the PoC,
 * drops the MMU and caches, and enters the next kernel with the Linux ARM
 * boot register contract (r0 = 0, r1 = 0, r2 = DTB physical address).
 *
 * Control-block fields are 64-bit slots holding 32-bit values; only the low
 * words are read.  Chain nodes use the 32-bit layout: next pointer at +0,
 * payload at +4, 4092 bytes per node.  Chain next pointers are kernel VAs;
 * PAGE_OFFSET is 0x80000000, so the physical node address is VA - 0x80000000.
 */

    .syntax unified
    .arch armv7-a
    .section .text
    .globl a20_arm32_kexec_enter
    .globl a20_arm32_kexec_trampoline
    .globl a20_arm32_kexec_trampoline_end

a20_arm32_kexec_enter:
    mov     r8, r0
    mov     r9, r1
    mov     r10, r2
    mov     r1, #0
    mcr     p15, 0, r1, c2, c0, 2   /* TTBCR = 0: short descriptors */
    mcr     p15, 0, r10, c2, c0, 0  /* TTBR0 = kexec L1 */
    isb
    mcr     p15, 0, r1, c8, c7, 0   /* TLBIALL */
    mcr     p15, 0, r1, c7, c5, 0   /* ICIALLU */
    dsb     sy
    isb
    mov     r0, r9
    bx      r8

a20_arm32_kexec_trampoline:
    mov     r4, r0                  @ control block physical address
    add     r5, r4, #40             @ seg[]
    ldr     r6, [r4, #24]           @ nr
    ldr     r14, =0x80000000        @ PAGE_OFFSET
.Lsegment:
    cmp     r6, #0
    beq     .Ljump
    ldr     r7, [r5]                @ chain head PA
    ldr     r8, [r5, #8]            @ destination cursor
    ldr     r9, [r5, #16]           @ bufsz
    ldr     r10, [r5, #24]          @ memsz
    mov     r11, #0                 @ bytes emitted for this segment
    mov     r12, #0                 @ data offset inside the chain node
.Lbyte:
    cmp     r11, r10
    bhs     .Lnext
    ldr     r0, =4092
    cmp     r12, r0
    blo     .Lsource
    cmp     r7, #0
    beq     .Lfail
    ldr     r7, [r7]                @ next node, stored as a kernel VA
    sub     r7, r7, r14             @ VA -> PA
    mov     r12, #0
.Lsource:
    cmp     r7, #0
    beq     .Lfail
    add     r0, r7, r12
    add     r0, r0, #4              @ chain payload starts at +4 on ARM32
    cmp     r11, r9
    bhs     .Lzero
    ldrb    r1, [r0]
    b       .Lstore
.Lzero:
    mov     r1, #0
.Lstore:
    strb    r1, [r8]
    add     r8, r8, #1
    add     r11, r11, #1
    add     r12, r12, #1
    b       .Lbyte
.Lnext:
    @ Clean the copied range to the PoC: the next kernel boots with the MMU
    @ and caches off and must observe the bytes in memory.
    ldr     r0, [r5, #8]            @ destination base
    ldr     r1, [r5, #24]
    add     r1, r1, r0
    mrc     p15, 0, r2, c0, c0, 1   @ CTR
    ubfx    r2, r2, #16, #4         @ DminLine: log2 words per line
    mov     r3, #4
    mov     r3, r3, lsl r2          @ cache line bytes
    sub     r2, r3, #1
    bic     r0, r0, r2
.Lclean:
    cmp     r0, r1
    bhs     .Lcleandone
    mcr     p15, 0, r0, c7, c14, 1  @ DCCVAC
    add     r0, r0, r3
    b       .Lclean
.Lcleandone:
    add     r5, r5, #32
    sub     r6, r6, #1
    b       .Lsegment
.Ljump:
    dsb     sy
    mov     r1, #0
    mcr     p15, 0, r1, c7, c5, 0   @ ICIALLU
    mrc     p15, 0, r1, c1, c0, 0
    bic     r1, r1, #1              @ M
    bic     r1, r1, #4              @ C
    bic     r1, r1, #0x1000         @ I
    mcr     p15, 0, r1, c1, c0, 0
    isb
    mov     r1, #0
    mcr     p15, 0, r1, c8, c7, 0   @ TLBIALL
    dsb     sy
    mov     r0, #0
    mov     r1, #0
    ldr     r2, [r4, #16]           @ boot_arg1: DTB physical address
    ldr     r3, [r4]                @ entry
    bx      r3
.Lfail:
    wfi
    b       .Lfail
    /* The trampoline is memcpy'd to a fresh page at handoff time; keep the
     * literal pool (e.g. the 4092 chain-capacity constant) inside the copied
     * [start, end) range or the PC-relative loads read garbage. */
    .ltorg
a20_arm32_kexec_trampoline_end:
