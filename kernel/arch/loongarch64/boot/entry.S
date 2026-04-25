.section .text.entry
.global _start

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

    /* DMW0/DMW1: identity map seg0→seg0, cached, PLV0 */
    li.d        $t0, 0x11
    csrwr       $t0, CSR_DMW0
    csrwr       $t0, CSR_DMW1

    invtlb      0, $zero, $zero

    /* CRMD = DA mode, PG=0, IE=0, PLV0 (still in DA for now) */
    li.d        $t0, 0x8
    csrwr       $t0, CSR_CRMD

    li.d        $t0, 0
    csrwr       $t0, CSR_PRMD

    li.d        $t0, 1
    csrwr       $t0, CSR_EUEN

    /* Set exception entry point */
    la.global   $t0, trap_entry_la64
    csrwr       $t0, CSR_EENTRY

    /* Set TLB refill exception entry (CSR 0x88) */
    la.global   $t0, tlb_refill_entry_la64
    csrwr       $t0, 0x88

    /* Set up boot stack */
    csrrd       $a0, CSR_CPUID
    li.d        $t0, 64 * 1024
    mul.d       $t1, $a0, $t0
    la.global   $sp, boot_stack_top
    sub.d       $sp, $sp, $t1

    /* Clear BSS */
    la.global   $t0, _bss_start
    la.global   $t1, _bss_end
bss_clear:
    bgeu        $t0, $t1, bss_done
    st.d        $zero, $t0, 0
    addi.d      $t0, $t0, 8
    b           bss_clear
bss_done:

    /* Zero out boot_pgdir (512 entries) */
    la.global   $t0, boot_pgdir
    li.d        $t1, 512
bss_pgdir:
    st.d        $zero, $t0, 0
    addi.d      $t0, $t0, 8
    addi.d      $t1, $t1, -1
    bnez        $t1, bss_pgdir

    /* Load boot_pgdir into PGDL and PGDH.
     * Kernel uses DMW direct mapping; page tables are for user space only.
     * PGDL/PGDH expect the physical address of the root page table.
     * Since PAGE_OFFSET=0 (identity mapped), VA == PA. */
    la.global   $t0, boot_pgdir
    csrwr       $t0, CSR_PGDL
    csrwr       $t0, CSR_PGDH

    /* Configure Page Walk Controller for 3-level page table.
     * Matches RocketOS configuration exactly.
     *
     * PWCL bit layout (CSR 0x1C):
     *   [5:0]   ptbase    = 12  (page offset bits, 4KB pages)
     *   [11:6]  ptwidth   = 9   (PT index width, 512 entries)
     *   [17:12] dir1_base = 21  (12 + 9)
     *   [23:18] dir1_width= 9   (512 entries)
     *   [29:24] dir2_base = 30  (12 + 9 + 9)
     *   [31:30] = 0
     *
     * PWCH (CSR 0x1D) = 0:
     *   No dir3/dir4 levels.  PTEWIDTH[31:30]=0 → 8-byte PTEs.
     */
    li.d        $t0, (12 << 0) | (9 << 5) | (21 << 10) | (9 << 15) | (30 << 20) | (9 << 25)
    csrwr       $t0, CSR_PWCL
    li.d        $t0, 0
    csrwr       $t0, CSR_PWCH

    /* STLBPS: set page size = 4KB (PS=12) for STLB entries.
     * CSR 0x1E, bits [5:0] = PS value.
     * Without this, STLBPS.PS defaults to 0 and TLB entries go to
     * MTLB instead of STLB (works, but suboptimal; also matches RocketOS). */
    li.d        $t0, 12
    csrwr       $t0, 0x1E              /* CSR_STLBPS */

    /* Enable paging: CRMD.PG=1, DA=0, IE=1, PLV=0
     *   DATF=01 (CC) bit [6:5]  — coherent cached for fetch
     *   DATM=01 (CC) bit [8:7]  — coherent cached for memory
     * CRMD = (1<<4)|(1<<2)|(1<<5)|(1<<7) = PG + IE + DATF_CC + DATM_CC = 0xB4
     * Wait: bit5=DATF[0], bit6=DATF[1]. 01 means only bit5 set.
     *       bit7=DATM[0], bit8=DATM[1]. 01 means only bit7 set.
     * So DATF=01 → bit5=1, bit6=0 → (1<<5) = 0x20
     *    DATM=01 → bit7=1, bit8=0 → (1<<7) = 0x80
     * Total = PG(0x10) + IE(0x04) + DATF(0x20) + DATM(0x80) = 0xB4
     *
     * Hmm, that IS 0xB4. But actual DATF[1:0] at bits [6:5]:
     *   bit5=1, bit6=0 → DATF = 01? No! bits [6:5] means bit6 is high bit, bit5 is low bit.
     *   So DATF = (bit6 << 1) | bit5 = (0 << 1) | 1 = 1 = CC. Correct!
     * And DATM = (bit8 << 1) | bit7 = (0 << 1) | 1 = 1 = CC. Correct!
     *
     * So 0xB4 already has DATF=01 (CC) and DATM=01 (CC). The CRMD is correct.
     */
    li.d        $t0, 0xB4
    csrwr       $t0, CSR_CRMD

    invtlb      0, $zero, $zero

    csrrd       $a0, CSR_CPUID
    li.d        $a1, 0x100000

    bl          kernel_main

halt:
    idle    0
    b           halt

    .section .bss.stack
    .globl boot_stack
boot_stack:
    .space 64 * 1024 * 4
    .globl boot_stack_top
boot_stack_top:

    .section .bss
    .globl boot_pgdir
    .align 4
boot_pgdir:
    .space 512 * 8
