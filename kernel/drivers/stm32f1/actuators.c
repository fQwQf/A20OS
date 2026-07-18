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
#define AFIO_MAPR (*(volatile uint32_t *)0x40010004UL)
#define AFIO_MAPR_TIM3_REMAP_MASK (3U << 10)
#define AFIO_MAPR_TIM3_REMAP_PARTIAL (2U << 10)

/* GPIOA @ 0x40010800 */
#define GPIOA_CRL (*(volatile uint32_t *)0x40010800UL)
#define GPIOA_BSRR (*(volatile uint32_t *)0x40010810UL)
#define GPIOA_BRR (*(volatile uint32_t *)0x40010814UL)
/* GPIOB @ 0x40010C00 */
#define GPIOB_CRL (*(volatile uint32_t *)0x40010C00UL)
#define GPIOB_CRH (*(volatile uint32_t *)0x40010C04UL)
#define GPIOB_IDR (*(volatile uint32_t *)0x40010C08UL)
#define GPIOB_ODR (*(volatile uint32_t *)0x40010C0CUL)
#define GPIOB_BSRR (*(volatile uint32_t *)0x40010C10UL)
#define GPIOB_BRR (*(volatile uint32_t *)0x40010C14UL)

/* TIM3 @ 0x40000400 */
#define TIM3_CR1 (*(volatile uint32_t *)0x40000400UL)
#define TIM3_CNT (*(volatile uint32_t *)0x40000424UL)
#define TIM3_CCMR1 (*(volatile uint32_t *)0x40000418UL)
#define TIM3_CCER (*(volatile uint32_t *)0x40000420UL)
#define TIM3_PSC (*(volatile uint32_t *)0x40000428UL)
#define TIM3_ARR (*(volatile uint32_t *)0x4000042CUL)
#define TIM3_CCR1 (*(volatile uint32_t *)0x40000434UL)
#define TIM3_CCR2 (*(volatile uint32_t *)0x40000438UL)
#define TIM3_EGR (*(volatile uint32_t *)0x40000414UL)

#define FAN_PERIOD 500U /* with a 1 MHz timer clock -> 2 kHz PWM */
/* Keep the first revision's fan control below full power.  The Xuanwu
 * ULN2003 channel and the small 5 V rail are happier with 40/50/60% than
 * with the previous 35/50/60% steps.  The first step is deliberately above
 * the motor's observed startup threshold; the second stays close to max. */
static const uint16_t fan_duty_steps[4] = {0U, 200U, 250U, 300U};
#define PUMP_PIN 7U  /* PA7 */
#define BUZZER_PIN 8U /* PB8 */

static volatile uint8_t fan_requested_level;
static volatile uint8_t fan_debug_override;
static volatile uint8_t fan_debug_gpio_mode;

static void fan_config_pwm_pin(void) {
#ifdef CONFIG_STM32_XUANWU
    uint32_t crl = GPIOB_CRL;
    crl &= ~(0xFU << (5U * 4U));
    crl |= 0xBU << (5U * 4U);
    GPIOB_CRL = crl;
    TIM3_CCER = (1U << 4) | (1U << 5); /* CC2E | CC2P */
#else
    uint32_t crl = GPIOA_CRL;
    crl &= ~(0xFU << (6U * 4U));
    crl |= 0xBU << (6U * 4U);
    GPIOA_CRL = crl;
    TIM3_CCER = 1U << 0;
#endif
}

static void fan_apply_level(uint8_t level) {
    uint32_t duty = fan_duty_steps[level > 3U ? 3U : level];
#ifdef CONFIG_STM32_XUANWU
    TIM3_CCR2 = FAN_PERIOD - duty;
#else
    TIM3_CCR1 = duty;
#endif
}

static void fan_init(void) {
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN |
                   RCC_APB2ENR_AFIOEN;
    RCC_APB1ENR |= RCC_APB1ENR_TIM3EN;

#ifdef CONFIG_STM32_XUANWU
    /*
     * The Xuanwu motor header is PB5/TIM3_CH2 (TIM3 partial remap), as in
     * docs/pz/12.  Its ULN2003 stage is inverting, so an active-low PWM
     * channel plus CCR2=(period-duty) produces the requested motor duty.
     */
    AFIO_MAPR = (AFIO_MAPR & ~AFIO_MAPR_TIM3_REMAP_MASK) |
                AFIO_MAPR_TIM3_REMAP_PARTIAL;
#endif
    fan_config_pwm_pin();

    uint32_t mhz = stm32_hclk_hz() / 1000000U;
    if (mhz == 0U)
        mhz = 1U;
    TIM3_PSC = mhz - 1U; /* count at 1 MHz */
    TIM3_ARR = FAN_PERIOD - 1U;
#ifdef CONFIG_STM32_XUANWU
    /* CH2 PWM mode 1 + preload; active-low output matches the vendor code. */
    TIM3_CCMR1 = (6U << 12) | (1U << 11);
    TIM3_CCR2 = FAN_PERIOD;
#else
    /* CH1: PWM mode 1 (OC1M=110), preload enable (OC1PE). */
    TIM3_CCMR1 = (6U << 4) | (1U << 3);
    TIM3_CCR1 = 0;
#endif
    TIM3_EGR = 1U << 0;  /* UG: load PSC/ARR */
    TIM3_CR1 = (1U << 7) | (1U << 0); /* ARPE | CEN */
    fan_requested_level = 0;
    fan_debug_override = 0;
    fan_debug_gpio_mode = 0;
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
    fan_requested_level = level;
    if (!fan_debug_override)
        fan_apply_level(level);
#endif
}

void stm32_fan_debug_pwm(uint8_t level) {
#ifndef CONFIG_STM32_QEMU
    if (level > 3U)
        level = 3U;
    fan_debug_override = 1;
    fan_debug_gpio_mode = 0;
    fan_config_pwm_pin();
    fan_apply_level(level);
#else
    (void)level;
#endif
}

void stm32_fan_debug_gpio(int high) {
#ifndef CONFIG_STM32_QEMU
    fan_debug_override = 1;
    fan_debug_gpio_mode = 1;
#ifdef CONFIG_STM32_XUANWU
    TIM3_CCER &= ~(1U << 4);
    uint32_t crl = GPIOB_CRL;
    crl &= ~(0xFU << (5U * 4U));
    crl |= 0x2U << (5U * 4U); /* push-pull output, 2 MHz */
    GPIOB_CRL = crl;
    if (high)
        GPIOB_BSRR = 1U << 5;
    else
        GPIOB_BRR = 1U << 5;
#else
    (void)high;
#endif
#else
    (void)high;
#endif
}

void stm32_fan_debug_release(void) {
#ifndef CONFIG_STM32_QEMU
    fan_config_pwm_pin();
    fan_apply_level(fan_requested_level);
    fan_debug_gpio_mode = 0;
    fan_debug_override = 0;
#endif
}

void stm32_fan_debug_snapshot(stm32_fan_debug_t *debug) {
    if (!debug)
        return;
#ifdef CONFIG_STM32_QEMU
    *debug = (stm32_fan_debug_t){0};
#else
    debug->afio_mapr = AFIO_MAPR;
#ifdef CONFIG_STM32_XUANWU
    debug->gpio_config = (GPIOB_CRL >> (5U * 4U)) & 0xFU;
    debug->gpio_idr = GPIOB_IDR;
    debug->gpio_odr = GPIOB_ODR;
    debug->tim_ccr = TIM3_CCR2;
    debug->pin_high = (uint8_t)((GPIOB_IDR >> 5) & 1U);
#else
    debug->gpio_config = (GPIOA_CRL >> (6U * 4U)) & 0xFU;
    debug->gpio_idr = 0;
    debug->gpio_odr = 0;
    debug->tim_ccr = TIM3_CCR1;
    debug->pin_high = 0;
#endif
    debug->tim_cr1 = TIM3_CR1;
    debug->tim_ccmr1 = TIM3_CCMR1;
    debug->tim_ccer = TIM3_CCER;
    debug->tim_psc = TIM3_PSC;
    debug->tim_arr = TIM3_ARR;
    debug->tim_cnt = TIM3_CNT;
    debug->requested_level = fan_requested_level;
    debug->override_active = fan_debug_override;
    debug->gpio_mode = fan_debug_gpio_mode;
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
