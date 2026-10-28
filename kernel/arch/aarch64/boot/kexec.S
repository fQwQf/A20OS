/*
 * AArch64 kexec handoff trampoline.
 *
 * a20_aarch64_kexec_enter() runs once at its linked high-half VA with the
 * current (boot) translation still live, swaps TTBR0 to the dedicated kexec
 * table pair (identity alias + PAGE_OFFSET alias), then branches to the
 * trampoline's physical address.  The trampoline is position independent:
 * it only touches physical addresses through the identity alias, copies each
 * staged segment to its final physical destination, cleans the destination
 * ranges back to the point of coherency, turns the MMU and caches off, and
 * enters the next kernel with the standard Image handoff (x0 = DTB PA).
 */

    .section .text
    .globl a20_aarch64_kexec_enter
    .globl a20_aarch64_kexec_trampoline
    .globl a20_aarch64_kexec_trampoline_end

a20_aarch64_kexec_enter:
    mov     x9, x0
    mov     x10, x1
    /* Clear WXN before the table swap: the kexec L1 RAM block is writable,
     * and with WXN still set the very next fetch under the new table would
     * take a permission fault.  The boot tables keep their text read-only,
     * so clearing it early changes nothing there. */
    mrs     x4, sctlr_el1
    bic     x4, x4, #(1 << 19)  /* WXN (set by arch_kernel_wx_finalize) */
    msr     sctlr_el1, x4
    isb
    ldr     x4, =0x000000000000FF00
    msr     mair_el1, x4
    ldr     x4, =0x0000000280003510
    msr     tcr_el1, x4
    msr     ttbr0_el1, x2
    tlbi    vmalle1
    dsb     sy
    isb
    mov     x0, x10
    br      x9

a20_aarch64_kexec_trampoline:
    mov     x19, x0             /* control block physical address */
    add     x20, x19, #40       /* seg[] */
    ldr     w21, [x19, #24]     /* nr */
.Lsegment:
    cbz     w21, .Ljump
    ldr     x0, [x20]           /* source chain head PA */
    ldr     x1, [x20, #8]       /* destination PA cursor */
    ldr     x2, [x20, #16]      /* bufsz */
    ldr     x3, [x20, #24]      /* memsz */
    mov     x4, #0              /* bytes emitted for this segment */
    mov     x5, #0              /* data offset inside the chain node */
.Lbyte:
    cmp     x4, x3
    b.hs    .Lsegdone
    cmp     x5, #4088
    b.lo    .Lsource
    cbz     x0, .Lfail
    ldr     x0, [x0]            /* next node, stored as a kernel VA */
    movz    x6, #0x0080, lsl #32
    sub     x0, x0, x6          /* VA -> PA (PAGE_OFFSET alias) */
    mov     x5, #0
.Lsource:
    cbz     x0, .Lfail
    add     x6, x0, x5
    add     x6, x6, #8          /* chain node payload starts at +8 */
    cmp     x4, x2
    b.hs    .Lzero
    ldrb    w7, [x6]
    b       .Lstore
.Lzero:
    mov     w7, #0
.Lstore:
    strb    w7, [x1]
    add     x1, x1, #1
    add     x4, x4, #1
    add     x5, x5, #1
    b       .Lbyte
.Lsegdone:
    /* Clean the copied range to the PoC: the next kernel boots with the
     * MMU and caches off and must observe the bytes in memory, not only in
     * this core's dirty lines. */
    ldr     x8, [x20, #8]       /* destination base */
    ldr     x9, [x20, #24]      /* memsz */
    add     x9, x9, x8
    mrs     x10, ctr_el0
    ubfx    x10, x10, #16, #4   /* DminLine: log2 words per line */
    mov     x11, #4
    lsl     x11, x11, x10       /* cache line bytes */
    sub     x12, x11, #1
    bic     x8, x8, x12
.Lclean:
    cmp     x8, x9
    b.hs    .Lcleandone
    dc      cvac, x8
    add     x8, x8, x11
    b       .Lclean
.Lcleandone:
    add     x20, x20, #32
    sub     w21, w21, #1
    b       .Lsegment
.Ljump:
    dsb     sy
    ic      iallu
    dsb     sy
    isb
    mrs     x3, sctlr_el1
    bic     x3, x3, #1          /* M */
    bic     x3, x3, #(1 << 2)   /* C */
    bic     x3, x3, #(1 << 12)  /* I */
    msr     sctlr_el1, x3
    isb
    tlbi    vmalle1
    dsb     sy
    ldr     x0, [x19, #8]       /* boot_arg0: DTB physical address */
    ldr     x1, [x19, #16]      /* boot_arg1 */
    ldr     x2, [x19, #32]      /* boot_arg2 */
    ldr     x3, [x19]           /* entry */
    br      x3
.Lfail:
    wfi
    b       .Lfail
a20_aarch64_kexec_trampoline_end:
