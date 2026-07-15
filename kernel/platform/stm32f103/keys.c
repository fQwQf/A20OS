#ifdef CONFIG_BOARD_STM32F103

#include "keys.h"

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)

#define GPIOA_CRL  (*(volatile uint32_t *)0x40010800UL)
#define GPIOA_IDR  (*(volatile uint32_t *)0x40010808UL)
#define GPIOA_BSRR (*(volatile uint32_t *)0x40010810UL)

#define GPIOE_CRL  (*(volatile uint32_t *)0x40011800UL)
#define GPIOE_IDR  (*(volatile uint32_t *)0x40011808UL)
#define GPIOE_BSRR (*(volatile uint32_t *)0x40011810UL)

#define RCC_APB2ENR_IOPAEN (1U << 2)
#define RCC_APB2ENR_IOPEEN (1U << 6)

/*
 * Xuanwu key block, from the vendor examples:
 * PA0 is active high; PE4, PE3, and PE2 are active low.
 */
#define KEY_UP_PIN    0U
#define KEY_LEFT_PIN  2U
#define KEY_DOWN_PIN  3U
#define KEY_RIGHT_PIN 4U

#define KEY_DEBOUNCE_POLLS 2U

static int keys_initialized;
static uint8_t key_raw;
static uint8_t key_stable;
static unsigned key_same_polls;

#ifdef CONFIG_STM32_XUANWU
static void gpio_config_input(volatile uint32_t *crl, unsigned pin,
                              int pull_up) {
    uint32_t shift = pin * 4U;
    uint32_t value = *crl;
    value &= ~(0xFU << shift);
    value |= 0x8U << shift;
    *crl = value;

    if (pull_up) {
        if (crl == &GPIOA_CRL)
            GPIOA_BSRR = 1U << pin;
        else
            GPIOE_BSRR = 1U << pin;
    } else if (crl == &GPIOA_CRL) {
        GPIOA_BSRR = 1U << (pin + 16U);
    } else {
        GPIOE_BSRR = 1U << (pin + 16U);
    }
}
#endif

static uint8_t keys_read_raw(void) {
    uint8_t pressed = 0;

    if (GPIOA_IDR & (1U << KEY_UP_PIN))
        pressed |= 1U << STM32_KEY_UP;
    if (!(GPIOE_IDR & (1U << KEY_DOWN_PIN)))
        pressed |= 1U << STM32_KEY_DOWN;
    if (!(GPIOE_IDR & (1U << KEY_LEFT_PIN)))
        pressed |= 1U << STM32_KEY_LEFT;
    if (!(GPIOE_IDR & (1U << KEY_RIGHT_PIN)))
        pressed |= 1U << STM32_KEY_RIGHT;
    return pressed;
}

int stm32_keys_init(void) {
    keys_initialized = 0;
#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPEEN;

    gpio_config_input(&GPIOA_CRL, KEY_UP_PIN, 0);
    gpio_config_input(&GPIOE_CRL, KEY_LEFT_PIN, 1);
    gpio_config_input(&GPIOE_CRL, KEY_DOWN_PIN, 1);
    gpio_config_input(&GPIOE_CRL, KEY_RIGHT_PIN, 1);

    key_raw = keys_read_raw();
    key_stable = key_raw;
    key_same_polls = 0;
    keys_initialized = 1;
    return 0;
#endif
}

stm32_key_t stm32_keys_poll(void) {
    if (!keys_initialized)
        return STM32_KEY_NONE;

    uint8_t raw = keys_read_raw();
    if (raw != key_raw) {
        key_raw = raw;
        key_same_polls = 0;
        return STM32_KEY_NONE;
    }
    if (raw == key_stable || ++key_same_polls < KEY_DEBOUNCE_POLLS)
        return STM32_KEY_NONE;

    uint8_t pressed = raw & (uint8_t)~key_stable;
    key_stable = raw;
    key_same_polls = 0;

    if (pressed & (1U << STM32_KEY_UP))
        return STM32_KEY_UP;
    if (pressed & (1U << STM32_KEY_DOWN))
        return STM32_KEY_DOWN;
    if (pressed & (1U << STM32_KEY_LEFT))
        return STM32_KEY_LEFT;
    if (pressed & (1U << STM32_KEY_RIGHT))
        return STM32_KEY_RIGHT;
    return STM32_KEY_NONE;
}

int stm32_keys_ready(void) {
    return keys_initialized;
}

#endif
