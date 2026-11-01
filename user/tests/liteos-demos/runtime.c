/*
 * The tiny freestanding runtime the demos link against.
 *
 * It is deliberately smaller than liba20c: the demos need the compiler's
 * string helpers, a bounded log ring and a few checked assertions.  Putting
 * console output in the shared run record keeps the user program free of any
 * non-CMSIS API while still letting the kernel show what happened.
 */
#include "a20_mcu_demo.h"
#include "a20_string.h"
#include "a20_sync.h"
#include "a20_task.h"

/* The compiler may emit calls to these names even in freestanding code. */
void *memcpy(void *dst, const void *src, size_t n)
{
    return a20_memcpy(dst, src, n);
}

void *memset(void *dst, int c, size_t n)
{
    return a20_memset(dst, c, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
    return a20_memmove(dst, src, n);
}

int memcmp(const void *a, const void *b, size_t n)
{
    return a20_memcmp(a, b, n);
}

size_t strlen(const char *s)
{
    return a20_strlen(s);
}

static a20_mcu_demo_record_t *g_record;
static uint32_t g_log_lock;

/* Two worker threads log concurrently; a bare head++ loses lines and lets
 * both writers pick the same slot.  The spinlock uses the Native atomic
 * syscall because RV32IMC has no A extension. */
static void log_lock(void)
{
    for (;;) {
        uint32_t swapped = 0;
        if (a20_atomic32(&g_log_lock, A20_ATOMIC32_COMPARE_EXCHANGE, 0, 1, 0,
                         &swapped) == A20_OK && swapped)
            return;
        (void)a20_thread_yield();
    }
}

static void log_unlock(void)
{
    (void)a20_atomic32_store(&g_log_lock, 0);
}

void demo_runtime_init(a20_mcu_demo_record_t *record)
{
    g_record = record;
}

static char *append_uint(char *out, char *end, unsigned long value,
                         unsigned base)
{
    char digits[16];
    unsigned n = 0;
    do {
        unsigned d = (unsigned)(value % base);
        digits[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= base;
    } while (value && n < sizeof(digits));
    while (n && out < end)
        *out++ = digits[--n];
    return out;
}

void demo_log(const char *fmt, ...)
{
    if (!g_record || g_record->head - g_record->tail >= A20_MCU_DEMO_LINES)
        return;
    log_lock();
    a20_mcu_demo_record_t *r = g_record;
    if (r->head - r->tail >= A20_MCU_DEMO_LINES) {
        log_unlock();
        return;
    }
    char *line = r->lines[r->head % A20_MCU_DEMO_LINES];
    char *out = line;
    char *end = line + A20_MCU_DEMO_LINE - 1;

    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char *p = fmt; *p && out < end; p++) {
        if (*p != '%') {
            *out++ = *p;
            continue;
        }
        p++;
        if (*p == '%') {
            *out++ = '%';
        } else if (*p == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            while (s && *s && out < end)
                *out++ = *s++;
        } else if (*p == 'c') {
            *out++ = (char)__builtin_va_arg(ap, int);
        } else if (*p == 'u' || *p == 'd' || *p == 'x') {
            unsigned long v = __builtin_va_arg(ap, unsigned long);
            if (*p == 'x') {
                *out++ = '0';
                *out++ = 'x';
                out = append_uint(out, end, v, 16);
            } else {
                if (*p == 'd' && (long)v < 0) {
                    if (out < end)
                        *out++ = '-';
                    v = (unsigned long)(-(long)v);
                }
                out = append_uint(out, end, v, 10);
            }
        }
    }
    __builtin_va_end(ap);
    *out = '\0';
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->head = r->head + 1;
    log_unlock();
}

int demo_check(int condition, const char *what)
{
    if (!g_record)
        return condition;
    g_record->checks = g_record->checks + 1;
    if (!condition)
        demo_log("  CHECK FAILED: %s", what);
    return condition;
}

void demo_finish(int status)
{
    if (!g_record)
        return;
    g_record->status = status;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_record->done = 1;
}

/*
 * 64-bit division helpers.
 *
 * RV32IMC has no 64-bit divide instruction, and the demo image is linked
 * with -nostdlib on purpose: pulling in a compiler runtime would be another
 * toolchain dependency for a 10 KiB test image.  Shift-subtract is enough
 * here (the CMSIS core only divides nanosecond deadlines by the tick period).
 */
static unsigned long long udivmod64(unsigned long long n,
                                    unsigned long long d,
                                    unsigned long long *rem)
{
    unsigned long long q = 0, r = 0;
    if (!d) {
        if (rem)
            *rem = 0;
        return 0;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) {
            r -= d;
            q |= 1ULL << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

unsigned long long __udivdi3(unsigned long long a, unsigned long long b)
{
    return udivmod64(a, b, 0);
}

unsigned long long __umoddi3(unsigned long long a, unsigned long long b)
{
    unsigned long long r;
    (void)udivmod64(a, b, &r);
    return r;
}

long long __divdi3(long long a, long long b)
{
    int neg = (a < 0) != (b < 0);
    unsigned long long ua = a < 0 ? (unsigned long long)(-a)
                                  : (unsigned long long)a;
    unsigned long long ub = b < 0 ? (unsigned long long)(-b)
                                  : (unsigned long long)b;
    unsigned long long q = udivmod64(ua, ub, 0);
    return neg ? -(long long)q : (long long)q;
}

long long __moddi3(long long a, long long b)
{
    int neg = a < 0;
    unsigned long long ua = a < 0 ? (unsigned long long)(-a)
                                  : (unsigned long long)a;
    unsigned long long ub = b < 0 ? (unsigned long long)(-b)
                                  : (unsigned long long)b;
    unsigned long long r;
    (void)udivmod64(ua, ub, &r);
    return neg ? -(long long)r : (long long)r;
}
