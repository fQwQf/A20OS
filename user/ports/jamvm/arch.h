/* A20OS JamVM architecture adapter, GPL-2.0-or-later (JamVM linkage).
 * The interpreter uses C atomics and libffi rather than Linux assembly stubs.
 * RV64/AArch64 use the same VM atomics; this is not MCU atomic admission.
 */
#ifndef A20_JAMVM_ARCH_H
#define A20_JAMVM_ARCH_H
#include <stdint.h>
#include <float.h>
_Static_assert(sizeof(void *) == 8,
               "This Native JamVM port requires 64-bit pointers");
_Static_assert(sizeof(float) == 4 && sizeof(double) == 8 && FLT_RADIX == 2 &&
                   DBL_MANT_DIG == 53,
               "Java IEEE floating point required");
#if defined(__riscv) && __riscv_xlen == 64
#define OS_ARCH "riscv64"
#elif defined(__aarch64__)
#define OS_ARCH "aarch64"
#else
#error "This full-libc JamVM port currently supports RV64/AArch64"
#endif
#define FFI_RET_EXTEND
#define HANDLER_TABLE_T static const void
#define DOUBLE_1_BITS 0x3ff0000000000000LL
#define READ_DBL(v, p, l)                                                      \
    do {                                                                       \
        (v) = ((u8)(p)[0] << 56) | ((u8)(p)[1] << 48) | ((u8)(p)[2] << 40) |   \
              ((u8)(p)[3] << 32) | ((u8)(p)[4] << 24) | ((u8)(p)[5] << 16) |   \
              ((u8)(p)[6] << 8) | (u8)(p)[7];                                  \
        (p) += 8;                                                              \
    } while (0)
#define FPU_HACK
#define A20_JAMVM_CAS(addr, old_val, new_val)                                  \
    ({                                                                         \
        __typeof__(*(addr)) expected_ = (__typeof__(*(addr)))(old_val);        \
        __atomic_compare_exchange_n((addr), &expected_,                        \
                                    (__typeof__(*(addr)))(new_val), 0,         \
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);       \
    })
#define COMPARE_AND_SWAP A20_JAMVM_CAS
#define COMPARE_AND_SWAP_32 A20_JAMVM_CAS
#define COMPARE_AND_SWAP_64 A20_JAMVM_CAS
#define LOCKWORD_COMPARE_AND_SWAP A20_JAMVM_CAS
#define LOCKWORD_READ(addr) __atomic_load_n((addr), __ATOMIC_ACQUIRE)
#define LOCKWORD_WRITE(addr, value)                                            \
    __atomic_store_n((addr), (value), __ATOMIC_RELEASE)
#define MBARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define JMM_LOCK_MBARRIER() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define JMM_UNLOCK_MBARRIER() __atomic_thread_fence(__ATOMIC_RELEASE)
#endif
