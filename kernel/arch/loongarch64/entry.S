/* LoongArch64 boot entry
 * Sets up DMW windows, configures stack, clears BSS, calls kernel_main.
 *
 * LoongArch Direct Mapping Windows (DMW):
 *   DMW0 (CSR 0x180): 0x00 window — maps physical 0..2GB to VA 0x00..
 *   DMW1 (CSR 0x181): 0x80 window — maps physical 0..2GB to VA 0x8000...
 *
 * On QEMU virt (loongarch64), RAM starts at 0x0 physical.
 * Kernel is loaded at 0x200000 physical by default.
 */

.section .text.entry
.global _start

/* CSR register numbers */
.equ CSR_CRMD,  0x0    /* Current mode info */
.equ CSR_PRMD,  0x1    /* Pre-exception mode info */
.equ CSR_EUEN,  0x2    /* Extended unit enable */
.equ CSR_MISC,  0x3    /* Miscellaneous control */
.equ CSR_ECFG,  0x4    /* Exception configuration */
.equ CSR_ESTAT, 0x5    /* Exception status */
.equ CSR_ERA,   0x6    /* Exception return address */
.equ CSR_BADV,  0x7    /* Bad virtual address */
.equ CSR_EENTRY, 0xC   /* Exception entry */
.equ CSR_TLBIDX, 0x10  /* TLB index */
.equ CSR_TLBEHI, 0x11  /* TLB entry hi */
.equ CSR_TLBELO0, 0x12 /* TLB entry lo 0 */
.equ CSR_TLBELO1, 0x13 /* TLB entry lo 1 */
.equ CSR_ASID,   0x18  /* Address space ID */
.equ CSR_PGDL,   0x19  /* Page global dir lo */
.equ CSR_PGDH,   0x1A  /* Page global dir hi */
.equ CSR_PGD,    0x1B  /* Page global dir pointer */
.equ CSR_CPUID,  0x20  /* CPU ID */
.equ CSR_SAVE0,  0x30  /* KSave 0 */
.equ CSR_SAVE1,  0x31  /* KSave 1 */
.equ CSR_TID,    0x40  /* Timer ID */
.equ CSR_TCFG,   0x41  /* Timer configuration */
.equ CSR_TVAL,   0x42  /* Timer value */
.equ CSR_CNTC,   0x43  /* Counter compensation */
.equ CSR_TICLR,  0x44  /* Timer interrupt clear */
.equ CSR_DMW0,   0x180 /* Direct map window 0 */
.equ CSR_DMW1,   0x181 /* Direct map window 1 */

_start:
    /* Read CPUID → $a0 */
    csrrd   $a0, CSR_CPUID

    /* Set up per-hart stack pointer — 64KB per hart */
    li.d    $t0, 64 * 1024
    mul.d   $t1, $a0, $t0
    la.global $sp, boot_stack_top
    sub.d   $sp, $sp, $t1

    /* ---- Configure DMW windows ----
     * We use the same approach as RocketOS:
     * Detect current segment (hi bits of PC) and set DMW1 to map it.
     * DMW entry format:
     *   [63:60] = VSEG (virtual segment index)
     *   [5:4]   = PLV (privilege, 0=kernel)
     *   [0]     = EMATWC=1 (mapping type: strongly ordered)
     */

    /* Read current PC to detect which segment we are in */
    pcaddi      $t0, 0
    srli.d      $t0, $t0, 60      /* extract top 4 bits = segment */
    slli.d      $t0, $t0, 60      /* shift back to position */
    ori         $t0, $t0, 0x11    /* PLV0 | EMATWC=1, MAT=1 (coherent cached) */

    /* Set DMW1 to keep current window mapped after switch */
    csrwr       $t0, CSR_DMW1

    /* Set DMW0 for low physical memory (0x00 window) */
    li.d        $t1, 0x11         /* VSEG=0, PLV0, EMATWC */
    csrwr       $t1, CSR_DMW0

    /* Flush TLB */
    invtlb      0, $zero, $zero

    /* Jump to low address (DMW0 window = physical identity) */
    pcaddi      $t0, 0
    slli.d      $t0, $t0, 4
    srli.d      $t0, $t0, 4      /* clear top 4 bits → DMW0 window */
    addi.d      $t0, $t0, 12     /* point to instruction after jirl */
    jirl        $zero, $t0, 0

    /* ---- Clear BSS ---- */
    la.global   $t0, __bss_start
    la.global   $t1, __bss_end
bss_clear:
    bgeu        $t0, $t1, bss_done
    st.d        $zero, $t0, 0
    addi.d      $t0, $t0, 8
    b           bss_clear
bss_done:

    /* Pass CPUID (hart ID) in $a0, DTB in $a1 (from firmware if available) */
    csrrd       $a0, CSR_CPUID
    li.d        $a1, 0x100000    /* QEMU loongarch dtb address */

    bl          kernel_main

    /* Should never return */
halt:
    idle        0x0, 0
    b           halt

    .section .bss.stack
    .globl boot_stack
boot_stack:
    .space 64 * 1024 * 4    /* 64KB * 4 harts */
    .globl boot_stack_top
boot_stack_top:
