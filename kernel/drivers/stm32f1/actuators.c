/*
 * Smart home hub — actuators (fan PWM, pump, buzzer). See actuators.h.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "drivers/stm32f1/actuators.h"
#include "drivers/stm32f1/stm32_uart.h" /* stm32_hclk_hz() */

#ifndef CONFIG_STM32_QEMU

#define RCC_APB1ENR (*(volatile uint32_t *)0x4002101CUL)
#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define RCC_APB2ENR_IOPAEN (1U << 2)
#define RCC_APB2ENR_IOPBEN (1U << 3)
#define RCC_APB2ENR_AFIOEN (1U << 0)
#define RCC_APB1ENR_TIM3EN (1U << 1)

/* GPIOA @ 0x40010800 */
#define GPIOA_CRL (*(volatile uint32_t *)0x40010800UL)
#define GPIOA_BSRR (*(volatile uint32_t *)0x40010810UL)
#define GPIOA_BRR (*(volatile uint32_t *)0x40010814UL)
/* GPIOB @ 0x40010C00 */
#define GPIOB_CRH (*(volatile uint32_t *)0x40010C04UL)
#define GPIOB_BSRR (*(volatile uint32_t *)0x40010C10UL)
#define GPIOB_BRR (*(volatile uint32_t *)0x40010C14UL)

/* TIM3 @ 0x40000400 */
#define TIM3_CR1 (*(volatile uint32_t *)0x40000400UL)
#define TIM3_CCMR1 (*(volatile uint32_t *)0x40000418UL)
#define TIM3_CCER (*(volatile uint32_t *)0x40000420UL)
#define TIM3_PSC (*(volatile uint32_t *)0x40000428UL)
#define TIM3_ARR (*(volatile uint32_t *)0x4000042CUL)
#define TIM3_CCR1 (*(volatile uint32_t *)0x40000434UL)
#define TIM3_EGR (*(volatile uint32_t *)0x40000414UL)

#define FAN_ARR 999U /* with PSC for 1 MHz -> ~1 kHz PWM */
#define PUMP_PIN 7U  /* PA7 */
#define BUZZER_PIN 8U /* PB8 */

static void fan_init(void) {
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN;
    RCC_APB1ENR |= RCC_APB1ENR_TIM3EN;

    /* PA6 (TIM3_CH1) alternate-function push-pull, 50 MHz (0xB). */
    uint32_t crl = GPIOA_CRL;
    crl &= ~(0xFU << (6U * 4U));
    crl |= 0xBU << (6U * 4U);
    GPIOA_CRL = crl;

    uint32_t mhz = stm32_hclk_hz() / 1000000U;
    if (mhz == 0U)
        mhz = 1U;
    TIM3_PSC = mhz - 1U; /* count at 1 MHz */
    TIM3_ARR = FAN_ARR;
    /* CH1: PWM mode 1 (OC1M=110), preload enable (OC1PE). */
    TIM3_CCMR1 = (6U << 4) | (1U << 3);
    TIM3_CCR1 = 0;
    TIM3_CCER = 1U << 0; /* CC1E: enable CH1 output */
    TIM3_EGR = 1U << 0;  /* UG: load PSC/ARR */
    TIM3_CR1 = 1U << 0;  /* CEN */
}

static void gpio_out_init(void) {
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;
    /* PA7 push-pull 2 MHz (0x2). */
    uint32_t crl = GPIOA_CRL;
    crl &= ~(0xFU << (PUMP_PIN * 4U));
    crl |= 0x2U << (PUMP_PIN * 4U);
    GPIOA_CRL = crl;
    GPIOA_BRR = 1U << PUMP_PIN;
    /* PB8 push-pull 2 MHz. PB8 = CRH bits for pin 8. */
    uint32_t crh = GPIOB_CRH;
    crh &= ~(0xFU << ((BUZZER_PIN - 8U) * 4U));
    crh |= 0x2U << ((BUZZER_PIN - 8U) * 4U);
    GPIOB_CRH = crh;
    GPIOB_BRR = 1U << BUZZER_PIN;
}

#endif /* !CONFIG_STM32_QEMU */

int stm32_actuators_init(void) {
#ifdef CONFIG_STM32_QEMU
    return 0;
#else
    fan_init();
    gpio_out_init();
    return 0;
#endif
}

void stm32_fan_set_level(uint8_t level) {
#ifdef CONFIG_STM32_QEMU
    (void)level;
#else
    if (level > 3U)
        level = 3U;
    /* 0/33/66/100% of ARR. */
    TIM3_CCR1 = (uint32_t)level * ((FAN_ARR + 1U) / 3U);
#endif
}

void stm32_pump_set(int on) {
#ifdef CONFIG_STM32_QEMU
    (void)on;
#else
    if (on)
        GPIOA_BSRR = 1U << PUMP_PIN;
    else
        GPIOA_BRR = 1U << PUMP_PIN;
#endif
}

void stm32_buzzer_set(int on) {
#ifdef CONFIG_STM32_QEMU
    (void)on;
#else
    if (on)
        GPIOB_BSRR = 1U << BUZZER_PIN;
    else
        GPIOB_BRR = 1U << BUZZER_PIN;
#endif
}

#endif /* CONFIG_BOARD_STM32F103 */
