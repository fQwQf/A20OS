#ifdef CONFIG_BOARD_STM32F103

#include "core/arch.h"
#include "core/stdio.h"
#include "drivers/char/uart.h"
#include "board.h"
#include "bluetooth.h"
#include "ir.h"
#include "wifi.h"
#include "proc/proc.h"

#define ARMV7M_PREEMPT_SVC 0x20U

extern void armv7m_preempt_trampoline(void);
volatile uint32_t armv7m_preemptions;

void armv7m_pendsv_prepare(uint32_t *stack) {
    task_t *current = proc_current();
    if (!stack || !current || current->pid == 0 ||
        current->state != PROC_RUNNING || current->arch_preempt_active ||
        current->arch_preempt_disable)
        return;

    current->arch_preempt_resume_pc = stack[6];
    current->arch_preempt_resume_xpsr = stack[7];
    current->arch_preempt_active = 1;
    armv7m_preemptions++;
    stack[6] = (uint32_t)(uintptr_t)armv7m_preempt_trampoline | 1U;
    /* An interrupted Thumb IT block stores its condition state in xPSR. The
     * trampoline is unrelated code, so start it with a clean Thumb xPSR and
     * restore the original word in the SVC return path. */
    stack[7] = 0x01000000U;
}

void armv7m_default_handler(void) {
    uint32_t ipsr;
    __asm__ __volatile__("mrs %0, ipsr" : "=r"(ipsr));
    printf("\n[ARMV7M] unhandled exception %u\n", ipsr);
    stm32_status_led_set(1);
    arch_halt();
}

void armv7m_fault_handler(uint32_t exception, uint32_t *stack) {
    uint32_t cfsr = *(volatile uint32_t *)0xE000ED28UL;
    uint32_t hfsr = *(volatile uint32_t *)0xE000ED2CUL;
    armv7m_trap_cause = CAUSE_LOAD_FAULT;
    armv7m_fault_pc = stack ? stack[6] : 0;
    armv7m_fault_addr = *(volatile uint32_t *)0xE000ED38UL;
    printf("\n[ARMV7M] fault exception=%u pc=0x%x lr=0x%x"
           " cfsr=0x%x hfsr=0x%x addr=0x%x\n",
           exception, armv7m_fault_pc, stack ? stack[5] : 0,
           cfsr, hfsr, armv7m_fault_addr);
    stm32_status_led_set(1);
    arch_halt();
}

void armv7m_svc_handler(uint32_t *stack) {
    if (stack && stack[6] >= 2U) {
        const uint8_t *svc = (const uint8_t *)(uintptr_t)(stack[6] - 2U);
        task_t *current = proc_current();
        if (svc[0] == ARMV7M_PREEMPT_SVC && svc[1] == 0xDFU && current &&
            current->arch_preempt_active && current->arch_preempt_resume_pc) {
            stack[6] = (uint32_t)current->arch_preempt_resume_pc;
            stack[7] = current->arch_preempt_resume_xpsr;
            current->arch_preempt_resume_pc = 0;
            current->arch_preempt_resume_xpsr = 0;
            current->arch_preempt_active = 0;
            return;
        }
    }
    armv7m_trap_cause = CAUSE_ECALL_U;
    if (stack)
        stack[0] = 0xA20U;
}

void armv7m_usart1_irq_handler(void) {
    int c;
    while ((c = arch_uart_poll_getc()) >= 0)
        uart_receive_char((char)c);
    arch_uart_ack_irq();
}

void armv7m_usart2_irq_handler(void) {
    stm32_wifi_irq();
}

void armv7m_usart3_irq_handler(void) {
    stm32_bluetooth_irq();
}

void armv7m_exti9_5_irq_handler(void) {
    stm32_ir_isr();
}

#endif
