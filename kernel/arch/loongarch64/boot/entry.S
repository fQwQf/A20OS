.section .text.entry
.global _start

#include "mmu_layout.h"

.equ CSR_CRMD,    0x0
.equ CSR_PRMD,    0x1
.equ CSR_EUEN,    0x2
.equ CSR_ECFG,    0x4
.equ CSR_ESTAT,   0x5
.equ CSR_ERA,     0x6
.equ CSR_BADV,    0x7
.equ CSR_EENTRY,  0xC
.equ CSR_TLBIDX,  0x10
.equ CSR_PGDL,    0x19
.equ CSR_PGDH,    0x1a
.equ CSR_PWCL,    0x1c
.equ CSR_PWCH,    0x1d
.equ CSR_CPUID,   0x20
.equ CSR_SAVE0,   0x30
.equ CSR_SAVE1,   0x31
.equ CSR_DMW0,    0x180
.equ CSR_DMW1,    0x181

_start:
    /*
     * Boot: CPU starts in DA (Direct Address) mode, PLV0.
     * We set up DMW identity mapping, then switch to PG (paging) mode.
     * DMW mappings remain active in PG mode for kernel direct access.
     */

    /*
     * LS2K1000 U-Boot enters through the cached VSEG 9 DMW.  Reinstall the
     * firmware's standard uncached/cached pair without ever dropping the
     * mapping that contains the current PC.  QEMU keeps its identity DMW.
     */
#ifdef CONFIG_BOARD_LS2K1000
    li.d        $t0, 0x800000000000000f
    csrwr       $t0, CSR_DMW0
    li.d        $t0, 0x900000000000001f
    csrwr       $t0, CSR_DMW1
#else
    li.d        $t0, 0x11
    csrwr       $t0, CSR_DMW0
    csrwr       $t0, CSR_DMW1
#endif

    invtlb      0, $zero, $zero

#ifdef CONFIG_BOARD_LS2K1000
    /* U-Boot already runs with paging and the cached DMW active. */
    li.d        $t0, 0xB0
#else
    /* CRMD = DA mode, PG=0, IE=0, PLV0 (still in DA for now) */
    li.d        $t0, 0x8
#endif
    csrwr       $t0, CSR_CRMD

    li.d        $t0, 0
    csrwr       $t0, CSR_PRMD

    /*
     * FPEN | LSXEN.  The final LoongArch glibc image executes LSX
     * instructions in its dynamic loader before main(), so every CPU must
     * enable LSX before it can enter userspace.
     */
    li.d        $t0, 3
    csrwr       $t0, CSR_EUEN

    /* Set exception entry point */
    la.global   $t0, trap_entry_la64
    csrwr       $t0, CSR_EENTRY

    /* Set TLB refill exception entry (CSR 0x88) */
    la.global   $t0, tlb_refill_entry_la64
    csrwr       $t0, 0x88

    /* Select a private early stack before touching any shared state. */
    csrrd       $a0, CSR_CPUID
    li.d        $t0, CONFIG_NR_CPUS
    bgeu        $a0, $t0, early_halt
    li.d        $t0, 64 * 1024
    mul.d       $t1, $a0, $t0
    la.global   $sp, boot_stack_top
    sub.d       $sp, $sp, $t1

    /* QEMU's reset ROM starts only CPU 0 here directly.  Secondary CPUs are
     * released later through IOCSR mailbox 0 and must not clear global BSS. */
    bnez        $a0, secondary_start

    /* Clear BSS */
    la.global   $t0, _bss_start
    la.global   $t1, _bss_end
bss_clear:
    bgeu        $t0, $t1, bss_done
    st.d        $zero, $t0, 0
    addi.d      $t0, $t0, 8
    b           bss_clear
bss_done:
    /* Preserve the firmware DTB pointer after clearing BSS. */
#ifdef CONFIG_BOARD_LS2K1000
    li.d        $t0, 0x900000000a000000
#else
    li.d        $t0, 0x100000
#endif
    la.global   $t1, __boot_dtb_ptr
    st.d        $t0, $t1, 0

    /* Zero out boot_pgdir (512 entries) */
    la.global   $t0, boot_pgdir
    li.d        $t1, 512
bss_pgdir:
    st.d        $zero, $t0, 0
    addi.d      $t0, $t0, 8
    addi.d      $t1, $t1, -1
    bnez        $t1, bss_pgdir

    b           setup_mmu

secondary_start:
    /* The mailbox write and boot IPI are ordered by the bootstrap CPU. */
    dbar        0

setup_mmu:

    /* Load the physical boot_pgdir address into PGDL and PGDH. */
    la.global   $t0, boot_pgdir
#ifdef CONFIG_BOARD_LS2K1000
    li.d        $t1, 0x9000000000000000
    sub.d       $t0, $t0, $t1
#endif
    csrwr       $t0, CSR_PGDL
    csrwr       $t0, CSR_PGDH

    /* Configure the Page Walk Controller from the board's MMU profile.
     * QEMU keeps Dir2 folded; LS2K1000LA uses Dir3/Dir2/Dir1/PT.
     *
     * PWCL bit layout (CSR 0x1C):
     *   [4:0]   ptbase     = 12  (page offset bits, 4KB pages)
     *   [9:5]   ptwidth    = 9   (PT index width, 512 entries)
     *   [14:10] dir1_base  = 21  (PMD index)
     *   [19:15] dir1_width = 9
     *   [24:20] dir2_base  = 30 for the four-level profile
     *   [29:25] dir2_width = 9, or both fields zero when folded
     *   [31:30] pte_width  = 0   (8-byte PTEs)
     *
     * PWCH bit layout (CSR 0x1D):
     *   [5:0]   dir3_base  = 30 (QEMU) or 39 (LS2K1000LA)
     *   [11:6]  dir3_width = 9
     *   dir4 is folded (zero).
     */
    li.d        $t0, LA64_PWCL_VALUE
    csrwr       $t0, CSR_PWCL
    li.d        $t0, LA64_PWCH_VALUE
    csrwr       $t0, CSR_PWCH

    /* STLBPS: set page size = 4KB (PS=12) for STLB entries.
     * CSR 0x1E, bits [5:0] = PS value.
     * Without this, STLBPS.PS defaults to 0 and TLB entries go to
     * MTLB instead of STLB (works, but suboptimal; also matches RocketOS). */
    li.d        $t0, 12
    csrwr       $t0, 0x1E              /* CSR_STLBPS */

    /* TLBREHI.PS controls entries created by the refill path.  Linux sets
     * this independently of STLBPS; leaving it at the firmware value makes
     * the first user TLB miss implementation-dependent. */
    li.d        $t0, 12
    csrwr       $t0, 0x8E              /* CSR_TLBREHI */

    /* Enable paging: CRMD.PG=1, DA=0, IE=0, PLV=0
     *   DATF=01 (CC) bit [6:5]  — coherent cached for fetch
     *   DATM=01 (CC) bit [8:7]  — coherent cached for memory
     * The common path uses 0xB0 with IE clear. CPU 0 restores its historical
     * 0xB4 value before kernel_main; secondary CPUs enable IE after local init.
     * Wait: bit5=DATF[0], bit6=DATF[1]. 01 means only bit5 set.
     *       bit7=DATM[0], bit8=DATM[1]. 01 means only bit7 set.
     * So DATF=01 → bit5=1, bit6=0 → (1<<5) = 0x20
     *    DATM=01 → bit7=1, bit8=0 → (1<<7) = 0x80
     * PG(0x10) + DATF(0x20) + DATM(0x80) = 0xB0.
     *
     * Hmm, that IS 0xB4. But actual DATF[1:0] at bits [6:5]:
     *   bit5=1, bit6=0 → DATF = 01? No! bits [6:5] means bit6 is high bit, bit5 is low bit.
     *   So DATF = (bit6 << 1) | bit5 = (0 << 1) | 1 = 1 = CC. Correct!
     * And DATM = (bit8 << 1) | bit7 = (0 << 1) | 1 = 1 = CC. Correct!
     *
     * Both 0xB0 and 0xB4 have DATF=01 (CC) and DATM=01 (CC).
     */
    li.d        $t0, 0xB0
    csrwr       $t0, CSR_CRMD

    invtlb      0, $zero, $zero

    csrrd       $a0, CSR_CPUID
#ifdef CONFIG_BOARD_LS2K1000
    li.d        $a1, 0x900000000a000000
#else
    li.d        $a1, 0x100000
#endif

    bnez        $a0, call_secondary
    /* The physical board's 100 MHz timer can fire while the allocator and
     * networking globals are still being initialized.  Keep IE clear until
     * kernel_main has initialized the process and progress subsystems. */
#ifdef CONFIG_BOARD_LS2K1000
    li.d        $t0, 0xB0
#else
    li.d        $t0, 0xB4
#endif
    csrwr       $t0, CSR_CRMD
    bl          kernel_main
    b           halt

call_secondary:
    bl          loongarch64_secondary_entry

halt:
    idle    0
    b           halt

early_halt:
    idle        0
    b           early_halt

    .section .bss.stack
    .globl boot_stack
boot_stack:
    .space 64 * 1024 * CONFIG_NR_CPUS
    .globl boot_stack_top
boot_stack_top:

    .section .bss
    .globl boot_pgdir
    .align 4
boot_pgdir:
    .space 512 * 8

    .globl __boot_dtb_ptr
__boot_dtb_ptr:
    .dword 0
