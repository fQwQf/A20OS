#include "arch_ops.h"
#include "sbi.h"
#include "riscv64_consts.h"

#define RISCV_SSTATUS_SIE (1UL << 1)
#define RISCV_SIE_SSIE    (1UL << 1)
#define RISCV_SIE_STIE    (1UL << 5)
#define RISCV_SIE_SEIE    (1UL << 9)

#define UART0 ((volatile uint8_t *)UART0_BASE)
#define UART_RHR 0
#define UART_THR 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_IER_RX_ENABLE (1 << 0)
#define UART_LSR_RX_READY  (1 << 0)
#define UART_LSR_TX_IDLE   (1 << 5)

uint64_t arch_irq_save(void) {
    uint64_t s;
    __asm__ volatile("csrr %0, sstatus" : "=r"(s));
    __asm__ volatile("csrc sstatus, %0" :: "r"(RISCV_SSTATUS_SIE));
    return s;
}

void arch_irq_restore(uint64_t state) {
    __asm__ volatile("csrw sstatus, %0" :: "r"(state));
}

void arch_irq_enable(void) {
    __asm__ volatile("csrs sstatus, %0" :: "r"(RISCV_SSTATUS_SIE));
}

void arch_irq_disable(void) {
    __asm__ volatile("csrc sstatus, %0" :: "r"(RISCV_SSTATUS_SIE));
}

void arch_irq_enable_timer(void) {
    uint64_t sie;
    __asm__ volatile("csrr %0, sie" : "=r"(sie));
    sie |= RISCV_SIE_STIE;
    __asm__ volatile("csrw sie, %0" :: "r"(sie));
}

void arch_irq_disable_timer(void) {
    uint64_t sie;
    __asm__ volatile("csrr %0, sie" : "=r"(sie));
    sie &= ~RISCV_SIE_STIE;
    __asm__ volatile("csrw sie, %0" :: "r"(sie));
}

void arch_irq_enable_external(void) {
    uint64_t sie;
    __asm__ volatile("csrr %0, sie" : "=r"(sie));
    sie |= RISCV_SIE_SEIE;
    __asm__ volatile("csrw sie, %0" :: "r"(sie));
}

void arch_softirq_clear(void) {
    uint64_t sip;
    __asm__ volatile("csrr %0, sip" : "=r"(sip));
    sip &= ~RISCV_SIE_SSIE;
    __asm__ volatile("csrw sip, %0" :: "r"(sip));
}

void arch_trap_vector_set(uint64_t addr) {
    __asm__ volatile("csrw stvec, %0" :: "r"(addr));
}

void arch_trap_scratch_set(uint64_t val) {
    __asm__ volatile("csrw sscratch, %0" :: "r"(val));
}

uint64_t arch_trap_cause(void) {
    uint64_t v;
    __asm__ volatile("csrr %0, scause" : "=r"(v));
    return v;
}

uint64_t arch_trap_epc(void) {
    uint64_t v;
    __asm__ volatile("csrr %0, sepc" : "=r"(v));
    return v;
}

void arch_trap_set_epc(uint64_t epc) {
    __asm__ volatile("csrw sepc, %0" :: "r"(epc));
}

uint64_t arch_trap_tval(void) {
    uint64_t v;
    __asm__ volatile("csrr %0, stval" : "=r"(v));
    return v;
}

uint64_t arch_mmu_token_from_pgdir(uint64_t *pgdir) {
    return pgdir ? ((0x8UL << 60) | ((uint64_t)(uintptr_t)pgdir >> 12)) : 0;
}

uint64_t arch_task_status_kernel_default(void) {
    return RISCV_SSTATUS_SIE;
}

void arch_set_current_task_ptr(void *task) {
    __asm__ volatile("mv tp, %0" :: "r"(task));
}

void arch_sync_icache(void) {
    __asm__ volatile("fence.i" ::: "memory");
}

void arch_cpu_wait(void) {
    __asm__ volatile("wfi");
}

void arch_cpu_relax(void) {
    __asm__ volatile("nop");
}

void arch_mb(void) {
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

void arch_rmb(void) {
    __asm__ volatile("fence ir, ir" ::: "memory");
}

void arch_wmb(void) {
    __asm__ volatile("fence ow, ow" ::: "memory");
}

uint64_t arch_timer_counter(void) {
    uint64_t val;
    __asm__ volatile("csrr %0, time" : "=r"(val));
    return val;
}

void arch_timer_program(uint64_t deadline) {
    sbi_set_timer(deadline);
}

void arch_uart_init_hw(void) {
    UART0[UART_IER] = 0x00;
    UART0[UART_LCR] = 0x80;
    UART0[0] = 0x03;
    UART0[1] = 0x00;
    UART0[UART_LCR] = 0x03;
    UART0[UART_FCR] = 0x07;
    UART0[UART_MCR] = 0x0B;
    UART0[UART_IER] = UART_IER_RX_ENABLE;
}

void arch_uart_putc_hw(char c) {
    while (!(UART0[UART_LSR] & UART_LSR_TX_IDLE)) {
        arch_cpu_relax();
    }
    UART0[UART_THR] = (uint8_t)c;
}

int arch_uart_try_getc_hw(void) {
    if (!(UART0[UART_LSR] & UART_LSR_RX_READY)) return -1;
    return (int)UART0[UART_RHR];
}
