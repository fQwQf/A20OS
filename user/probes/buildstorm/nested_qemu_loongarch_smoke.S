/*
 * Minimal direct-boot guest for proving that qemu-system-loongarch64 can
 * execute TCG code inside A20OS.  It needs no firmware, stack, libc, timer,
 * block device, or allocator: one vCPU writes a decisive marker to the first
 * ns16550 UART of QEMU's LoongArch virt machine and then spins.
 */

    .section .text.entry, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    la.local $t0, marker
    li.d     $t1, 0x1fe001e0
1:
    ld.bu    $t2, $t0, 0
    beqz     $t2, 3f
    st.b     $t2, $t1, 0
    addi.d   $t0, $t0, 1
    b        1b
3:
    b        3b
    .size _start, . - _start

    .section .rodata, "a", @progbits
marker:
    .asciz "A20_NESTED_QEMU_LOONGARCH_TCG_OK\n"
