#ifdef CONFIG_MCU_NATIVE_PROGRAM
#include "program_stack.h"
#endif
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
    /* External interrupt numbering belongs to the platform. All device IRQs
     * enter a common architecture stub and are dispatched by the board port. */
    .rept 68
    .word External_IRQHandler
    .endr

    .section .text.entry, "ax", %progbits
    .thumb_func
    .global Reset_Handler
Reset_Handler:
    /*
     * A physical reset loads MSP from vector[0].  Some low-cost CMSIS-DAP
     * probes resume by changing PC only, so establish MSP here as well.
     */
#ifdef CONFIG_MCU_SHARED_KERNEL
    cpsid i
#endif
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
#ifdef CONFIG_MCU_SHARED_KERNEL
    bl armv7m_shared_reset
#else
    bl kernel_main
#endif
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
#ifdef CONFIG_MCU_NATIVE_PROGRAM
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    push {r3-r11, lr}
    add r1, sp, #4             /* interrupted r4-r11 */
    mov r2, lr
    bl armv7m_program_svc
    pop {r3-r11, lr}
    cbz r0, 8f
    /* Resume a complete user frame, including flags/Thumb IT state. */
    ldmia r0!, {r4-r11}
    msr psp, r0
    /* PSP tasks can migrate. Handler mode must never keep an outgoing
     * task's shared kernel stack as this CPU's MSP: that task may already
     * execute on another CPU when the next SysTick/SVC arrives. The old
     * MSP exception frame is discarded by the PSP EXC_RETURN below. */
    ldr r0, =(armv7m_exception_stack + ARMV7M_EXCEPTION_STACK_BYTES)
    msr msp, r0
    movs r0, #3
    msr control, r0
    isb
    ldr lr, =0xfffffffd
8:
    bx lr
#else
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
#ifdef CONFIG_MCU_SHARED_KERNEL
    /* Keep AAPCS 8-byte stack alignment at the C exception hook. */
    push {r3, lr}
#else
    push {lr}
#endif
    bl armv7m_svc_handler
#ifdef CONFIG_MCU_SHARED_KERNEL
    pop {r3, pc}
#else
    pop {pc}
#endif
#endif

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
#ifdef CONFIG_MCU_SHARED_KERNEL
    /* Keep AAPCS 8-byte stack alignment at the C exception hook. */
    push {r3, lr}
#else
    push {lr}
#endif
    bl armv7m_pendsv_prepare
#ifdef CONFIG_MCU_SHARED_KERNEL
    pop {r3, lr}
#else
    pop {lr}
#endif
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
#ifdef CONFIG_MCU_SHARED_KERNEL
    /* Keep AAPCS 8-byte stack alignment at the C exception hook. */
    push {r3, lr}
#else
    push {lr}
#endif
    bl armv7m_systick_handler
#ifdef CONFIG_MCU_NATIVE_PROGRAM
    pop {r3, lr}
    bx lr
#else
#ifdef CONFIG_MCU_SHARED_KERNEL
    pop {r3, pc}
#else
    pop {pc}
#endif
#endif

    .thumb_func
External_IRQHandler:
#ifdef CONFIG_MCU_SHARED_KERNEL
    /* Keep AAPCS 8-byte stack alignment at the C exception hook. */
    push {r3, lr}
#else
    push {lr}
#endif
    bl armv7m_external_irq_handler
#ifdef CONFIG_MCU_SHARED_KERNEL
    pop {r3, pc}
#else
    pop {pc}
#endif

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
#ifdef CONFIG_MCU_SHARED_KERNEL
    mrs r4, primask
    str r4, [r1, #20]          /* ctx->status: saved local IRQ mask */
#endif

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
#ifdef CONFIG_MCU_NATIVE_PROGRAM
    /* A PSP context owns only PSP; handler MSP is CPU-private. A kernel
     * thread using MSP owns its active stack until switch completion. */
    tst lr, #2
    beq 7f
    ldr r3, =(armv7m_exception_stack + ARMV7M_EXCEPTION_STACK_BYTES)
    msr msp, r3
7:
#endif
#ifdef CONFIG_MCU_SHARED_KERNEL
    ldr r0, [r0, #20]          /* next ctx->status */
    msr primask, r0
#endif
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
