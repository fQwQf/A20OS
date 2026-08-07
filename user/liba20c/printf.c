/*
 * A20OS liba20c — printf implementation.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

static void outc(char c, char *buf, size_t size, int *pos)
{
    if ((size_t)*pos < size) buf[*pos] = c;
    (*pos)++;
}

static void outs(const char *s, int len, char *buf, size_t size, int *pos)
{
    for (int i = 0; i < len; i++) {
        if ((size_t)*pos < size) buf[*pos] = s[i];
        (*pos)++;
    }
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int parse_num(const char **fmt)
{
    int n = 0;
    while (is_digit(**fmt)) {
        n = n * 10 + (**fmt - '0');
        (*fmt)++;
    }
    return n;
}

static int64_t get_signed(va_list *pap, int len)
{
    switch (len) {
    case 1:  return (int64_t)va_arg(*pap, long);
    case 2:  return (int64_t)va_arg(*pap, long long);
    case 3:  return (int64_t)va_arg(*pap, ptrdiff_t); /* z */
    case 4:  return (int64_t)va_arg(*pap, ptrdiff_t);       /* t */
    case 5:  return (int64_t)(short)va_arg(*pap, int);       /* h */
    case 6:  return (int64_t)(signed char)va_arg(*pap, int); /* hh */
    default: return (int64_t)va_arg(*pap, int);
    }
}

static uint64_t get_unsigned(va_list *pap, int len)
{
    switch (len) {
    case 1:  return (uint64_t)va_arg(*pap, unsigned long);
    case 2:  return (uint64_t)va_arg(*pap, unsigned long long);
    case 3:  return (uint64_t)va_arg(*pap, size_t);                  /* z */
    case 4:  return (uint64_t)(size_t)va_arg(*pap, ptrdiff_t);      /* t */
    case 5:  return (uint64_t)(unsigned short)va_arg(*pap, int);     /* h */
    case 6:  return (uint64_t)(unsigned char)va_arg(*pap, int);    /* hh */
    default: return (uint64_t)va_arg(*pap, unsigned int);
    }
}

static void fmt_int(char *buf, size_t size, int *pos,
                    int64_t val, int width, int prec, int left, int zero,
                    int plus, int space)
{
    int neg = val < 0;
    uint64_t u = neg ? (uint64_t)(-val) : (uint64_t)val;

    char digits[32];
    int dlen = 0;
    if (u == 0) {
        if (prec != 0) digits[dlen++] = '0';
    } else {
        while (u > 0) {
            digits[dlen++] = (char)('0' + (u % 10));
            u /= 10;
        }
    }

    char prefix[4];
    int plen = 0;
    if (neg) prefix[plen++] = '-';
    else if (plus) prefix[plen++] = '+';
    else if (space) prefix[plen++] = ' ';

    int prec_pad = (prec > dlen) ? (prec - dlen) : 0;
    int zero_pad = 0;
    if (zero && !left && prec < 0) {
        zero_pad = width - plen - dlen - prec_pad;
        if (zero_pad < 0) zero_pad = 0;
    }
    int total = plen + prec_pad + zero_pad + dlen;
    int space_pad = width - total;
    if (space_pad < 0) space_pad = 0;

    if (!left) {
        for (int i = 0; i < space_pad; i++) outc(' ', buf, size, pos);
        outs(prefix, plen, buf, size, pos);
        for (int i = 0; i < prec_pad + zero_pad; i++) outc('0', buf, size, pos);
        for (int i = dlen - 1; i >= 0; i--) outc(digits[i], buf, size, pos);
    } else {
        outs(prefix, plen, buf, size, pos);
        for (int i = 0; i < prec_pad + zero_pad; i++) outc('0', buf, size, pos);
        for (int i = dlen - 1; i >= 0; i--) outc(digits[i], buf, size, pos);
        for (int i = 0; i < space_pad; i++) outc(' ', buf, size, pos);
    }
}

static void fmt_uint(char *buf, size_t size, int *pos,
                     uint64_t val, int base, int upper, int alt,
                     int width, int prec, int left, int zero)
{
    const char *table = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[32];
    int dlen = 0;
    if (val == 0) {
        if (prec != 0) digits[dlen++] = '0';
    } else {
        while (val > 0) {
            digits[dlen++] = table[val % base];
            val /= base;
        }
    }

    char prefix[4];
    int plen = 0;
    if (alt) {
        if (base == 8 && dlen > 0) {
            prefix[plen++] = '0';
        } else if (base == 16) {
            prefix[plen++] = '0';
            prefix[plen++] = upper ? 'X' : 'x';
        }
    }

    int prec_pad = (prec > dlen) ? (prec - dlen) : 0;
    int zero_pad = 0;
    if (zero && !left && prec < 0) {
        zero_pad = width - plen - dlen - prec_pad;
        if (zero_pad < 0) zero_pad = 0;
    }
    int total = plen + prec_pad + zero_pad + dlen;
    int space_pad = width - total;
    if (space_pad < 0) space_pad = 0;

    if (!left) {
        for (int i = 0; i < space_pad; i++) outc(' ', buf, size, pos);
        outs(prefix, plen, buf, size, pos);
        for (int i = 0; i < prec_pad + zero_pad; i++) outc('0', buf, size, pos);
        for (int i = dlen - 1; i >= 0; i--) outc(digits[i], buf, size, pos);
    } else {
        outs(prefix, plen, buf, size, pos);
        for (int i = 0; i < prec_pad + zero_pad; i++) outc('0', buf, size, pos);
        for (int i = dlen - 1; i >= 0; i--) outc(digits[i], buf, size, pos);
        for (int i = 0; i < space_pad; i++) outc(' ', buf, size, pos);
    }
}

static void fmt_str(char *buf, size_t size, int *pos,
                    const char *s, int width, int prec, int left)
{
    if (!s) s = "(null)";
    int len = 0;
    while (s[len] && (prec < 0 || len < prec)) len++;
    int pad = width - len;
    if (pad < 0) pad = 0;

    if (!left) {
        for (int i = 0; i < pad; i++) outc(' ', buf, size, pos);
        outs(s, len, buf, size, pos);
    } else {
        outs(s, len, buf, size, pos);
        for (int i = 0; i < pad; i++) outc(' ', buf, size, pos);
    }
}

static void fmt_char(char *buf, size_t size, int *pos,
                     char c, int width, int left)
{
    int pad = width - 1;
    if (pad < 0) pad = 0;
    if (!left) {
        for (int i = 0; i < pad; i++) outc(' ', buf, size, pos);
        outc(c, buf, size, pos);
    } else {
        outc(c, buf, size, pos);
        for (int i = 0; i < pad; i++) outc(' ', buf, size, pos);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    int pos = 0;
    if (size > 0) buf[0] = '\0';

    /* Work on a copy: where va_list is an array type (x86_64), the
     * function parameter has decayed to a pointer and &ap no longer has
     * type va_list *, which get_signed/get_unsigned require. */
    va_list aq;
    __builtin_va_copy(aq, ap);

    while (*fmt) {
        if (*fmt != '%') {
            outc(*fmt, buf, size, &pos);
            fmt++;
            continue;
        }
        fmt++;

        int left = 0, plus = 0, space = 0, zero = 0, alt = 0;
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' ||
               *fmt == '0' || *fmt == '#') {
            if (*fmt == '-') left = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '#') alt = 1;
            fmt++;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(aq, int);
            fmt++;
        } else if (is_digit(*fmt)) {
            width = parse_num(&fmt);
        }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                prec = va_arg(aq, int);
                fmt++;
            } else if (is_digit(*fmt)) {
                prec = parse_num(&fmt);
            } else {
                prec = 0;
            }
        }

        int len = 0;
        if (*fmt == 'h') {
            len = 5;
            fmt++;
            if (*fmt == 'h') { len = 6; fmt++; }
        } else if (*fmt == 'l') {
            len = 1;
            fmt++;
            if (*fmt == 'l') { len = 2; fmt++; }
        } else if (*fmt == 'z' || *fmt == 'Z') {
            len = 3;
            fmt++;
        } else if (*fmt == 't' || *fmt == 'T') {
            len = 4;
            fmt++;
        } else if (*fmt == 'j') {
            len = 3;
            fmt++;
        }

        char spec = *fmt;
        if (spec == '\0') break;
        fmt++;

        switch (spec) {
        case 'd':
        case 'i': {
            int64_t v = get_signed(&aq, len);
            fmt_int(buf, size, &pos, v, width, prec, left, zero, plus, space);
            break;
        }
        case 'u': {
            uint64_t v = get_unsigned(&aq, len);
            fmt_uint(buf, size, &pos, v, 10, 0, 0, width, prec, left, zero);
            break;
        }
        case 'o': {
            uint64_t v = get_unsigned(&aq, len);
            fmt_uint(buf, size, &pos, v, 8, 0, alt, width, prec, left, zero);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t v = get_unsigned(&aq, len);
            fmt_uint(buf, size, &pos, v, 16, spec == 'X', alt,
                     width, prec, left, zero);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(aq, void *);
            fmt_uint(buf, size, &pos, v, 16, 0, 1, width, prec, left, zero);
            break;
        }
        case 's': {
            const char *s = va_arg(aq, const char *);
            fmt_str(buf, size, &pos, s, width, prec, left);
            break;
        }
        case 'c': {
            char c = (char)va_arg(aq, int);
            fmt_char(buf, size, &pos, c, width, left);
            break;
        }
        case '%': {
            outc('%', buf, size, &pos);
            break;
        }
        default: {
            outc('%', buf, size, &pos);
            outc(spec, buf, size, &pos);
            break;
        }
        }
    }

    if (size > 0) {
        if ((size_t)pos < size) buf[pos] = '\0';
        else buf[size - 1] = '\0';
    }
    __builtin_va_end(aq);
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
    if (r > 0) {
        size_t n = (size_t)r < sizeof(buf) ? (size_t)r : sizeof(buf) - 1;
        fwrite(buf, 1, n, stdout);
    }
    return r;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (r > 0) {
        size_t n = (size_t)r < sizeof(buf) ? (size_t)r : sizeof(buf) - 1;
        fwrite(buf, 1, n, f);
    }
    return r;
}
