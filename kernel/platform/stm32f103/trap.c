#ifdef CONFIG_BOARD_STM32F103

#include "core/arch.h"
#include "core/stdio.h"
#include "drivers/char/uart.h"
#include "board.h"

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
    armv7m_trap_cause = CAUSE_ECALL_U;
    if (stack)
        stack[0] = 0xA20U;
}

void armv7m_usart1_irq_handler(void) {
    int c = arch_uart_poll_getc();
    if (c >= 0)
        uart_receive_char((char)c);
    arch_uart_ack_irq();
}

#endif
