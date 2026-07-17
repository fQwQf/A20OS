#ifdef CONFIG_BOARD_STM32F103

#include "stm32_wifi_config.h"
#include "wifi.h"
#include "core/arch.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "stm32_uart.h"

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define AFIO_MAPR   (*(volatile uint32_t *)0x40010004UL)
#define GPIOC_CRL   (*(volatile uint32_t *)0x40011000UL)
#define GPIOC_CRH   (*(volatile uint32_t *)0x40011004UL)
#define GPIOC_BSRR  (*(volatile uint32_t *)0x40011010UL)
#define GPIOC_BRR   (*(volatile uint32_t *)0x40011014UL)

#define RCC_APB2ENR_AFIOEN (1U << 0)
#define RCC_APB2ENR_IOPCEN (1U << 4)
#define AFIO_MAPR_SWJ_CFG_MASK (7U << 24)
#define AFIO_MAPR_SWJ_NOJTAG   (2U << 24)
#define WIFI_CH_PIN 6U
#define WIFI_RST_PIN 7U
#define WIFI_UART STM32_UART_USART2
#ifdef CONFIG_STM32_QEMU
/* QEMU has no ESP8266 model; retain small buffers for link/runtime coverage. */
#define WIFI_RX_SIZE 64U
#define WIFI_RESPONSE_SIZE 128U
#define WIFI_SEND_SIZE 64U
#define WIFI_DATA_SIZE 64U
#else
#define WIFI_RX_SIZE 512U
#define WIFI_RESPONSE_SIZE 768U
#define WIFI_SEND_SIZE 192U
#define WIFI_DATA_SIZE 512U
#endif
#define WIFI_RESET_ASSERT_MS 500U
#define WIFI_BOOT_WAIT_MS 2000U
#define WIFI_AT_TIMEOUT_MS 700U
#define WIFI_AT_ATTEMPTS 3U
#define WIFI_COMMAND_TIMEOUT_MS 1200U
#define WIFI_JOIN_TIMEOUT_MS 12000U
#define WIFI_SCAN_TIMEOUT_MS 8000U
#define WIFI_TX_TIMEOUT 100000U

#ifndef STM32_WIFI_SSID
#define STM32_WIFI_SSID ""
#endif
#ifndef STM32_WIFI_PASSWORD
#define STM32_WIFI_PASSWORD ""
#endif

static volatile uint8_t wifi_rx[WIFI_RX_SIZE];
static volatile unsigned wifi_rx_head;
static volatile unsigned wifi_rx_tail;
static stm32_wifi_info_t wifi;
static char wifi_response[WIFI_RESPONSE_SIZE];
static size_t wifi_response_length;
static uint64_t wifi_deadline;
static char wifi_password[64];
static uint8_t wifi_pending_send[WIFI_SEND_SIZE];
static size_t wifi_pending_send_length;
static volatile uint8_t wifi_data[WIFI_DATA_SIZE];
static volatile unsigned wifi_data_head;
static volatile unsigned wifi_data_tail;
static size_t wifi_response_processed;
static size_t wifi_scan_parse_offset;
static char wifi_command[64];
static uint32_t wifi_command_baud;
static uint32_t wifi_command_errors;
static unsigned wifi_probe_baud_index;
static unsigned wifi_probe_attempt;
/* 9600 first: at the 8 MHz HSI clock the FIFO-less USART2 RX overruns at
 * 115200 (≈15% byte errors on the dupont-wired ESP8266), which corrupts the
 * multi-byte inbound CONTROL/+IPD frames. The module is set to 9600 via
 * AT+UART_DEF (persists in its flash) — measured 0% RX errors at 9600, and the
 * full cloud loop (SNAPSHOT→CONTROL) then closes. 115200 stays in the list so a
 * factory-default module is still detected (just probed second). */
static const uint32_t wifi_probe_baud_rates[] = {
    9600U, 115200U, 57600U, 38400U, 19200U,
};

static void wifi_set_event(const char *text);

/* Per-AT-command logging is OFF by default: at 8 MHz, printing a full command +
 * reply on USART1 (~6 ms) blocks long enough for the FIFO-less USART2 RX to
 * overrun and drop the proxy's inbound +IPD CONTROL bytes. Re-enable for manual
 * debugging via `wifi debug on`. */
static int wifi_log_verbose;

void stm32_wifi_set_verbose(int on) { wifi_log_verbose = on ? 1 : 0; }
int stm32_wifi_verbose(void) { return wifi_log_verbose; }

static void wifi_log_response(void) {
    if (!wifi_log_verbose)
        return;
    printf("[WIFI-AT] baud=%u cmd=%s reply=",
           (unsigned)wifi_command_baud,
           wifi_command[0] ? wifi_command : "(none)");
    if (wifi_response_length == 0U) {
        printf("<none>");
    } else {
        size_t shown = wifi_response_length < 160U ?
            wifi_response_length : 160U;
        for (size_t i = 0; i < shown; i++) {
            uint8_t c = (uint8_t)wifi_response[i];
            if (c == '\r')
                printf("\\r");
            else if (c == '\n')
                printf("\\n");
            else if (c >= 32U && c <= 126U)
                printf("%c", (char)c);
            else
                printf("\\x%02x", (unsigned)c);
        }
        if (shown != wifi_response_length)
            printf("...");
    }
    printf(" bytes=%u errors=%u\n", (unsigned)wifi_response_length,
           (unsigned)(wifi.uart_errors - wifi_command_errors));
}

#ifdef CONFIG_STM32_XUANWU
static void gpio_config_pin(volatile uint32_t *crl, volatile uint32_t *crh,
                            unsigned pin, uint32_t mode) {
    volatile uint32_t *reg = pin < 8U ? crl : crh;
    uint32_t shift = (pin & 7U) * 4U;
    uint32_t value = *reg;
    value &= ~(0xFU << shift);
    value |= mode << shift;
    *reg = value;
}
#endif

static void wifi_copy(char *dest, size_t capacity, const char *source) {
    size_t i = 0;
    if (capacity == 0U)
        return;
    if (source)
        while (i + 1U < capacity && source[i]) {
            dest[i] = source[i];
            i++;
        }
    dest[i] = '\0';
}

static int wifi_token_valid(const char *text, size_t minimum,
                            size_t maximum) {
    size_t length = 0;
    if (!text)
        return 0;
    while (text[length]) {
        char c = text[length++];
        if (length > maximum || c < 33 || c > 126 || c == '"' || c == '\\')
            return 0;
    }
    return length >= minimum;
}

static int wifi_send_bytes(const void *data, size_t length) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; i++)
        if (stm32_uart_send_byte(WIFI_UART, bytes[i],
                                 WIFI_TX_TIMEOUT) != 0)
            return -1;
    if (stm32_uart_wait_tx_complete(WIFI_UART,
                                    WIFI_TX_TIMEOUT) != 0)
        return -1;
    wifi.transmitted_bytes += (uint32_t)length;
    return 0;
}

static void wifi_clear_response(void) {
    uint32_t flags = arch_irq_save();
    wifi_rx_tail = wifi_rx_head;
    arch_irq_restore(flags);
    wifi_response_length = 0;
    wifi_response_processed = 0;
    wifi_response[0] = '\0';
}

static int wifi_begin_command(const char *command,
                              stm32_wifi_phase_t phase,
                              uint64_t now, uint32_t timeout_ms) {
    if (wifi.command_busy)
        wifi_log_response();
    wifi_clear_response();
    wifi_copy(wifi_command, sizeof(wifi_command),
              strncmp(command, "AT+CWJAP=", 9U) == 0 ?
                  "AT+CWJAP=<credentials-redacted>" : command);
    wifi_command_baud = wifi.baud_rate;
    wifi_command_errors = wifi.uart_errors;
    if (wifi_send_bytes(command, strlen(command)) != 0 ||
        wifi_send_bytes("\r\n", 2U) != 0) {
        wifi.command_busy = 0;
        wifi.phase = STM32_WIFI_READY;
        wifi_set_event("UART TX FAILED");
        return -1;
    }
    wifi.phase = phase;
    wifi.command_busy = 1;
    wifi_deadline = now + timeout_ms;
    if (wifi_log_verbose)
        printf("[WIFI-AT] baud=%u cmd=%s\n",
               (unsigned)wifi.baud_rate,
               strncmp(command, "AT+CWJAP=", 9U) == 0 ?
                   "AT+CWJAP=<credentials-redacted>" : command);
    return 0;
}

static int wifi_response_has(const char *token) {
    return token && strstr(wifi_response, token) != NULL;
}

static int wifi_response_failed(void) {
    return wifi_response_has("ERROR") || wifi_response_has("FAIL");
}

static void wifi_capture_quoted(const char *key, char *dest,
                                size_t capacity) {
    char *start = strstr(wifi_response, key);
    if (!start)
        return;
    start += strlen(key);
    while (*start && *start != '"')
        start++;
    if (*start != '"')
        return;
    start++;
    size_t length = 0;
    while (start[length] && start[length] != '"' &&
           length + 1U < capacity) {
        dest[length] = start[length];
        length++;
    }
    dest[length] = '\0';
}

static void wifi_parse_ip(void) {
    wifi_capture_quoted("+CIFSR:STAIP", wifi.ip_address,
                        sizeof(wifi.ip_address));
    if (!wifi.ip_address[0])
        wifi_capture_quoted("STAIP", wifi.ip_address,
                            sizeof(wifi.ip_address));
    wifi_capture_quoted("+CIFSR:STAMAC", wifi.mac_address,
                        sizeof(wifi.mac_address));
    if (!wifi.mac_address[0])
        wifi_capture_quoted("STAMAC", wifi.mac_address,
                            sizeof(wifi.mac_address));
    if (!wifi.ip_address[0]) {
        for (size_t i = 0; wifi_response[i]; i++) {
            if (wifi_response[i] < '0' || wifi_response[i] > '9')
                continue;
            size_t length = 0;
            unsigned dots = 0;
            while (wifi_response[i + length] && length < 15U) {
                char c = wifi_response[i + length];
                if (c == '.')
                    dots++;
                else if (c < '0' || c > '9')
                    break;
                length++;
            }
            if (dots == 3U && length < sizeof(wifi.ip_address)) {
                memcpy(wifi.ip_address, wifi_response + i, length);
                wifi.ip_address[length] = '\0';
                break;
            }
        }
    }
    wifi.got_ip = wifi.ip_address[0] &&
                  strcmp(wifi.ip_address, "0.0.0.0") != 0;
}

static void wifi_parse_scan_incremental(int final) {
    const char *cursor = wifi_response + wifi_scan_parse_offset;
    while ((cursor = strstr(cursor, "+CWLAP:")) != NULL) {
        const char *line_end = strchr(cursor, '\n');
        if (!line_end && !final) {
            wifi_scan_parse_offset = (size_t)(cursor - wifi_response);
            return;
        }
        wifi.access_points++;
        if (!wifi.scan_ssid[0]) {
            const char *quote = strchr(cursor, '"');
            if (quote) {
                quote++;
                size_t length = 0;
                while (quote[length] && quote[length] != '"' &&
                       length + 1U < sizeof(wifi.scan_ssid)) {
                    wifi.scan_ssid[length] = quote[length];
                    length++;
                }
                wifi.scan_ssid[length] = '\0';
            }
        }
        cursor = line_end ? line_end + 1 : wifi_response + wifi_response_length;
    }
    wifi_scan_parse_offset = wifi_response_length > 7U ?
        wifi_response_length - 7U : 0U;
}

static void wifi_make_response_space(void) {
    if (wifi_response_length + 1U < sizeof(wifi_response))
        return;

    size_t discard;
    if (wifi.phase == STM32_WIFI_SCAN_WAIT) {
        wifi_parse_scan_incremental(0);
        discard = wifi_scan_parse_offset;
    } else {
        discard = wifi_response_length / 2U;
    }
    if (discard == 0U)
        discard = 1U;

    memmove(wifi_response, wifi_response + discard,
            wifi_response_length - discard);
    wifi_response_length -= discard;
    wifi_response[wifi_response_length] = '\0';
    wifi_response_processed = wifi_response_processed > discard ?
        wifi_response_processed - discard : 0U;
    wifi_scan_parse_offset = wifi_scan_parse_offset > discard ?
        wifi_scan_parse_offset - discard : 0U;
}

static void wifi_parse_ipd(void) {
    size_t cursor = wifi_response_processed;
    while (cursor + 5U < wifi_response_length) {
        char *marker = strstr(wifi_response + cursor, "+IPD,");
        if (!marker) {
            wifi_response_processed = wifi_response_length > 5U ?
                wifi_response_length - 5U : 0U;
            return;
        }
        size_t start = (size_t)(marker - wifi_response);
        char *colon = strchr(marker, ':');
        if (!colon || (size_t)(colon - wifi_response) >= wifi_response_length)
            return;

        const char *number = marker + 5;
        const char *scan = number;
        while (scan < colon) {
            if (*scan == ',')
                number = scan + 1;
            scan++;
        }
        size_t length = 0;
        for (const char *p = number; p < colon; p++) {
            if (*p < '0' || *p > '9') {
                cursor = start + 5U;
                goto next_marker;
            }
            length = length * 10U + (size_t)(*p - '0');
            if (length > WIFI_DATA_SIZE * 4U) {
                cursor = start + 5U;
                goto next_marker;
            }
        }
        size_t payload = (size_t)(colon - wifi_response) + 1U;
        if (payload + length > wifi_response_length)
            return;
        for (size_t i = 0; i < length; i++) {
            unsigned next = (wifi_data_head + 1U) % WIFI_DATA_SIZE;
            if (next == wifi_data_tail) {
                wifi.dropped_bytes++;
                break;
            }
            wifi_data[wifi_data_head] = (uint8_t)wifi_response[payload + i];
            wifi_data_head = next;
        }
        wifi_set_event("IPD DATA RECEIVED");
        cursor = payload + length;
        wifi_response_processed = cursor;
next_marker:
        ;
    }
}

static void wifi_set_event(const char *text) {
    wifi_copy(wifi.last_event, sizeof(wifi.last_event), text);
}

static void wifi_drain_rx(void) {
    while (wifi_rx_tail != wifi_rx_head) {
        uint8_t value = wifi_rx[wifi_rx_tail];
        wifi_rx_tail = (wifi_rx_tail + 1U) % WIFI_RX_SIZE;
        wifi_make_response_space();
        if (wifi_response_length + 1U < sizeof(wifi_response)) {
            wifi_response[wifi_response_length++] = (char)value;
            wifi_response[wifi_response_length] = '\0';
        }
    }

    if (wifi.phase == STM32_WIFI_SCAN_WAIT)
        wifi_parse_scan_incremental(0);

    wifi_parse_ipd();

    if (wifi_response_has("WIFI CONNECTED")) {
        wifi.joined = 1;
        wifi_set_event("WIFI CONNECTED");
    }
    if (wifi_response_has("WIFI GOT IP")) {
        wifi.joined = 1;
        wifi.got_ip = 1;
        wifi_set_event("WIFI GOT IP");
    }
    if (wifi_response_has("WIFI DISCONNECT")) {
        wifi.joined = 0;
        wifi.got_ip = 0;
        wifi.socket_connected = 0;
        wifi.ip_address[0] = '\0';
        wifi_set_event("WIFI DISCONNECT");
    }
    if (wifi_response_has("CLOSED")) {
        wifi.socket_connected = 0;
        wifi_set_event("SOCKET CLOSED");
    }
    if (!wifi.command_busy && wifi_response_processed != 0U) {
        size_t remaining = wifi_response_length - wifi_response_processed;
        memmove(wifi_response, wifi_response + wifi_response_processed,
                remaining);
        wifi_response_length = remaining;
        wifi_response[remaining] = '\0';
        wifi_response_processed = 0;
    }
}

static void wifi_enter_ready(void) {
    if (wifi.command_busy)
        wifi_log_response();
    wifi.phase = STM32_WIFI_READY;
    wifi.command_busy = 0;
    wifi.connecting = 0;
}

static void wifi_start_mode(uint64_t now) {
    if (wifi_begin_command("AT+CWMODE=1", STM32_WIFI_MODE_WAIT, now,
                           WIFI_COMMAND_TIMEOUT_MS) != 0)
        wifi_enter_ready();
}

static void wifi_start_mux(uint64_t now) {
    if (wifi_begin_command("AT+CIPMUX=0", STM32_WIFI_MUX_WAIT, now,
                           WIFI_COMMAND_TIMEOUT_MS) != 0)
        wifi_enter_ready();
}

static void wifi_start_ip_query(uint64_t now) {
    if (wifi_begin_command("AT+CIFSR", STM32_WIFI_IP_WAIT, now,
                           WIFI_COMMAND_TIMEOUT_MS) != 0)
        wifi_enter_ready();
}

static void wifi_start_join(uint64_t now) {
    char command[128];
    snprintf(command, sizeof(command), "AT+CWJAP=\"%s\",\"%s\"",
             wifi.ssid, wifi_password);
    wifi.connecting = 1;
    if (wifi_begin_command(command, STM32_WIFI_JOIN_WAIT, now,
                           WIFI_JOIN_TIMEOUT_MS) != 0)
        wifi_enter_ready();
}

static int wifi_retry_probe(uint64_t now) {
    if (wifi_probe_attempt < WIFI_AT_ATTEMPTS) {
        wifi_probe_attempt++;
        printf("[WIFI] no AT reply at %u, retry=%u/%u\n",
               (unsigned)wifi.baud_rate, wifi_probe_attempt,
               WIFI_AT_ATTEMPTS);
        return wifi_begin_command("AT", STM32_WIFI_AT_WAIT, now,
                                  WIFI_AT_TIMEOUT_MS);
    }

    while (++wifi_probe_baud_index <
           sizeof(wifi_probe_baud_rates) / sizeof(wifi_probe_baud_rates[0])) {
        uint32_t baud = wifi_probe_baud_rates[wifi_probe_baud_index];
        if (stm32_uart_set_baud(WIFI_UART, baud) != 0)
            continue;
        printf("[WIFI] no AT reply at %u, trying baud=%u\n",
               (unsigned)wifi.baud_rate, (unsigned)baud);
        wifi.baud_rate = baud;
        wifi_probe_attempt = 1U;
        return wifi_begin_command("AT", STM32_WIFI_AT_WAIT, now,
                                  WIFI_AT_TIMEOUT_MS);
    }
    return -1;
}

int stm32_wifi_init(void) {
#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    memset(&wifi, 0, sizeof(wifi));
    wifi_copy(wifi.ssid, sizeof(wifi.ssid), STM32_WIFI_SSID);
    wifi_copy(wifi_password, sizeof(wifi_password), STM32_WIFI_PASSWORD);
    wifi.active = 1;
    wifi.phase = STM32_WIFI_RESET_WAIT;
    wifi_probe_baud_index = 0;
    wifi_probe_attempt = 0;
    wifi.baud_rate = wifi_probe_baud_rates[wifi_probe_baud_index];
    wifi_rx_head = 0;
    wifi_rx_tail = 0;
    wifi_response_length = 0;
    wifi_response_processed = 0;
    wifi_scan_parse_offset = 0;
    wifi_command[0] = '\0';
    wifi_command_baud = 0;
    wifi_data_head = 0;
    wifi_data_tail = 0;

    RCC_APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPCEN;
    AFIO_MAPR = (AFIO_MAPR & ~AFIO_MAPR_SWJ_CFG_MASK) |
                AFIO_MAPR_SWJ_NOJTAG;
    GPIOC_BRR = (1U << WIFI_CH_PIN) | (1U << WIFI_RST_PIN);
    gpio_config_pin(&GPIOC_CRL, &GPIOC_CRH, WIFI_CH_PIN, 0x3U);
    gpio_config_pin(&GPIOC_CRL, &GPIOC_CRH, WIFI_RST_PIN, 0x3U);
    if (stm32_uart_init(WIFI_UART, wifi.baud_rate, 1) != 0) {
        wifi.active = 0;
        wifi.phase = STM32_WIFI_DISABLED;
        return -1;
    }
    GPIOC_BSRR = 1U << WIFI_CH_PIN;
    wifi_deadline = timer_get_ticks() + WIFI_RESET_ASSERT_MS;
    wifi_set_event("RESETTING MODULE");
    printf("[WIFI] USART2 Dupont interface selected, reset asserted, probe scheduled\n");
    return 0;
#endif
}

void stm32_wifi_shutdown(void) {
#ifdef CONFIG_STM32_XUANWU
    stm32_uart_set_rx_irq(WIFI_UART, 0);
    GPIOC_BRR = 1U << WIFI_CH_PIN;
    GPIOC_BSRR = 1U << WIFI_RST_PIN;
    gpio_config_pin(&GPIOC_CRL, &GPIOC_CRH, WIFI_RST_PIN, 0x8U);
#endif
    wifi.active = 0;
    wifi.command_busy = 0;
    wifi.phase = STM32_WIFI_DISABLED;
}

int stm32_wifi_reprobe(void) {
    stm32_wifi_shutdown();
    return stm32_wifi_init();
}

void stm32_wifi_irq(void) {
    if (!wifi.active)
        return;
    for (;;) {
        uint8_t value;
        int result = stm32_uart_poll_byte(WIFI_UART, &value);
        if (result == 0)
            break;
        if (result < 0) {
            wifi.uart_errors++;
            continue;
        }
        unsigned next = (wifi_rx_head + 1U) % WIFI_RX_SIZE;
        if (next == wifi_rx_tail) {
            wifi.dropped_bytes++;
            continue;
        }
        wifi_rx[wifi_rx_head] = value;
        wifi_rx_head = next;
        wifi.received_bytes++;
    }
}

void stm32_wifi_service(uint64_t now) {
    if (!wifi.active)
        return;
    /* RX polling keeps AT usable even if an IRQ was masked or delayed. */
    uint32_t irq_flags = arch_irq_save();
    stm32_wifi_irq();
    arch_irq_restore(irq_flags);
    wifi_drain_rx();
    if (wifi.phase == STM32_WIFI_RESET_WAIT) {
        if (now < wifi_deadline)
            return;
        GPIOC_BSRR = 1U << WIFI_RST_PIN;
        wifi.phase = STM32_WIFI_BOOT_WAIT;
        wifi_deadline = now + WIFI_BOOT_WAIT_MS;
        wifi_set_event("MODULE BOOTING");
        return;
    }

    if (wifi.phase == STM32_WIFI_BOOT_WAIT) {
        if (now < wifi_deadline)
            return;
        wifi_probe_attempt = 1U;
        wifi_begin_command("AT", STM32_WIFI_AT_WAIT, now,
                           WIFI_AT_TIMEOUT_MS);
        return;
    }

    if (!wifi.command_busy)
        return;

    if (wifi.phase == STM32_WIFI_AT_WAIT && wifi_response_has("OK")) {
        wifi.detected = 1;
        wifi.at_responsive = 1;
        wifi_set_event("ESP8266 AT READY");
        if (wifi_begin_command("AT+GMR", STM32_WIFI_GMR_WAIT, now,
                               WIFI_COMMAND_TIMEOUT_MS) != 0)
            wifi_enter_ready();
        return;
    }
    if (wifi.phase == STM32_WIFI_GMR_WAIT && wifi_response_has("OK")) {
        wifi_set_event("ESP8266 READY");
        wifi_start_mode(now);
        return;
    }
    if (wifi.phase == STM32_WIFI_MODE_WAIT &&
        (wifi_response_has("OK") || wifi_response_has("no change"))) {
        wifi.station_mode = 1;
        wifi_start_mux(now);
        return;
    }
    if (wifi.phase == STM32_WIFI_MUX_WAIT &&
        (wifi_response_has("OK") || wifi_response_has("no change"))) {
        wifi.configured = 1;
        if (wifi.ssid[0])
            wifi_start_join(now);
        else
            wifi_enter_ready();
        return;
    }
    if (wifi.phase == STM32_WIFI_JOIN_WAIT && wifi_response_has("OK")) {
        wifi.joined = 1;
        wifi_start_ip_query(now);
        return;
    }
    if (wifi.phase == STM32_WIFI_IP_WAIT && wifi_response_has("OK")) {
        wifi_parse_ip();
        wifi_enter_ready();
        return;
    }
    if (wifi.phase == STM32_WIFI_SCAN_WAIT && wifi_response_has("OK")) {
        wifi_parse_scan_incremental(1);
        wifi_set_event("AP SCAN COMPLETE");
        wifi_enter_ready();
        return;
    }
    if (wifi.phase == STM32_WIFI_SOCKET_WAIT &&
        (wifi_response_has("CONNECT") || wifi_response_has("ALREADY") ||
         wifi_response_has("Linked") || wifi_response_has("OK"))) {
        wifi.socket_connected = 1;
        wifi_set_event("SOCKET CONNECTED");
        wifi_enter_ready();
        return;
    }
    if (wifi.phase == STM32_WIFI_SEND_PROMPT_WAIT &&
        wifi_response_has(">")) {
        wifi_clear_response();
        if (wifi_send_bytes(wifi_pending_send,
                            wifi_pending_send_length) != 0) {
            wifi_enter_ready();
            return;
        }
        wifi.phase = STM32_WIFI_SEND_RESULT_WAIT;
        wifi.command_busy = 1;
        wifi_deadline = now + WIFI_COMMAND_TIMEOUT_MS;
        return;
    }
    if (wifi.phase == STM32_WIFI_SEND_RESULT_WAIT &&
        wifi_response_has("SEND OK")) {
        wifi_set_event("SEND OK");
        wifi_enter_ready();
        return;
    }
    if (wifi.phase == STM32_WIFI_CLOSE_WAIT &&
        (wifi_response_has("OK") || wifi_response_has("CLOSED") ||
         wifi_response_has("ERROR"))) {
        wifi.socket_connected = 0;
        wifi_enter_ready();
        return;
    }
    if (wifi.phase == STM32_WIFI_RAW_AT_WAIT &&
        (wifi_response_has("OK") || wifi_response_failed())) {
        wifi_set_event(wifi_response_failed() ? "AT ERROR" : "AT OK");
        wifi_enter_ready();
        return;
    }

    if (wifi.phase == STM32_WIFI_GMR_WAIT && wifi_response_failed()) {
        wifi_start_mode(now);
        return;
    }
    if (wifi_response_failed() && wifi.phase != STM32_WIFI_CLOSE_WAIT) {
        wifi_set_event("COMMAND FAILED");
        wifi_enter_ready();
        return;
    }
    if (now < wifi_deadline)
        return;

    wifi.command_timeouts++;
    if (wifi.phase == STM32_WIFI_AT_WAIT &&
        wifi_retry_probe(now) == 0)
        return;
    if (wifi.phase == STM32_WIFI_GMR_WAIT) {
        wifi_start_mode(now);
        return;
    }
    wifi_set_event(wifi.phase == STM32_WIFI_AT_WAIT ?
                       "MODULE NOT FOUND" : "COMMAND TIMEOUT");
    wifi_enter_ready();
}

int stm32_wifi_scan(void) {
    if (!wifi.active || !wifi.at_responsive || wifi.command_busy)
        return -1;
    wifi.access_points = 0;
    wifi.scan_ssid[0] = '\0';
    wifi_scan_parse_offset = 0;
    return wifi_begin_command("AT+CWLAP", STM32_WIFI_SCAN_WAIT,
                              timer_get_ticks(), WIFI_SCAN_TIMEOUT_MS);
}

int stm32_wifi_join(const char *ssid, const char *password) {
    if (!wifi.active || !wifi.at_responsive || wifi.command_busy ||
        !wifi_token_valid(ssid, 1U, 32U) || !password ||
        (password[0] && !wifi_token_valid(password, 8U, 63U)))
        return -1;
    wifi_copy(wifi.ssid, sizeof(wifi.ssid), ssid);
    wifi_copy(wifi_password, sizeof(wifi_password), password);
    wifi.joined = 0;
    wifi.got_ip = 0;
    wifi.ip_address[0] = '\0';
    wifi_start_join(timer_get_ticks());
    return wifi.command_busy ? 0 : -1;
}

int stm32_wifi_open(const char *protocol, const char *host, uint16_t port) {
    if (!wifi.active || !wifi.got_ip || wifi.command_busy || !host ||
        (strcmp(protocol, "TCP") != 0 && strcmp(protocol, "UDP") != 0 &&
         strcmp(protocol, "tcp") != 0 && strcmp(protocol, "udp") != 0))
        return -1;
    if (!wifi_token_valid(host, 1U, 63U) || port == 0U)
        return -1;
    char command[112];
    const char *wire_protocol =
        protocol[0] == 't' ? "TCP" : protocol[0] == 'u' ? "UDP" : protocol;
    snprintf(command, sizeof(command), "AT+CIPSTART=\"%s\",\"%s\",%u",
             wire_protocol, host, (unsigned)port);
    return wifi_begin_command(command, STM32_WIFI_SOCKET_WAIT,
                              timer_get_ticks(), 8000U);
}

int stm32_wifi_close(void) {
    if (!wifi.active || wifi.command_busy)
        return -1;
    return wifi_begin_command("AT+CIPCLOSE", STM32_WIFI_CLOSE_WAIT,
                              timer_get_ticks(), WIFI_COMMAND_TIMEOUT_MS);
}

int stm32_wifi_send(const void *data, size_t length) {
    if (!wifi.active || !wifi.socket_connected || wifi.command_busy ||
        !data || length == 0U || length > sizeof(wifi_pending_send))
        return -1;
    memcpy(wifi_pending_send, data, length);
    wifi_pending_send_length = length;
    char command[32];
    snprintf(command, sizeof(command), "AT+CIPSEND=%u", (unsigned)length);
    return wifi_begin_command(command, STM32_WIFI_SEND_PROMPT_WAIT,
                              timer_get_ticks(), WIFI_COMMAND_TIMEOUT_MS);
}

int stm32_wifi_read(void *data, size_t capacity) {
    if (!data || capacity == 0U)
        return -1;
    uint8_t *output = data;
    uint32_t flags = arch_irq_save();
    size_t length = 0;
    while (length < capacity && wifi_data_tail != wifi_data_head) {
        output[length++] = wifi_data[wifi_data_tail];
        wifi_data_tail = (wifi_data_tail + 1U) % WIFI_DATA_SIZE;
    }
    arch_irq_restore(flags);
    return (int)length;
}

int stm32_wifi_debug_at(const char *command) {
    if (!wifi.active || !wifi.at_responsive || wifi.command_busy || !command ||
        strncmp(command, "AT", 2U) != 0 || strlen(command) >= 120U)
        return -1;
    return wifi_begin_command(command, STM32_WIFI_RAW_AT_WAIT,
                              timer_get_ticks(), 5000U);
}

void stm32_wifi_debug_status(void) {
    const stm32_uart_info_t *uart = stm32_uart_info(WIFI_UART);
    printf("[WIFI-DIAG] active=%d detected=%d at=%d configured=%d"
           " phase=%u busy=%d baud=%u station=%d connecting=%d"
           " joined=%d ip=%s mac=%s socket=%d ssid=%s"
           " aps=%u first=%s rx=%u tx=%u drop=%u errors=%u timeouts=%u"
           " event=%s\n",
           wifi.active, wifi.detected, wifi.at_responsive, wifi.configured,
           (unsigned)wifi.phase, wifi.command_busy,
           (unsigned)wifi.baud_rate, wifi.station_mode, wifi.connecting,
           wifi.joined, wifi.ip_address[0] ? wifi.ip_address : "(none)",
           wifi.mac_address[0] ? wifi.mac_address : "(none)",
           wifi.socket_connected,
           wifi.ssid[0] ? wifi.ssid : "(not-configured)",
           (unsigned)wifi.access_points,
           wifi.scan_ssid[0] ? wifi.scan_ssid : "(none)",
           (unsigned)wifi.received_bytes,
           (unsigned)wifi.transmitted_bytes,
           (unsigned)wifi.dropped_bytes, (unsigned)wifi.uart_errors,
           (unsigned)wifi.command_timeouts,
           wifi.last_event[0] ? wifi.last_event : "(none)");
    printf("[WIFI-UART] requested=%u actual=%u brr=0x%x irq=%d"
           " pin=%d rx=%u tx=%u edges=%u last=0x%x errors=%u\n",
           (unsigned)uart->requested_baud, (unsigned)uart->actual_baud,
           (unsigned)uart->divider, uart->rx_irq_enabled,
           stm32_uart_rx_pin_level(WIFI_UART),
           (unsigned)uart->rx_bytes, (unsigned)uart->tx_bytes,
           (unsigned)uart->rx_transitions, (unsigned)uart->last_rx_byte,
           (unsigned)uart->error_count);
}

const stm32_wifi_info_t *stm32_wifi_info(void) {
    return &wifi;
}

#endif
