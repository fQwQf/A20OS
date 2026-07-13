    .syntax unified
    .cpu cortex-m3
    .thumb

    .section .text, "ax", %progbits

    .global __aeabi_uldivmod
    .thumb_func
__aeabi_uldivmod:
    push {r4-r7, lr}
    sub sp, sp, #12
    add r4, sp, #4
    str r4, [sp]
    bl __a20_armv7m_udivmod64
    ldr r2, [sp, #4]
    ldr r3, [sp, #8]
    add sp, sp, #12
    pop {r4-r7, pc}

    .global __aeabi_ldivmod
    .thumb_func
__aeabi_ldivmod:
    push {r4-r7, lr}
    sub sp, sp, #12
    add r4, sp, #4
    str r4, [sp]
    bl __a20_armv7m_sdivmod64
    ldr r2, [sp, #4]
    ldr r3, [sp, #8]
    add sp, sp, #12
    pop {r4-r7, pc}
