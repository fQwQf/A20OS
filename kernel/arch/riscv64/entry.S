/*
 * A20OS kernel entry point for RISC-V 64-bit
 * QEMU virt machine with OpenSBI loads kernel at 0x80200000
 * We run in physical address space (no virtual memory mapping)
 */

    .section .text.entry
    .globl _start
_start:
    /* Only let hart 0 proceed, park other harts */
    /* OpenSBI passes hartid in a0 */
    bnez    a0, .park

    /* Set up stack pointer */
    la      sp, _stack_end

    /* Clear BSS section */
    la      t0, _bss_start
    la      t1, _bss_end
.clear_bss:
    bgeu    t0, t1, .done_bss
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       .clear_bss
.done_bss:

    /* Set up initial trap vector (kernel trap mode) */
    la      t0, __trap_from_kernel
    csrw    stvec, t0

    /* Jump to C kernel main */
    call    kernel_main

    /* Should never return, but halt if it does */
.park:
    wfi
    j       .park

/*
 * Boot stack - 16KB, used only during early init before
 * the kernel heap and task stacks are allocated
 */
    .section .bss
    .align 12
_stack_bottom:
    .space  4096 * 16
    .globl _stack_end
_stack_end:
