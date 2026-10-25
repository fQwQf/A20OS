    .section .text.entry
    .globl _start
_start:
    mv      tp, zero
    /* OpenSBI may elect any hart for cold boot. Only that hart runs _start;
     * HSM secondaries use riscv32_secondary_start below. */

    la      sp, _stack_end

    la      t0, _bss_start
    la      t1, _bss_end
1:
    bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:
    la      t0, __boot_hart_id
    sw      a0, 0(t0)
    la      t0, __boot_dtb_ptr
    sw      a1, 0(t0)

#ifndef CONFIG_NOMMU
    la      t0, boot_pgdir
    li      t1, 512
    li      t2, 0x80000000
3:
    li      t3, 768
    bgeu    t1, t3, 4f
    srli    t4, t2, 12
    slli    t4, t4, 10
    li      t5, 0xCF
    or      t4, t4, t5
    slli    t6, t1, 2
    add     t6, t0, t6
    sw      t4, 0(t6)
    addi    t1, t1, 1
    li      t6, 0x400000
    add     t2, t2, t6
    j       3b
4:
    li      t1, 8
    li      t2, 0x02000000
    call    .Lmap_leaf
    li      t1, 48
    li      t2, 0x0C000000
    call    .Lmap_leaf
    li      t1, 64
    li      t2, 0x10000000
    call    .Lmap_leaf

    /*
     * Mirror supervisor MMIO into the upper half so every task page table
     * copied from boot_pgdir keeps UART/PLIC/virtio/timer access after the
     * first SATP switch.
     */
    li      t1, 776
    li      t2, 0x02000000
    call    .Lmap_leaf
    li      t1, 816
    li      t2, 0x0C000000
    call    .Lmap_leaf
    li      t1, 832
    li      t2, 0x10000000
    call    .Lmap_leaf

    la      t0, boot_pgdir
    srli    t0, t0, 12
    li      t1, 1
    slli    t1, t1, 31
    or      t0, t0, t1
    csrw    satp, t0
    sfence.vma
#else
    csrwi   satp, 0
    sfence.vma
#endif

    la      t0, __trap_from_kernel
    csrw    stvec, t0
    call    kernel_main

.park:
    wfi
    j       .park

.Lmap_leaf:
    srli    t3, t2, 12
    slli    t3, t3, 10
    li      t4, 0xCF
    or      t3, t3, t4
    slli    t4, t1, 2
    add     t4, t0, t4
    sw      t3, 0(t4)
    ret

    .section .bss
    .align 12
    .globl boot_pgdir
boot_pgdir:
    .space 4096

    .globl __boot_hart_id
__boot_hart_id:
    .word 0
    .globl __boot_dtb_ptr
__boot_dtb_ptr:
    .word 0

/* BSP boot stack, 64 KiB.  .bss.stack is collected first by
 * boot/ldscript.ld, so this range sits at the bottom of .bss and no other
 * statically allocated kernel object can be placed inside it; it also stays
 * below _bss_end, which is where the physical frame allocator's reserved
 * region begins (mm_init -> pfa_init(va_to_pa(_bss_end))). */
    .section .bss.stack
    .align 12
    .globl _stack_bottom
_stack_bottom:
    .space 4096 * 16 * CONFIG_NR_CPUS
    .globl _stack_end
_stack_end:

    .section .text.entry
    .align 2
    .globl riscv32_secondary_start
riscv32_secondary_start:
    mv      tp, zero
    /* SBI passes logical CPU id in a1. Boot page tables are published before
     * hart_start, so secondaries only install the already-built root. */
    mv      s0, a1
    la      sp, _stack_end
    li      t0, 65536
    mul     t0, s0, t0
    sub     sp, sp, t0
#ifndef CONFIG_NOMMU
    la      t0, boot_pgdir
    srli    t0, t0, 12
    li      t1, 0x80000000
    or      t0, t0, t1
    csrw    satp, t0
    sfence.vma
#endif
    la      t0, __trap_from_kernel
    csrw    stvec, t0
    li      t0, 0x6100
    csrs    sstatus, t0
    mv      a0, s0
    call    riscv32_secondary_entry
    j       .park
