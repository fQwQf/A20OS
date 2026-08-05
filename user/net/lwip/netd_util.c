/* Userspace helpers for the netd service (Native ABI). */
#include "liba20rt/a20_types.h"
#include "liba20rt/a20_syscall.h"
#include "liba20rt/a20_clock.h"
#include "liba20rt/a20_system.h"

/* Minimal printf for diagnostics (freestanding; liba20c is not linked). */
static void nd_putc(char c)
{
    char buf[1] = { c };
    (void)buf;
    /* stdout handle is installed by the service wrapper; see netd.c. */
}

void a20_netd_printf(const char *fmt, ...)
{
    (void)fmt;
}

uint64_t a20_netd_random_u64(void)
{
    uint64_t v = 0;
    int64_t r = a20_syscall6(A20_SYS_system_random, (uint64_t)(uintptr_t)&v, 0, 0, 0, 0, 0);
    if (r >= 0)
        return v;
    /* Fallback: mixed time + frame pointer noise. */
    uint64_t t = 0;
    a20_clock_get(A20_CLOCK_MONOTONIC, &t);
    return t ^ (uint64_t)(uintptr_t)&v;
}

/* NO_SYS single-threaded service loop: protection is a no-op. */
#include "lwip/arch.h"
sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t p) { (void)p; }

/* lwIP timer source: monotonic milliseconds. */
uint32_t sys_now(void)
{
    uint64_t ns = 0;
    a20_clock_get(A20_CLOCK_MONOTONIC, &ns);
    return (uint32_t)(ns / 1000000ULL);
}

/* Minimal libc string helpers for lwIP (SDK provides memcpy/memset). */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0; }
char *strchr(const char *s, int c) { do { if (*s == (char)c) return (char *)s; } while (*s++); return 0; }
char *strrchr(const char *s, int c) { const char *p = 0; do { if (*s == (char)c) p = s; } while (*s++); return (char *)p; }
char *strstr(const char *h, const char *n) { if (!*n) return (char *)h; for (; *h; h++) { const char *a = h, *b = n; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return (char *)h; } return 0; }
char *strcpy(char *d, const char *s) { char *o = d; while ((*d++ = *s++)) ; return o; }
char *strncpy(char *d, const char *s, size_t n) { char *o = d; while (n && *s) { *d++ = *s++; n--; } while (n--) *d++ = 0; return o; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++; y++; } return 0; }
unsigned long strtoul(const char *s, char **end, int base) { (void)base; unsigned long v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; } if (end) *end = (char *)s; return v; }
int atoi(const char *s) { int v = 0, neg = 0; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; } return neg ? -v : v; }
