/* Compiler atomics for the private network personality. No FreeBSD CPU
 * feature globals or userland libatomic calls are part of this ABI. */
#if defined(__amd64__)
#include_next <machine/atomic.h>
#else
#ifndef _MACHINE_ATOMIC_H_
#define _MACHINE_ATOMIC_H_
#include <sys/types.h>

#define A20_ATOMIC_RMW(name, type, suffix, order)                         \
static inline void atomic_add_##suffix##name(volatile type *p, type v)   \
{ (void)__atomic_fetch_add(p, v, order); }                               \
static inline void atomic_subtract_##suffix##name(volatile type *p, type v) \
{ (void)__atomic_fetch_sub(p, v, order); }                               \
static inline void atomic_set_##suffix##name(volatile type *p, type v)   \
{ (void)__atomic_fetch_or(p, v, order); }                                \
static inline void atomic_clear_##suffix##name(volatile type *p, type v) \
{ (void)__atomic_fetch_and(p, (type)~v, order); }                         \
static inline int atomic_cmpset_##suffix##name(volatile type *p, type old, type v) \
{ return __atomic_compare_exchange_n(p, &old, v, 0, order, __ATOMIC_RELAXED); } \
static inline int atomic_fcmpset_##suffix##name(volatile type *p, type *old, type v) \
{ return __atomic_compare_exchange_n(p, old, v, 1, order, __ATOMIC_RELAXED); }

#define A20_ATOMIC(name, type)                                            \
A20_ATOMIC_RMW(name, type, , __ATOMIC_SEQ_CST)                             \
A20_ATOMIC_RMW(name, type, acq_, __ATOMIC_ACQUIRE)                         \
A20_ATOMIC_RMW(name, type, rel_, __ATOMIC_RELEASE)                         \
static inline type atomic_load_acq_##name(const volatile type *p)        \
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }                          \
static inline void atomic_store_rel_##name(volatile type *p, type v)     \
{ __atomic_store_n(p, v, __ATOMIC_RELEASE); }                             \
static inline type atomic_fetchadd_##name(volatile type *p, type v)      \
{ return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }                    \
static inline type atomic_swap_##name(volatile type *p, type v)          \
{ return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }                   \
static inline type atomic_readandclear_##name(volatile type *p)          \
{ return __atomic_exchange_n(p, 0, __ATOMIC_SEQ_CST); }                   \
static inline int atomic_testandset_##name(volatile type *p, unsigned bit) \
{ type mask = (type)1 << (bit % (sizeof(type) * 8));                      \
  return (__atomic_fetch_or(p, mask, __ATOMIC_SEQ_CST) & mask) != 0; }    \
static inline int atomic_testandclear_##name(volatile type *p, unsigned bit) \
{ type mask = (type)1 << (bit % (sizeof(type) * 8));                      \
  return (__atomic_fetch_and(p, (type)~mask, __ATOMIC_SEQ_CST) & mask) != 0; }

A20_ATOMIC(8, uint8_t)
A20_ATOMIC(16, uint16_t)
A20_ATOMIC(32, uint32_t)
A20_ATOMIC(64, uint64_t)
A20_ATOMIC(char, unsigned char)
A20_ATOMIC(short, unsigned short)
A20_ATOMIC(int, unsigned int)
A20_ATOMIC(long, unsigned long)
A20_ATOMIC(ptr, uintptr_t)
#undef A20_ATOMIC
#undef A20_ATOMIC_RMW


/* atomic_subword.h uses these capability macros to select native operations. */
#define atomic_load_acq_8 atomic_load_acq_8
#define atomic_load_acq_16 atomic_load_acq_16
#define atomic_fcmpset_8 atomic_fcmpset_8
#define atomic_fcmpset_16 atomic_fcmpset_16
#define mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
static inline void atomic_thread_fence_acq(void) { rmb(); }
static inline void atomic_thread_fence_rel(void) { wmb(); }
static inline void atomic_thread_fence_acq_rel(void)
{ __atomic_thread_fence(__ATOMIC_ACQ_REL); }
static inline void atomic_thread_fence_seq_cst(void) { mb(); }
#include <sys/atomic_common.h>
#endif
#endif
