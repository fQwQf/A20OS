#include "stdio.h"
#include "string.h"
#include "uart.h"

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

void vprintf(const char *fmt, va_list args) {
    char buf[1024];
    char num_buf[32];
    int pos = 0;
    int long_mode = 0;

#define PUTC(c) do { if (pos < (int)sizeof(buf)-1) buf[pos++] = (c); } while(0)

    while (*fmt) {
        if (*fmt != '%') { PUTC(*fmt); fmt++; continue; }
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
            while (*s) PUTC(*s++);
            nlen = 0;
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            PUTC(c);
            nlen = 0;
            break;
        }
        case '%': PUTC('%'); nlen = 0; break;
        default: PUTC('%'); PUTC(*fmt); nlen = 0; break;
        }

        if (nlen > 0) {
            int neg = (num_buf[0] == '-') ? 1 : 0;
            int pad = (width > nlen) ? width - nlen : 0;
            for (int i = 0; i < pad; i++) PUTC(pad_zero && !neg ? '0' : ' ');
            for (int i = 0; i < nlen; i++) PUTC(num_buf[i]);
        }
        fmt++;
    }

#undef PUTC

    buf[pos] = '\0';
    uart_puts(buf);
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

/* vsnprintf — same as snprintf but takes va_list */
int vsnprintf(char *out, size_t size, const char *fmt, va_list args) {
    char num_buf[32];
    size_t pos = 0;
    int long_mode = 0;

#define OUTC2(c) do { if (pos < size - 1) out[pos++] = (c); } while(0)

    while (*fmt) {
        if (*fmt != '%') { OUTC2(*fmt); fmt++; continue; }
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
        case 's': {
            const char *s = va_arg(args, const char*);
            if (!s) s = "(null)";
            while (*s) OUTC2(*s++);
            nlen = 0;
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            OUTC2(c);
            nlen = 0;
            break;
        }
        case '%': OUTC2('%'); nlen = 0; break;
        default: OUTC2('%'); OUTC2(*fmt); nlen = 0; break;
        }

        if (nlen > 0) {
            int neg = (num_buf[0] == '-') ? 1 : 0;
            int pad = (width > nlen) ? width - nlen : 0;
            for (int i = 0; i < pad; i++) OUTC2(pad_zero && !neg ? '0' : ' ');
            for (int i = 0; i < nlen; i++) OUTC2(num_buf[i]);
        }
        fmt++;
    }

#undef OUTC2

    if (size > 0) out[pos < size ? pos : size - 1] = '\0';
    return (int)pos;
}

int snprintf(char *out, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out, size, fmt, args);
    va_end(args);
    return n;
}
