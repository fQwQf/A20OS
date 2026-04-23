#ifndef _DEFS_H
#define _DEFS_H

#include "types.h"
#include "consts.h"
#include "arch.h"

#define ALWAYS_INLINE __attribute__((always_inline)) inline static
#define NORETURN __attribute__((noreturn))
#define PACKED __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ROUND_UP(a, b) (((a) + (b) - 1) & ~((b) - 1))
#define ROUND_DOWN(a, b) ((a) & ~((b) - 1))
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define ALIGN_UP(x, a) ROUND_UP(x, a)
#define ALIGN_DOWN(x, a) ROUND_DOWN(x, a)

#define BIT(n) (1UL << (n))
#define BITS(h, l) (((1UL << ((h) - (l) + 1)) - 1) << (l))

#define mb()  arch_mb()
#define rmb() arch_rmb()
#define wmb() arch_wmb()

#endif /* _DEFS_H */
