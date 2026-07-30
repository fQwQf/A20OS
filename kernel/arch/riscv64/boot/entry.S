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
    /* a0 = hart ID, a1 = DTB pointer (passed by OpenSBI).
     * Only the boot hart runs global initialization; others wait for release.
     * BSS is cleared below, so boot arguments are saved after that. */

    /* OpenSBI may choose any hart as the cold-boot hart.  Elect exactly one
     * initializer without touching BSS, which that hart clears below. */
    la      t0, __boot_hart_claim
.Lclaim_boot:
    lr.d    t1, 0(t0)
    bgez    t1, .secondary_wait
    sc.d    t2, a0, 0(t0)
    bnez    t2, .Lclaim_boot

    la      sp, _stack_end

    la      t0, _bss_start
    la      t1, _bss_end
.clear_bss:
    bgeu    t0, t1, .done_bss
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       .clear_bss
.done_bss:

    /* Save boot arguments now that BSS is clean. */
    la      t0, __boot_hart_id
    sd      a0, 0(t0)
    la      t0, __boot_dtb_ptr
    sd      a1, 0(t0)

    /*
     * Fill the boot page directory with Sv39 1GB megapages:
     *   pgdir[2..9]     → PA 0x80000000..0x27fffffff (identity RAM)
     *   pgdir[256]      → PA 0x00000000              (HH MMIO)
     *   pgdir[258..265] → PA 0x80000000..0x27fffffff (HH RAM)
     *
     * QEMU places the DTB near the top of RAM.  The final uses -m 8G, so a
     * single RAM megapage leaves the DTB inaccessible before C code can parse
     * the memory node.  Mapping the competition maximum here is safe when the
     * VM has less RAM: the allocator later uses the DTB-reported range and
     * never allocates pages beyond actual memory.
     *
     * PTE = (PPN << 10) | flags
     * flags = V|R|W|X|A|D = 0xAF
     */
#ifndef CONFIG_NOMMU
    la      t0, boot_pgdir

    /* Map eight 1GB RAM windows in both the identity and higher-half slots. */
    li      t1, 0x200000AF
    li      t2, 2
    li      t4, 10
.Lmap_boot_ram:
    slli    t3, t2, 3
    add     t3, t0, t3
    sd      t1, 0(t3)

    addi    t5, t2, 256
    slli    t5, t5, 3
    add     t5, t0, t5
    sd      t1, 0(t5)

    li      t6, 0x10000000
    add     t1, t1, t6
    addi    t2, t2, 1
    bltu    t2, t4, .Lmap_boot_ram

    /* Higher-half MMIO maps PA 0..1GB. */
    li      t1, 0x000000AF
    li      t2, 256*8
    add     t3, t0, t2
    sd      t1, 0(t3)

    /* QEMU virt PCIe MMIO window: PA 1GB..2GB. */
    li      t1, 0x100000AF
    li      t2, 257*8
    add     t3, t0, t2
    sd      t1, 0(t3)

    j       .enable_mmu

.enable_mmu:
    /* satp = (MODE=8 << 60) | (boot_pgdir_phys >> 12) */
    la      t0, boot_pgdir
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
#else
    la      t0, .Lhh_entry
    jr      t0
#endif

.secondary_wait:
    /* CPU 0 clears BSS before publishing this flag.  Secondary harts need no
     * stack while parked, so they cannot race global C initialization. */
    la      t0, riscv64_smp_release
.Lsecondary_wait:
    ld      t1, 0(t0)
    bnez    t1, .Lsecondary_released
    nop
    j       .Lsecondary_wait
.Lsecondary_released:
    fence   rw, rw
    /* Resolve the platform-published hardware ID mapping. This also supports
     * firmware that entered every hart before the boot CPU initialized SMP;
     * such harts wait until their topology slot becomes valid. */
.Lsecondary_resolve:
    la      t0, riscv64_smp_context_valid
    lw      t1, 0(t0)
    la      t2, riscv64_smp_context_hw_id
    li      t3, 1
.Lsecondary_scan:
    li      t4, CONFIG_NR_CPUS
    bgeu    t3, t4, .Lsecondary_wait
    li      t4, 1
    sll     t4, t4, t3
    and     t4, t4, t1
    beqz    t4, .Lsecondary_next
    slli    t4, t3, 3
    add     t4, t2, t4
    ld      t4, 0(t4)
    beq     a0, t4, .Lsecondary_found
.Lsecondary_next:
    addi    t3, t3, 1
    j       .Lsecondary_scan
.Lsecondary_found:
    mv      s0, t3
#ifndef CONFIG_NOMMU
    j       .enable_mmu
#else
    la      t0, .Lhh_entry
    jr      t0
#endif

.Lhh_entry:
    /* The bootstrap CPU is logical CPU 0. Secondaries consume the logical ID
     * selected by the platform and passed through SBI HSM's opaque value. */
    la      t0, __boot_hart_id
    ld      t1, 0(t0)
    beq     a0, t1, .Lboot_logical_id
    mv      a0, s0
    j       .Llogical_id_ready
.Lboot_logical_id:
    li      a0, 0
.Llogical_id_ready:
    li      t0, CONFIG_NR_CPUS
    bgeu    a0, t0, .park
    la      sp, _stack_end
    li      t0, 65536
    mul     t0, a0, t0
    sub     sp, sp, t0

    la      t0, __trap_from_kernel
    csrw    stvec, t0

    /* Ensure the boot thread enters kernel_main with SPP=1 so that a
     * supervisor-mode trap/interrupt returns to supervisor mode. */
    li      t0, 0x100
    csrs    sstatus, t0

    beqz    a0, .Lboot_main
    call    riscv64_secondary_entry
    j       .park
.Lboot_main:
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
    .space  4096 * 16 * CONFIG_NR_CPUS
    .globl _stack_end
_stack_end:

    /* 启动时保存的 hart ID 与 DTB 指针，由 entry.S _start 写入 */
    .globl __boot_hart_id
__boot_hart_id:
    .dword 0
    .globl __boot_dtb_ptr
__boot_dtb_ptr:
    .dword 0

    .section .data
    .align 3
    .globl __boot_hart_claim
__boot_hart_claim:
    .dword -1
