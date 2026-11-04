/*
 * Hi3861 driver smoke-test shell.
 *
 * Kept in the MCU arch layer because it is a board bring-up interface, not a
 * driver.  The commands call the same generic controller APIs a real consumer
 * would use (gpio_set_value, i2c_write_read, pwm_start, adc_read_raw, ...),
 * so a PASS here exercises the driver core, platform matching, probe and the
 * hardware programming path.
 */
#if defined(CONFIG_MCU_DRIVER_MODEL)
#include "core/stdio.h"
#include "core/string.h"
#include "drivers/adc/adc.h"
#include "drivers/core/driver_core.h"
#include "drivers/gpio/gpio.h"
#include "drivers/i2c/i2c.h"
#include "drivers/pwm/pwm.h"
#include "drivers/spi/spi.h"

#define TOKEN_MAX 24

static const char *token(const char **pp, char *out, size_t n)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p) {
        *pp = p;
        return NULL;
    }
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\t') {
        if (i + 1 < n)
            out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    *pp = p;
    return out;
}

static int number(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    if (!s || !*s)
        return -1;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (!*s)
        return -1;
    for (; *s; s++) {
        unsigned d;
        if (*s >= '0' && *s <= '9')
            d = (unsigned)(*s - '0');
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            d = (unsigned)(*s - 'a' + 10);
        else if (base == 16 && *s >= 'A' && *s <= 'F')
            d = (unsigned)(*s - 'A' + 10);
        else
            return -1;
        v = v * (uint32_t)base + d;
    }
    *out = v;
    return 0;
}

static void gpio_cmd(const char *rest)
{
    char op[12], a[TOKEN_MAX], b[TOKEN_MAX];
    uint32_t pin, val;
    if (!token(&rest, op, sizeof(op)) || !token(&rest, a, sizeof(a)) ||
        number(a, &pin) < 0 || pin >= 15) {
        printf("usage: gpio get|set|dir|pull|func <pin> [value]\n");
        return;
    }
    if (!strcmp(op, "get")) {
        (void)gpio_set_function(pin, 0);
        (void)gpio_set_direction(pin, GPIO_DIR_IN);
        (void)gpio_set_input_enable(pin, 1);
        printf("gpio%u = %d\n", (unsigned)pin, gpio_get_value(pin));
    } else if (!strcmp(op, "set")) {
        if (!token(&rest, b, sizeof(b)) || number(b, &val) < 0) {
            printf("usage: gpio set <pin> <0|1>\n");
            return;
        }
        (void)gpio_set_function(pin, 0);
        (void)gpio_set_direction(pin, GPIO_DIR_OUT);
        (void)gpio_set_value(pin, (int)val);
        printf("gpio%u = %u\n", (unsigned)pin, (unsigned)val);
    } else if (!strcmp(op, "dir")) {
        if (!token(&rest, b, sizeof(b))) {
            printf("usage: gpio dir <pin> in|out\n");
            return;
        }
        int out = strcmp(b, "out") == 0;
        (void)gpio_set_function(pin, 0);
        (void)gpio_set_input_enable(pin, !out);
        int rc = gpio_set_direction(pin, out ? GPIO_DIR_OUT : GPIO_DIR_IN);
        printf("gpio%u dir=%s rc=%d\n", (unsigned)pin, out ? "out" : "in", rc);
    } else if (!strcmp(op, "pull")) {
        if (!token(&rest, b, sizeof(b))) {
            printf("usage: gpio pull <pin> none|up|down\n");
            return;
        }
        unsigned pull = !strcmp(b, "up") ? GPIO_PULL_UP
                        : !strcmp(b, "down") ? GPIO_PULL_DOWN
                                             : GPIO_PULL_NONE;
        printf("gpio%u pull rc=%d\n", (unsigned)pin,
               gpio_set_pull(pin, pull));
    } else if (!strcmp(op, "func")) {
        if (!token(&rest, b, sizeof(b)) || number(b, &val) < 0) {
            printf("usage: gpio func <pin> <function>\n");
            return;
        }
        printf("gpio%u func=%u rc=%d\n", (unsigned)pin, (unsigned)val,
               gpio_set_function(pin, val));
    } else {
        printf("unknown gpio op\n");
    }
}

static void adc_cmd(const char *rest)
{
    char a[TOKEN_MAX];
    uint32_t ch;
    if (!token(&rest, a, sizeof(a)) || number(a, &ch) < 0 || ch > 6) {
        printf("usage: adc <0..6>\n");
        return;
    }
    uint16_t raw = 0;
    int rc = adc_read_raw(ch, &raw);
    if (rc)
        printf("adc%u rc=%d\n", (unsigned)ch, rc);
    else
        printf("adc%u raw=%u mv=%u\n", (unsigned)ch, (unsigned)raw,
               (unsigned)adc_raw_to_mv(raw));
}

static void pwm_cmd(const char *rest)
{
    char a[TOKEN_MAX], b[TOKEN_MAX], c[TOKEN_MAX];
    uint32_t ch, duty, period;
    if (!token(&rest, a, sizeof(a))) {
        printf("usage: pwm <ch> <duty> <period> | pwm stop <ch>\n");
        return;
    }
    if (!strcmp(a, "stop")) {
        if (!token(&rest, b, sizeof(b)) || number(b, &ch) < 0) {
            printf("usage: pwm stop <ch>\n");
            return;
        }
        printf("pwm%u stop rc=%d\n", (unsigned)ch, pwm_stop(ch));
        return;
    }
    if (number(a, &ch) < 0 || !token(&rest, b, sizeof(b)) ||
        number(b, &duty) < 0 || !token(&rest, c, sizeof(c)) ||
        number(c, &period) < 0) {
        printf("usage: pwm <ch> <duty> <period>\n");
        return;
    }
    printf("pwm%u duty=%u period=%u rc=%d\n", (unsigned)ch, (unsigned)duty,
           (unsigned)period, pwm_start(ch, (uint16_t)duty, (uint16_t)period));
}

static void i2c_cmd(const char *rest)
{
    char op[12], a[TOKEN_MAX], b[TOKEN_MAX], c[TOKEN_MAX], d[TOKEN_MAX];
    uint32_t bus, addr, reg, len;

    if (!token(&rest, op, sizeof(op))) {
        printf("usage: i2c scan <bus> | i2c rd <bus> <addr> <reg> <len> | "
               "i2c wr <bus> <addr> <reg> <byte...>\n");
        return;
    }
    if (!strcmp(op, "scan")) {
        if (!token(&rest, a, sizeof(a)) || number(a, &bus) < 0) {
            printf("usage: i2c scan <bus>\n");
            return;
        }
        printf("i2c%u scan:", (unsigned)bus);
        for (uint32_t addr = 0x08; addr < 0x78; addr++) {
            uint8_t v;
            if (i2c_read(bus, (uint16_t)addr, &v, 1) == 0)
                printf(" %02x", (unsigned)addr);
        }
        printf("\n");
        return;
    }
    if (!token(&rest, a, sizeof(a)) || number(a, &bus) < 0 ||
        !token(&rest, b, sizeof(b)) || number(b, &addr) < 0 ||
        !token(&rest, c, sizeof(c)) || number(c, &reg) < 0) {
        printf("i2c: bad arguments\n");
        return;
    }
    uint8_t r = (uint8_t)reg;
    if (!strcmp(op, "rd")) {
        if (!token(&rest, d, sizeof(d)) || number(d, &len) < 0 || len > 32) {
            printf("i2c rd: bad length\n");
            return;
        }
        uint8_t buf[32];
        int rc = i2c_write_read(bus, (uint16_t)addr, &r, 1, buf, (uint16_t)len);
        printf("i2c%u rd %02x/%02x rc=%d:", (unsigned)bus, (unsigned)addr,
               (unsigned)reg, rc);
        if (!rc)
            for (uint32_t i = 0; i < len; i++)
                printf(" %02x", buf[i]);
        printf("\n");
    } else if (!strcmp(op, "wr")) {
        uint8_t buf[32];
        uint32_t n = 0;
        while (n < sizeof(buf) && token(&rest, d, sizeof(d)) &&
               number(d, &len) == 0)
            buf[n++] = (uint8_t)len;
        if (!n) {
            printf("i2c wr: no data\n");
            return;
        }
        /* Register byte first, then payload. */
        uint8_t all[33];
        all[0] = r;
        memcpy(all + 1, buf, n);
        printf("i2c%u wr %02x/%02x n=%u rc=%d\n", (unsigned)bus,
               (unsigned)addr, (unsigned)reg, (unsigned)n,
               i2c_write(bus, (uint16_t)addr, all, (uint16_t)(n + 1)));
    } else {
        printf("unknown i2c op\n");
    }
}

static void spi_cmd(const char *rest)
{
    char op[12], a[TOKEN_MAX];
    uint32_t bus;
    if (!token(&rest, op, sizeof(op)) || !token(&rest, a, sizeof(a)) ||
        number(a, &bus) < 0) {
        printf("usage: spi loop <bus>\n");
        return;
    }
    if (strcmp(op, "loop")) {
        printf("unknown spi op\n");
        return;
    }
    static uint8_t tx[8] = {0x55, 0xAA, 0x00, 0xFF, 0x12, 0x34, 0x5A, 0xA5};
    uint8_t rx[8] = {0};
    int rc = spi_transfer(bus, tx, rx, sizeof(tx), 1000000, 0);
    printf("spi%u loop rc=%d:", (unsigned)bus, rc);
    if (!rc)
        for (unsigned i = 0; i < sizeof(rx); i++)
            printf(" %02x", rx[i]);
    printf("\n");
}

int mcu_driver_shell(const char *line)
{
    char cmd[12];
    const char *rest = line;
    if (!token(&rest, cmd, sizeof(cmd)))
        return 0;

    if (!strcmp(cmd, "drivers")) {
        char buf[512];
        driver_core_list_drivers(buf, sizeof(buf));
        printf("%s", buf);
        return 1;
    }
    if (!strcmp(cmd, "devs")) {
        char buf[768];
        driver_core_list_devices(buf, sizeof(buf));
        printf("%s", buf);
        return 1;
    }
    if (!strcmp(cmd, "gpio")) {
        gpio_cmd(rest);
        return 1;
    }
    if (!strcmp(cmd, "adc")) {
        adc_cmd(rest);
        return 1;
    }
    if (!strcmp(cmd, "pwm")) {
        pwm_cmd(rest);
        return 1;
    }
    if (!strcmp(cmd, "i2c")) {
        i2c_cmd(rest);
        return 1;
    }
    if (!strcmp(cmd, "spi")) {
        spi_cmd(rest);
        return 1;
    }
    return 0;
}
#endif
