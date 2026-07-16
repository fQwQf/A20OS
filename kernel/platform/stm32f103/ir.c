/*
 * Smart home hub — IR NEC receiver (PB9 / EXTI9). See ir.h.
 * Register-level port of docs/pz/28-红外遥控实验.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "ir.h"
#include "stm32_uart.h" /* stm32_hclk_hz() */

static volatile uint32_t ir_code;
static volatile int ir_ready;

#ifndef CONFIG_STM32_QEMU

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define RCC_APB2ENR_IOPBEN (1U << 3)
#define RCC_APB2ENR_AFIOEN (1U << 0)

/* GPIOB @ 0x40010C00 */
#define GPIOB_CRH (*(volatile uint32_t *)0x40010C04UL)
#define GPIOB_IDR (*(volatile uint32_t *)0x40010C08UL)
#define GPIOB_BSRR (*(volatile uint32_t *)0x40010C10UL)

/* AFIO @ 0x40010000 */
#define AFIO_EXTICR3 (*(volatile uint32_t *)0x40010010UL)

/* EXTI @ 0x40010400 */
#define EXTI_IMR (*(volatile uint32_t *)0x40010400UL)
#define EXTI_FTSR (*(volatile uint32_t *)0x4001040CUL)
#define EXTI_PR (*(volatile uint32_t *)0x40010414UL)

/* NVIC */
#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100UL)
#define NVIC_IPR ((volatile uint8_t *)0xE000E400UL)
#define EXTI9_5_IRQN 23U

#define IR_PIN 9U

#define IR_CAL 4U
static void ir_delay_us(uint32_t us) {
    uint32_t mhz = stm32_hclk_hz() / 1000000U;
    if (mhz == 0U)
        mhz = 1U;
    volatile uint32_t iters = (us * mhz) / IR_CAL + 1U;
    while (iters--)
        __asm__ __volatile__("nop");
}

static int ir_pin_high(void) { return (GPIOB_IDR >> IR_PIN) & 1U; }

/* High-level duration in ~20us units, capped at 250 (matches docs/pz). */
static uint8_t ir_high_time(void) {
    uint8_t t = 0;
    while (ir_pin_high()) {
        t++;
        ir_delay_us(20);
        if (t >= 250)
            return t;
    }
    return t;
}

#endif /* !CONFIG_STM32_QEMU */

int stm32_ir_init(void) {
    ir_ready = 0;
    ir_code = 0;
#ifdef CONFIG_STM32_QEMU
    return -1;
#else
    RCC_APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;

    /* PB9 = input pull-up (CNF=10b, MODE=00b -> 0x8), then ODR=1 for pull-up. */
    uint32_t crh = GPIOB_CRH;
    crh &= ~(0xFU << ((IR_PIN - 8U) * 4U));
    crh |= 0x8U << ((IR_PIN - 8U) * 4U);
    GPIOB_CRH = crh;
    GPIOB_BSRR = 1U << IR_PIN;

    /* Route EXTI9 to port B: EXTICR3 nibble for line 9 = bits [7:4]. */
    AFIO_EXTICR3 = (AFIO_EXTICR3 & ~(0xFU << 4)) | (0x1U << 4);

    EXTI_FTSR |= 1U << IR_PIN; /* falling edge */
    EXTI_IMR |= 1U << IR_PIN;  /* unmask */
    EXTI_PR = 1U << IR_PIN;    /* clear pending */

    /* Lower priority than SysTick so the ms tick keeps running during the
     * busy-loop decode. */
    NVIC_IPR[EXTI9_5_IRQN] = 0x80U;
    NVIC_ISER0 = 1U << EXTI9_5_IRQN;
    return 0;
#endif
}

void stm32_ir_isr(void) {
#ifndef CONFIG_STM32_QEMU
    uint8_t tim, ok = 0, data = 0, num = 0;
    uint32_t code = 0;

    while (1) {
        if (ir_pin_high()) {
            tim = ir_high_time();
            if (tim >= 250)
                break; /* not a valid signal / timeout */
            if (tim >= 200)
                ok = 1; /* leader */
            else if (tim >= 60 && tim < 90)
                data = 1;
            else if (tim >= 10 && tim < 50)
                data = 0;

            if (ok) {
                code <<= 1;
                code += data;
                if (num >= 32) {
                    ir_code = code;
                    ir_ready = 1;
                    break;
                }
            }
            num++;
        }
    }
    EXTI_PR = 1U << IR_PIN; /* clear pending */
#endif
}

int stm32_ir_poll(uint32_t *code) {
    if (!ir_ready)
        return 0;
    *code = ir_code;
    ir_ready = 0;
    return 1;
}

#endif /* CONFIG_BOARD_STM32F103 */
