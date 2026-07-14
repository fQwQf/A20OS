#ifdef CONFIG_BOARD_STM32F103

#include "touch.h"

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)

#define GPIOB_CRL  (*(volatile uint32_t *)0x40010C00UL)
#define GPIOB_CRH  (*(volatile uint32_t *)0x40010C04UL)
#define GPIOB_IDR  (*(volatile uint32_t *)0x40010C08UL)
#define GPIOB_BSRR (*(volatile uint32_t *)0x40010C10UL)
#define GPIOB_BRR  (*(volatile uint32_t *)0x40010C14UL)

#define GPIOF_CRH  (*(volatile uint32_t *)0x40011C04UL)
#define GPIOF_IDR  (*(volatile uint32_t *)0x40011C08UL)
#define GPIOF_BSRR (*(volatile uint32_t *)0x40011C10UL)
#define GPIOF_BRR  (*(volatile uint32_t *)0x40011C14UL)

/*
 * Xuanwu TFT connector, from the vendor schematic:
 * PB1=T_SCK, PB2=T_MISO, PB10=T_MOSI, PF10=T_PEN, PF11=T_CS.
 */
#define TOUCH_SCK_PIN  1U
#define TOUCH_MISO_PIN 2U
#define TOUCH_MOSI_PIN 10U
#define TOUCH_PEN_PIN  10U
#define TOUCH_CS_PIN   11U

#define TOUCH_WIDTH 320U
#define TOUCH_HEIGHT 480U
#define TOUCH_SAMPLE_COUNT 7U
#define TOUCH_MAX_SPREAD 180U

static int touch_initialized;
static int touch_was_pressed;
static uint16_t last_x;
static uint16_t last_y;
static stm32_touch_calibration_t touch_calibration = {
    .x_min = 180U,
    .x_max = 3900U,
    .y_min = 220U,
    .y_max = 3900U,
    .swap_xy = 0,
    .invert_x = 1,
    .invert_y = 0,
};

static void gpio_config_pin(volatile uint32_t *crl, volatile uint32_t *crh,
                            unsigned pin, uint32_t mode) {
    volatile uint32_t *reg = pin < 8U ? crl : crh;
    uint32_t shift = (pin & 7U) * 4U;
    uint32_t value = *reg;
    value &= ~(0xFU << shift);
    value |= mode << shift;
    *reg = value;
}

static void gpio_set(volatile uint32_t *bsrr, volatile uint32_t *brr,
                     unsigned pin, int high) {
    if (high)
        *bsrr = 1U << pin;
    else
        *brr = 1U << pin;
}

static int gpio_get(volatile uint32_t *idr, unsigned pin) {
    return !!(*idr & (1U << pin));
}

static void touch_sck(int high) {
    gpio_set(&GPIOB_BSRR, &GPIOB_BRR, TOUCH_SCK_PIN, high);
}

static void touch_mosi(int high) {
    gpio_set(&GPIOB_BSRR, &GPIOB_BRR, TOUCH_MOSI_PIN, high);
}

static void touch_cs(int high) {
    gpio_set(&GPIOF_BSRR, &GPIOF_BRR, TOUCH_CS_PIN, high);
}

static int touch_pen_released(void) {
    return gpio_get(&GPIOF_IDR, TOUCH_PEN_PIN);
}

static void touch_delay(void) {
    for (volatile unsigned i = 0; i < 12; i++)
        __asm__ __volatile__("nop");
}

static uint8_t touch_spi_transfer(uint8_t value) {
    uint8_t result = 0;
    for (unsigned i = 0; i < 8; i++) {
        touch_mosi(!!(value & 0x80U));
        value <<= 1;
        touch_sck(1);
        touch_delay();
        result = (uint8_t)((result << 1) |
                          gpio_get(&GPIOB_IDR, TOUCH_MISO_PIN));
        touch_sck(0);
        touch_delay();
    }
    return result;
}

static uint16_t touch_read_adc(uint8_t command) {
    touch_cs(0);
    touch_spi_transfer(command);
    uint16_t value = (uint16_t)touch_spi_transfer(0) << 8;
    value |= touch_spi_transfer(0);
    touch_cs(1);
    return (value >> 3) & 0x0FFFU;
}

static int touch_filtered_sample(uint8_t command, uint16_t *sample) {
    uint32_t total = 0;
    uint16_t min = 0xFFFFU;
    uint16_t max = 0;

    for (unsigned i = 0; i < TOUCH_SAMPLE_COUNT; i++) {
        uint16_t value = touch_read_adc(command);
        total += value;
        if (value < min)
            min = value;
        if (value > max)
            max = value;
    }
    if (max - min > TOUCH_MAX_SPREAD)
        return -1;
    *sample = (uint16_t)((total - min - max) /
                         (TOUCH_SAMPLE_COUNT - 2U));
    return 0;
}

static uint16_t scale_axis(uint16_t raw, uint16_t min, uint16_t max,
                           uint16_t pixels) {
    if (raw <= min)
        return 0;
    if (raw >= max)
        return pixels - 1U;
    return (uint16_t)(((uint32_t)(raw - min) * (pixels - 1U)) /
                      (max - min));
}

int stm32_touch_init(void) {
    touch_initialized = 0;
    touch_was_pressed = 0;
#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    RCC_APB2ENR |= (1U << 3) | (1U << 7);

    gpio_config_pin(&GPIOB_CRL, &GPIOB_CRH, TOUCH_SCK_PIN, 0x3U);
    gpio_config_pin(&GPIOB_CRL, &GPIOB_CRH, TOUCH_MISO_PIN, 0x8U);
    gpio_config_pin(&GPIOB_CRL, &GPIOB_CRH, TOUCH_MOSI_PIN, 0x3U);
    gpio_config_pin(&GPIOF_CRH, &GPIOF_CRH, TOUCH_PEN_PIN, 0x8U);
    gpio_config_pin(&GPIOF_CRH, &GPIOF_CRH, TOUCH_CS_PIN, 0x3U);

    GPIOB_BSRR = 1U << TOUCH_MISO_PIN;
    GPIOF_BSRR = 1U << TOUCH_PEN_PIN;
    touch_cs(1);
    touch_sck(0);
    touch_mosi(0);
    touch_initialized = 1;

    /*
     * A populated resistive controller pulls PENIRQ low only while pressed.
     * A missing display/touch module leaves it pulled high, which is a valid
     * optional-device state and must not delay boot.
     */
    return 0;
#endif
}

int stm32_touch_ready(void) {
    return touch_initialized;
}

int stm32_touch_poll(uint16_t *x, uint16_t *y) {
    if (!touch_initialized || touch_pen_released()) {
        touch_was_pressed = 0;
        return 0;
    }

    uint16_t raw_x;
    uint16_t raw_y;
    if (touch_filtered_sample(0xD0U, &raw_x) != 0 ||
        touch_filtered_sample(0x90U, &raw_y) != 0 ||
        touch_pen_released())
        return 0;
    if (raw_x < 50U || raw_y < 50U)
        return 0;

    if (touch_calibration.swap_xy) {
        uint16_t tmp = raw_x;
        raw_x = raw_y;
        raw_y = tmp;
    }

    uint16_t point_x =
        scale_axis(raw_x, touch_calibration.x_min, touch_calibration.x_max,
                   TOUCH_WIDTH);
    uint16_t point_y =
        scale_axis(raw_y, touch_calibration.y_min, touch_calibration.y_max,
                   TOUCH_HEIGHT);
    if (touch_calibration.invert_x)
        point_x = TOUCH_WIDTH - 1U - point_x;
    if (touch_calibration.invert_y)
        point_y = TOUCH_HEIGHT - 1U - point_y;

    if (touch_was_pressed) {
        point_x = (uint16_t)(((uint32_t)last_x * 3U + point_x) / 4U);
        point_y = (uint16_t)(((uint32_t)last_y * 3U + point_y) / 4U);
    }
    last_x = point_x;
    last_y = point_y;
    touch_was_pressed = 1;

    if (x)
        *x = point_x;
    if (y)
        *y = point_y;
    return 1;
}

void stm32_touch_set_calibration(
    const stm32_touch_calibration_t *calibration) {
    if (!calibration || calibration->x_min >= calibration->x_max ||
        calibration->y_min >= calibration->y_max)
        return;
    touch_calibration = *calibration;
}

const stm32_touch_calibration_t *stm32_touch_get_calibration(void) {
    return &touch_calibration;
}

#endif
