/*
 * Smart home hub — ESP8266 AT driver on USART2. See esp8266.h.
 * Self-contained USART2 access (the shared stm32_uart.c only covers USART1/3).
 */
#ifdef CONFIG_BOARD_STM32F103

#include "esp8266.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "stm32_uart.h" /* stm32_pclk1_hz() */

#ifndef CONFIG_STM32_QEMU

#define RCC_APB1ENR (*(volatile uint32_t *)0x4002101CUL)
#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define RCC_APB1ENR_USART2EN (1U << 17)
#define RCC_APB2ENR_IOPAEN (1U << 2)
#define RCC_APB2ENR_AFIOEN (1U << 0)

#define GPIOA_CRL (*(volatile uint32_t *)0x40010800UL)

/* USART2 @ 0x40004400 */
#define USART2_SR (*(volatile uint32_t *)0x40004400UL)
#define USART2_DR (*(volatile uint32_t *)0x40004404UL)
#define USART2_BRR (*(volatile uint32_t *)0x40004408UL)
#define USART2_CR1 (*(volatile uint32_t *)0x4000440CUL)

#define SR_RXNE (1U << 5)
#define SR_TC (1U << 6)
#define SR_TXE (1U << 7)
#define CR1_UE (1U << 13)
#define CR1_TE (1U << 3)
#define CR1_RE (1U << 2)

static int esp_connected;

/* ---- low-level USART2 ---- */

static void u2_putc(uint8_t c) {
    while (!(USART2_SR & SR_TXE))
        ;
    USART2_DR = c;
}

static void u2_puts(const char *s) {
    while (*s)
        u2_putc((uint8_t)*s++);
}

/* Poll one byte with a millisecond timeout. 1 = got byte, 0 = timeout. */
static int u2_getc(uint32_t timeout_ms, uint8_t *out) {
    uint64_t deadline = timer_get_ticks() + timeout_ms;
    while (timer_get_ticks() < deadline) {
        if (USART2_SR & SR_RXNE) {
            *out = (uint8_t)(USART2_DR & 0xFF);
            return 1;
        }
    }
    return 0;
}

/* ---- small string helpers (freestanding) ---- */

static int e_contains(const char *hay, const char *needle) {
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b)
            return 1;
    }
    return 0;
}

/*
 * Read the response into a sliding window and return 1 if either token
 * appears before the timeout. rep2 may be NULL.
 */
static int esp_wait(const char *rep1, const char *rep2, uint32_t timeout_ms) {
    char win[96];
    unsigned n = 0;
    uint64_t deadline = timer_get_ticks() + timeout_ms;
    for (;;) {
        uint8_t c;
        uint32_t left = 0;
        uint64_t now = timer_get_ticks();
        if (now < deadline)
            left = (uint32_t)(deadline - now);
        if (!u2_getc(left ? left : 1, &c))
            return 0;
        if (n >= sizeof(win) - 1) {
            /* keep the tail so a token straddling the boundary still matches */
            for (unsigned i = 0; i < 32; i++)
                win[i] = win[sizeof(win) - 32 + i];
            n = 32;
        }
        win[n++] = (char)c;
        win[n] = '\0';
        if (e_contains(win, rep1) || (rep2 && e_contains(win, rep2)))
            return 1;
    }
}

static void esp_drain(uint32_t ms) {
    uint8_t c;
    while (u2_getc(ms, &c))
        ;
}

/* Send "cmd\r\n" and wait for rep1/rep2. */
static int esp_cmd(const char *cmd, const char *rep1, const char *rep2,
                   uint32_t timeout_ms) {
    esp_drain(2);
    u2_puts(cmd);
    u2_puts("\r\n");
    if (!rep1)
        return 0;
    return esp_wait(rep1, rep2, timeout_ms) ? 0 : -1;
}

#endif /* !CONFIG_STM32_QEMU */

int stm32_esp8266_init(void) {
#ifdef CONFIG_STM32_QEMU
    return -1;
#else
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN;
    RCC_APB1ENR |= RCC_APB1ENR_USART2EN;

    /* PA2 = AF push-pull 50 MHz (0xB); PA3 = floating input (0x4). */
    uint32_t crl = GPIOA_CRL;
    crl &= ~((0xFU << (2U * 4U)) | (0xFU << (3U * 4U)));
    crl |= (0xBU << (2U * 4U)) | (0x4U << (3U * 4U));
    GPIOA_CRL = crl;

    uint32_t pclk1 = stm32_pclk1_hz();
    USART2_BRR = (pclk1 + 115200U / 2U) / 115200U;
    USART2_CR1 = CR1_UE | CR1_TE | CR1_RE;

    esp_connected = 0;

    /* AT handshake (retry a few times), echo off, STA mode, single conn. */
    int ok = 0;
    for (int i = 0; i < 5 && !ok; i++)
        ok = esp_cmd("AT", "OK", 0, 300) == 0;
    if (!ok)
        return -1;
    esp_cmd("ATE0", "OK", 0, 300);
    esp_cmd("AT+CWMODE=1", "OK", "no change", 500);
    esp_cmd("AT+CIPMUX=0", "OK", 0, 500);
    return 0;
#endif
}

int stm32_esp8266_join(const char *ssid, const char *pass) {
#ifdef CONFIG_STM32_QEMU
    (void)ssid;
    (void)pass;
    return -1;
#else
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, pass);
    return esp_cmd(cmd, "OK", "WIFI GOT IP", 12000);
#endif
}

int stm32_esp8266_connect(const char *ip, int port) {
#ifdef CONFIG_STM32_QEMU
    (void)ip;
    (void)port;
    return -1;
#else
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d", ip, port);
    int r = esp_cmd(cmd, "OK", "ALREADY CONNECT", 6000);
    esp_connected = (r == 0);
    return r;
#endif
}

int stm32_esp8266_send(const uint8_t *buf, int len) {
#ifdef CONFIG_STM32_QEMU
    (void)buf;
    (void)len;
    return -1;
#else
    if (!esp_connected || len <= 0)
        return -1;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d", len);
    if (esp_cmd(cmd, ">", 0, 1000) != 0)
        return -1;
    for (int i = 0; i < len; i++)
        u2_putc(buf[i]);
    return esp_wait("SEND OK", 0, 2000) ? 0 : -1;
#endif
}

int stm32_esp8266_recv(uint8_t *buf, int max, uint32_t timeout_ms) {
#ifdef CONFIG_STM32_QEMU
    (void)buf;
    (void)max;
    (void)timeout_ms;
    return -1;
#else
    /* Scan the stream for "+IPD,<len>:" then copy <len> data bytes. */
    static const char tag[] = "+IPD,";
    unsigned match = 0;
    uint64_t deadline = timer_get_ticks() + timeout_ms;
    uint8_t c;

    while (timer_get_ticks() < deadline) {
        uint32_t left = (uint32_t)(deadline - timer_get_ticks());
        if (!u2_getc(left ? left : 1, &c))
            break;
        if (c == (uint8_t)tag[match]) {
            if (++match == sizeof(tag) - 1)
                break; /* matched "+IPD," */
        } else {
            match = (c == (uint8_t)tag[0]) ? 1 : 0;
        }
    }
    if (match != sizeof(tag) - 1)
        return 0; /* no packet in time */

    /* parse decimal length up to ':' */
    int n = 0;
    while (u2_getc(timeout_ms, &c)) {
        if (c == ':')
            break;
        if (c >= '0' && c <= '9')
            n = n * 10 + (c - '0');
        else
            return -1;
    }
    if (n <= 0)
        return -1;
    if (n > max)
        n = max;
    for (int i = 0; i < n; i++) {
        if (!u2_getc(timeout_ms, &c))
            return -1;
        buf[i] = c;
    }
    return n;
#endif
}

int stm32_esp8266_connected(void) {
#ifdef CONFIG_STM32_QEMU
    return 0;
#else
    return esp_connected;
#endif
}

#endif /* CONFIG_BOARD_STM32F103 */
