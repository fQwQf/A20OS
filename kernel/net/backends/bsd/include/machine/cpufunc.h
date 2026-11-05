#if !defined(A20_BSD_PORTABLE_LP64)
#include_next <machine/cpufunc.h>
#else
#ifndef A20_BSD_CPUFUNC_H
#define A20_BSD_CPUFUNC_H

/* Only pure integer primitives are needed by the selected network sources.
 * Interrupt control and machine context switching belong to A20OS. */
#define HAVE_INLINE_FFS
#define HAVE_INLINE_FFSL
#define HAVE_INLINE_FFSLL
#define HAVE_INLINE_FLS
#define HAVE_INLINE_FLSL
#define HAVE_INLINE_FLSLL
static inline int ffs(int v) { return __builtin_ffs(v); }
static inline int ffsl(long v) { return __builtin_ffsl(v); }
static inline int ffsll(long long v) { return __builtin_ffsll(v); }
static inline int fls(int v) { return v ? 32 - __builtin_clz((unsigned)v) : 0; }
static inline int flsl(long v)
{ return v ? sizeof(v) * 8 - __builtin_clzl((unsigned long)v) : 0; }
static inline int flsll(long long v)
{ return v ? 64 - __builtin_clzll((unsigned long long)v) : 0; }
static inline void breakpoint(void) { __builtin_trap(); }

#endif
#endif
