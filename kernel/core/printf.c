#include "core/stdio.h"
#include "core/string.h"
#include "drv/uart.h"

static int utoa(uint64_t value, char *buf, int base, int upper) {
    char tmp[32];
    int i = 0, len = 0;
    if (value == 0) { buf[0] = '0'; return 1; }
    while (value > 0) {
        int d = value % base;
        tmp[i++] = d < 10 ? '0' + d : ((upper ? 'A' : 'a') + d - 10);
        value /= base;
    }
    while (i-- > 0) buf[len++] = tmp[i];
    return len;
}

static int itoa(int64_t value, char *buf, int base) {
    int len = 0;
    if (value < 0 && base == 10) { buf[len++] = '-'; value = -value; }
    return len + utoa((uint64_t)value, buf + len, base, 0);
}

typedef void (*putc_fn)(char c, void *ctx);

static void do_format(const char *fmt, va_list args, putc_fn putc, void *ctx) {
    char num_buf[32];
    int long_mode = 0;

    while (*fmt) {
        if (*fmt != '%') { putc(*fmt, ctx); fmt++; continue; }
        fmt++;
        long_mode = 0;

        int width = 0;
        int pad_zero = 0;
        if (*fmt == '0') { pad_zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        if (*fmt == 'l') { long_mode = 1; fmt++; }
        if (*fmt == 'l') { long_mode = 1; fmt++; }

        int nlen = 0;
        switch (*fmt) {
        case 'd': {
            int64_t v = long_mode ? va_arg(args, int64_t) : va_arg(args, int);
            nlen = itoa(v, num_buf, 10);
            break;
        }
        case 'u': {
            uint64_t v = long_mode ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
            nlen = utoa(v, num_buf, 10, 0);
            break;
        }
        case 'x': {
            uint64_t v = long_mode ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
            nlen = utoa(v, num_buf, 16, 0);
            break;
        }
        case 'X': {
            uint64_t v = long_mode ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
            nlen = utoa(v, num_buf, 16, 1);
            break;
        }
        case 'p': {
            num_buf[0] = '0'; num_buf[1] = 'x';
            uint64_t v = (uint64_t)va_arg(args, void*);
            nlen = 2 + utoa(v, num_buf + 2, 16, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(args, const char*);
            if (!s) s = "(null)";
            while (*s) putc(*s++, ctx);
            nlen = 0;
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            putc(c, ctx);
            nlen = 0;
            break;
        }
        case '%': putc('%', ctx); nlen = 0; break;
        default: putc('%', ctx); putc(*fmt, ctx); nlen = 0; break;
        }

        if (nlen > 0) {
            int neg = (num_buf[0] == '-') ? 1 : 0;
            int pad = (width > nlen) ? width - nlen : 0;
            for (int i = 0; i < pad; i++) putc(pad_zero && !neg ? '0' : ' ', ctx);
            for (int i = 0; i < nlen; i++) putc(num_buf[i], ctx);
        }
        fmt++;
    }
}

/* vprintf: buffer-then-UART backend */

typedef struct { char buf[1024]; int pos; } vprintf_ctx_t;

static void vprintf_putc(char c, void *ctx) {
    vprintf_ctx_t *vc = ctx;
    if (vc->pos < (int)sizeof(vc->buf) - 1)
        vc->buf[vc->pos++] = c;
}

void vprintf(const char *fmt, va_list args) {
    vprintf_ctx_t vc = { .pos = 0 };
    do_format(fmt, args, vprintf_putc, &vc);
    vc.buf[vc.pos] = '\0';
    uart_puts(vc.buf);
}

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

int puts(const char *s) { uart_puts(s); uart_putc('\n'); return 0; }
int putchar(char c) { uart_putc(c); return (int)c; }
int getchar(void) { return uart_getc(); }

/* vsnprintf: caller-buffer backend */

typedef struct { char *out; size_t size; size_t pos; } snprintf_ctx_t;

static void snprintf_putc(char c, void *ctx) {
    snprintf_ctx_t *sc = ctx;
    if (sc->size > 0 && sc->pos < sc->size - 1)
        sc->out[sc->pos++] = c;
}

int vsnprintf(char *out, size_t size, const char *fmt, va_list args) {
    snprintf_ctx_t sc = { .out = out, .size = size, .pos = 0 };
    do_format(fmt, args, snprintf_putc, &sc);
    if (size > 0) out[sc.pos < size ? sc.pos : size - 1] = '\0';
    return (int)sc.pos;
}

int snprintf(char *out, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out, size, fmt, args);
    va_end(args);
    return n;
}
