/*
 * A20OS liba20c — printf implementation.
 * Supports: %d %u %x %s %c %p %% %ld %lu
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

static int puts_buf(char *buf, int *pos, const char *s, int max)
{
    while (*s && *pos < max - 1)
        buf[(*pos)++] = *s++;
    return *pos;
}

static int put_int(char *buf, int *pos, int64_t val, int max)
{
    char tmp[21];
    int i = 0;
    int neg = 0;

    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) { tmp[i++] = '0'; }
    else { while (val > 0) { tmp[i++] = '0' + (char)(val % 10); val /= 10; } }

    if (neg && *pos < max - 1) buf[(*pos)++] = '-';
    for (int j = i - 1; j >= 0 && *pos < max - 1; j--)
        buf[(*pos)++] = tmp[j];
    return *pos;
}

static int put_uint(char *buf, int *pos, uint64_t val, int base, int max)
{
    const char *digits = "0123456789abcdef";
    char tmp[21];
    int i = 0;
    if (val == 0) { tmp[i++] = '0'; }
    else { while (val > 0) { tmp[i++] = digits[val % base]; val /= base; } }
    for (int j = i - 1; j >= 0 && *pos < max - 1; j--)
        buf[(*pos)++] = tmp[j];
    return *pos;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    int pos = 0;
    if (!buf || size == 0) return 0;

    while (*fmt && pos < (int)size - 1) {
        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
        fmt++;
        int long_flag = 0;
        if (*fmt == 'l') { long_flag = 1; fmt++; }
        if (*fmt == 'l') { long_flag = 2; fmt++; }

        switch (*fmt) {
        case 'd': {
            int64_t v = long_flag ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            put_int(buf, &pos, v, (int)size);
            break;
        }
        case 'u': {
            uint64_t v = long_flag ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            put_uint(buf, &pos, v, 10, (int)size);
            break;
        }
        case 'x': {
            uint64_t v = long_flag ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            put_uint(buf, &pos, v, 16, (int)size);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            puts_buf(buf, &pos, s, (int)size);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (pos < (int)size - 1) buf[pos++] = c;
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            if (pos < (int)size - 1) buf[pos++] = '0';
            if (pos < (int)size - 1) buf[pos++] = 'x';
            put_uint(buf, &pos, v, 16, (int)size);
            break;
        }
        case '%':
            if (pos < (int)size - 1) buf[pos++] = '%';
            break;
        default:
            if (pos < (int)size - 1) buf[pos++] = '%';
            if (pos < (int)size - 1) buf[pos++] = *fmt;
            break;
        }
        fmt++;
    }
    buf[pos] = '\0';
    return pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, 65536, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (r > 0) fwrite(buf, 1, r, stdout);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (r > 0) fwrite(buf, 1, r, f);
    return r;
}
