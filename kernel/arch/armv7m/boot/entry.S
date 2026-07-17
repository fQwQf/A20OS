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
    .rept 23
    .word Default_Handler
    .endr
    .word EXTI9_5_IRQHandler        /* IRQ 23: EXTI lines 5..9 (IR on PB9) */
    .rept 13
    .word Default_Handler
    .endr
    .word USART1_IRQHandler         /* IRQ 37 */
    .word USART2_IRQHandler         /* IRQ 38 */
    .word USART3_IRQHandler         /* IRQ 39 */
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
    /*
     * Do not switch MSP while an exception frame belongs to the interrupted
     * task. The C hook rewrites the stacked PC to a thread-mode trampoline;
     * that trampoline safely enters the generic scheduler on the task stack.
     */
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    push {lr}
    bl armv7m_pendsv_prepare
    pop {lr}
    bx lr

    .global armv7m_preempt_trampoline
    .thumb_func
armv7m_preempt_trampoline:
    /* Preserve all hardware-stacked volatile registers while proc_yield()
     * follows the normal AAPCS and __switch path. */
    push {r0-r3, r12, lr}
    bl proc_yield
    pop {r0-r3, r12, lr}
    /* The SVC restores the saved interrupted PC without consuming a register. */
    svc #0x20
    b .

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
USART2_IRQHandler:
    push {lr}
    bl armv7m_usart2_irq_handler
    pop {pc}

    .thumb_func
USART3_IRQHandler:
    push {lr}
    bl armv7m_usart3_irq_handler
    pop {pc}

    .thumb_func
EXTI9_5_IRQHandler:
    push {lr}
    bl armv7m_exti9_5_irq_handler
    pop {pc}

    .global __switch
    .thumb_func
__switch:
    /*
     * void __switch(uint32_t next_kstack)
     *   r0 = pointer to next task's saved task_context_t
     *        (which is the same as next->kstack / next->kstack_base).
     *
     * ARMv7-M keeps the current task pointer in the global
     * armv7m_task_pointer.  Save the current callee-saved registers on the
     * current task's kernel stack, then load the next task's context and
     * resume at its saved return address.
     */

    /* r3 = &armv7m_task_pointer, r2 = current task pointer */
    ldr r3, =armv7m_task_pointer
    ldr r2, [r3]

    /* r1 = current task_context_t (== current->kstack_base) */
    ldr r1, [r2, #4]

    /* Save r4-r11 into ctx->r4_r11[0..7]; r1 now points to ctx->sp */
    stmia r1!, {r4-r11}

    /* Save the remaining callee-owned context */
    str sp, [r1, #0]           /* ctx->sp */
    str lr, [r1, #4]           /* ctx->ra (return address after this call) */
    str r2, [r1, #8]           /* ctx->tp (current task pointer) */
    mrs r4, control
    str r4, [r1, #12]          /* ctx->control */

    /*
     * Restore the next task's context.  r0 still holds next->kstack
     * (== next task_context_t).
     */
    ldmia r0!, {r4-r11}        /* restore r4-r11 from ctx->r4_r11[0..7] */

    ldr r1, [r0, #0]           /* next ctx->sp */
    ldr r2, [r0, #4]           /* next ctx->ra (resume address) */
    ldr r12, [r0, #8]          /* next ctx->tp (next task pointer) */
    ldr lr, [r0, #12]           /* next ctx->control */
    msr control, lr
    isb

    /* Update the global current task pointer to the next task. */
    str r12, [r3]

    /* Switch to the next task's stack and resume. */
    mov sp, r1
    bx r2

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
