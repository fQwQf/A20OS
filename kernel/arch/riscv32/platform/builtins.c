#ifdef CONFIG_RISCV32

#include "core/types.h"

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem) {
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

uint64_t __udivdi3(uint64_t n, uint64_t d) {
    return udivmod64(n, d, 0);
}

uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t r = 0;
    udivmod64(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d) {
    int neg = 0;
    uint64_t un;
    uint64_t ud;
    if (n < 0) {
        neg ^= 1;
        un = (uint64_t)(-(n + 1)) + 1ULL;
    } else {
        un = (uint64_t)n;
    }
    if (d < 0) {
        neg ^= 1;
        ud = (uint64_t)(-(d + 1)) + 1ULL;
    } else {
        ud = (uint64_t)d;
    }
    uint64_t q = udivmod64(un, ud, 0);
    if (!neg)
        return (int64_t)q;
    return -(int64_t)q;
}

#endif /* CONFIG_RISCV32 */
