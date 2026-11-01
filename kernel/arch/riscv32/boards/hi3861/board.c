#include "core/arch.h"
#include "drivers/core/driver_core.h"
#include "drivers/char/uart.h"
#include "mcu.h"

/*
 * Hi3861V100 minimal M-mode board support.
 *
 * This file owns the registers the A20OS MCU execution environment needs:
 * UART0 and its GPIO3/GPIO4 pin mux, the 24 MHz hardware timer and the SoC
 * reset path.  ROM/FlashBoot and the flash layout remain SoC-owned.
 */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define UART0_BASE      0x40008000UL
#define UART_DR         0x000UL
#define UART_FR         0x018UL
#define UART_IBRD       0x024UL
#define UART_FBRD       0x028UL
#define UART_LCR_H      0x02CUL
#define UART_CR         0x030UL
#define UART_IFLS       0x034UL
#define UART_IMSC       0x038UL
#define UART_ICR        0x044UL
#define UART_DMACR      0x048UL

#define UART_FR_TXFE    (1U << 7)
#define UART_FR_TXFF    (1U << 5)
#define UART_FR_RXFE    (1U << 4)

/* 80 MHz UART clock / (16 * 115200) = 43.402..., FBRD = round(.402 * 64). */
#define UART_IBRD_115200  43U
#define UART_FBRD_115200  26U
#define UART_LCR_H_8N1    0x70U       /* 8 data bits, no parity, FIFO enabled */
#define UART_CR_ENABLE    0x301U      /* UART, TX and RX enable */
#define UART_IFLS_RX16_TX32 0x10AU

#define IOMUX_BASE      0x5000A000UL
#define IOMUX_FUNC(n)   (IOMUX_BASE + 0x604UL + (n) * 4UL)
#define GPIO3_UART0_TXD 1U
#define GPIO4_UART0_RXD 2U

#define TIMER_BASE       0x40050028UL /* timer 2: the block interrupt_demo uses */
#define TIMER_LOAD       0x000UL
#define TIMER_CURRENT    0x004UL
#define TIMER_CONTROL    0x008UL
#define TIMER_EOI        0x00CUL
#define TIMER_INT_STATUS 0x010UL
#define TIMER_CTRL_EN       (1U << 0)
/* The SDK driver (hwtimer_init/hwtimer_set_oneshot) and the official
 * interrupt_demo both run the block with bit 1 set: TIMER_CTL_CFG_USERDEF
 * (0x3) reloads the counter from TIMER_LOAD and raises one interrupt per
 * period.  With bit 1 clear (TIMER_CTL_CFG_FREERUN, 0x1) the counter simply
 * free-runs, which is why the timer produced no tick before. */
#define TIMER_CTRL_RELOAD   (1U << 1)
#define TIMER_CTRL_UNMASK   (0U << 2) /* bit 2 set masks the interrupt */
#define TIMER_HZ         24000000U
#define TIMER_IRQ        28U          /* g_timer_property[2].irq */

/* hi_hwtimer_init_new() only toggles the clock gate for timers 1 and 2:
 * CLDO_CTL_CLKEN bit 7 for timer 1, bit 8 for timer 2.  Timer 0 has no gate
 * here, which is why its counter never ran and its (level) interrupt stayed
 * asserted after every EOI. */
#define CLDO_CTL_CLKEN   0x40010030UL
#define CLDO_TIMER2_CLK  (1U << 8)

#define GLB_CTL_WDT_RST_SEL 0x50000030UL
#define CLDO_CTL_SOFT_RESET 0x40010020UL

static uint32_t timer_lo;
static uint32_t timer_hi;

static void uart_pins(void) {
    REG(IOMUX_FUNC(3)) = GPIO3_UART0_TXD;
    REG(IOMUX_FUNC(4)) = GPIO4_UART0_RXD;
}

static void early_init(void) {
    /* The Huawei flash boot enables the watchdog; the kernel must own time. */
    REG(GLB_CTL_WDT_RST_SEL) = 0;
}

void arch_uart_init(void) {
    uart_pins();
    REG(UART0_BASE + UART_CR) = 0;
    REG(UART0_BASE + UART_IMSC) = 0;
    REG(UART0_BASE + UART_ICR) = 0x7FF;
    REG(UART0_BASE + UART_DMACR) = 0;
    REG(UART0_BASE + UART_IBRD) = UART_IBRD_115200;
    REG(UART0_BASE + UART_FBRD) = UART_FBRD_115200;
    REG(UART0_BASE + UART_LCR_H) = UART_LCR_H_8N1;
    REG(UART0_BASE + UART_IFLS) = UART_IFLS_RX16_TX32;
    REG(UART0_BASE + UART_CR) = UART_CR_ENABLE;
}

void arch_uart_putc(char c) {
    if (c == '\n')
        arch_uart_putc('\r');
    while (REG(UART0_BASE + UART_FR) & UART_FR_TXFF) {}
    REG(UART0_BASE + UART_DR) = (uint8_t)c;
}

int arch_uart_poll_getc(void) {
    if (REG(UART0_BASE + UART_FR) & UART_FR_RXFE)
        return -1;
    return (int)(REG(UART0_BASE + UART_DR) & 0xFFU);
}

void arch_uart_flush(void) {
    while (!(REG(UART0_BASE + UART_FR) & UART_FR_TXFE)) {}
}

void arch_uart_ack_irq(void) {}
int arch_uart_irq(void) { return -1; }

static void local_irq_priority(void) {
    /* The HimiDeer local controller carries a 4-bit priority per IRQ in the
     * locie0..locie4 CSRs.  IRQ 28 is entry 2 of locie0: bits [11:8]. */
    uint32_t value;
    __asm__ volatile("csrr %0, 0xbc0" : "=r"(value));
    value = (value & ~(0xFU << 8)) | (2U << 8);
    __asm__ volatile("csrw 0xbc0, %0" :: "r"(value) : "memory");
}

static void timer_clock_enable(void) {
    uint16_t value = *(volatile uint16_t *)(uintptr_t)CLDO_CTL_CLKEN;
    *(volatile uint16_t *)(uintptr_t)CLDO_CTL_CLKEN =
        (uint16_t)(value | CLDO_TIMER2_CLK);
}

void riscv_mcu_timer_init(void) {
    timer_clock_enable();
    (void)REG(TIMER_BASE + TIMER_EOI); /* clear a stale comparator event */
    REG(TIMER_BASE + TIMER_CONTROL) = 0;
    REG(TIMER_BASE + TIMER_LOAD) = TIMER_HZ / ARCH_TIMER_FREQ;
    REG(TIMER_BASE + TIMER_CONTROL) =
        TIMER_CTRL_EN | TIMER_CTRL_RELOAD | TIMER_CTRL_UNMASK;
    local_irq_priority();
    arch_write_sie(1U << TIMER_IRQ);
}

uint64_t riscv_mcu_timer_read(void) {
    uint32_t flags = arch_irq_save();
    uint32_t lo = timer_lo;
    uint32_t hi = timer_hi;
    arch_irq_restore(flags);
    return ((uint64_t)hi << 32) | lo;
}

void riscv_mcu_timer_enable(int enabled) {
    if (enabled) {
        REG(TIMER_BASE + TIMER_LOAD) = TIMER_HZ / ARCH_TIMER_FREQ;
        REG(TIMER_BASE + TIMER_CONTROL) =
            TIMER_CTRL_EN | TIMER_CTRL_RELOAD | TIMER_CTRL_UNMASK;
        local_irq_priority();
        arch_write_sie(1U << TIMER_IRQ);
    } else {
        REG(TIMER_BASE + TIMER_CONTROL) = 0;
        arch_write_sie(0);
    }
}

void riscv_mcu_timer_rearm(void) {} /* hardware periodic comparator */

int riscv_mcu_irq_dispatch(uint32_t cause) {
    if (cause != TIMER_IRQ)
        return -1;

    (void)REG(TIMER_BASE + TIMER_EOI); /* read clears the IRQ */
    timer_lo++;
    if (timer_lo == 0)
        timer_hi++;

    int c;
    while ((c = arch_uart_poll_getc()) >= 0)
        uart_receive_char((char)c);
    return 0;
}

void riscv_mcu_reboot(void) {
    arch_local_irq_disable();
    arch_uart_flush();
    /* Soft reset of the CPU subsystem; the ROM boot chain restarts. */
    *(volatile uint16_t *)(uintptr_t)CLDO_CTL_SOFT_RESET = 0;
    arch_halt();
}

static const board_config_t board = {
    .name = "hi3861",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .early_init = early_init,
    .reboot = riscv_mcu_reboot,
};

const board_config_t *const current_board = &board;
