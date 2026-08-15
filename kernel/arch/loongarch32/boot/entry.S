.section .text.entry
.global _start

/* LoongArch32 CSR numbers (NaiLoong Core) */
.equ CSR_CRMD,    0x0
.equ CSR_PRMD,    0x1
.equ CSR_ECFG,    0x4
.equ CSR_ESTAT,   0x5
.equ CSR_ERA,     0x6
.equ CSR_BADV,    0x7
.equ CSR_EENTRY,  0xC
.equ CSR_TLBIDX,  0x10
.equ CSR_TLBEHI,  0x11
.equ CSR_TLBELO0, 0x12
.equ CSR_TLBELO1, 0x13
.equ CSR_ASID,    0x18
.equ CSR_PGDL,    0x19
.equ CSR_CPUID,   0x20
.equ CSR_SAVE0,   0x30
.equ CSR_SAVE1,   0x31
.equ CSR_SAVE2,   0x32
.equ CSR_SAVE3,   0x33
.equ CSR_TICLR,   0x44
.equ CSR_TLBRENTRY, 0x88
.equ CSR_DMW0,    0x180
.equ CSR_DMW1,    0x181

_start:
    /*
     * Boot: CPU starts in DA (Direct Address) mode, PLV0.  DRAM is the
     * 512 MiB window at 0x80000000; MMIO lives in the 0x1C000000 window.
     * DMW0 maps 0x80000000 (VA seg 4 → PA seg 4) cached for the kernel;
     * DMW1 maps 0x1C000000 (VA seg 7 → PA seg 7) uncached for peripherals.
     */

    /* DMW0: vseg=4, pseg=4, MAT=CC, PLV0 → identity 0x80000000-0x9FFFFFFF */
    li.w        $t0, 0x88000011
    csrwr       $t0, CSR_DMW0

    /* DMW1: vseg=0, pseg=0, MAT=00 (uncached), PLV0 → identity
     * 0x00000000-0x1FFFFFFF (NaiLoong SoC MMIO, UART at 0x1FE001E0). */
    li.w        $t0, 0x00000001
    csrwr       $t0, CSR_DMW1

    invtlb      0, $zero, $zero

    /* CRMD = DA mode, PG=0, IE=0 (reset value 0x8, set explicitly) */
    li.w        $t0, 0x8
    csrwr       $t0, CSR_CRMD

    li.w        $t0, 0
    csrwr       $t0, CSR_PRMD

    /* General exception entry */
    la.global   $t0, trap_entry_la32
    csrwr       $t0, CSR_EENTRY

    /* TLB refill exception entry (CSR 0x88) */
    la.global   $t0, tlb_refill_entry_la32
    csrwr       $t0, CSR_TLBRENTRY

    /* Select a private early stack.  NaiLoong is single-core; keep the
     * per-CPU layout so the code survives a multi-core SoC. */
    csrrd       $a0, CSR_CPUID
    li.w        $t0, CONFIG_NR_CPUS
    bgeu        $a0, $t0, early_halt
    li.w        $t0, 64 * 1024
    mul.w       $t1, $a0, $t0
    la.global   $sp, boot_stack_top
    sub.w       $sp, $sp, $t1

    /* Clear BSS */
    la.global   $t0, _bss_start
    la.global   $t1, _bss_end
bss_clear:
    bgeu        $t0, $t1, bss_done
    st.w        $zero, $t0, 0
    addi.w      $t0, $t0, 4
    b           bss_clear
bss_done:

    /* Zero out boot_pgdir (1024 entries) */
    la.global   $t0, boot_pgdir
    li.w        $t1, 1024
bss_pgdir:
    st.w        $zero, $t0, 0
    addi.w      $t0, $t0, 4
    addi.w      $t1, $t1, -1
    bnez        $t1, bss_pgdir

    /*
     * PGDL holds the physical address of the root page table (bits [31:12]).
     * PAGE_OFFSET == 0 so VA == PA for kernel accesses via DMW0.
     */
    la.global   $t0, boot_pgdir
    csrwr       $t0, CSR_PGDL

    /* ASID must match the ASID the MMU compares against during TLB hits.
     * Keep it 0 for now; arch_make_addr_space_token does not allocate ASIDs. */
    li.w        $t0, 0
    csrwr       $t0, CSR_ASID

    /*
     * Enable paging: CRMD = PG | DATF=CC | DATM=CC, IE=0.
     *   0xB0 = PG(bit4) | DATF=01(bit5) | DATM=01(bit7)
     */
    li.w        $t0, 0xB0
    csrwr       $t0, CSR_CRMD

    invtlb      0, $zero, $zero

    /* CPU0: enable interrupts and enter the kernel. */
    li.w        $t0, 0xB4
    csrwr       $t0, CSR_CRMD
    bl          kernel_main
    b           halt

halt:
    idle        0
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
    .space 1024 * 4
