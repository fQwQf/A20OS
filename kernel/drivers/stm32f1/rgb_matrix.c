/* Xuanwu 5x5 WS2812B RGB matrix, DAT on PE5. */
#ifdef CONFIG_BOARD_STM32F103

#include "drivers/stm32f1/rgb_matrix.h"
#include "core/arch.h"
#include "drivers/stm32f1/stm32_uart.h"

#define RGB_MATRIX_DEFAULT_BRIGHTNESS 32U
#define RGB_MATRIX_MIN_HCLK_HZ 48000000U
#define RGB_MATRIX_BIT_RATE 800000U
#define RGB_MATRIX_RESET_US 300U

static uint8_t pixels[STM32_RGB_MATRIX_PIXELS][3]; /* RGB, row-major */
static uint8_t matrix_brightness = RGB_MATRIX_DEFAULT_BRIGHTNESS;
static int matrix_ready;

#if defined(CONFIG_STM32_XUANWU) && !defined(CONFIG_STM32_QEMU)

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define RCC_APB2ENR_IOPEEN (1U << 6)

#define GPIOE_CRL  (*(volatile uint32_t *)0x40011800UL)
#define GPIOE_BSRR (*(volatile uint32_t *)0x40011810UL)
#define GPIOE_BRR  (*(volatile uint32_t *)0x40011814UL)
#define RGB_MATRIX_PIN 5U

#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004UL)
#define SCB_DEMCR  (*(volatile uint32_t *)0xE000EDFCUL)
#define DWT_CTRL_CYCCNTENA (1U << 0)
#define DWT_CTRL_NOCYCCNT   (1U << 25)
#define SCB_DEMCR_TRCENA   (1U << 24)

static inline __attribute__((always_inline)) void matrix_high(void) {
    GPIOE_BSRR = 1U << RGB_MATRIX_PIN;
}

static inline __attribute__((always_inline)) void matrix_low(void) {
    GPIOE_BRR = 1U << RGB_MATRIX_PIN;
}

static inline __attribute__((always_inline)) void wait_cycles(uint32_t start,
                                                               uint32_t cycles) {
    while ((uint32_t)(DWT_CYCCNT - start) < cycles)
        ;
}

static void delay_us(uint32_t us, uint32_t cycles_per_us) {
    uint32_t start = DWT_CYCCNT;
    wait_cycles(start, us * cycles_per_us);
}

static inline __attribute__((always_inline)) void write_bit(
    unsigned one, uint32_t bit_cycles, uint32_t zero_high_cycles,
    uint32_t one_high_cycles) {
    uint32_t high_cycles = one ? one_high_cycles : zero_high_cycles;
    matrix_high();
    uint32_t start = DWT_CYCCNT;
    wait_cycles(start, high_cycles);
    matrix_low();
    start = DWT_CYCCNT;
    wait_cycles(start, bit_cycles - high_cycles);
}

static void write_byte(uint8_t value, uint32_t bit_cycles,
                       uint32_t zero_high_cycles, uint32_t one_high_cycles) {
    for (uint8_t mask = 0x80U; mask != 0; mask >>= 1)
        write_bit(value & mask, bit_cycles, zero_high_cycles, one_high_cycles);
}

static uint8_t scale_channel(uint8_t value) {
    return (uint8_t)(((uint16_t)value * matrix_brightness + 127U) / 255U);
}

#endif

int stm32_rgb_matrix_init(void) {
    stm32_rgb_matrix_clear();
    matrix_brightness = RGB_MATRIX_DEFAULT_BRIGHTNESS;
    matrix_ready = 0;
#if !defined(CONFIG_STM32_XUANWU) || defined(CONFIG_STM32_QEMU)
    return -1;
#else
    RCC_APB2ENR |= RCC_APB2ENR_IOPEEN;
    /* PE5: 50 MHz general-purpose push-pull output. */
    GPIOE_CRL = (GPIOE_CRL & ~(0xFU << (RGB_MATRIX_PIN * 4U))) |
                (0x3U << (RGB_MATRIX_PIN * 4U));
    matrix_low();

    SCB_DEMCR |= SCB_DEMCR_TRCENA;
    if (DWT_CTRL & DWT_CTRL_NOCYCCNT)
        return -1;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
    if (stm32_hclk_hz() < RGB_MATRIX_MIN_HCLK_HZ) {
        return -1;
    }
    uint32_t probe = DWT_CYCCNT;
    for (volatile unsigned i = 0; i < 16U; i++)
        __asm__ __volatile__("nop");
    if (DWT_CYCCNT == probe)
        return -1;

    delay_us(RGB_MATRIX_RESET_US, stm32_hclk_hz() / 1000000U);
    matrix_ready = 1;
    return stm32_rgb_matrix_show();
#endif
}

int stm32_rgb_matrix_ready(void) { return matrix_ready; }

int stm32_rgb_matrix_set_pixel(uint8_t x, uint8_t y, uint32_t rgb) {
    if (x >= STM32_RGB_MATRIX_WIDTH || y >= STM32_RGB_MATRIX_HEIGHT)
        return -1;
    unsigned index = (unsigned)y * STM32_RGB_MATRIX_WIDTH + x;
    pixels[index][0] = (uint8_t)(rgb >> 16);
    pixels[index][1] = (uint8_t)(rgb >> 8);
    pixels[index][2] = (uint8_t)rgb;
    return 0;
}

uint32_t stm32_rgb_matrix_get_pixel(uint8_t x, uint8_t y) {
    if (x >= STM32_RGB_MATRIX_WIDTH || y >= STM32_RGB_MATRIX_HEIGHT)
        return STM32_RGB_COLOR_BLACK;
    unsigned index = (unsigned)y * STM32_RGB_MATRIX_WIDTH + x;
    return ((uint32_t)pixels[index][0] << 16) |
           ((uint32_t)pixels[index][1] << 8) | pixels[index][2];
}

void stm32_rgb_matrix_fill(uint32_t rgb) {
    uint8_t red = (uint8_t)(rgb >> 16);
    uint8_t green = (uint8_t)(rgb >> 8);
    uint8_t blue = (uint8_t)rgb;
    for (unsigned i = 0; i < STM32_RGB_MATRIX_PIXELS; i++) {
        pixels[i][0] = red;
        pixels[i][1] = green;
        pixels[i][2] = blue;
    }
}

void stm32_rgb_matrix_clear(void) {
    stm32_rgb_matrix_fill(STM32_RGB_COLOR_BLACK);
}

void stm32_rgb_matrix_set_brightness(uint8_t brightness) {
    matrix_brightness = brightness;
}

uint8_t stm32_rgb_matrix_brightness(void) { return matrix_brightness; }

int stm32_rgb_matrix_show(void) {
#if !defined(CONFIG_STM32_XUANWU) || defined(CONFIG_STM32_QEMU)
    return -1;
#else
    if (!matrix_ready)
        return -1;

    uint32_t hclk = stm32_hclk_hz();
    if (hclk < RGB_MATRIX_MIN_HCLK_HZ) {
        matrix_ready = 0;
        matrix_low();
        return -1;
    }
    uint32_t bit_cycles = (hclk + RGB_MATRIX_BIT_RATE / 2U) /
                          RGB_MATRIX_BIT_RATE;
    uint32_t zero_high_cycles = (bit_cycles * 28U + 50U) / 100U;
    uint32_t one_high_cycles = (bit_cycles * 56U + 50U) / 100U;
    uint32_t cycles_per_us = hclk / 1000000U;

    /* A complete 25-pixel frame is about 0.75 ms. Interrupts must not stretch
     * any pulse inside that window; the reset-low interval can be interruptible. */
    uint32_t flags = arch_irq_save();
    for (unsigned i = 0; i < STM32_RGB_MATRIX_PIXELS; i++) {
        write_byte(scale_channel(pixels[i][1]), bit_cycles,
                   zero_high_cycles, one_high_cycles); /* G */
        write_byte(scale_channel(pixels[i][0]), bit_cycles,
                   zero_high_cycles, one_high_cycles); /* R */
        write_byte(scale_channel(pixels[i][2]), bit_cycles,
                   zero_high_cycles, one_high_cycles); /* B */
    }
    matrix_low();
    arch_irq_restore(flags);
    delay_us(RGB_MATRIX_RESET_US, cycles_per_us);
    return 0;
#endif
}

#endif
