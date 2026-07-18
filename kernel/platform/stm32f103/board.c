#ifdef CONFIG_BOARD_STM32F103

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/timer.h"
#include "board.h"
#include "drivers/stm32f1/bluetooth.h"
#include "drivers/stm32f1/sdcard.h"

#define NVIC_ISER_BASE 0xE000E100UL
#define NVIC_ICER_BASE 0xE000E180UL

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define RCC_CR      (*(volatile uint32_t *)0x40021000UL)
#define RCC_CFGR    (*(volatile uint32_t *)0x40021004UL)
#define FLASH_ACR   (*(volatile uint32_t *)0x40022000UL)
#define GPIOB_CRL   (*(volatile uint32_t *)0x40010C00UL)
#define GPIOB_ODR   (*(volatile uint32_t *)0x40010C0CUL)
#define GPIOB_BSRR  (*(volatile uint32_t *)0x40010C10UL)

#define RCC_APB2ENR_IOPBEN (1U << 3)
#define STATUS_LED_PIN     5U
#define STATUS_LED_MASK    (1U << STATUS_LED_PIN)

#define RCC_CR_HSEON  (1U << 16)
#define RCC_CR_HSERDY (1U << 17)
#define RCC_CR_PLLON  (1U << 24)
#define RCC_CR_PLLRDY (1U << 25)
#define RCC_CFGR_SWS_MASK (3U << 2)
#define RCC_CFGR_SWS_PLL  (2U << 2)
#define RCC_CFGR_PPRE1_DIV2 (4U << 8)
#define RCC_CFGR_PLLSRC_HSE (1U << 16)
#define RCC_CFGR_PLLMUL9    (7U << 18)
#define FLASH_ACR_LATENCY_2 2U
#define FLASH_ACR_PRFTBE    (1U << 4)

#if defined(CONFIG_STM32_XUANWU) && !defined(CONFIG_STM32_QEMU)
static void stm32_clock_init(void) {
    uint32_t timeout;

    RCC_CR |= RCC_CR_HSEON;
    for (timeout = 500000U; timeout && !(RCC_CR & RCC_CR_HSERDY); timeout--)
        ;
    if (!(RCC_CR & RCC_CR_HSERDY))
        return;

    FLASH_ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
    RCC_CFGR = (RCC_CFGR & ~((0xFU << 4) | (7U << 8) | (7U << 11))) |
               RCC_CFGR_PPRE1_DIV2;
    RCC_CFGR = (RCC_CFGR &
                ~((1U << 16) | (1U << 17) | (0xFU << 18))) |
               RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLMUL9;

    RCC_CR |= RCC_CR_PLLON;
    for (timeout = 500000U; timeout && !(RCC_CR & RCC_CR_PLLRDY); timeout--)
        ;
    if (!(RCC_CR & RCC_CR_PLLRDY)) {
        RCC_CFGR &= ~((7U << 8) | (1U << 16) | (1U << 17) |
                      (0xFU << 18));
        FLASH_ACR = 0;
        return;
    }

    RCC_CFGR = (RCC_CFGR & ~3U) | 2U;
    for (timeout = 500000U;
         timeout && (RCC_CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL;
         timeout--)
        ;
    if ((RCC_CFGR & RCC_CFGR_SWS_MASK) != RCC_CFGR_SWS_PLL) {
        RCC_CFGR &= ~3U;
        for (timeout = 500000U;
             timeout && (RCC_CFGR & RCC_CFGR_SWS_MASK) != 0;
             timeout--)
            ;
        if ((RCC_CFGR & RCC_CFGR_SWS_MASK) == 0) {
            RCC_CFGR &= ~(7U << 8);
            FLASH_ACR = 0;
        }
    }
}
#endif

void stm32_status_led_set(int on) {
#ifdef CONFIG_STM32_XUANWU
    /* PB5 belongs to the onboard ULN2003 fan PWM on Xuanwu. */
    (void)on;
#else
    /* Xuanwu LED0 is active low on PB5. */
    GPIOB_BSRR = on ? (STATUS_LED_MASK << 16) : STATUS_LED_MASK;
#endif
}

void stm32_status_led_toggle(void) {
#ifndef CONFIG_STM32_XUANWU
    if (GPIOB_ODR & STATUS_LED_MASK)
        GPIOB_BSRR = STATUS_LED_MASK << 16;
    else
        GPIOB_BSRR = STATUS_LED_MASK;
#endif
}

void stm32_status_led_init(void) {
#ifndef CONFIG_STM32_XUANWU
    RCC_APB2ENR |= RCC_APB2ENR_IOPBEN;

    uint32_t crl = GPIOB_CRL;
    crl &= ~(0xFU << (STATUS_LED_PIN * 4U));
    crl |= 0x2U << (STATUS_LED_PIN * 4U);
    GPIOB_CRL = crl;
    stm32_status_led_set(1);
#endif
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
#if defined(CONFIG_STM32_XUANWU) && !defined(CONFIG_STM32_QEMU)
    stm32_clock_init();
#endif
    stm32_status_led_init();
    stm32_bluetooth_early_key_init();
}

static void stm32_poweroff(void) { firmware_shutdown(); }
static void stm32_reboot(void) { firmware_reboot(); }
static void stm32_enumerate_devices(void) {
    /* SDIO is a fixed platform device, but it still follows the same
     * register -> match -> probe lifecycle as PCI/VirtIO devices. */
    (void)stm32_sdcard_register_device();
}

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
