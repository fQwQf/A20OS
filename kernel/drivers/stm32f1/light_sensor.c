#ifdef CONFIG_BOARD_STM32F103

#include "drivers/stm32f1/light_sensor.h"
#include "drivers/stm32f1/backlight.h"

#define RCC_CFGR     (*(volatile uint32_t *)0x40021004UL)
#define RCC_APB2RSTR (*(volatile uint32_t *)0x4002100CUL)
#define RCC_APB2ENR  (*(volatile uint32_t *)0x40021018UL)

#define GPIOF_CRH (*(volatile uint32_t *)0x40011C04UL)

#define ADC3_BASE 0x40013C00UL
#define ADC3_SR    (*(volatile uint32_t *)(ADC3_BASE + 0x00UL))
#define ADC3_CR1   (*(volatile uint32_t *)(ADC3_BASE + 0x04UL))
#define ADC3_CR2   (*(volatile uint32_t *)(ADC3_BASE + 0x08UL))
#define ADC3_SMPR1 (*(volatile uint32_t *)(ADC3_BASE + 0x0CUL))
#define ADC3_SMPR2 (*(volatile uint32_t *)(ADC3_BASE + 0x10UL))
#define ADC3_SQR1  (*(volatile uint32_t *)(ADC3_BASE + 0x2CUL))
#define ADC3_SQR2  (*(volatile uint32_t *)(ADC3_BASE + 0x30UL))
#define ADC3_SQR3  (*(volatile uint32_t *)(ADC3_BASE + 0x34UL))
#define ADC3_DR    (*(volatile uint32_t *)(ADC3_BASE + 0x4CUL))

#define RCC_APB2ENR_IOPFEN (1U << 7)
#define RCC_APB2ENR_ADC3EN (1U << 15)
#define LIGHT_ADC_CHANNEL  6U
#define LIGHT_ADC_TIMEOUT  100000U
#define LIGHT_SAMPLE_COUNT 10U

#define ADC_SR_EOC      (1U << 1)
#define ADC_CR2_ADON    (1U << 0)
#define ADC_CR2_CAL     (1U << 2)
#define ADC_CR2_RSTCAL  (1U << 3)
#define ADC_CR2_EXTSEL  (7U << 17)
#define ADC_CR2_EXTTRIG (1U << 20)
#define ADC_CR2_SWSTART (1U << 22)

static stm32_light_sensor_info_t light;

#ifdef CONFIG_STM32_XUANWU
static int wait_clear(volatile uint32_t *reg, uint32_t mask) {
    uint32_t timeout = LIGHT_ADC_TIMEOUT;
    while ((*reg & mask) && timeout--)
        ;
    return (*reg & mask) ? -1 : 0;
}

static int adc_read_once(uint16_t *value) {
    (void)ADC3_DR;
    ADC3_SR = 0;
    ADC3_CR2 |= ADC_CR2_EXTTRIG | ADC_CR2_SWSTART;

    uint32_t timeout = LIGHT_ADC_TIMEOUT;
    while (!(ADC3_SR & ADC_SR_EOC) && timeout--)
        ;
    if (!(ADC3_SR & ADC_SR_EOC))
        return -1;
    *value = (uint16_t)(ADC3_DR & 0x0FFFU);
    return 0;
}

static uint8_t intensity_from_adc(uint16_t adc) {
    uint32_t clamped = adc > 4000U ? 4000U : adc;
    return (uint8_t)(100U - clamped / 40U);
}

#endif

int stm32_light_sensor_init(void) {
    light.ready = 0;
    light.auto_brightness = 0;
    light.raw_adc = 0;
    light.filtered_adc = 0;
    light.intensity_percent = 0;
    light.target_backlight_percent = stm32_backlight_percent();
    light.backlight_percent = stm32_backlight_percent();
    light.samples = 0;
    light.brightness_updates = 0;
    light.errors = 0;
#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    RCC_APB2ENR |= RCC_APB2ENR_IOPFEN | RCC_APB2ENR_ADC3EN;
    RCC_APB2RSTR |= RCC_APB2ENR_ADC3EN;
    RCC_APB2RSTR &= ~RCC_APB2ENR_ADC3EN;

    GPIOF_CRH &= ~0xFU;

    /* Keep the ADC clock below 14 MHz for all supported clock trees. */
    RCC_CFGR = (RCC_CFGR & ~(3U << 14)) | (2U << 14);

    ADC3_CR1 = 0;
    ADC3_CR2 = ADC_CR2_EXTSEL;
    ADC3_SMPR1 = 0;
    ADC3_SMPR2 = 7U << (LIGHT_ADC_CHANNEL * 3U);
    ADC3_SQR1 = 0;
    ADC3_SQR2 = 0;
    ADC3_SQR3 = LIGHT_ADC_CHANNEL;

    ADC3_CR2 |= ADC_CR2_ADON;
    for (volatile unsigned i = 0; i < 1000U; i++)
        __asm__ __volatile__("nop");
    ADC3_CR2 |= ADC_CR2_RSTCAL;
    if (wait_clear(&ADC3_CR2, ADC_CR2_RSTCAL) != 0)
        return -1;
    ADC3_CR2 |= ADC_CR2_CAL;
    if (wait_clear(&ADC3_CR2, ADC_CR2_CAL) != 0)
        return -1;

    light.ready = 1;
    light.auto_brightness = 0;
    if (stm32_light_sensor_sample() != 0) {
        light.ready = 0;
        light.auto_brightness = 0;
        return -1;
    }
    light.target_backlight_percent = 100U;
    stm32_backlight_set_percent(100U);
    light.backlight_percent = stm32_backlight_percent();
    return 0;
#endif
}

int stm32_light_sensor_sample(void) {
#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    if (!light.ready)
        return -1;

    uint32_t total = 0;
    uint16_t minimum = 0xFFFFU;
    uint16_t maximum = 0;
    for (unsigned i = 0; i < LIGHT_SAMPLE_COUNT; i++) {
        uint16_t value;
        if (adc_read_once(&value) != 0) {
            light.errors++;
            return -1;
        }
        total += value;
        if (value < minimum)
            minimum = value;
        if (value > maximum)
            maximum = value;
    }

    light.raw_adc =
        (uint16_t)((total - minimum - maximum) /
                   (LIGHT_SAMPLE_COUNT - 2U));
    if (light.samples == 0)
        light.filtered_adc = light.raw_adc;
    else
        light.filtered_adc =
            (uint16_t)(((uint32_t)light.filtered_adc * 3U +
                        light.raw_adc + 2U) / 4U);
    light.samples++;
    light.intensity_percent = intensity_from_adc(light.filtered_adc);
    if (light.auto_brightness) {
        uint8_t target = (uint8_t)(20U +
            ((uint16_t)light.intensity_percent * 80U) / 100U);
        light.target_backlight_percent = target;
        stm32_backlight_set_percent(target);
    }
    light.backlight_percent = stm32_backlight_percent();
    return 0;
#endif
}

void stm32_light_sensor_set_auto_brightness(int enable) {
    light.auto_brightness = enable ? 1 : 0;
    if (light.auto_brightness)
        light.target_backlight_percent =
            (uint8_t)(20U + ((uint16_t)light.intensity_percent * 80U) / 100U);
    stm32_backlight_set_percent(light.target_backlight_percent);
    light.backlight_percent = stm32_backlight_percent();
}

void stm32_light_sensor_set_manual_brightness(uint8_t percent) {
    if (percent > 100U)
        percent = 100U;
    light.auto_brightness = 0;
    light.target_backlight_percent = percent;
    stm32_backlight_set_percent(percent);
    light.backlight_percent = stm32_backlight_percent();
}

const stm32_light_sensor_info_t *stm32_light_sensor_info(void) {
    return &light;
}

#endif
