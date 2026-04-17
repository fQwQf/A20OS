/*
 * A20OS kernel entry point for RISC-V 64-bit (Higher-Half Kernel)
 *
 * OpenSBI loads kernel.elf at PA 0x80200000 and jumps there in S-mode.
 * The kernel is linked at VA 0xFFFFFFC080200000.
 *
 * With mcmodel=medany, 'la' expands to auipc+addi (PC-relative).
 * When executing at PA, auipc gives PA-based addresses → la gives PA.
 * When executing at VA (after MMU on), auipc gives VA → la gives VA.
 * So la works correctly in both contexts without any fixup.
 *
 * Boot sequence:
 *   1. Set up stack and clear BSS using la (gives PA)
 *   2. Fill boot_pgdir with identity + HH megapage entries
 *   3. Enable Sv39 MMU (both mappings active)
 *   4. Jump to .Lhh_entry: la gives PA, add PAGE_OFFSET for VA
 *   5. Re-initialize SP with la (now gives VA)
 *   6. Set stvec, call kernel_main
 */

    .section .text.entry
    .globl _start
_start:
    bnez    a0, .park

    la      sp, _stack_end

    la      t0, _bss_start
    la      t1, _bss_end
.clear_bss:
    bgeu    t0, t1, .done_bss
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       .clear_bss
.done_bss:

    /*
     * Fill boot page directory with 3 Sv39 1GB megapages:
     *   pgdir[2]   → PA 0x80000000  (identity RAM)
     *   pgdir[256] → PA 0x00000000  (HH MMIO)
     *   pgdir[258] → PA 0x80000000  (HH RAM)
     *
     * PTE = (PPN << 10) | flags
     * flags = V|R|W|X|A|D = 0xAF
     */
    la      t0, boot_pgdir

    li      t1, 0x200000AF
    li      t2, 2*8
    add     t3, t0, t2
    sd      t1, 0(t3)

    li      t1, 0x000000AF
    li      t2, 256*8
    add     t3, t0, t2
    sd      t1, 0(t3)

    li      t1, 0x200000AF
    li      t2, 258*8
    add     t3, t0, t2
    sd      t1, 0(t3)

    /* satp = (MODE=8 << 60) | (boot_pgdir_phys >> 12) */
    li      t2, 8
    slli    t2, t2, 60
    srli    t0, t0, 12
    or      t0, t0, t2
    csrw    satp, t0
    sfence.vma

    /*
     * Jump to HH entry.  la gives PA (identity mapping still active),
     * add PAGE_OFFSET to get the HH VA.
     */
    la      t0, .Lhh_entry
    li      t1, 0xFFFFFFC000000000
    add     t0, t0, t1
    jr      t0

.Lhh_entry:
    /* Now at HH VA. la gives VA. Fix SP to VA. */
    la      sp, _stack_end

    la      t0, __trap_from_kernel
    csrw    stvec, t0

    call    kernel_main

.park:
    wfi
    j       .park

    .section .bss
    .align 12
    .globl boot_pgdir
boot_pgdir:
    .space  4096

    .align 12
_stack_bottom:
    .space  4096 * 16
    .globl _stack_end
_stack_end:
