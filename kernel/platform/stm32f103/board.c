#ifdef CONFIG_BOARD_STM32F103

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/timer.h"
#include "board.h"

#define NVIC_ISER_BASE 0xE000E100UL
#define NVIC_ICER_BASE 0xE000E180UL

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define GPIOB_CRL   (*(volatile uint32_t *)0x40010C00UL)
#define GPIOB_ODR   (*(volatile uint32_t *)0x40010C0CUL)
#define GPIOB_BSRR  (*(volatile uint32_t *)0x40010C10UL)

#define RCC_APB2ENR_IOPBEN (1U << 3)
#define STATUS_LED_PIN     5U
#define STATUS_LED_MASK    (1U << STATUS_LED_PIN)

void stm32_status_led_set(int on) {
    /* Xuanwu LED0 is active low on PB5. */
    GPIOB_BSRR = on ? (STATUS_LED_MASK << 16) : STATUS_LED_MASK;
}

void stm32_status_led_toggle(void) {
    if (GPIOB_ODR & STATUS_LED_MASK)
        GPIOB_BSRR = STATUS_LED_MASK << 16;
    else
        GPIOB_BSRR = STATUS_LED_MASK;
}

void stm32_status_led_init(void) {
    RCC_APB2ENR |= RCC_APB2ENR_IOPBEN;

    uint32_t crl = GPIOB_CRL;
    crl &= ~(0xFU << (STATUS_LED_PIN * 4U));
    crl |= 0x2U << (STATUS_LED_PIN * 4U);
    GPIOB_CRL = crl;
    stm32_status_led_set(1);
}

static void stm32_irqchip_init(void) {}

static void stm32_irqchip_enable(uint32_t irq) {
    volatile uint32_t *iser =
        (volatile uint32_t *)(NVIC_ISER_BASE + (irq / 32U) * 4U);
    *iser = 1UL << (irq % 32U);
}

static void stm32_irqchip_disable(uint32_t irq) {
    volatile uint32_t *icer =
        (volatile uint32_t *)(NVIC_ICER_BASE + (irq / 32U) * 4U);
    *icer = 1UL << (irq % 32U);
}

static uint32_t stm32_irqchip_ack(void) {
    uint32_t ipsr;
    __asm__ __volatile__("mrs %0, ipsr" : "=r"(ipsr));
    return ipsr >= 16U ? ipsr - 16U : ipsr;
}

static void stm32_irqchip_eoi(uint32_t irq) { (void)irq; }
static void stm32_irqchip_send_ipi(uint64_t mask) { (void)mask; }

static const irqchip_ops_t stm32_irqchip_ops = {
    .init = stm32_irqchip_init,
    .enable_irq = stm32_irqchip_enable,
    .disable_irq = stm32_irqchip_disable,
    .ack = stm32_irqchip_ack,
    .eoi = stm32_irqchip_eoi,
    .send_ipi = stm32_irqchip_send_ipi,
};

static uint64_t stm32_timer_read_ticks(void) { return timer_get_ticks(); }
static uint64_t stm32_timer_ticks_per_sec(void) { return ARCH_TIMER_FREQ; }

static const timer_ops_t stm32_timer_ops = {
    .read_ticks = stm32_timer_read_ticks,
    .ticks_per_sec = stm32_timer_ticks_per_sec,
};

static void stm32_early_init(void) {
    stm32_status_led_init();
}

static void stm32_poweroff(void) { firmware_shutdown(); }
static void stm32_reboot(void) { firmware_reboot(); }
static void stm32_enumerate_devices(void) {}

static const board_config_t stm32f103_board = {
    .name = "stm32f103",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &stm32_irqchip_ops,
    .timer = &stm32_timer_ops,
    .early_init = stm32_early_init,
    .poweroff = stm32_poweroff,
    .reboot = stm32_reboot,
    .enumerate_devices = stm32_enumerate_devices,
};

const board_config_t *const current_board = &stm32f103_board;

#endif
