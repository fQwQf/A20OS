    .abiversion 2

#include "asm/ppc64-regs.h"

    .section .text.entry
    .globl _start
_start:
    /*
     * SLOF's RAM-kernel "go-direct" path enters us in 32-bit big-endian
     * mode (MSR[SF]=0, MSR[LE]=0).  We must reach 64-bit little-endian
     * mode before we can run the rest of the kernel.
     *
     * The 32-bit BE stub below installs a tiny Program-Check handler at
     * physical 0x700 and then executes a 32-bit BE trap.  Exceptions
     * always set MSR[SF]=1, so the handler runs in 64-bit BE.  It then
     * sets MSR[LE]=1 and returns to enter_64le (_start + 0x100).
     */
    .byte 0x39, 0x40, 0x07, 0x00
    .byte 0x3d, 0x60, 0x7c, 0x7a
    .byte 0x61, 0x6b, 0x02, 0xa6
    .byte 0x91, 0x6a, 0x00, 0x00
    .byte 0x3d, 0x60, 0x38, 0x63
    .byte 0x61, 0x6b, 0x00, 0x88
    .byte 0x91, 0x6a, 0x00, 0x04
    .byte 0x3d, 0x60, 0x7c, 0x7a
    .byte 0x61, 0x6b, 0x03, 0xa6
    .byte 0x91, 0x6a, 0x00, 0x08
    .byte 0x3d, 0x60, 0x7c, 0x80
    .byte 0x61, 0x6b, 0x00, 0xa6
    .byte 0x91, 0x6a, 0x00, 0x0c
    .byte 0x3d, 0x60, 0x60, 0x84
    .byte 0x61, 0x6b, 0x00, 0x01
    .byte 0x91, 0x6a, 0x00, 0x10
    .byte 0x3d, 0x60, 0x7c, 0x9b
    .byte 0x61, 0x6b, 0x03, 0xa6
    .byte 0x91, 0x6a, 0x00, 0x14
    .byte 0x3d, 0x60, 0x4c, 0x00
    .byte 0x61, 0x6b, 0x00, 0x24
    .byte 0x91, 0x6a, 0x00, 0x18
    .byte 0x7c, 0x00, 0x50, 0x6c
    .byte 0x7c, 0x00, 0x04, 0xac
    .byte 0x7c, 0x00, 0x57, 0xac
    .byte 0x39, 0x4a, 0x00, 0x20
    .byte 0x7c, 0x00, 0x50, 0x6c
    .byte 0x7c, 0x00, 0x04, 0xac
    .byte 0x7c, 0x00, 0x57, 0xac
    .byte 0x4c, 0x00, 0x01, 0x2c
    .byte 0x7f, 0xe0, 0x00, 0x08

    .org 0x100
enter_64le:
    /*
     * Now executing in 64-bit little-endian mode, still at the physical
     * load address with the MMU off.
     */

    bl      1f
1:
    mflr    30                  /* r30 = address of 1b (physical) */
    clrrdi  30,30,20            /* r30 = physical load base (1MB aligned) */

    /* r29 = phys_base - virt_base. */
    lis     6,_start@highest
    ori     6,6,_start@higher
    sldi    6,6,32
    oris    6,6,_start@high
    ori     6,6,_start@l        /* r6 = linked virtual _start */
    subf    29,6,30             /* r29 = phys - virt */

    /* Clear BSS using physical addresses. */
    lis     3,_bss_start@highest
    ori     3,3,_bss_start@higher
    sldi    3,3,32
    oris    3,3,_bss_start@high
    ori     3,3,_bss_start@l
    add     3,3,29

    lis     4,_bss_end@highest
    ori     4,4,_bss_end@higher
    sldi    4,4,32
    oris    4,4,_bss_end@high
    ori     4,4,_bss_end@l
    add     4,4,29

    li      0,0
0:
    cmpld   3,4
    bge     2f
    std     0,0(3)
    addi    3,3,8
    b       0b
2:

    /* SLOF go-direct sets r3=0; no device-tree pointer to save. */
    lis     9,__boot_dtb_ptr@highest
    ori     9,9,__boot_dtb_ptr@higher
    sldi    9,9,32
    oris    9,9,__boot_dtb_ptr@high
    ori     9,9,__boot_dtb_ptr@l
    add     9,9,29
    li      0,0
    std     0,0(9)

    /*
     * Set up a temporary physical stack while the MMU is still off.
     * After the MMU is enabled we switch to the linked virtual stack.
     */
    lis     1,_stack_end@highest
    ori     1,1,_stack_end@higher
    sldi    1,1,32
    oris    1,1,_stack_end@high
    ori     1,1,_stack_end@l
    add     1,1,29              /* r1 = physical stack top */

    /*
     * Determine the HPT size.  On pseries the hypervisor owns the HPT;
     * SDR1 is not readable by the guest.  We first try to allocate a
     * small HPT of our own with H_RESIZE_HPT_PREPARE/COMMIT so we know
     * the exact size.  If that fails (resize disabled), fall back to
     * the size QEMU uses for 1 GiB of RAM (htab_shift = 23).
     */
    li      24,0x3ff            /* default pteg mask for shift=18 (1024 PTEGs) */
    li      3,PPC64_H_RESIZE_HPT_PREPARE
    li      4,0
    li      5,18                /* 256KB HPT = 2^11 PTEGs */
    sc      1
    cmpdi   3,0                 /* H_SUCCESS */
    beq     1f
    cmpdi   3,PPC64_H_LONG_BUSY_ORDER_1_MSEC
    beq     0f
    cmpdi   3,PPC64_H_LONG_BUSY_ORDER_10_MSEC
    beq     0f
    cmpdi   3,PPC64_H_LONG_BUSY_ORDER_100_MSEC
    beq     0f
    cmpdi   3,PPC64_H_LONG_BUSY_ORDER_1_SEC
    beq     0f
    cmpdi   3,PPC64_H_LONG_BUSY_ORDER_10_SEC
    beq     0f
    cmpdi   3,PPC64_H_LONG_BUSY_ORDER_100_SEC
    beq     0f
    b       resize_fallback

0:
    /* Spin a little waiting for prepare; then commit. */
    lis     6,1
1:
    addi    6,6,-1
    cmpdi   6,0
    bne     1b

1:
    li      3,PPC64_H_RESIZE_HPT_COMMIT
    li      4,0
    li      5,18
    sc      1
    cmpdi   3,0
    bne     resize_fallback
    b       hpt_ready

resize_fallback:
    /* 1 GiB RAM -> htab_shift=23 -> PTEG mask = (1 << 15) - 1 = 0x7FFF */
    li      24,0
    ori     24,24,0x7FFF


hpt_ready:

    /* PTE templates for 64KB pages (L=1, LP=01). */
    li      22,(PPC64_HPTE_V_LARGE | PPC64_HPTE_V_VALID)
    li      23,(PPC64_HPTE_R_R | PPC64_HPTE_R_C | PPC64_HPTE_R_M | PPC64_HPTE_R_PP)
    ori     23,23,0x1000        /* LP = 1 */

    /*
     * Map a single 64KB identity page at VA=PA=0x800000.  After we turn
     * the MMU on in-place the next few instructions execute from this
     * virtual address before we branch to the high virtual kernel.
     */
    li      14,2
    sldi    14,14,12            /* VSID << 12 (vpn high bits) */
    li      15,0
    oris    15,15,0x80          /* r15 = 0x800000 */
    li      16,0
    oris    16,16,0x80          /* r16 = 0x800000 */
    li      17,1                /* one page */
    li      18,2                /* hash seed = VSID */
    bl      map_hptes

    /*
     * Direct map all of RAM (1 GiB) at PAGE_OFFSET using 256 MB segments.
     * Each segment gets its own VSID so the page index stays within 12 bits.
     */
    /* Segment 0: VA 0xC000000000000000 -> PA 0x00000000, VSID=1 */
    li      14,1
    sldi    14,14,12
    li      15,0
    oris    15,15,0xC000
    sldi    15,15,32
    li      16,0
    li      17,4096             /* 256MB / 64KB */
    li      18,1
    bl      map_hptes

    /* Segment 1: VA 0xC000000001000000 -> PA 0x01000000, VSID=3 */
    li      14,3
    sldi    14,14,12
    li      15,0
    oris    15,15,0xC000
    sldi    15,15,32
    oris    15,15,0x0100
    li      16,0
    oris    16,16,0x0100
    li      17,4096
    li      18,3
    bl      map_hptes

    /* Segment 2: VA 0xC000000002000000 -> PA 0x02000000, VSID=4 */
    li      14,4
    sldi    14,14,12
    li      15,0
    oris    15,15,0xC000
    sldi    15,15,32
    oris    15,15,0x0200
    li      16,0
    oris    16,16,0x0200
    li      17,4096
    li      18,4
    bl      map_hptes

    /* Segment 3: VA 0xC000000003000000 -> PA 0x03000000, VSID=5 */
    li      14,5
    sldi    14,14,12
    li      15,0
    oris    15,15,0xC000
    sldi    15,15,32
    oris    15,15,0x0300
    li      16,0
    oris    16,16,0x0300
    li      17,4096
    li      18,5
    bl      map_hptes

    /* Set up SLB entries for the direct-map segments and identity window. */
    li      3,0
    slbia
    ptesync

    /* SLB[0]: ESID=0xC00000000, VSID=1, 64KB pages. */
    li      3,0
    lis     3,0xC
    sldi    3,3,16
    sldi    3,3,28
    li      5,0
    oris    5,5,PPC64_SLB_ESID_V@h
    or      3,3,5
    ori     3,3,0             /* SLB entry 0 */
    li      4,1
    sldi    4,4,12
    ori     4,4,(PPC64_SLB_VSID_KP | PPC64_SLB_VSID_L | PPC64_SLB_VSID_LP_01)
    slbmte  4,3
    isync

    /* SLB[1]: ESID=0xC00000001, VSID=3, 64KB pages. */
    li      3,0
    lis     3,0xC
    sldi    3,3,16
    ori     3,3,1
    sldi    3,3,28
    li      5,0
    oris    5,5,PPC64_SLB_ESID_V@h
    or      3,3,5
    ori     3,3,1             /* SLB entry 1 */
    li      4,3
    sldi    4,4,12
    ori     4,4,(PPC64_SLB_VSID_KP | PPC64_SLB_VSID_L | PPC64_SLB_VSID_LP_01)
    slbmte  4,3
    isync

    /* SLB[2]: ESID=0xC00000002, VSID=4, 64KB pages. */
    li      3,0
    lis     3,0xC
    sldi    3,3,16
    ori     3,3,2
    sldi    3,3,28
    li      5,0
    oris    5,5,PPC64_SLB_ESID_V@h
    or      3,3,5
    ori     3,3,2             /* SLB entry 2 */
    li      4,4
    sldi    4,4,12
    ori     4,4,(PPC64_SLB_VSID_KP | PPC64_SLB_VSID_L | PPC64_SLB_VSID_LP_01)
    slbmte  4,3
    isync

    /* SLB[3]: ESID=0xC00000003, VSID=5, 64KB pages. */
    li      3,0
    lis     3,0xC
    sldi    3,3,16
    ori     3,3,3
    sldi    3,3,28
    li      5,0
    oris    5,5,PPC64_SLB_ESID_V@h
    or      3,3,5
    ori     3,3,3             /* SLB entry 3 */
    li      4,5
    sldi    4,4,12
    ori     4,4,(PPC64_SLB_VSID_KP | PPC64_SLB_VSID_L | PPC64_SLB_VSID_LP_01)
    slbmte  4,3
    isync

    /* SLB[4]: ESID=0, VSID=2, 64KB pages (identity window). */
    lis     3,PPC64_SLB_ESID_V@h
    ori     3,3,4             /* SLB entry 4 */
    li      4,2
    sldi    4,4,12
    ori     4,4,(PPC64_SLB_VSID_KP | PPC64_SLB_VSID_L | PPC64_SLB_VSID_LP_01)
    slbmte  4,3
    isync

    ptesync
    isync

    /*
     * Turn on the MMU in-place.  The current PC is identity-mapped, so
     * fetching the next instruction with IR/DR enabled succeeds.
     */
    mfmsr   5
    li      6, PPC64_MSR_DR | PPC64_MSR_IR
    or      5, 5, 6
    mtmsrd  5
    isync

    /* Switch to the virtual stack and jump to kernel_main. */
    lis     1, _stack_end@highest
    ori     1, 1, _stack_end@higher
    sldi    1, 1, 32
    oris    1, 1, _stack_end@high
    ori     1, 1, _stack_end@l

    lis     12, kernel_main@highest
    ori     12, 12, kernel_main@higher
    sldi    12, 12, 32
    oris    12, 12, kernel_main@high
    ori     12, 12, kernel_main@l
    mtctr   12
    bctr

3:
    b       3b

    /*
     * map_hptes -- insert 64KB Hash PTEs via the H_ENTER hypercall.
     *
     * Inputs:
     *   r14  VSID << 12
     *   r15  start VA
     *   r16  start PA
     *   r17  page count
     *   r18  VSID (used in hash)
     *   r22  PTE word0 template (LARGE|VALID)
     *   r23  PTE word1 template (R/C/M/PP/LP)
     *   r24  PTEG mask
     *
     * Clobbers nothing visible to caller.
     */
map_hptes:
    mflr    0
    std     0, 16(1)
    stdu    1, -192(1)
    std     14, 8(1)
    std     15, 16(1)
    std     16, 24(1)
    std     17, 32(1)
    std     18, 40(1)
    std     19, 48(1)
    std     20, 56(1)
    std     21, 64(1)
    std     22, 72(1)
    std     23, 80(1)
    std     24, 88(1)
    std     25, 96(1)
    std     26, 104(1)

    mtctr   17
1:
    /* page index within the 256MB segment (12 bits for 64KB pages) */
    srdi    19, 15, 16
    andi.   20, 19, 0x0FFF

    /* hash = VSID ^ page_index; keep only the bits used by the HPT */
    xor     19, 20, 18
    and     5, 19, 24
    sldi    5, 5, 3             /* r5 = byte offset of the target PTEG */

    /* vpn = (VSID << 12) | page_index */
    or      20, 20, 14          /* r20 = vpn */

    /* word0 = (vpn & ~0x7F) | LARGE | VALID */
    li      21,-128
    and     20, 20, 21
    or      20, 20, 22          /* r20 = hpte_v */

    /* word1 = (PA & ~0xFFFF) | flags */
    clrrdi  21, 16, 16
    or      21, 21, 23          /* r21 = hpte_r */

    /* H_ENTER(flags=0, ptex, hpte_v, hpte_r) */
    li      3, PPC64_H_ENTER
    li      4, 0
    mr      6, 20
    mr      7, 21
    sc      1

    cmpdi   3, PPC64_H_SUCCESS
    bne     2f

    addis   15, 15, 1           /* VA += 64KB */
    addis   16, 16, 1           /* PA += 64KB */
    bdnz    1b

    ld      14, 8(1)
    ld      15, 16(1)
    ld      16, 24(1)
    ld      17, 32(1)
    ld      18, 40(1)
    ld      19, 48(1)
    ld      20, 56(1)
    ld      21, 64(1)
    ld      22, 72(1)
    ld      23, 80(1)
    ld      24, 88(1)
    ld      25, 96(1)
    ld      26, 104(1)
    addi    1, 1, 192
    ld      0, 16(1)
    mtlr    0
    blr

2:
    b       2b

    .section .bss
    .align 12
    .globl boot_pgdir
boot_pgdir:
    .space 4096

    .globl __boot_dtb_ptr
__boot_dtb_ptr:
    .quad 0

    .section .bss.stack
    .align 12
_stack_bottom:
    .space 4096 * 16
    .globl _stack_end
_stack_end:
