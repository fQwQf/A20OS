    .section .text.entry
    .globl _start
_start:
    bnez    a0, .park

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

    .align 12
_stack_bottom:
    .space 4096 * 16
    .globl _stack_end
_stack_end:

    .globl __boot_hart_id
__boot_hart_id:
    .word 0
    .globl __boot_dtb_ptr
__boot_dtb_ptr:
    .word 0
