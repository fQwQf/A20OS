#ifdef CONFIG_MCU_SHARED_KERNEL
#include "shared_kernel.h"

/* GCC's intrinsic CAS takes weak, but its size-specific runtime libcall ABI
 * omits that argument. Bind a separate C name to the ABI symbol to avoid the
 * intrinsic declaration, and implement strong CAS for both strong/weak calls. */

#define DEFINE_ATOMICS(N, T)                                                   \
    _Bool a20_compare_exchange_##N(                                            \
        volatile void *, void *, T, int,                                       \
        int) __asm__("__atomic_compare_exchange_" #N);                         \
                                                                               \
    T __atomic_load_##N(const volatile void *ptr, int order) {                 \
        (void)order;                                                           \
        uint32_t flags = mcu_fabric_lock(MCU_FABRIC_ATOMIC_LOCK);              \
        T value = *(const volatile T *)ptr;                                    \
        mcu_fabric_unlock(MCU_FABRIC_ATOMIC_LOCK, flags);                      \
        return value;                                                          \
    }                                                                          \
    void __atomic_store_##N(volatile void *ptr, T value, int order) {          \
        (void)order;                                                           \
        uint32_t flags = mcu_fabric_lock(MCU_FABRIC_ATOMIC_LOCK);              \
        *(volatile T *)ptr = value;                                            \
        mcu_fabric_unlock(MCU_FABRIC_ATOMIC_LOCK, flags);                      \
    }                                                                          \
    T __atomic_exchange_##N(volatile void *ptr, T value, int order) {          \
        (void)order;                                                           \
        uint32_t flags = mcu_fabric_lock(MCU_FABRIC_ATOMIC_LOCK);              \
        T old = *(volatile T *)ptr;                                            \
        *(volatile T *)ptr = value;                                            \
        mcu_fabric_unlock(MCU_FABRIC_ATOMIC_LOCK, flags);                      \
        return old;                                                            \
    }                                                                          \
    _Bool a20_compare_exchange_##N(volatile void *ptr, void *expected,         \
                                   T desired, int success, int failure) {      \
        (void)success;                                                         \
        (void)failure;                                                         \
        uint32_t flags = mcu_fabric_lock(MCU_FABRIC_ATOMIC_LOCK);              \
        T old = *(volatile T *)ptr;                                            \
        bool equal = old == *(T *)expected;                                    \
        if (equal)                                                             \
            *(volatile T *)ptr = desired;                                      \
        else                                                                   \
            *(T *)expected = old;                                              \
        mcu_fabric_unlock(MCU_FABRIC_ATOMIC_LOCK, flags);                      \
        return equal;                                                          \
    }

#define DEFINE_FETCH(N, T, NAME, OP)                                           \
    T __atomic_fetch_##NAME##_##N(volatile void *ptr, T value, int order) {    \
        (void)order;                                                           \
        uint32_t flags = mcu_fabric_lock(MCU_FABRIC_ATOMIC_LOCK);              \
        T old = *(volatile T *)ptr;                                            \
        *(volatile T *)ptr = (T)(old OP value);                                \
        mcu_fabric_unlock(MCU_FABRIC_ATOMIC_LOCK, flags);                      \
        return old;                                                            \
    }                                                                          \
    T __atomic_##NAME##_fetch_##N(volatile void *ptr, T value, int order) {    \
        (void)order;                                                           \
        uint32_t flags = mcu_fabric_lock(MCU_FABRIC_ATOMIC_LOCK);              \
        T result = (T)(*(volatile T *)ptr OP value);                           \
        *(volatile T *)ptr = result;                                           \
        mcu_fabric_unlock(MCU_FABRIC_ATOMIC_LOCK, flags);                      \
        return result;                                                         \
    }
#define DEFINE_WIDTH(N, T)                                                     \
    DEFINE_ATOMICS(N, T)                                                       \
    DEFINE_FETCH(N, T, add, +)                                                 \
    DEFINE_FETCH(N, T, sub, -)                                                 \
    DEFINE_FETCH(N, T, or, |)                                                  \
    DEFINE_FETCH(N, T, and, &)                                                 \
    DEFINE_FETCH(N, T, xor, ^)
DEFINE_WIDTH(1, uint8_t)
DEFINE_WIDTH(2, uint16_t)
DEFINE_WIDTH(4, unsigned int)
DEFINE_WIDTH(8, uint64_t)
#endif
