#ifdef CONFIG_BOARD_STM32F103

#include "stm32_bluetooth_config.h"
#include "bluetooth.h"
#include "core/arch.h"
#include "core/stdio.h"
#include "stm32_uart.h"

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define AFIO_MAPR   (*(volatile uint32_t *)0x40010004UL)

#define GPIOA_CRL  (*(volatile uint32_t *)0x40010800UL)
#define GPIOA_CRH  (*(volatile uint32_t *)0x40010804UL)
#define GPIOA_IDR  (*(volatile uint32_t *)0x40010808UL)
#define GPIOA_BSRR (*(volatile uint32_t *)0x40010810UL)
#define GPIOA_BRR  (*(volatile uint32_t *)0x40010814UL)
#define GPIOB_CRL  (*(volatile uint32_t *)0x40010C00UL)
#define GPIOB_CRH  (*(volatile uint32_t *)0x40010C04UL)
#define GPIOB_IDR  (*(volatile uint32_t *)0x40010C08UL)
#define GPIOB_BSRR (*(volatile uint32_t *)0x40010C10UL)
#define GPIOB_BRR  (*(volatile uint32_t *)0x40010C14UL)

#define CORE_DEMCR (*(volatile uint32_t *)0xE000EDFCUL)
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004UL)

#define RCC_APB2ENR_AFIOEN  (1U << 0)
#define RCC_APB2ENR_IOPAEN  (1U << 2)
#define RCC_APB2ENR_IOPBEN  (1U << 3)

#define AFIO_MAPR_SWJ_CFG_MASK (7U << 24)
#define AFIO_MAPR_SWJ_NOJTAG   (2U << 24)

#define BLUETOOTH_KEY_PIN   4U
#define BLUETOOTH_STATE_PIN 15U
#define BLUETOOTH_UART STM32_UART_USART3
#ifndef STM32_BLUETOOTH_BAUD_RATE
#define STM32_BLUETOOTH_BAUD_RATE 9600U
#endif
#ifndef STM32_BLUETOOTH_BAUD_RATE_TEXT
#define STM32_BLUETOOTH_BAUD_RATE_TEXT "9600"
#endif
#define BLUETOOTH_BAUD_RATE STM32_BLUETOOTH_BAUD_RATE
#define BLUETOOTH_RX_SIZE   256U
#define BLUETOOTH_FRAME_IDLE_MS 20U
#define BLUETOOTH_TX_TIMEOUT 100000U
#define BLUETOOTH_AT_TIMEOUT_MS 150U
#define BLUETOOTH_AT_PROBE_TIMEOUT_MS 150U
#define BLUETOOTH_AT_RETRIES 1U
#define BLUETOOTH_BOOT_DELAY_MS 100U
#define BLUETOOTH_RESET_DELAY_MS 200U
#define BLUETOOTH_INIT_RETRIES 3U
#define BLUETOOTH_INIT_RETRY_DELAY_MS 1000U
#define BLUETOOTH_KEY_SETUP_MS 10U
#define BLUETOOTH_CONNECT_ASSERT_MS 500U
#define BLUETOOTH_DISCONNECT_ASSERT_MS 200U
#define BLUETOOTH_AT_RESPONSE_MAX 96U

#define CORE_DEMCR_TRCENA  (1U << 24)
#define DWT_CTRL_NOCYCCNT  (1U << 25)
#define DWT_CTRL_CYCCNTENA (1U << 0)

static volatile uint8_t bluetooth_rx[BLUETOOTH_RX_SIZE];
static volatile unsigned bluetooth_rx_head;
static volatile unsigned bluetooth_rx_tail;
static volatile int bluetooth_frame_ready;
static stm32_bluetooth_info_t bluetooth;
static uint32_t bluetooth_observed_rx;
static uint64_t bluetooth_last_rx_time;
static uint64_t bluetooth_state_changed_time;
static int bluetooth_state_high;
static int bluetooth_at_key_mode;
static uint32_t bluetooth_uart_baud;

#ifdef CONFIG_STM32_XUANWU
static void gpio_config_pin(volatile uint32_t *crl, volatile uint32_t *crh,
                            unsigned pin, uint32_t mode);

static int bluetooth_uart_send_byte(uint8_t value) {
    return stm32_uart_send_byte(
        BLUETOOTH_UART, value, BLUETOOTH_TX_TIMEOUT);
}

static void bluetooth_copy_text(char *dest, size_t capacity,
                                const char *source) {
    size_t i = 0;

    if (capacity == 0)
        return;
    if (source) {
        while (i + 1U < capacity && source[i]) {
            dest[i] = source[i];
            i++;
        }
    }
    dest[i] = '\0';
}

static int bluetooth_text_equal(const char *left, const char *right) {
    if (!left || !right)
        return 0;
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static uint16_t bluetooth_parse_uuid(const char *text, int *valid) {
    uint32_t value = 0;
    unsigned digits = 0;

    if (valid)
        *valid = 0;
    if (!text)
        return 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    while (*text) {
        unsigned digit;
        if (*text >= '0' && *text <= '9')
            digit = (unsigned)(*text - '0');
        else if (*text >= 'a' && *text <= 'f')
            digit = (unsigned)(*text - 'a') + 10U;
        else if (*text >= 'A' && *text <= 'F')
            digit = (unsigned)(*text - 'A') + 10U;
        else
            return 0;
        if (++digits > 4U)
            return 0;
        value = (value << 4) | digit;
        text++;
    }
    if (digits == 0U)
        return 0;
    if (valid)
        *valid = 1;
    return (uint16_t)value;
}

static void bluetooth_key_set(int high) {
    if (high)
        GPIOA_BSRR = 1U << BLUETOOTH_KEY_PIN;
    else
        GPIOA_BRR = 1U << BLUETOOTH_KEY_PIN;
}

static void bluetooth_delay_ms(unsigned ms) {
    if (ms == 0U)
        return;

    CORE_DEMCR |= CORE_DEMCR_TRCENA;
    if (!(DWT_CTRL & DWT_CTRL_NOCYCCNT)) {
        DWT_CTRL |= DWT_CTRL_CYCCNTENA;
        uint32_t probe = DWT_CYCCNT;
        for (volatile unsigned i = 0; i < 16U; i++)
            __asm__ __volatile__("nop");
        if (DWT_CYCCNT != probe) {
            uint32_t cycles_per_ms = stm32_hclk_hz() / 1000U;
            while (ms--) {
                uint32_t start = DWT_CYCCNT;
                while ((uint32_t)(DWT_CYCCNT - start) < cycles_per_ms)
                    ;
            }
            return;
        }
    }

    uint32_t fallback_loops = stm32_hclk_hz() / 4000U;
    while (ms--)
        for (volatile uint32_t i = 0; i < fallback_loops; i++)
            __asm__ __volatile__("nop");
}

static void bluetooth_uart_set_baud(uint32_t baud_rate) {
    if (stm32_uart_set_baud(BLUETOOTH_UART, baud_rate) == 0)
        bluetooth_uart_baud = baud_rate;
}

static void bluetooth_uart_flush(void) {
    stm32_uart_drain_rx(BLUETOOTH_UART);
}

static int bluetooth_rx_line_is_present(int state) {
    return state == STM32_BLUETOOTH_RX_DRIVEN_HIGH ||
           state == STM32_BLUETOOTH_RX_DRIVEN_LOW;
}

static int bluetooth_probe_rx_line(void) {
    gpio_config_pin(&GPIOB_CRL, &GPIOB_CRH, 11U, 0x8U);
    GPIOB_BSRR = 1U << 11;
    bluetooth_delay_ms(5U);
    int pull_up_read = !!(GPIOB_IDR & (1U << 11));
    GPIOB_BRR = 1U << 11;
    bluetooth_delay_ms(5U);
    int pull_down_read = !!(GPIOB_IDR & (1U << 11));
    gpio_config_pin(&GPIOB_CRL, &GPIOB_CRH, 11U, 0x4U);

    if (pull_up_read && pull_down_read)
        return STM32_BLUETOOTH_RX_DRIVEN_HIGH;
    if (pull_up_read && !pull_down_read)
        return STM32_BLUETOOTH_RX_FLOATING;
    if (!pull_up_read && !pull_down_read)
        return STM32_BLUETOOTH_RX_DRIVEN_LOW;
    return STM32_BLUETOOTH_RX_UNKNOWN;
}

static int bluetooth_response_contains(const char *response,
                                       const char *needle) {
    if (!response || !needle || !*needle)
        return 0;

    for (size_t i = 0; response[i]; i++) {
        size_t j = 0;
        while (needle[j] && response[i + j] == needle[j])
            j++;
        if (!needle[j])
            return 1;
    }
    return 0;
}

static void bluetooth_log_response(const char *command, const char *response,
                                   size_t length, int key_mode,
                                   uint32_t error_bytes) {
    const char *key_name =
        key_mode == STM32_BLUETOOTH_AT_KEY_PULSE ? "pulse" :
        key_mode == STM32_BLUETOOTH_AT_KEY_HIGH ? "high" : "low";
    printf("[BT-AT] baud=%u key=%s cmd=",
           (unsigned)bluetooth_uart_baud,
           key_name);
    for (size_t i = 0; command && command[i]; i++) {
        char c = command[i];
        if (c == '\r')
            printf("\\r");
        else if (c == '\n')
            printf("\\n");
        else if (c >= 32 && c <= 126)
            printf("%c", c);
        else
            printf("\\x%02x", (unsigned)(uint8_t)c);
    }
    printf(" reply=");
    if (length == 0) {
        printf("<none>");
    } else {
        for (size_t i = 0; i < length; i++) {
            char c = response[i];
            if (c == '\r')
                printf("\\r");
            else if (c == '\n')
                printf("\\n");
            else if (c >= 32 && c <= 126)
                printf("%c", c);
            else
                printf("\\x%02x", (unsigned)(uint8_t)c);
        }
    }
    printf(" bytes=%u errors=%u\n",
           (unsigned)length, (unsigned)error_bytes);
}

static int bluetooth_response_value(const char *response, const char *key,
                                    char *value, size_t capacity) {
    if (!response || !key || !value || capacity < 2U)
        return 0;

    for (size_t i = 0; response[i]; i++) {
        size_t j = 0;
        while (key[j] && response[i + j] == key[j])
            j++;
        if (key[j])
            continue;

        size_t pos = i + j;
        while (response[pos] == ':' || response[pos] == '=' ||
               response[pos] == '"' || response[pos] == ' ')
            pos++;
        size_t out = 0;
        while (response[pos] && response[pos] != '\r' &&
               response[pos] != '\n' && response[pos] != '"' &&
               out + 1U < capacity)
            value[out++] = response[pos++];
        value[out] = '\0';
        return out != 0;
    }
    return 0;
}

static int bluetooth_at_exchange(const char *command, char *response,
                                 size_t capacity, unsigned timeout_ms,
                                 int key_mode) {
    if (!command || !response || capacity < 2U)
        return -1;

    if (key_mode == STM32_BLUETOOTH_AT_KEY_LOW) {
        bluetooth_key_set(0);
    } else {
        bluetooth_key_set(1);
        bluetooth_delay_ms(BLUETOOTH_KEY_SETUP_MS);
    }
    bluetooth.at_attempts++;
    response[0] = '\0';
    bluetooth_uart_flush();
    for (size_t i = 0; command[i]; i++) {
        if (bluetooth_uart_send_byte((uint8_t)command[i]) != 0) {
            if (key_mode == STM32_BLUETOOTH_AT_KEY_PULSE)
                bluetooth_key_set(0);
            return -1;
        }
    }
    if (stm32_uart_wait_tx_complete(
            BLUETOOTH_UART, BLUETOOTH_TX_TIMEOUT) != 0) {
        if (key_mode == STM32_BLUETOOTH_AT_KEY_PULSE)
            bluetooth_key_set(0);
        return -1;
    }
    /* The supplied PZ-HC05 driver drops KEY immediately after transmission. */
    if (key_mode == STM32_BLUETOOTH_AT_KEY_PULSE)
        bluetooth_key_set(0);

    size_t length = 0;
    unsigned idle_ms = 0;
    uint32_t error_bytes = 0;
    for (unsigned waited = 0; waited < timeout_ms; waited++) {
        int received = 0;
        for (;;) {
            uint8_t value;
            int result = stm32_uart_poll_byte(
                BLUETOOTH_UART, &value);
            if (result == 0)
                break;
            if (result < 0) {
                error_bytes++;
                bluetooth.at_error_bytes++;
                continue;
            }
            received = 1;
            bluetooth.at_received_bytes++;
            if (length + 1U < capacity)
                response[length++] = (char)value;
        }
        if (received)
            idle_ms = 0;
        else if (length != 0 && ++idle_ms >= 20U)
            break;
        bluetooth_delay_ms(1U);
    }
    response[length] = '\0';
    bluetooth_log_response(command, response, length,
                           key_mode, error_bytes);
    return bluetooth_response_contains(response, "OK") ? 0 : -1;
}

static int bluetooth_at_command(const char *command, char *response,
                                size_t capacity) {
    for (unsigned attempt = 0; attempt < BLUETOOTH_AT_RETRIES; attempt++) {
        if (bluetooth_at_exchange(command, response, capacity,
                                  BLUETOOTH_AT_TIMEOUT_MS,
                                  bluetooth_at_key_mode) == 0)
            return 0;
        bluetooth_delay_ms(20U);
    }
    return -1;
}

static int bluetooth_reset_to_data_mode(char *response, size_t capacity) {
    static const char reset_command[] = "AT+RESET\r\n";

    if (!response || capacity < 2U)
        return -1;

    if (bluetooth_at_key_mode != STM32_BLUETOOTH_AT_KEY_LOW) {
        bluetooth_key_set(1);
        bluetooth_delay_ms(10U);
    } else {
        bluetooth_key_set(0);
    }
    bluetooth_uart_flush();
    for (size_t i = 0; reset_command[i]; i++) {
        if (bluetooth_uart_send_byte((uint8_t)reset_command[i]) != 0) {
            bluetooth_key_set(0);
            return -1;
        }
    }

    /*
     * KEY must be low when the module performs its software reset. Keeping
     * it high here boots standard HC-05 firmware straight back into full AT
     * mode, where phones cannot discover it.
     */
    if (stm32_uart_wait_tx_complete(
            BLUETOOTH_UART, BLUETOOTH_TX_TIMEOUT) != 0) {
        bluetooth_key_set(0);
        return -1;
    }
    bluetooth_key_set(0);

    size_t length = 0;
    unsigned idle_ms = 0;
    for (unsigned waited = 0; waited < BLUETOOTH_AT_TIMEOUT_MS; waited++) {
        int received = 0;
        for (;;) {
            uint8_t value;
            int result = stm32_uart_poll_byte(
                BLUETOOTH_UART, &value);
            if (result == 0)
                break;
            if (result < 0)
                continue;
            received = 1;
            if (length + 1U < capacity)
                response[length++] = (char)value;
        }
        if (received)
            idle_ms = 0;
        else if (length != 0 && ++idle_ms >= 20U)
            break;
        bluetooth_delay_ms(1U);
    }
    response[length] = '\0';

    /*
     * Some firmware resets before transmitting OK. The command was sent on
     * an already verified AT link, so silence is acceptable; an explicit
     * ERROR is not.
     */
    return bluetooth_response_contains(response, "ERROR") ? -1 : 0;
}

static int bluetooth_find_at_baud(char *response, size_t capacity) {
    static const uint32_t baud_rates[] = {
        9600U, 38400U, 115200U, 57600U, 19200U, 4800U,
    };

    for (unsigned i = 0; i < sizeof(baud_rates) / sizeof(baud_rates[0]);
         i++) {
        bluetooth_uart_set_baud(baud_rates[i]);
        bluetooth_delay_ms(20U);
        if (bluetooth_at_exchange("AT\r\n", response, capacity,
                                  BLUETOOTH_AT_PROBE_TIMEOUT_MS,
                                  STM32_BLUETOOTH_AT_KEY_HIGH) == 0) {
            bluetooth_at_key_mode = STM32_BLUETOOTH_AT_KEY_HIGH;
            return (int)baud_rates[i];
        }
        for (unsigned attempt = 0; attempt < BLUETOOTH_AT_RETRIES; attempt++) {
            if (bluetooth_at_exchange("AT\r\n", response, capacity,
                                      BLUETOOTH_AT_PROBE_TIMEOUT_MS,
                                      STM32_BLUETOOTH_AT_KEY_PULSE) == 0) {
                bluetooth_at_key_mode = STM32_BLUETOOTH_AT_KEY_PULSE;
                return (int)baud_rates[i];
            }
            bluetooth_delay_ms(20U);
        }
    }
    return -1;
}

static int bluetooth_enter_at_mode(char *response, size_t capacity,
                                   uint32_t *at_baud) {
    /*
     * The supplied PZ-HC05 example enters command mode at the transparent-data
     * baud by pulsing KEY around each command. Try that path first. It works
     * after both a power cycle and a debugger-only MCU reset.
     */
    bluetooth_key_set(1);
    bluetooth_uart_set_baud(BLUETOOTH_BAUD_RATE);
    bluetooth_delay_ms(20U);
    if (bluetooth_at_exchange("AT\r\n", response, capacity,
                              BLUETOOTH_AT_PROBE_TIMEOUT_MS,
                              STM32_BLUETOOTH_AT_KEY_HIGH) == 0) {
        bluetooth_at_key_mode = STM32_BLUETOOTH_AT_KEY_HIGH;
        bluetooth.at_key_mode = bluetooth_at_key_mode;
        bluetooth.at_pulse_mode = 0;
        bluetooth.at_power_on_mode = 0;
        *at_baud = BLUETOOTH_BAUD_RATE;
        return 0;
    }
    bluetooth_key_set(0);

    int baud = bluetooth_find_at_baud(response, capacity);
    if (baud >= 0) {
        bluetooth.at_key_mode = bluetooth_at_key_mode;
        bluetooth.at_pulse_mode =
            bluetooth_at_key_mode == STM32_BLUETOOTH_AT_KEY_PULSE;
        bluetooth.at_power_on_mode =
            bluetooth_at_key_mode == STM32_BLUETOOTH_AT_KEY_HIGH &&
            baud == 38400;
        *at_baud = (uint32_t)baud;
        return 0;
    }
    return -1;
}

static int bluetooth_set_and_verify(const char *set_command,
                                    const char *query_command,
                                    const char *key,
                                    const char *expected,
                                    char *response, size_t capacity) {
    char value[40];

    if (bluetooth_at_command(set_command, response, capacity) != 0)
        return 0;
    if (bluetooth_at_command(query_command, response, capacity) != 0)
        return 0;
    if (!bluetooth_response_value(response, key, value, sizeof(value)))
        return 0;
    return bluetooth_text_equal(value, expected);
}

static int bluetooth_set_pin(char *response, size_t capacity) {
    static const char pin_set[] =
        "AT+PSWD=" STM32_BLUETOOTH_PIN "\r\n";
    static const char pin_set_quoted[] =
        "AT+PSWD=\"" STM32_BLUETOOTH_PIN "\"\r\n";
    char pin[sizeof(bluetooth.pin)];

    if (bluetooth_at_command(pin_set, response, capacity) == 0 &&
        bluetooth_at_command("AT+PSWD?\r\n", response, capacity) == 0 &&
        bluetooth_response_value(response, "PSWD", pin, sizeof(pin))) {
        bluetooth_copy_text(bluetooth.pin, sizeof(bluetooth.pin), pin);
        if (bluetooth_text_equal(pin, STM32_BLUETOOTH_PIN))
            return 1;
    }

    /*
     * The second supplied example deliberately retries with quotes because
     * several HC-05 clone firmwares acknowledge the unquoted form without
     * actually changing the PIN.
     */
    if (bluetooth_at_command(pin_set_quoted, response, capacity) != 0 ||
        bluetooth_at_command("AT+PSWD?\r\n", response, capacity) != 0 ||
        !bluetooth_response_value(response, "PSWD", pin, sizeof(pin)))
        return 0;
    bluetooth_copy_text(bluetooth.pin, sizeof(bluetooth.pin), pin);
    return bluetooth_text_equal(pin, STM32_BLUETOOTH_PIN);
}

static int bluetooth_set_name(char *response, size_t capacity) {
    static const char name_set[] =
        "AT+NAME=" STM32_BLUETOOTH_DEVICE_NAME "\r\n";
    char name[sizeof(bluetooth.device_name)];

    if (bluetooth_at_command(name_set, response, capacity) != 0 ||
        bluetooth_at_command("AT+NAME?\r\n", response, capacity) != 0 ||
        !bluetooth_response_value(response, "NAME", name, sizeof(name)))
        return 0;
    bluetooth_copy_text(bluetooth.device_name,
                        sizeof(bluetooth.device_name), name);
    return bluetooth_text_equal(name, STM32_BLUETOOTH_DEVICE_NAME);
}

static void bluetooth_configure_uuid(char *response, size_t capacity) {
    static const char uuid_set[] =
        "AT+UUID=" STM32_BLUETOOTH_SERVICE_UUID_TEXT "\r\n";
    char uuid_text[16];
    int valid = 0;

    bluetooth.service_uuid = 0;
    bluetooth.uuid_supported = 0;
    bluetooth.uuid_configured = 0;
    if (bluetooth_at_command(uuid_set, response, capacity) != 0) {
        bluetooth.service_uuid = 0x1101U;
        return;
    }
    bluetooth.uuid_supported = 1;
    if (bluetooth_at_command("AT+UUID?\r\n", response, capacity) != 0 ||
        !bluetooth_response_value(response, "UUID",
                                  uuid_text, sizeof(uuid_text)))
        return;
    uint16_t uuid = bluetooth_parse_uuid(uuid_text, &valid);
    if (!valid)
        return;
    bluetooth.service_uuid = uuid;
    bluetooth.uuid_configured = uuid == bluetooth.requested_uuid;
}

static int bluetooth_set_uart(uint32_t at_baud, char *response,
                              size_t capacity) {
    static const char uart_set[] =
        "AT+UART=" STM32_BLUETOOTH_BAUD_RATE_TEXT ",0,0\r\n";
    static const char uart_expected[] =
        STM32_BLUETOOTH_BAUD_RATE_TEXT ",0,0";
    char value[40];

    if (bluetooth_at_command(uart_set, response, capacity) != 0)
        return 0;
    if (bluetooth_at_command("AT+UART?\r\n", response, capacity) == 0 &&
        bluetooth_response_value(response, "UART", value, sizeof(value)) &&
        bluetooth_text_equal(value, uart_expected))
        return 1;

    bluetooth_uart_set_baud(BLUETOOTH_BAUD_RATE);
    if (bluetooth_at_command("AT+UART?\r\n", response, capacity) == 0) {
        if (bluetooth_response_value(
                response, "UART", value, sizeof(value)) &&
            bluetooth_text_equal(value, uart_expected))
            return 1;
    }
    bluetooth_uart_set_baud(at_baud);
    return 0;
}

static int bluetooth_configure(void) {
    char response[BLUETOOTH_AT_RESPONSE_MAX];

    bluetooth.at_responsive = 0;
    bluetooth.configured = 0;
    bluetooth.waiting = 0;
    bluetooth.slave_mode = 0;
    bluetooth.discoverable = 0;
    bluetooth.name_configured = 0;
    bluetooth.pin_configured = 0;
    bluetooth.uuid_supported = 0;
    bluetooth.uuid_configured = 0;
    bluetooth.reset_performed = 0;
    bluetooth.at_baud_rate = 0;
    bluetooth.service_uuid = 0;
    bluetooth.device_name[0] = '\0';
    bluetooth.pin[0] = '\0';
    bluetooth.address[0] = '\0';

    uint32_t at_baud = 0;
    bluetooth_at_key_mode = STM32_BLUETOOTH_AT_KEY_PULSE;
    if (bluetooth_enter_at_mode(response, sizeof(response),
                                &at_baud) != 0) {
        bluetooth.detected = 0;
        bluetooth_uart_set_baud(BLUETOOTH_BAUD_RATE);
        bluetooth_key_set(0);
        return -1;
    }
    bluetooth.at_responsive = 1;
    bluetooth.at_baud_rate = at_baud;

    char role[8];
    if (bluetooth_at_command("AT+ROLE?\r\n", response,
                             sizeof(response)) != 0 ||
        !bluetooth_response_value(response, "ROLE", role, sizeof(role))) {
        /* ESP8266 also answers plain AT; ROLE is the HC-05 discriminator. */
        bluetooth.at_responsive = 0;
        bluetooth.detected = 0;
        bluetooth_key_set(0);
        bluetooth_uart_set_baud(BLUETOOTH_BAUD_RATE);
        return -1;
    }
    bluetooth.detected = 1;

    bluetooth.slave_mode = bluetooth_set_and_verify(
        "AT+ROLE=0\r\n", "AT+ROLE?\r\n", "ROLE", "0",
        response, sizeof(response));
    bluetooth.discoverable = bluetooth_set_and_verify(
        "AT+CMODE=1\r\n", "AT+CMODE?\r\n", "CMODE", "1",
        response, sizeof(response));
    bluetooth.name_configured =
        bluetooth_set_name(response, sizeof(response));

    bluetooth.pin_configured =
        bluetooth_set_pin(response, sizeof(response));
    if (bluetooth_at_command("AT+ADDR?\r\n", response,
                             sizeof(response)) == 0)
        (void)bluetooth_response_value(
            response, "ADDR", bluetooth.address,
            sizeof(bluetooth.address));

    bluetooth_configure_uuid(response, sizeof(response));
    int uart_configured =
        bluetooth_set_uart(at_baud, response, sizeof(response));
    int base_configured =
        bluetooth.slave_mode && bluetooth.discoverable &&
        bluetooth.name_configured && bluetooth.pin_configured &&
        uart_configured;
    int uuid_satisfied =
        bluetooth.uuid_configured ||
        (!bluetooth.uuid_supported && bluetooth.requested_uuid == 0x1101U);
    bluetooth.configured = base_configured && uuid_satisfied;

    /*
     * Always leave command mode, even when a vendor firmware fails one of
     * the read-back checks. Otherwise the module remains undiscoverable and
     * looks physically absent.
     */
    if (bluetooth_reset_to_data_mode(response, sizeof(response)) == 0)
        bluetooth.reset_performed = 1;
    bluetooth_uart_set_baud(BLUETOOTH_BAUD_RATE);
    bluetooth_uart_flush();
    if (bluetooth.reset_performed)
        bluetooth_delay_ms(BLUETOOTH_RESET_DELAY_MS);
    bluetooth.waiting = base_configured && bluetooth.reset_performed;
    return bluetooth.waiting ? 0 : -1;
}

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

static int bluetooth_buffer_has_newline(void) {
    unsigned tail = bluetooth_rx_tail;

    while (tail != bluetooth_rx_head) {
        if (bluetooth_rx[tail] == '\n')
            return 1;
        tail = (tail + 1U) % BLUETOOTH_RX_SIZE;
    }
    return 0;
}

void stm32_bluetooth_early_key_init(void) {
#ifdef CONFIG_STM32_XUANWU
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN;
    gpio_config_pin(&GPIOA_CRL, &GPIOA_CRH, BLUETOOTH_KEY_PIN, 0x3U);
    GPIOA_BRR = 1U << BLUETOOTH_KEY_PIN;
#endif
}

int stm32_bluetooth_init(void) {
    bluetooth.ready = 0;
    bluetooth.detected = 0;
    bluetooth.at_responsive = 0;
    bluetooth.at_pulse_mode = 0;
    bluetooth.at_power_on_mode = 0;
    bluetooth.at_key_mode = STM32_BLUETOOTH_AT_KEY_LOW;
    bluetooth.configured = 0;
    bluetooth.connected = 0;
    bluetooth.waiting = 0;
    bluetooth.slave_mode = 0;
    bluetooth.discoverable = 0;
    bluetooth.name_configured = 0;
    bluetooth.pin_configured = 0;
    bluetooth.uuid_supported = 0;
    bluetooth.uuid_configured = 0;
    bluetooth.reset_performed = 0;
    bluetooth.rx_line_state = STM32_BLUETOOTH_RX_UNKNOWN;
    bluetooth.baud_rate = BLUETOOTH_BAUD_RATE;
    bluetooth.at_baud_rate = 0;
    bluetooth.service_uuid = 0;
    bluetooth.requested_uuid = STM32_BLUETOOTH_SERVICE_UUID;
    bluetooth.received_bytes = 0;
    bluetooth.transmitted_bytes = 0;
    bluetooth.dropped_bytes = 0;
    bluetooth.at_received_bytes = 0;
    bluetooth.at_error_bytes = 0;
    bluetooth.at_attempts = 0;
    bluetooth.device_name[0] = '\0';
    bluetooth.pin[0] = '\0';
    bluetooth.address[0] = '\0';
    bluetooth_rx_head = 0;
    bluetooth_rx_tail = 0;
    bluetooth_frame_ready = 0;
    bluetooth_observed_rx = 0;
    bluetooth_last_rx_time = 0;
    bluetooth_state_changed_time = 0;
    bluetooth_state_high = 0;
    bluetooth_at_key_mode = STM32_BLUETOOTH_AT_KEY_LOW;
    bluetooth_uart_baud = 0;

#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    RCC_APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPAEN |
                   RCC_APB2ENR_IOPBEN;
    stm32_bluetooth_early_key_init();

    /*
     * PA15 carries HC-05 STATE on Xuanwu, so retain SWD but release the
     * JTAG-only pins before configuring it as an input.
     */
    AFIO_MAPR = (AFIO_MAPR & ~AFIO_MAPR_SWJ_CFG_MASK) |
                AFIO_MAPR_SWJ_NOJTAG;

    gpio_config_pin(&GPIOA_CRL, &GPIOA_CRH, BLUETOOTH_KEY_PIN, 0x3U);
    gpio_config_pin(&GPIOA_CRL, &GPIOA_CRH, BLUETOOTH_STATE_PIN, 0x8U);
    /*
     * Probe the module TX path while KEY is low. The PZ-HC05 board drives
     * TX through a Schottky level-shift network with pull-ups on both sides;
     * leaving ATSET high while probing can make an otherwise connected path
     * look floating.
     */
    bluetooth_delay_ms(20U);
    bluetooth.rx_line_state = bluetooth_probe_rx_line();
    bluetooth.detected =
        bluetooth_rx_line_is_present(bluetooth.rx_line_state);

    GPIOA_BSRR = 1U << BLUETOOTH_STATE_PIN;

    if (stm32_uart_init(BLUETOOTH_UART, BLUETOOTH_BAUD_RATE, 0) != 0)
        return -1;
    bluetooth_uart_baud = BLUETOOTH_BAUD_RATE;
    bluetooth_delay_ms(BLUETOOTH_BOOT_DELAY_MS);

    /*
     * Re-probe after the module boot delay. Some HC-05 modules do not drive
     * their TX pin until they have finished their own power-on sequence, so an
     * early probe can report floating even when the module is wired correctly.
     */
    bluetooth.rx_line_state = bluetooth_probe_rx_line();
    bluetooth.detected =
        bluetooth_rx_line_is_present(bluetooth.rx_line_state) ||
        bluetooth.detected;

    /*
     * When the module never drives USART3 RX after its power-on delay there is
     * no HC-05 answering, so the exhaustive multi-baud AT scan cannot receive a
     * single byte: the extra retry rounds only stall boot (tens of seconds) and
     * flood the console without changing the outcome (ready stays 0). Scan once
     * in that case. A module whose TX line is driven keeps the full retry
     * budget for robust auto-baud detection, and even the single fallback scan
     * is a complete 6-baud sweep, so a genuinely present module is never missed.
     */
    unsigned init_retries = bluetooth.detected ? BLUETOOTH_INIT_RETRIES : 0U;
    int configured = 0;
    for (unsigned attempt = 0; attempt <= init_retries; attempt++) {
        configured = bluetooth_configure() == 0;
        if (configured)
            break;
        if (attempt < init_retries) {
            printf("[BT] configuration attempt %u failed, retrying in %u ms\n",
                   attempt + 1U,
                   (unsigned)BLUETOOTH_INIT_RETRY_DELAY_MS);
            bluetooth_key_set(0);
            stm32_uart_set_rx_irq(STM32_UART_USART3, 0);
            bluetooth_uart_set_baud(BLUETOOTH_BAUD_RATE);
            bluetooth_uart_flush();
            bluetooth_delay_ms(BLUETOOTH_INIT_RETRY_DELAY_MS);
        }
    }

    bluetooth.connected = 0;
    bluetooth_state_high =
        (bluetooth.detected || bluetooth.at_responsive) &&
        !!(GPIOA_IDR & (1U << BLUETOOTH_STATE_PIN));
    /* Keep the interface armed even when the optional module does not reply. */
    bluetooth.ready = 1;
    bluetooth.waiting = configured;
    stm32_uart_set_rx_irq(BLUETOOTH_UART, 1);
    return configured ? 0 : -1;
#endif
}

int stm32_bluetooth_reprobe(void) {
#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    if (bluetooth.connected)
        return 0;

    bluetooth_key_set(0);
    bluetooth_delay_ms(20U);
    bluetooth.rx_line_state = bluetooth_probe_rx_line();
    bluetooth.detected =
        bluetooth_rx_line_is_present(bluetooth.rx_line_state);
    stm32_uart_set_rx_irq(BLUETOOTH_UART, 0);
    bluetooth_uart_set_baud(BLUETOOTH_BAUD_RATE);
    bluetooth_key_set(0);
    bluetooth_delay_ms(20U);
    int configured = bluetooth_configure() == 0;
    bluetooth.ready = 1;
    bluetooth.waiting = configured;
    bluetooth.connected = 0;
    bluetooth_state_high =
        (bluetooth.detected || bluetooth.at_responsive) &&
        !!(GPIOA_IDR & (1U << BLUETOOTH_STATE_PIN));
    bluetooth_state_changed_time = 0;
    stm32_uart_set_rx_irq(BLUETOOTH_UART, bluetooth.ready);
    return configured ? 0 : -1;
#endif
}

void stm32_bluetooth_irq(void) {
    if (!bluetooth.ready)
        return;

    for (;;) {
        uint8_t value;
        int result = stm32_uart_poll_byte(BLUETOOTH_UART, &value);
        if (result == 0)
            break;
        if (result < 0) {
            bluetooth.dropped_bytes++;
            continue;
        }

        unsigned next = (bluetooth_rx_head + 1U) % BLUETOOTH_RX_SIZE;
        if (next == bluetooth_rx_tail) {
            bluetooth.dropped_bytes++;
            continue;
        }
        bluetooth_rx[bluetooth_rx_head] = value;
        bluetooth_rx_head = next;
        bluetooth.received_bytes++;
        if (value == '\n')
            bluetooth_frame_ready = 1;
    }
}

void stm32_bluetooth_service(uint64_t now) {
    if (!bluetooth.ready)
        return;

    uint32_t irq_flags = arch_irq_save();
    stm32_bluetooth_irq();
    arch_irq_restore(irq_flags);

    int module_present = bluetooth.detected || bluetooth.at_responsive;
    int state_high = module_present &&
        !!(GPIOA_IDR & (1U << BLUETOOTH_STATE_PIN));
    if (state_high != bluetooth_state_high) {
        bluetooth_state_high = state_high;
        bluetooth_state_changed_time = now;
    } else if (state_high && !bluetooth.connected &&
               now - bluetooth_state_changed_time >=
                   BLUETOOTH_CONNECT_ASSERT_MS) {
        bluetooth.connected = 1;
    } else if (!state_high && bluetooth.connected &&
               now - bluetooth_state_changed_time >=
                   BLUETOOTH_DISCONNECT_ASSERT_MS) {
        bluetooth.connected = 0;
    }
    bluetooth.waiting =
        bluetooth.slave_mode && bluetooth.discoverable &&
        bluetooth.name_configured && bluetooth.pin_configured &&
        bluetooth.reset_performed && !bluetooth.connected;

    if (bluetooth_observed_rx != bluetooth.received_bytes) {
        bluetooth_observed_rx = bluetooth.received_bytes;
        bluetooth_last_rx_time = now;
    } else if (bluetooth_rx_head != bluetooth_rx_tail &&
               now - bluetooth_last_rx_time >= BLUETOOTH_FRAME_IDLE_MS) {
        bluetooth_frame_ready = 1;
    }
}

int stm32_bluetooth_connected(void) {
    return bluetooth.ready && bluetooth.connected;
}

int stm32_bluetooth_send(const void *data, size_t length) {
#ifndef CONFIG_STM32_XUANWU
    (void)data;
    (void)length;
    return -1;
#else
    if (!bluetooth.ready || !bluetooth.connected || (!data && length))
        return -1;

    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; i++) {
        if (bluetooth_uart_send_byte(bytes[i]) != 0)
            return -1;
    }
    if (stm32_uart_wait_tx_complete(
            BLUETOOTH_UART, BLUETOOTH_TX_TIMEOUT) != 0)
        return -1;
    bluetooth.transmitted_bytes += (uint32_t)length;
    return 0;
#endif
}

int stm32_bluetooth_send_text(const char *text) {
    if (!text)
        return -1;
    size_t length = 0;
    while (text[length])
        length++;
    return stm32_bluetooth_send(text, length);
}

int stm32_bluetooth_read_line(char *buffer, size_t capacity) {
    if (!buffer || capacity < 2U || !bluetooth.ready ||
        !bluetooth_frame_ready)
        return 0;

    uint32_t flags = arch_irq_save();
    size_t length = 0;
    int complete = 0;

    while (bluetooth_rx_tail != bluetooth_rx_head) {
        uint8_t value = bluetooth_rx[bluetooth_rx_tail];
        bluetooth_rx_tail = (bluetooth_rx_tail + 1U) % BLUETOOTH_RX_SIZE;
        if (length + 1U < capacity)
            buffer[length++] = (char)value;
        if (value == '\n') {
            complete = 1;
            break;
        }
    }
    bluetooth_frame_ready = bluetooth_buffer_has_newline();
    arch_irq_restore(flags);

    if (!complete && bluetooth_rx_tail != bluetooth_rx_head)
        bluetooth_frame_ready = 1;
    buffer[length] = '\0';
    return (int)length;
}

const stm32_bluetooth_info_t *stm32_bluetooth_info(void) {
    return &bluetooth;
}

void stm32_bluetooth_debug_status(void) {
    static const char *const rx_states[] = {
        "unknown", "driven-high", "floating", "driven-low",
    };
    unsigned rx_state = (unsigned)bluetooth.rx_line_state;
    if (rx_state >= sizeof(rx_states) / sizeof(rx_states[0]))
        rx_state = 0;

    const stm32_uart_info_t *uart = stm32_uart_info(BLUETOOTH_UART);
    printf("[BT-DIAG] ready=%d detected=%d rx-line=%s"
           " at=%d at-mode=%s boot-at=%d at-baud=%u data-baud=%u"
           " attempts=%u at-rx=%u at-errors=%u"
           " configured=%d role=%s waiting=%d connected=%d"
           " name=%s pin=%s uuid=0x%x\n",
           bluetooth.ready, bluetooth.detected, rx_states[rx_state],
           bluetooth.at_responsive,
           bluetooth.at_key_mode == STM32_BLUETOOTH_AT_KEY_PULSE ?
               "key-pulse" :
           bluetooth.at_key_mode == STM32_BLUETOOTH_AT_KEY_HIGH ?
               "key-high" : "key-low",
           bluetooth.at_power_on_mode,
           (unsigned)bluetooth.at_baud_rate,
           (unsigned)bluetooth.baud_rate,
           (unsigned)bluetooth.at_attempts,
           (unsigned)bluetooth.at_received_bytes,
           (unsigned)bluetooth.at_error_bytes,
           bluetooth.configured,
           bluetooth.slave_mode ? "slave" : "unknown",
           bluetooth.waiting, bluetooth.connected,
           bluetooth.device_name[0] ? bluetooth.device_name : "<empty>",
           bluetooth.pin[0] ? bluetooth.pin : "<empty>",
           (unsigned)bluetooth.service_uuid);
    printf("[BT-UART] requested=%u actual=%u brr=0x%x irq=%d"
           " pin=%d rx=%u tx=%u edges=%u last=0x%x errors=%u\n",
           (unsigned)uart->requested_baud, (unsigned)uart->actual_baud,
           (unsigned)uart->divider, uart->rx_irq_enabled,
           stm32_uart_rx_pin_level(BLUETOOTH_UART),
           (unsigned)uart->rx_bytes, (unsigned)uart->tx_bytes,
           (unsigned)uart->rx_transitions, (unsigned)uart->last_rx_byte,
           (unsigned)uart->error_count);
}

int stm32_bluetooth_debug_probe(void) {
#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    char response[BLUETOOTH_AT_RESPONSE_MAX];
    int rx_irq_enabled =
        stm32_uart_rx_irq_enabled(BLUETOOTH_UART);

    printf("[BT-DIAG] probing runtime KEY-pulse AT modes\n");
    stm32_uart_set_rx_irq(BLUETOOTH_UART, 0);
    bluetooth_key_set(0);
    bluetooth_delay_ms(20U);
    int at_baud = bluetooth_find_at_baud(response, sizeof(response));
    bluetooth_key_set(0);
    bluetooth_uart_set_baud(BLUETOOTH_BAUD_RATE);
    bluetooth_uart_flush();
    stm32_uart_set_rx_irq(BLUETOOTH_UART, rx_irq_enabled);

    if (at_baud < 0) {
        printf("[BT-DIAG] probe failed: no valid OK response\n");
        return -1;
    }
    printf("[BT-DIAG] probe success: baud=%u\n", (unsigned)at_baud);
    return 0;
#endif
}

int stm32_bluetooth_debug_at(const char *command) {
#ifndef CONFIG_STM32_XUANWU
    (void)command;
    return -1;
#else
    if (!command || command[0] != 'A' || command[1] != 'T')
        return -1;

    char command_line[64];
    size_t length = 0;
    while (command[length] && length + 3U < sizeof(command_line)) {
        command_line[length] = command[length];
        length++;
    }
    command_line[length++] = '\r';
    command_line[length++] = '\n';
    command_line[length] = '\0';

    char response[BLUETOOTH_AT_RESPONSE_MAX];
    int rx_irq_enabled =
        stm32_uart_rx_irq_enabled(BLUETOOTH_UART);
    stm32_uart_set_rx_irq(BLUETOOTH_UART, 0);
    bluetooth_key_set(0);
    bluetooth_delay_ms(20U);
    int at_baud = bluetooth_find_at_baud(response, sizeof(response));
    if (at_baud < 0) {
        bluetooth_key_set(0);
        bluetooth_uart_set_baud(BLUETOOTH_BAUD_RATE);
        stm32_uart_set_rx_irq(BLUETOOTH_UART, rx_irq_enabled);
        printf("[BT-DIAG] cannot send command: AT mode not found\n");
        return -1;
    }

    bluetooth_uart_set_baud((uint32_t)at_baud);
    int result = bluetooth_at_exchange(
        command_line, response, sizeof(response),
        BLUETOOTH_AT_TIMEOUT_MS, STM32_BLUETOOTH_AT_KEY_PULSE);
    bluetooth_key_set(0);
    bluetooth_uart_set_baud(BLUETOOTH_BAUD_RATE);
    bluetooth_uart_flush();
    stm32_uart_set_rx_irq(BLUETOOTH_UART, rx_irq_enabled);
    return result;
#endif
}

#endif
