/*
 * Smart home hub — IR NEC receiver (PB9 / EXTI9). See ir.h.
 *
 * Decodes from falling-edge *intervals*: NEC encodes each bit in the gap
 * between one falling edge and the next (leader 13.5ms, '0' 1.125ms, '1'
 * 2.25ms), so timestamping edges is enough — no pulse needs to be watched.
 *
 * This replaces a register-level port of docs/pz/28-红外遥控实验 that measured
 * every high pulse by busy-waiting inside the ISR, ~60ms per frame. That cost
 * more than it looked: it starved the USART RX interrupt (a byte is 1ms at
 * 9600, so a frame could eat 60 of them) and it was the reason SDIO's polled
 * FIFO overran (§4o) until the data phase started masking interrupts. It also
 * timed those pulses with a calibrated nop loop, the same unreliable pattern
 * §4l had to remove from DHT11 — at 72MHz its "20us" unit was a guess.
 *
 * The ISR is now a timestamp, a subtraction and a compare: tens of cycles.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "drivers/stm32f1/ir.h"
#include "drivers/stm32f1/stm32_uart.h" /* stm32_hclk_hz() */

static volatile uint32_t ir_code;
static volatile int ir_ready;

#ifndef CONFIG_STM32_QEMU

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define RCC_APB2ENR_IOPBEN (1U << 3)
#define RCC_APB2ENR_AFIOEN (1U << 0)

/* GPIOB @ 0x40010C00 */
#define GPIOB_CRH (*(volatile uint32_t *)0x40010C04UL)
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

#define SCB_DEMCR (*(volatile uint32_t *)0xE000EDFCUL)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004UL)

#define IR_PIN 9U

/*
 * Falling-edge interval windows, in microseconds.
 *
 * NEC nominals: leader 9ms mark + 4.5ms space = 13.5ms; '0' 562.5us + 562.5us
 * = 1.125ms; '1' 562.5us + 1687.5us = 2.25ms. The windows are wide enough for
 * cheap remotes but keep '0' and '1' well apart (1.5ms vs 1.8ms edges).
 *
 * A repeat burst (9ms + 2.25ms = 11.25ms) deliberately falls *outside* the
 * leader window and is therefore ignored — holding a key does not retrigger,
 * which matches the previous decoder's behaviour.
 */
#define IR_LEADER_MIN_US 12000U
#define IR_LEADER_MAX_US 15500U
#define IR_ZERO_MIN_US 800U
#define IR_ZERO_MAX_US 1500U
#define IR_ONE_MIN_US 1800U
#define IR_ONE_MAX_US 2700U

/* Decoder state, touched only by the ISR. */
static uint32_t ir_last_edge_cyc;
static uint32_t ir_shift;
static uint8_t ir_bit_count;
static uint8_t ir_in_frame;
static uint32_t ir_cyc_per_us = 1U;

/* Diagnostics: how the receiver is being exercised, and how often it is being
 * handed something that is not NEC (ambient IR, fluorescent light). */
static volatile uint32_t ir_edges;
static volatile uint32_t ir_frames;
static volatile uint32_t ir_aborts;

#endif /* !CONFIG_STM32_QEMU */

int stm32_ir_init(void) {
    ir_ready = 0;
    ir_code = 0;
#ifdef CONFIG_STM32_QEMU
    return -1;
#else
    ir_last_edge_cyc = 0;
    ir_shift = 0;
    ir_bit_count = 0;
    ir_in_frame = 0;
    ir_edges = 0;
    ir_frames = 0;
    ir_aborts = 0;

    ir_cyc_per_us = stm32_hclk_hz() / 1000000U;
    if (ir_cyc_per_us == 0U)
        ir_cyc_per_us = 1U;
    SCB_DEMCR |= 1U << 24; /* TRCENA  */
    DWT_CTRL |= 1U;        /* CYCCNTENA */

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

    /* Below SysTick, as before. The handler is short now, so this matters far
     * less than it did — but IR is still the least urgent thing on the board. */
    NVIC_IPR[EXTI9_5_IRQN] = 0x80U;
    NVIC_ISER0 = 1U << EXTI9_5_IRQN;
    return 0;
#endif
}

void stm32_ir_isr(void) {
#ifndef CONFIG_STM32_QEMU
    uint32_t now = DWT_CYCCNT;
    uint32_t us = (now - ir_last_edge_cyc) / ir_cyc_per_us;

    ir_last_edge_cyc = now;
    EXTI_PR = 1U << IR_PIN; /* ack immediately: nothing below blocks */
    ir_edges++;

    if (!ir_in_frame) {
        /* Waiting for a leader. The very first edge of a burst measures against
         * a stale timestamp and lands here harmlessly; the *next* edge is the
         * one 13.5ms later that identifies the leader. */
        if (us >= IR_LEADER_MIN_US && us <= IR_LEADER_MAX_US) {
            ir_in_frame = 1;
            ir_shift = 0;
            ir_bit_count = 0;
        }
        return;
    }

    uint32_t bit;
    if (us >= IR_ZERO_MIN_US && us <= IR_ZERO_MAX_US) {
        bit = 0;
    } else if (us >= IR_ONE_MIN_US && us <= IR_ONE_MAX_US) {
        bit = 1;
    } else {
        /* Out of spec: a dropped edge, noise, or a gap between frames. Abandon
         * the partial frame rather than shifting in a wrong bit — a corrupted
         * code could dispatch a real action. */
        ir_in_frame = 0;
        ir_aborts++;
        return;
    }

    ir_shift = (ir_shift << 1) | bit; /* MSB first: addr:~addr:cmd:~cmd */
    if (++ir_bit_count >= 32U) {
        ir_code = ir_shift;
        ir_ready = 1;
        ir_in_frame = 0;
        ir_frames++;
    }
#endif
}

int stm32_ir_poll(uint32_t *code) {
    if (!ir_ready)
        return 0;
    *code = ir_code;
    ir_ready = 0;
    return 1;
}

void stm32_ir_stats(uint32_t *edges, uint32_t *frames, uint32_t *aborts) {
#ifdef CONFIG_STM32_QEMU
    if (edges)
        *edges = 0;
    if (frames)
        *frames = 0;
    if (aborts)
        *aborts = 0;
#else
    if (edges)
        *edges = ir_edges;
    if (frames)
        *frames = ir_frames;
    if (aborts)
        *aborts = ir_aborts;
#endif
}

#endif /* CONFIG_BOARD_STM32F103 */
