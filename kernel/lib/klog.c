#include "klog.h"
#include "uart.h"
#include "stdio.h"
#include "string.h"
#include "types.h"
#include "defs.h"

int klog_level = KLOG_DEBUG;

static char   klog_buf[KLOG_BUF_SIZE];
static size_t klog_head __attribute__((section(".data"))) = 0;
static size_t klog_tail __attribute__((section(".data"))) = 0;
static size_t klog_used __attribute__((section(".data"))) = 0;

void klog_init(void) {
    memset(klog_buf, 0, sizeof(klog_buf));
    klog_head = 0;
    klog_tail = 0;
    klog_used = 0;
}

/* Append a string to the ring buffer */
static void klog_append(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        klog_buf[klog_head] = s[i];
        klog_head = (klog_head + 1) % KLOG_BUF_SIZE;
        if (klog_used < KLOG_BUF_SIZE) {
            klog_used++;
        } else {
            klog_tail = (klog_tail + 1) % KLOG_BUF_SIZE;
        }
    }
}

void klog_write(const char *fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    klog_append(msg, strlen(msg));
    uart_puts(msg);
}

int klog_read(char *buf, size_t size, size_t *pos) {
    if (!pos || *pos >= klog_used) return 0;
    size_t avail = klog_used - *pos;
    size_t to_read = avail < size ? avail : size;

    for (size_t i = 0; i < to_read; i++) {
        size_t idx = (klog_tail + *pos + i) % KLOG_BUF_SIZE;
        buf[i] = klog_buf[idx];
    }
    *pos += to_read;
    return (int)to_read;
}

void klog_clear(void) {
    klog_head = 0;
    klog_tail = 0;
    klog_used = 0;
}

size_t klog_len(void) {
    return klog_used;
}
