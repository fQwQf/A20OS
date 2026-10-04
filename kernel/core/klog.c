#include "core/klog.h"
#include "drivers/char/uart.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/types.h"
#include "core/defs.h"
#include "core/lock.h"

int klog_level = KLOG_INFO;

static char   klog_buf[KLOG_BUF_SIZE];
static size_t klog_head __attribute__((section(".data"))) = 0;
static size_t klog_tail __attribute__((section(".data"))) = 0;
static size_t klog_used __attribute__((section(".data"))) = 0;
static spinlock_t klog_lock = SPINLOCK_INIT;
/* One console writer at a time.  Each character costs a polled MMIO write,
 * so without this two CPUs emitting concurrently splice their lines
 * mid-word and the serial log stops being parseable.  Held with interrupts
 * disabled: the ring append and the console emission must appear together or
 * a reader cannot tell which bytes belong to which record.  Shared with
 * vprintf() in kernel/core/printf.c. */
spinlock_t klog_console_lock = SPINLOCK_INIT;

/* Defined in kernel/core/printf.c, which owns the other console entry point;
 * declared here rather than in a shared header so both console writers stay
 * serialized by one lock. */
void console_emit_locked(const char *s, size_t len);

void klog_init(void) {
    memset(klog_buf, 0, sizeof(klog_buf));
    klog_head = 0;
    klog_tail = 0;
    klog_used = 0;
    spin_init(&klog_lock);
    spin_init(&klog_console_lock);
}

static void klog_append(const char *s, size_t len) {
    uint64_t flags = spin_lock_irqsave(&klog_lock);
    for (size_t i = 0; i < len; i++) {
        klog_buf[klog_head] = s[i];
        klog_head = (klog_head + 1) % KLOG_BUF_SIZE;
        if (klog_used < KLOG_BUF_SIZE) {
            klog_used++;
        } else {
            klog_tail = (klog_tail + 1) % KLOG_BUF_SIZE;
        }
    }
    spin_unlock_irqrestore(&klog_lock, flags);
}

void klog_write(const char *fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    /* vsnprintf already reports how many bytes it produced, so the message
     * never has to be walked a second time to find its length. */
    int len = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (len < 0)
        len = 0;
    if ((size_t)len >= sizeof(msg))
        len = (int)sizeof(msg) - 1;

    uint64_t flags = spin_lock_irqsave(&klog_console_lock);
    klog_append(msg, (size_t)len);
    console_emit_locked(msg, (size_t)len);
    spin_unlock_irqrestore(&klog_console_lock, flags);
}

/* Raw write used by /dev/kmsg: appends bytes verbatim to the ring. */
void klog_write_raw(const char *buf, size_t len) {
    uint64_t flags = spin_lock_irqsave(&klog_console_lock);
    klog_append(buf, len);
    for (size_t i = 0; i < len; i++)
        uart_putc(buf[i]);
    spin_unlock_irqrestore(&klog_console_lock, flags);
}

int klog_read(char *buf, size_t size, size_t *pos) {
    uint64_t flags = spin_lock_irqsave(&klog_lock);
    if (!pos || *pos >= klog_used) {
        spin_unlock_irqrestore(&klog_lock, flags);
        return 0;
    }
    size_t avail = klog_used - *pos;
    size_t to_read = avail < size ? avail : size;

    for (size_t i = 0; i < to_read; i++) {
        size_t idx = (klog_tail + *pos + i) % KLOG_BUF_SIZE;
        buf[i] = klog_buf[idx];
    }
    *pos += to_read;
    spin_unlock_irqrestore(&klog_lock, flags);
    return (int)to_read;
}

void klog_clear(void) {
    uint64_t flags = spin_lock_irqsave(&klog_lock);
    klog_head = 0;
    klog_tail = 0;
    klog_used = 0;
    spin_unlock_irqrestore(&klog_lock, flags);
}

size_t klog_len(void) {
    uint64_t flags = spin_lock_irqsave(&klog_lock);
    size_t used = klog_used;
    spin_unlock_irqrestore(&klog_lock, flags);
    return used;
}
