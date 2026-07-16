/*
 * Log-line formatter. Pure string logic — see hub_log.h. Host-testable.
 * Freestanding: manual integer formatting (no snprintf dependency).
 */
#ifdef CONFIG_BOARD_STM32F103

#include "hub_log.h"

/* Append a zero-padded decimal (width digits) to buf at *pos, if it fits. */
static int put_padded(char *buf, unsigned cap, unsigned *pos, uint32_t v,
                      unsigned width) {
    char tmp[10];
    unsigned n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && n < sizeof(tmp));
    while (n < width && n < sizeof(tmp))
        tmp[n++] = '0';
    if (*pos + n >= cap)
        return -1;
    while (n > 0)
        buf[(*pos)++] = tmp[--n];
    return 0;
}

static int put_str(char *buf, unsigned cap, unsigned *pos, const char *s) {
    while (*s) {
        if (*pos + 1u >= cap)
            return -1;
        buf[(*pos)++] = *s++;
    }
    return 0;
}

static int put_ch(char *buf, unsigned cap, unsigned *pos, char c) {
    if (*pos + 1u >= cap)
        return -1;
    buf[(*pos)++] = c;
    return 0;
}

int hub_log_format(char *buf, unsigned cap, uint32_t ts_ms, const char *tag,
                   const char *msg) {
    if (!buf || !tag || !msg || cap == 0)
        return -1;

    uint32_t ms = ts_ms % 1000u;
    uint32_t total_s = ts_ms / 1000u;
    uint32_t s = total_s % 60u;
    uint32_t m = (total_s / 60u) % 60u;
    uint32_t h = total_s / 3600u;

    unsigned pos = 0;
    if (put_padded(buf, cap, &pos, h, 3) || put_ch(buf, cap, &pos, ':') ||
        put_padded(buf, cap, &pos, m, 2) || put_ch(buf, cap, &pos, ':') ||
        put_padded(buf, cap, &pos, s, 2) || put_ch(buf, cap, &pos, '.') ||
        put_padded(buf, cap, &pos, ms, 3) || put_str(buf, cap, &pos, " [") ||
        put_str(buf, cap, &pos, tag) || put_str(buf, cap, &pos, "] ") ||
        put_str(buf, cap, &pos, msg) || put_ch(buf, cap, &pos, '\n'))
        return -1;

    buf[pos] = '\0';
    return (int)pos;
}

#endif /* CONFIG_BOARD_STM32F103 */
