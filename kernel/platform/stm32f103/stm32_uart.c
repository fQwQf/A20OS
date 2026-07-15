#ifdef CONFIG_BOARD_STM32F103

#include "stm32_uart.h"

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

    if (source == 0U) {
        sysclk = STM32_HSI_HZ;
    } else if (source == 1U) {
        sysclk = STM32_HSE_HZ;
    } else if (source == 2U) {
        uint32_t multiplier_bits = (cfgr >> 18) & 0xFU;
        uint32_t multiplier =
            multiplier_bits >= 14U ? 16U : multiplier_bits + 2U;
        uint32_t input;

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
    static const uint16_t divisors[16] = {
        1, 1, 1, 1, 1, 1, 1, 1, 2, 4, 8, 16, 64, 128, 256, 512,
    };
    uint32_t cfgr = RCC_CFGR;
    return stm32_sysclk_hz() / divisors[(cfgr >> 4) & 0xFU];
}

uint32_t stm32_pclk1_hz(void) {
    static const uint8_t divisors[8] = {1, 1, 1, 1, 2, 4, 8, 16};
    return stm32_hclk_hz() / divisors[(RCC_CFGR >> 8) & 7U];
}

uint32_t stm32_pclk2_hz(void) {
    static const uint8_t divisors[8] = {1, 1, 1, 1, 2, 4, 8, 16};
    return stm32_hclk_hz() / divisors[(RCC_CFGR >> 11) & 7U];
}

static void nvic_set_enabled(uint32_t irq, int enable) {
    volatile uint32_t *reg = (volatile uint32_t *)(
        (enable ? NVIC_ISER_BASE : NVIC_ICER_BASE) + (irq / 32U) * 4U);
    *reg = 1UL << (irq % 32U);
}

static void uart_configure_pins(stm32_uart_port_t port) {
    if (port == STM32_UART_USART1) {
        RCC_APB2ENR |= RCC_APB2ENR_IOPAEN;
        uint32_t crh = GPIOA_CRH;
        crh &= ~((0xFU << 4) | (0xFU << 8));
        crh |= (0xBU << 4) | (0x4U << 8);
        GPIOA_CRH = crh;
        return;
    }

    if (port == STM32_UART_USART2) {
        RCC_APB2ENR |= RCC_APB2ENR_IOPAEN;
        uint32_t crl = GPIOA_CRL;
        crl &= ~((0xFU << 8) | (0xFU << 12));
        crl |= (0xBU << 8) | (0x4U << 12);
        GPIOA_CRL = crl;
        return;
    }

    RCC_APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPBEN;
    AFIO_MAPR &= ~AFIO_MAPR_USART3_REMAP_MASK;
    uint32_t crh = GPIOB_CRH;
    crh &= ~((0xFU << 8) | (0xFU << 12));
    crh |= (0xBU << 8) | (0x4U << 12);
    GPIOB_CRH = crh;
}

static void uart_enable_and_reset(stm32_uart_port_t port) {
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
    uint32_t clock_hz = desc->apb2 ? stm32_pclk2_hz() : stm32_pclk1_hz();
    uint32_t divider = (clock_hz + baud_rate / 2U) / baud_rate;
    if (divider < 16U || divider > 0xFFFFU)
        return -1;

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

    (void)desc->regs->sr;
    (void)desc->regs->dr;
    info->initialized = 1;
    uart_rx_level[port] = stm32_uart_rx_pin_level(port);
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

    stm32_usart_regs_t *regs = uart_desc[port].regs;
    while (!(regs->sr & USART_SR_TC) && timeout--)
        ;
    return (regs->sr & USART_SR_TC) ? 0 : -1;
}

int stm32_uart_poll_byte(stm32_uart_port_t port, uint8_t *value) {
    if (!uart_port_valid(port) || !uart_info[port].initialized || !value)
        return 0;

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
