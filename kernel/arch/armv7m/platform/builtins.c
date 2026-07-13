#ifdef CONFIG_ARMV7M

#include "core/types.h"

static uint64_t armv7m_udivmod64(uint64_t n, uint64_t d, uint64_t *rem) {
    if (d == 0) {
        if (rem)
            *rem = 0;
        return 0;
    }
    uint64_t q = 0;
    uint64_t r = 0;
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

uint64_t __a20_armv7m_udivmod64(uint64_t n, uint64_t d, uint64_t *rem) {
    return armv7m_udivmod64(n, d, rem);
}

int64_t __a20_armv7m_sdivmod64(int64_t n, int64_t d, int64_t *rem) {
    int neg_q = (n < 0) ^ (d < 0);
    int neg_r = n < 0;
    uint64_t un = n < 0 ? (uint64_t)(-(n + 1)) + 1ULL : (uint64_t)n;
    uint64_t ud = d < 0 ? (uint64_t)(-(d + 1)) + 1ULL : (uint64_t)d;
    uint64_t ur = 0;
    uint64_t uq = armv7m_udivmod64(un, ud, &ur);
    if (rem)
        *rem = neg_r ? -(int64_t)ur : (int64_t)ur;
    return neg_q ? -(int64_t)uq : (int64_t)uq;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) {
    return armv7m_udivmod64(n, d, 0);
}

uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t rem;
    armv7m_udivmod64(n, d, &rem);
    return rem;
}

int64_t __divdi3(int64_t n, int64_t d) {
    return __a20_armv7m_sdivmod64(n, d, 0);
}

int64_t __moddi3(int64_t n, int64_t d) {
    int64_t rem;
    __a20_armv7m_sdivmod64(n, d, &rem);
    return rem;
}

void __aeabi_memcpy(void *dst, const void *src, size_t n);
void __aeabi_memcpy4(void *dst, const void *src, size_t n);
void __aeabi_memcpy8(void *dst, const void *src, size_t n);
void __aeabi_memset(void *dst, size_t n, int c);
void __aeabi_memclr(void *dst, size_t n);
void __aeabi_memclr4(void *dst, size_t n);
void __aeabi_memclr8(void *dst, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

void __aeabi_memcpy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy4(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memcpy8(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void __aeabi_memset(void *dst, size_t n, int c) { memset(dst, c, n); }
void __aeabi_memclr(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr4(void *dst, size_t n) { memset(dst, 0, n); }
void __aeabi_memclr8(void *dst, size_t n) { memset(dst, 0, n); }

int raise(int sig) {
    (void)sig;
    return 0;
}

#endif
