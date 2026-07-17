/*
 * Smart home hub — DHT11 driver (single-wire on PG11).
 * See dht11.h. Register style matches the rest of kernel/platform/stm32f103.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "drivers/stm32f1/dht11.h"
#include "core/arch.h"  /* arch_irq_save/restore — bit-bang critical section */
#include "drivers/stm32f1/stm32_uart.h" /* stm32_hclk_hz() */

#ifndef CONFIG_STM32_QEMU

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define RCC_APB2ENR_IOPGEN (1U << 8)

/* GPIOG @ 0x40012000 (APB2 GPIO bases are 0x400 apart: GPIOF=0x40011C00,
 * GPIOG=0x40012000 — matches display.c/extsram.c. The old 0x40011E00 here was
 * GPIOF+0x200, a reserved region, so PG11 was never actually driven/read and
 * every DHT11 read timed out -> valid=0.) */
#define GPIOG_CRH (*(volatile uint32_t *)0x40012004UL)
#define GPIOG_IDR (*(volatile uint32_t *)0x40012008UL)
#define GPIOG_BSRR (*(volatile uint32_t *)0x40012010UL)
#define GPIOG_BRR (*(volatile uint32_t *)0x40012014UL)

#define DHT_PIN 11U /* PG11 */

/* Cortex-M3 DWT cycle counter — accurate timing independent of flash wait
 * states / compiler codegen (the old nop-loop delay was ~1.5x off at 8 MHz,
 * which mis-sampled the bit stream). */
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004UL)
#define SCB_DEMCR  (*(volatile uint32_t *)0xE000EDFCUL)
#define DWT_CTRL_CYCCNTENA (1U << 0)
#define SCB_DEMCR_TRCENA   (1U << 24)

static uint32_t dht_cyc_per_us(void) {
    uint32_t hz = stm32_hclk_hz();
    uint32_t per = hz / 1000000U;
    return per ? per : 8U;
}

static void dht_dwt_enable(void) {
    SCB_DEMCR |= SCB_DEMCR_TRCENA;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
}

static void dht_delay_us(uint32_t us) {
    uint32_t start = DWT_CYCCNT;
    uint32_t cycles = us * dht_cyc_per_us();
    while ((uint32_t)(DWT_CYCCNT - start) < cycles)
        ;
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

/* Wait for the line to reach `level`, up to `timeout_us` (DWT-timed). Returns
 * elapsed us or -1 on timeout. */
static int dht_wait_level(int level, uint32_t timeout_us) {
    uint32_t start = DWT_CYCCNT;
    uint32_t limit = timeout_us * dht_cyc_per_us();
    while (dht_read_pin() != level) {
        if ((uint32_t)(DWT_CYCCNT - start) >= limit)
            return -1;
    }
    return (int)((uint32_t)(DWT_CYCCNT - start) / dht_cyc_per_us());
}

static int dht_read_byte(uint8_t *out) {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        /* Each bit is ~50us low then a high pulse whose WIDTH encodes the value
         * (26-28us='0', ~70us='1'). Wait out the low, then measure the high —
         * width-thresholding is far more timing-tolerant than sampling at a
         * fixed offset. */
        if (dht_wait_level(1, 100) < 0)
            return -1;
        uint32_t start = DWT_CYCCNT;
        uint32_t limit = 200U * dht_cyc_per_us();
        while (dht_read_pin()) {
            if ((uint32_t)(DWT_CYCCNT - start) >= limit)
                return -1;
        }
        uint32_t high_us = (uint32_t)(DWT_CYCCNT - start) / dht_cyc_per_us();
        v <<= 1;
        if (high_us > 45U) /* between 28us ('0') and 70us ('1') */
            v |= 1U;
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
    dht_dwt_enable();
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

    /* Start signal: pull low >=18ms (IRQs fine during this long low). */
    dht_pin_output();
    dht_low();
    for (uint32_t i = 0; i < 20U; i++)
        dht_delay_us(1000); /* ~20 ms */

    /* Time-critical from release through the last bit (~5 ms): disable IRQs so a
     * SysTick/USART ISR can't stretch a ~50us bit window and desync the read. */
    uint32_t flags = arch_irq_save();
    dht_high();
    dht_delay_us(30);
    dht_pin_input();

    /* Sensor response: ~80us low then ~80us high, then 5 data bytes. */
    int failed = (dht_wait_level(0, 100) < 0) || (dht_wait_level(1, 100) < 0) ||
                 (dht_wait_level(0, 100) < 0);
    for (int i = 0; !failed && i < 5; i++)
        if (dht_read_byte(&b[i]) < 0)
            failed = 1;
    arch_irq_restore(flags);

    if (failed)
        return -1;
    if ((uint8_t)(b[0] + b[1] + b[2] + b[3]) != b[4])
        return -1;

    *humidity = b[0];       /* integer %RH (DHT11 has no fraction) */
    *temp_c = (int16_t)b[2]; /* integer degrees C */
    return 0;
#endif
}

int stm32_dht11_read_debug(stm32_dht11_debug_t *dbg) {
#ifdef CONFIG_STM32_QEMU
    (void)dbg;
    return -1;
#else
    if (!dbg)
        return -1;
    for (int i = 0; i < 5; i++)
        dbg->raw[i] = 0;
    for (int i = 0; i < 3; i++)
        dbg->resp_us[i] = -1;
    for (int i = 0; i < 8; i++)
        dbg->bit_us[i] = -1;
    dbg->bytes_read = 0;
    dbg->failed_stage = -1;
    dbg->checksum_calc = 0;
    dbg->checksum_recv = 0;
    dbg->temp_c = 0;
    dbg->humidity = 0;

    /* Idle line level (pull-up + idle sensor should read high). */
    dht_pin_input();
    dht_delay_us(50);
    dbg->rest_level = dht_read_pin();

    /* Start signal: pull low >=18ms, release, wait for the sensor. */
    dht_pin_output();
    dht_low();
    for (uint32_t i = 0; i < 20U; i++)
        dht_delay_us(1000);

    uint32_t flags = arch_irq_save(); /* IRQ-off for the timing-critical read */
    dht_high();
    dht_delay_us(30);
    dht_pin_input();

    int e;
    if ((e = dht_wait_level(0, 100)) < 0) {
        dbg->failed_stage = 1;
        arch_irq_restore(flags);
        return -1;
    }
    dbg->resp_us[0] = e;
    if ((e = dht_wait_level(1, 100)) < 0) {
        dbg->failed_stage = 2;
        arch_irq_restore(flags);
        return -1;
    }
    dbg->resp_us[1] = e;
    if ((e = dht_wait_level(0, 100)) < 0) {
        dbg->failed_stage = 3;
        arch_irq_restore(flags);
        return -1;
    }
    dbg->resp_us[2] = e;

    /* First byte inline, capturing each bit's measured high-pulse width. */
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (dht_wait_level(1, 100) < 0) {
            dbg->failed_stage = 10;
            arch_irq_restore(flags);
            return -1;
        }
        uint32_t start = DWT_CYCCNT;
        uint32_t limit = 200U * dht_cyc_per_us();
        int timed_out = 0;
        while (dht_read_pin()) {
            if ((uint32_t)(DWT_CYCCNT - start) >= limit) {
                timed_out = 1;
                break;
            }
        }
        if (timed_out) {
            dbg->failed_stage = 10;
            arch_irq_restore(flags);
            return -1;
        }
        uint32_t high_us = (uint32_t)(DWT_CYCCNT - start) / dht_cyc_per_us();
        dbg->bit_us[i] = (int)high_us;
        v <<= 1;
        if (high_us > 45U)
            v |= 1U;
    }
    dbg->raw[0] = v;
    dbg->bytes_read = 1;

    for (int i = 1; i < 5; i++) {
        if (dht_read_byte(&dbg->raw[i]) < 0) {
            dbg->failed_stage = 10 + i;
            arch_irq_restore(flags);
            return -1;
        }
        dbg->bytes_read = i + 1;
    }
    arch_irq_restore(flags); /* reads done — checksum below isn't timing-critical */
    dbg->checksum_calc =
        (uint8_t)(dbg->raw[0] + dbg->raw[1] + dbg->raw[2] + dbg->raw[3]);
    dbg->checksum_recv = dbg->raw[4];
    dbg->humidity = dbg->raw[0];
    dbg->temp_c = (int16_t)dbg->raw[2];
    if (dbg->checksum_calc != dbg->checksum_recv) {
        dbg->failed_stage = 20;
        return -1;
    }
    return 0;
#endif
}

#endif /* CONFIG_BOARD_STM32F103 */
