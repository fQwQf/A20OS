    .syntax unified
    .cpu cortex-m3
    .thumb

    .section .isr_vector, "a", %progbits
    .align 8
    .global armv7m_vector_table
armv7m_vector_table:
    .word _stack_top
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler
    .rept 37
    .word Default_Handler
    .endr
    .word USART1_IRQHandler
    .word Default_Handler
    .word USART3_IRQHandler
    .rept 29
    .word Default_Handler
    .endr

    .section .text.entry, "ax", %progbits
    .thumb_func
    .global Reset_Handler
Reset_Handler:
    /*
     * A physical reset loads MSP from vector[0].  Some low-cost CMSIS-DAP
     * probes resume by changing PC only, so establish MSP here as well.
     */
    ldr sp, =_stack_top
    ldr r0, =_data_load
    ldr r1, =_data_start
    ldr r2, =_data_end
1:
    cmp r1, r2
    bcs 2f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 1b
2:
    ldr r0, =_bss_start
    ldr r1, =_bss_end
    movs r2, #0
3:
    cmp r0, r1
    bcs 4f
    str r2, [r0], #4
    b 3b
4:
    /*
     * Fill the reserved stack before its first C use. The memory monitor
     * scans this pattern to report the real high-water mark.
     */
    ldr r0, =_stack_limit
    ldr r1, =_stack_top
    ldr r2, =0xA55AA55A
6:
    cmp r0, r1
    bcs 7f
    str r2, [r0], #4
    b 6b
7:
    ldr r0, =armv7m_vector_table
    ldr r1, =0xE000ED08
    str r0, [r1]
    dsb
    isb
    bl kernel_main
5:
    wfi
    b 5b

    .thumb_func
NMI_Handler:
    b Default_Handler

    .thumb_func
DebugMon_Handler:
    b Default_Handler

    .thumb_func
Default_Handler:
    bl armv7m_default_handler
    b .

    .thumb_func
HardFault_Handler:
    movs r0, #3
    b armv7m_fault_entry

    .thumb_func
MemManage_Handler:
    movs r0, #4
    b armv7m_fault_entry

    .thumb_func
BusFault_Handler:
    movs r0, #5
    b armv7m_fault_entry

    .thumb_func
UsageFault_Handler:
    movs r0, #6
    b armv7m_fault_entry

    .thumb_func
armv7m_fault_entry:
    tst lr, #4
    ite eq
    mrseq r1, msp
    mrsne r1, psp
    b armv7m_fault_handler

    .thumb_func
SVC_Handler:
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    push {lr}
    bl armv7m_svc_handler
    pop {pc}

    .thumb_func
PendSV_Handler:
    bx lr

    .thumb_func
SysTick_Handler:
    push {lr}
    bl armv7m_systick_handler
    pop {pc}

    .thumb_func
USART1_IRQHandler:
    push {lr}
    bl armv7m_usart1_irq_handler
    pop {pc}

    .thumb_func
USART3_IRQHandler:
    push {lr}
    bl armv7m_usart3_irq_handler
    pop {pc}

    .global __switch
    .thumb_func
__switch:
    bx lr

    .global user_trap_return
    .global __trap_from_user
    .global __return_to_user
    .global __trap_from_kernel
    .thumb_func
user_trap_return:
__trap_from_user:
__return_to_user:
__trap_from_kernel:
    bx lr
