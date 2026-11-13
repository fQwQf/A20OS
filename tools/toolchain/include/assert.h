/* Freestanding assertions for building compiler-rt without a host libc. */
#ifndef A20_TOOLCHAIN_ASSERT_H
#define A20_TOOLCHAIN_ASSERT_H
#define static_assert _Static_assert
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression) ((expression) ? (void)0 : __builtin_trap())
#endif
#endif
