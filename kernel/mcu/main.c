#include "core/arch.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_core.h"
#include "mm/slab.h"
#include "backlight.h"
#include "bluetooth.h"
#include "console.h"
#include "peripherals.h"
#include "heap.h"
#include "light_sensor.h"
#include "stm32_uart.h"

static volatile char diagnostic_line[64];
static unsigned diagnostic_length;

static int text_equal(const char *left, const char *right) {
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int text_starts_with(const char *text, const char *prefix) {
    while (*prefix && *text == *prefix) {
        text++;
        prefix++;
    }
    return *prefix == '\0';
}

static void diagnostic_light_status(void) {
    const stm32_light_sensor_info_t *light = stm32_light_sensor_info();
    stm32_backlight_debug_t pwm;
    stm32_backlight_debug(&pwm);
    printf("[LIGHT] ready=%d raw=%u filtered=%u level=%u/100"
           " backlight=fixed-on samples=%u errors=%u\n",
           light->ready, (unsigned)light->raw_adc,
           (unsigned)light->filtered_adc,
           (unsigned)light->intensity_percent,
           (unsigned)light->samples, (unsigned)light->errors);
    printf("[BACKLIGHT] ready=%d percent=%u PB0-mode=0x%x"
           " IDR=%u ODR=%u pin=%u accumulator=%u"
           " writes=%u ticks=%u transitions=%u\n",
           pwm.initialized, (unsigned)pwm.percent,
           (unsigned)(pwm.gpio_crl & 0xFU),
           (unsigned)(pwm.gpio_idr & 1U),
           (unsigned)(pwm.gpio_odr & 1U),
           (unsigned)pwm.pin_high,
           (unsigned)pwm.accumulator,
           (unsigned)pwm.updates,
           (unsigned)pwm.modulation_ticks,
           (unsigned)pwm.pin_transitions);
}

static void diagnostic_prompt(void) {
    printf("a20> ");
}

static void diagnostic_performance(void) {
    const stm32_peripheral_state_t *state = stm32_peripherals_state();
    printf("[PERF] uptime=%lu ms hclk=%u pclk1=%u pclk2=%u"
           " service-calls=%u last=%u ms max=%u ms slow=%u"
           " light-max=%u ms sd-max=%u ms bt-retry-max=%u ms\n",
           (unsigned long)timer_get_ticks(),
           (unsigned)stm32_hclk_hz(),
           (unsigned)stm32_pclk1_hz(),
           (unsigned)stm32_pclk2_hz(),
           (unsigned)state->service_calls,
           (unsigned)state->service_last_ms,
           (unsigned)state->service_max_ms,
           (unsigned)state->service_slow_calls,
           (unsigned)state->light_max_ms,
           (unsigned)state->sdcard_max_ms,
           (unsigned)state->bluetooth_retry_max_ms);
}

static void diagnostic_help(void) {
    printf("Commands:\n");
    printf("  uart          show USART1/USART3 clock/baud/error state\n");
    printf("  perf          show clocks and main service latency\n");
    printf("  light         show ADC3 light sensor state\n");
    printf("  sd retry      explicitly probe and initialize the TF card\n");
    printf("  bt            show HC-05 detection/configuration state\n");
    printf("  bt retry      rerun HC-05 detection and slave configuration\n");
    printf("  bt probe      scan HC-05 runtime AT baud/key modes\n");
    printf("  bt at <cmd>   send an AT command, for example AT+ROLE?\n");
    printf("  help          show this command list\n");
}

static void diagnostic_execute(char *line) {
    if (!line[0])
        return;
    if (text_equal(line, "help") || text_equal(line, "?")) {
        diagnostic_help();
    } else if (text_equal(line, "uart")) {
        printf("[UART] USART1 PA9/PA10 clock=%u requested=%u"
               " actual=%u BRR=0x%x 8N1 errors=%u\n",
               (unsigned)arch_uart_clock_hz(),
               (unsigned)arch_uart_baud_rate(),
               (unsigned)arch_uart_actual_baud_rate(),
               (unsigned)arch_uart_divider(),
               (unsigned)arch_uart_error_count());
        const stm32_uart_info_t *uart3 =
            stm32_uart_info(STM32_UART_USART3);
        printf("[UART] USART3 PB10/PB11 initialized=%d clock=%u"
               " requested=%u actual=%u BRR=0x%x 8N1 rx-irq=%d"
               " errors=%u rx=%u tx=%u edges=%u pin=%d last=0x%x\n",
               uart3->initialized, (unsigned)uart3->clock_hz,
               (unsigned)uart3->requested_baud,
               (unsigned)uart3->actual_baud,
               (unsigned)uart3->divider, uart3->rx_irq_enabled,
               (unsigned)uart3->error_count,
               (unsigned)uart3->rx_bytes,
               (unsigned)uart3->tx_bytes,
               (unsigned)uart3->rx_transitions,
               stm32_uart_rx_pin_level(STM32_UART_USART3),
               (unsigned)uart3->last_rx_byte);
    } else if (text_equal(line, "perf")) {
        diagnostic_performance();
    } else if (text_equal(line, "light")) {
        diagnostic_light_status();
    } else if (text_equal(line, "sd retry")) {
        int result = stm32_peripherals_retry_sdcard();
        printf("[TF-DIAG] retry=%s\n",
               result == 0 ? "ready" : "absent-or-failed");
        diagnostic_performance();
    } else if (text_equal(line, "bt")) {
        stm32_bluetooth_debug_status();
    } else if (text_equal(line, "bt retry")) {
        int result = stm32_peripherals_retry_bluetooth();
        printf("[BT-DIAG] retry=%s\n",
               result == 0 ? "module-present" : "failed");
        stm32_bluetooth_debug_status();
        diagnostic_performance();
    } else if (text_equal(line, "bt probe")) {
        (void)stm32_bluetooth_debug_probe();
    } else if (text_starts_with(line, "bt at ")) {
        (void)stm32_bluetooth_debug_at(line + 6);
    } else {
        printf("Unknown command: %s\n", line);
        printf("Type help for available commands.\n");
    }
}

static void diagnostic_service(void) {
    int c;
    while ((c = uart_try_getc()) >= 0) {
        if (c == '\r' || c == '\n') {
            if (diagnostic_length == 0)
                continue;
            uart_putc('\n');
            diagnostic_line[diagnostic_length] = '\0';
            diagnostic_execute((char *)diagnostic_line);
            diagnostic_length = 0;
            diagnostic_prompt();
            continue;
        }
        if (c == 8 || c == 127) {
            if (diagnostic_length != 0) {
                diagnostic_length--;
                uart_puts("\b \b");
            }
            continue;
        }
        if (c < 32 || c > 126)
            continue;
        if (diagnostic_length + 1U >= sizeof(diagnostic_line)) {
            uart_putc('\a');
            continue;
        }
        diagnostic_line[diagnostic_length++] = (char)c;
        uart_putc((char)c);
    }
}

void kernel_main(void) {
    arch_local_irq_disable();
    if (current_board && current_board->early_init)
        current_board->early_init();
    uart_init();
    mcu_heap_init();
    diagnostic_length = 0;

    printf("\n======================================\n");
    printf(" A20OS ARMv7-M STM32F103 bringup\n");
    printf("======================================\n");
    printf("[BOOT] board=%s arch=%s\n",
           current_board ? current_board->name : "unknown", ARCH_NAME);
    printf("[BOOT] USART1=PA9/PA10 clock=%u requested=%u"
           " actual=%u BRR=0x%x 8N1 RXNE-IRQ\n",
           (unsigned)arch_uart_clock_hz(),
           (unsigned)arch_uart_baud_rate(),
           (unsigned)arch_uart_actual_baud_rate(),
           (unsigned)arch_uart_divider());
    printf("[BOOT] heap=%u bytes\n", (unsigned)mcu_heap_available());

    void *probe = kmalloc(64);
    printf("[BOOT] allocator=%s ptr=%p\n", probe ? "ok" : "failed", probe);
    kfree(probe);

    timer_init();
    stm32_peripherals_init();

    arch_local_irq_enable();
    printf("[BOOT] SysTick=1000Hz source=HCLK=%u, entering WFI loop\n",
           (unsigned)stm32_hclk_hz());
    diagnostic_help();
    diagnostic_prompt();

    for (;;) {
        uint64_t now = timer_get_ticks();
        stm32_peripherals_service(now);
        diagnostic_service();
        arch_wfi();
    }
}
