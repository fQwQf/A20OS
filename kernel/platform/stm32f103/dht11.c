/*
 * Smart home hub — DHT11 driver (single-wire on PG11).
 * See dht11.h. Register style matches the rest of kernel/platform/stm32f103.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "dht11.h"
#include "stm32_uart.h" /* stm32_hclk_hz() */

#ifndef CONFIG_STM32_QEMU

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define RCC_APB2ENR_IOPGEN (1U << 8)

/* GPIOG @ 0x40011E00 */
#define GPIOG_CRH (*(volatile uint32_t *)0x40011E04UL)
#define GPIOG_IDR (*(volatile uint32_t *)0x40011E08UL)
#define GPIOG_BSRR (*(volatile uint32_t *)0x40011E10UL)
#define GPIOG_BRR (*(volatile uint32_t *)0x40011E14UL)

#define DHT_PIN 11U /* PG11 */

/*
 * Busy-loop microsecond delay scaled by the live core clock. This is a
 * best-effort starting point; the exact cycles/iteration depends on flash
 * wait states, so verify the DHT11 waveform on-board and tweak DHT_CAL if
 * reads are flaky. (~4 cycles per iteration assumed.)
 */
#define DHT_CAL 4U
static void dht_delay_us(uint32_t us) {
    uint32_t mhz = stm32_hclk_hz() / 1000000U;
    if (mhz == 0U)
        mhz = 1U;
    volatile uint32_t iters = (us * mhz) / DHT_CAL + 1U;
    while (iters--)
        __asm__ __volatile__("nop");
}

static void dht_pin_output(void) {
    /* PG11 = CRH bits [15:12] -> general purpose push-pull, 2 MHz (0x2). */
    uint32_t crh = GPIOG_CRH;
    crh &= ~(0xFU << ((DHT_PIN - 8U) * 4U));
    crh |= 0x2U << ((DHT_PIN - 8U) * 4U);
    GPIOG_CRH = crh;
}

static void dht_pin_input(void) {
    /* PG11 = input with pull-up (0x8), pull direction set via ODR=1/BSRR. */
    uint32_t crh = GPIOG_CRH;
    crh &= ~(0xFU << ((DHT_PIN - 8U) * 4U));
    crh |= 0x8U << ((DHT_PIN - 8U) * 4U);
    GPIOG_CRH = crh;
    GPIOG_BSRR = 1U << DHT_PIN; /* pull-up */
}

static void dht_high(void) { GPIOG_BSRR = 1U << DHT_PIN; }
static void dht_low(void) { GPIOG_BRR = 1U << DHT_PIN; }
static int dht_read_pin(void) { return (GPIOG_IDR >> DHT_PIN) & 1U; }

/* Wait for the line to reach `level`, up to `timeout_us`. Returns elapsed us
 * (rough) or -1 on timeout. */
static int dht_wait_level(int level, uint32_t timeout_us) {
    uint32_t t = 0;
    while (dht_read_pin() != level) {
        if (t >= timeout_us)
            return -1;
        dht_delay_us(1);
        t++;
    }
    return (int)t;
}

static int dht_read_byte(uint8_t *out) {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (dht_wait_level(1, 80) < 0) /* wait for 50us low -> high */
            return -1;
        /* 26-28us high = '0', ~70us high = '1'. Sample after 40us. */
        dht_delay_us(40);
        v <<= 1;
        if (dht_read_pin())
            v |= 1U;
        if (dht_wait_level(0, 80) < 0) /* wait for the rest of the high */
            return -1;
    }
    *out = v;
    return 0;
}

#endif /* !CONFIG_STM32_QEMU */

int stm32_dht11_init(void) {
#ifdef CONFIG_STM32_QEMU
    return -1; /* no DHT11 in the QEMU model */
#else
    RCC_APB2ENR |= RCC_APB2ENR_IOPGEN;
    dht_pin_input();
    return 0;
#endif
}

int stm32_dht11_read(int16_t *temp_c, uint8_t *humidity) {
#ifdef CONFIG_STM32_QEMU
    (void)temp_c;
    (void)humidity;
    return -1;
#else
    uint8_t b[5];

    /* Start signal: pull low >=18ms, release, wait for the sensor. */
    dht_pin_output();
    dht_low();
    for (uint32_t i = 0; i < 20U; i++)
        dht_delay_us(1000); /* ~20 ms */
    dht_high();
    dht_delay_us(30);
    dht_pin_input();

    /* Sensor response: ~80us low then ~80us high. */
    if (dht_wait_level(0, 100) < 0)
        return -1;
    if (dht_wait_level(1, 100) < 0)
        return -1;
    if (dht_wait_level(0, 100) < 0)
        return -1;

    for (int i = 0; i < 5; i++) {
        if (dht_read_byte(&b[i]) < 0)
            return -1;
    }
    if ((uint8_t)(b[0] + b[1] + b[2] + b[3]) != b[4])
        return -1;

    *humidity = b[0];       /* integer %RH (DHT11 has no fraction) */
    *temp_c = (int16_t)b[2]; /* integer degrees C */
    return 0;
#endif
}

#endif /* CONFIG_BOARD_STM32F103 */
