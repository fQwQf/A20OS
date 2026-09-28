#ifdef CONFIG_BOARD_STM32F103

#include "drivers/stm32f1/stm32_uart.h"

#define RCC_CFGR    (*(volatile uint32_t *)0x40021004UL)
#define RCC_APB1RSTR (*(volatile uint32_t *)0x40021010UL)
#define RCC_APB2RSTR (*(volatile uint32_t *)0x4002100CUL)
#define RCC_APB1ENR (*(volatile uint32_t *)0x4002101CUL)
#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define AFIO_MAPR   (*(volatile uint32_t *)0x40010004UL)

#define GPIOA_CRL (*(volatile uint32_t *)0x40010800UL)
#define GPIOA_CRH (*(volatile uint32_t *)0x40010804UL)
#define GPIOA_IDR (*(volatile uint32_t *)0x40010808UL)
#define GPIOB_CRH (*(volatile uint32_t *)0x40010C04UL)
#define GPIOB_IDR (*(volatile uint32_t *)0x40010C08UL)

#define NVIC_ISER_BASE 0xE000E100UL
#define NVIC_ICER_BASE 0xE000E180UL
#define NVIC_IPR       ((volatile uint8_t *)0xE000E400UL)
/* NVIC_ISER/ICER are write-only 32-bit words at a 4-byte stride, one word per
 * 32 exception numbers (ISER0 = 0xE000E100), and reading either returns 0, so
 * the enable mask cannot be recovered from the register and has to be tracked
 * in software.  NVIC_IPR is an array of 8-bit priority bytes at 0xE000E400, one
 * per exception number; on the Cortex-M3 only the top four bits of each byte
 * are implemented ([7:4] preemption priority, [3:0] sub-priority) and the
 * readback is delayed, so a priority write must be confirmed in software. */

#define USART1_BASE 0x40013800UL
#define USART2_BASE 0x40004400UL
#define USART3_BASE 0x40004800UL

#define RCC_APB2ENR_AFIOEN  (1U << 0)
#define RCC_APB2ENR_IOPAEN  (1U << 2)
#define RCC_APB2ENR_IOPBEN  (1U << 3)
#define RCC_APB2ENR_USART1EN (1U << 14)
#define RCC_APB1ENR_USART2EN (1U << 17)
#define RCC_APB1ENR_USART3EN (1U << 18)

#define USART_SR_PE   (1U << 0)
#define USART_SR_FE   (1U << 1)
#define USART_SR_NE   (1U << 2)
#define USART_SR_ORE  (1U << 3)
#define USART_SR_RXNE (1U << 5)
#define USART_SR_TC   (1U << 6)
#define USART_SR_TXE  (1U << 7)
#define USART_SR_ERROR_MASK \
    (USART_SR_PE | USART_SR_FE | USART_SR_NE | USART_SR_ORE)

#define USART_CR1_RE     (1U << 2)
#define USART_CR1_TE     (1U << 3)
#define USART_CR1_RXNEIE (1U << 5)
#define USART_CR1_UE     (1U << 13)

#define AFIO_MAPR_USART3_REMAP_MASK (3U << 4)

#define STM32_HSI_HZ 8000000U
#define STM32_HSE_HZ 8000000U

typedef struct stm32_usart_regs {
    volatile uint32_t sr;
    volatile uint32_t dr;
    volatile uint32_t brr;
    volatile uint32_t cr1;
    volatile uint32_t cr2;
    volatile uint32_t cr3;
} stm32_usart_regs_t;

typedef struct stm32_uart_desc {
    stm32_usart_regs_t *regs;
    uint32_t irq;
    int apb2;
} stm32_uart_desc_t;

static const stm32_uart_desc_t uart_desc[STM32_UART_PORT_COUNT] = {
    [STM32_UART_USART1] = {
        .regs = (stm32_usart_regs_t *)USART1_BASE,
        .irq = 37U,
        .apb2 = 1,
    },
    [STM32_UART_USART2] = {
        .regs = (stm32_usart_regs_t *)USART2_BASE,
        .irq = 38U,
        .apb2 = 0,
    },
    [STM32_UART_USART3] = {
        .regs = (stm32_usart_regs_t *)USART3_BASE,
        .irq = 39U,
        .apb2 = 0,
    },
};

static stm32_uart_info_t uart_info[STM32_UART_PORT_COUNT];
static int uart_rx_level[STM32_UART_PORT_COUNT] = {-1, -1, -1};

static int uart_port_valid(stm32_uart_port_t port) {
    return (unsigned)port < STM32_UART_PORT_COUNT;
}

static uint32_t stm32_sysclk_hz(void) {
    uint32_t cfgr = RCC_CFGR;
    uint32_t source = (cfgr >> 2) & 3U;
    uint32_t sysclk;

    /* RCC_CFGR[3:2] is SWS, the read-only *status* of the system clock switch.
     * Bits [1:0] are SW, the writable request; only SWS reports which source
     * the part actually switched to.  00 = HSI, 01 = HSE, 10 = PLL, 11 = not a
     * defined mode, which is why the last arm is a defensive fallback. */
    if (source == 0U) {
        sysclk = STM32_HSI_HZ;
    } else if (source == 1U) {
        sysclk = STM32_HSE_HZ;
    } else if (source == 2U) {
        /* PLLMUL[21:18] is an *encoding*, not the multiplier itself: codes 0..13
         * mean x2..x15 and codes 14 and 15 both mean x16, which is why the
         * decode is "bits + 2" with 14 as the one special case.  The board
         * writes code 7 (RCC_CFGR_PLLMUL9) to get 8 MHz HSE x 9 = 72 MHz
         * SYSCLK.  Reading the field as a literal multiplier scales the whole
         * clock tree by the same ratio: PCLK, and therefore every BRR written
         * below, comes out wrong while nothing else in the system looks
         * broken. */
        uint32_t multiplier_bits = (cfgr >> 18) & 0xFU;
        uint32_t multiplier =
            multiplier_bits >= 14U ? 16U : multiplier_bits + 2U;
        uint32_t input;

        /* PLLSRC[16]: 0 = HSI/2, 1 = HSE.  PLLXTPRE[17] exists only when
         * PLLSRC selected HSE: 0 = HSE undivided, 1 = HSE/2.  The Xuanwu board
         * drives the PLL from the 8 MHz HSE with PLLXTPRE clear, so the divide
         * below is the HSI/2 path and not a second HSE divider. */
        if (cfgr & (1U << 16)) {
            input = STM32_HSE_HZ;
            if (cfgr & (1U << 17))
                input /= 2U;
        } else {
            input = STM32_HSI_HZ / 2U;
        }
        sysclk = input * multiplier;
    } else {
        sysclk = STM32_HSI_HZ;
    }
    return sysclk;
}

uint32_t stm32_hclk_hz(void) {
    /* RCC_CFGR[15:4] is HPRE, 12 bits wide, of which only the low 4 select the
     * AHB prescaler: 0b0xxx = /1, 0b1000 = /2, 0b1001 = /4, 0b1010 = /8,
     * 0b1011 = /16, 0b1100 = /64, 0b1101 = /128, 0b1110 = /256, 0b1111 = /512.
     * Index 7 (0b0111) is a reserved encoding and is reported as /1 here. */
    static const uint16_t divisors[16] = {
        1, 1, 1, 1, 1, 1, 1, 1, 2, 4, 8, 16, 64, 128, 256, 512,
    };
    uint32_t cfgr = RCC_CFGR;
    return stm32_sysclk_hz() / divisors[(cfgr >> 4) & 0xFU];
}

uint32_t stm32_pclk1_hz(void) {
    /* RCC_CFGR[10:8] is PPRE1, 3 bits: 0xx = /1, 100 = /2, 101 = /4, 110 = /8,
     * 111 = /16.  Table indices 1..3 are the other 0xx encodings and are /1 as
     * well.  USART2 and USART3 are on this APB, so their BRR comes out of
     * here. */
    static const uint8_t divisors[8] = {1, 1, 1, 1, 2, 4, 8, 16};
    return stm32_hclk_hz() / divisors[(RCC_CFGR >> 8) & 7U];
}

uint32_t stm32_pclk2_hz(void) {
    /* RCC_CFGR[14:11] is PPRE2, the same 3-bit encoding as PPRE1 above.  USART1
     * and the GPIO/AFIO blocks are on this APB. */
    static const uint8_t divisors[8] = {1, 1, 1, 1, 2, 4, 8, 16};
    return stm32_hclk_hz() / divisors[(RCC_CFGR >> 11) & 7U];
}

static void nvic_set_enabled(uint32_t irq, int enable) {
    /* The base selects the register pair, never the polarity: the same
     * 1 << (irq % 32) bit is written to ISER to enable and to ICER to disable,
     * because both blocks are write-only with one meaning per bit.  The word
     * index is irq / 32, and a whole word is written instead of a
     * read-modify-write so a concurrent enable on another line is not lost. */
    volatile uint32_t *reg = (volatile uint32_t *)(
        (enable ? NVIC_ISER_BASE : NVIC_ICER_BASE) + (irq / 32U) * 4U);
    *reg = 1UL << (irq % 32U);
}

static void uart_configure_pins(stm32_uart_port_t port) {
    if (port == STM32_UART_USART1) {
        /* GPIO CRL/CRH is four bits per pin: CNF[1:0] in bits [5:4] picks the
         * driver (0b10 alternate-function push-pull, 0b01 floating input) and
         * MODE[1:0] in bits [3:2] picks the speed (0b11 = 50 MHz, 0b00 =
         * input).  So 0xB is a 50 MHz alternate-function output and 0x4 is a
         * floating input.  USART1 is PA9 (TX) and PA10 (RX) on GPIOA, i.e.
         * GPIOA_CRH nibbles 1 and 2.  IOPAEN is set first because the GPIO
         * block does not decode CRL/CRH until the APB2 GPIO clock is on. */
        RCC_APB2ENR |= RCC_APB2ENR_IOPAEN;
        uint32_t crh = GPIOA_CRH;
        crh &= ~((0xFU << 4) | (0xFU << 8));
        crh |= (0xBU << 4) | (0x4U << 8);
        GPIOA_CRH = crh;
        return;
    }

    if (port == STM32_UART_USART2) {
        /* Same CNF/MODE nibble as above.  USART2 is PA2 (TX) and PA3 (RX) on
         * GPIOA, i.e. GPIOA_CRL nibbles 2 and 3. */
        RCC_APB2ENR |= RCC_APB2ENR_IOPAEN;
        uint32_t crl = GPIOA_CRL;
        crl &= ~((0xFU << 8) | (0xFU << 12));
        crl |= (0xBU << 8) | (0x4U << 12);
        GPIOA_CRL = crl;
        return;
    }

    /* AFIOEN is required because AFIO_MAPR is not decoded without the AFIO
     * clock.  AFIO_MAPR[5:4] is USART3_REMAP: 00 = PB10/PB11 (no remap),
     * 01 = PB8/PB9 (partial), 11 = PC10/PC11 (full).  The field is cleared
     * rather than set so the port always lands on the board's PB10/PB11 route
     * even when a debugger or a previously flashed image left AFIO in another
     * state. */
    RCC_APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPBEN;
    AFIO_MAPR &= ~AFIO_MAPR_USART3_REMAP_MASK;
    /* USART3 is PB10 (TX) and PB11 (RX) on GPIOB, i.e. GPIOB_CRH nibbles 2
     * and 3. */
    uint32_t crh = GPIOB_CRH;
    crh &= ~((0xFU << 8) | (0xFU << 12));
    crh |= (0xBU << 8) | (0x4U << 12);
    GPIOB_CRH = crh;
}

static void uart_enable_and_reset(stm32_uart_port_t port) {
    /* ENR gates the peripheral clock and every access to the peripheral's own
     * registers is dropped while it is clear, so ENR is written first.  RSTR
     * is a write-1 *pulse*, not a level: 1 resets, 0 releases, so the set/clear
     * pair is the shortest reset the part accepts.  The reset also zeroes BRR,
     * which is why the divider programmed afterwards by stm32_uart_set_baud()
     * is the one that reaches the line.  ENR and RSTR use the same bit
     * position for the same peripheral, which is why one mask serves both. */
    if (port == STM32_UART_USART1) {
        RCC_APB2ENR |= RCC_APB2ENR_USART1EN;
        RCC_APB2RSTR |= RCC_APB2ENR_USART1EN;
        RCC_APB2RSTR &= ~RCC_APB2ENR_USART1EN;
    } else if (port == STM32_UART_USART2) {
        RCC_APB1ENR |= RCC_APB1ENR_USART2EN;
        RCC_APB1RSTR |= RCC_APB1ENR_USART2EN;
        RCC_APB1RSTR &= ~RCC_APB1ENR_USART2EN;
    } else {
        RCC_APB1ENR |= RCC_APB1ENR_USART3EN;
        RCC_APB1RSTR |= RCC_APB1ENR_USART3EN;
        RCC_APB1RSTR &= ~RCC_APB1ENR_USART3EN;
    }
}

int stm32_uart_set_baud(stm32_uart_port_t port, uint32_t baud_rate) {
    if (!uart_port_valid(port) || baud_rate == 0U)
        return -1;

    const stm32_uart_desc_t *desc = &uart_desc[port];
    stm32_uart_info_t *info = &uart_info[port];
    /* USART1 is on APB2 and USART2/USART3 on APB1, so the divisor is derived
     * from the live RCC registers rather than an assumed 72 MHz.  On the F1 the
     * USART has no clock-enable divider of its own and BRR holds USARTDIV * 16
     * because OVER8 (CR1[15]) is 0, i.e. 16x oversampling; the rounded
     * pclk/baud therefore goes into BRR as a single 16-bit value with no
     * mantissa/fraction split to get wrong.  The + baud_rate / 2 term is the
     * rounding, so a one-step error in `divider` is a sub-percent baud error
     * rather than a wrong register. */
    uint32_t clock_hz = desc->apb2 ? stm32_pclk2_hz() : stm32_pclk1_hz();
    uint32_t divider = (clock_hz + baud_rate / 2U) / baud_rate;
    /* BRR[15:4] is the integer part and BRR[3:0] the 4-bit fraction, so a
     * value below 16 leaves the integer part at 0, which the part does not
     * accept, and 0xFFFF is the top of the field. */
    if (divider < 16U || divider > 0xFFFFU)
        return -1;

    /* UE (CR1[13]) has to be clear before BRR changes, so CR1 is zeroed, the
     * divider is written to a disabled USART, and the enables are restored in
     * one store.  Only RXNEIE is carried across: it is the one bit a caller may
     * have set, and re-requesting a baud rate must not silently drop an RX
     * interrupt source.  TE (CR1[3]) and RE (CR1[2]) are re-asserted
     * unconditionally because this driver always wants both directions. */
    uint32_t saved_cr1 = desc->regs->cr1;
    desc->regs->cr1 = 0;
    desc->regs->brr = divider;
    desc->regs->cr1 =
        (saved_cr1 & USART_CR1_RXNEIE) |
        USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

    info->clock_hz = clock_hz;
    info->requested_baud = baud_rate;
    info->divider = divider;
    info->actual_baud = clock_hz / divider;
    info->rx_irq_enabled = !!(desc->regs->cr1 & USART_CR1_RXNEIE);
    return 0;
}

int stm32_uart_init(stm32_uart_port_t port, uint32_t baud_rate,
                    int enable_rx_irq) {
    if (!uart_port_valid(port))
        return -1;

    const stm32_uart_desc_t *desc = &uart_desc[port];
    stm32_uart_info_t *info = &uart_info[port];
    /* Disable the NVIC line before touching the peripheral: the APBxRSTR pulse
     * below clears BRR, so an RXNE taken inside that window would compute its
     * baud from a zero divider. */
    nvic_set_enabled(desc->irq, 0);
    uart_configure_pins(port);
    uart_enable_and_reset(port);

    desc->regs->cr1 = 0;
    desc->regs->cr2 = 0;
    desc->regs->cr3 = 0;
    info->error_count = 0;
    info->rx_bytes = 0;
    info->tx_bytes = 0;
    info->rx_transitions = 0;
    info->last_rx_byte = 0;
    info->initialized = 0;
    info->rx_irq_enabled = 0;
    if (stm32_uart_set_baud(port, baud_rate) != 0)
        return -1;

    /* Mandatory error-clear sequence: RXNE, ORE, PE, FE, NE and TC are all
     * cleared by a read of SR followed by a read of DR, and the DR read also
     * discards any byte the peripheral shifted in while the port was disabled.
     * Reading SR on its own clears nothing. */
    (void)desc->regs->sr;
    (void)desc->regs->dr;
    info->initialized = 1;
    uart_rx_level[port] = stm32_uart_rx_pin_level(port);
    /* 0xC0 is preemption priority level 12 and 0x80 is level 8, sub-priority 0
     * in both cases (only [7:4] exist on the Cortex-M3).  The encoder treats
     * the larger number as the less urgent one, so the console on USART1 is
     * deliberately the lowest of the three: a stalled APB1 modem cannot starve
     * the console, while a stalled console can delay a modem. */
    NVIC_IPR[desc->irq] = port == STM32_UART_USART1 ? 0xC0U : 0x80U;
    stm32_uart_set_rx_irq(port, enable_rx_irq);
    return 0;
}

void stm32_uart_set_rx_irq(stm32_uart_port_t port, int enable) {
    if (!uart_port_valid(port) || !uart_info[port].initialized)
        return;

    const stm32_uart_desc_t *desc = &uart_desc[port];
    if (enable)
        desc->regs->cr1 |= USART_CR1_RXNEIE;
    else
        desc->regs->cr1 &= ~USART_CR1_RXNEIE;
    uart_info[port].rx_irq_enabled = !!enable;
    nvic_set_enabled(desc->irq, enable);
}

int stm32_uart_rx_irq_enabled(stm32_uart_port_t port) {
    return uart_port_valid(port) && uart_info[port].rx_irq_enabled;
}

int stm32_uart_send_byte(stm32_uart_port_t port, uint8_t value,
                         uint32_t timeout) {
    if (!uart_port_valid(port) || !uart_info[port].initialized)
        return -1;

    /* TXE (SR[7]) means the data register is empty, not that the wire is idle:
     * it goes high as soon as the previous byte is moved into the shift
     * register.  Waiting on TXE therefore only guarantees DR is writable;
     * TC via stm32_uart_wait_tx_complete() is what says the line is idle. */
    stm32_usart_regs_t *regs = uart_desc[port].regs;
    while (!(regs->sr & USART_SR_TXE) && timeout--)
        ;
    if (!(regs->sr & USART_SR_TXE))
        return -1;
    regs->dr = value;
    uart_info[port].tx_bytes++;
    return 0;
}

int stm32_uart_wait_tx_complete(stm32_uart_port_t port, uint32_t timeout) {
    if (!uart_port_valid(port) || !uart_info[port].initialized)
        return -1;

    /* TC (SR[6]) is set only once the last stop bit has left the shift
     * register, i.e. the line is idle again.  TC is cleared by the same
     * SR-then-DR read pair, so any caller that reads DR while checking for
     * transmission complete restarts the wait. */
    stm32_usart_regs_t *regs = uart_desc[port].regs;
    while (!(regs->sr & USART_SR_TC) && timeout--)
        ;
    return (regs->sr & USART_SR_TC) ? 0 : -1;
}

int stm32_uart_poll_byte(stm32_uart_port_t port, uint8_t *value) {
    if (!uart_port_valid(port) || !uart_info[port].initialized || !value)
        return 0;

    /* SR is snapshotted before DR is touched: the flag set decides what the
     * following DR read yields, and that read is itself the side effect which
     * clears RXNE and the error flags.  An errored frame still has to be
     * drained with the DR read below -- skipping it latches ORE and RXNE never
     * comes back -- which is why the byte is discarded only afterwards. */
    stm32_usart_regs_t *regs = uart_desc[port].regs;
    uint32_t status = regs->sr;
    if (!(status & (USART_SR_RXNE | USART_SR_ERROR_MASK)))
        return 0;

    uint8_t received = (uint8_t)regs->dr;
    if (status & USART_SR_ERROR_MASK)
        uart_info[port].error_count++;
    if (!(status & USART_SR_RXNE) ||
        (status & (USART_SR_PE | USART_SR_FE | USART_SR_NE)))
        return -1;
    int level = stm32_uart_rx_pin_level(port);
    if (uart_rx_level[port] >= 0 && level != uart_rx_level[port])
        uart_info[port].rx_transitions++;
    uart_rx_level[port] = level;
    uart_info[port].rx_bytes++;
    uart_info[port].last_rx_byte = received;
    *value = received;
    return 1;
}

void stm32_uart_drain_rx(stm32_uart_port_t port) {
    uint8_t value;
    while (stm32_uart_poll_byte(port, &value) != 0)
        ;
}

int stm32_uart_rx_pin_level(stm32_uart_port_t port) {
    /* The IDR bit index is the pin number: PA10, PA3 and PB11 respectively --
     * the same three pins configured as floating RX inputs in
     * uart_configure_pins().  The level is sampled to detect the idle/busy edge
     * that bounds a frame, so it has to stay the RX pin and not drift to the
     * TX pin of the same port. */
    if (port == STM32_UART_USART1)
        return !!(GPIOA_IDR & (1U << 10));
    if (port == STM32_UART_USART2)
        return !!(GPIOA_IDR & (1U << 3));
    if (port == STM32_UART_USART3)
        return !!(GPIOB_IDR & (1U << 11));
    return -1;
}

const stm32_uart_info_t *stm32_uart_info(stm32_uart_port_t port) {
    if (!uart_port_valid(port))
        return NULL;
    return &uart_info[port];
}

#endif
