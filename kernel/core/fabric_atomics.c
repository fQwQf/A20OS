#ifdef CONFIG_SHARED_FABRIC_ATOMICS
#include "core/shared_fabric.h"

static uintptr_t atomic_begin(void) {
    uintptr_t token;
    if (a20_fabric_gate_lock(arch_shared_fabric(), A20_FABRIC_ATOMIC_GATE,
                             &token))
        arch_shared_fabric_stop();
    return token;
}
static void atomic_end(uintptr_t token) {
    if (a20_fabric_gate_unlock(arch_shared_fabric(), A20_FABRIC_ATOMIC_GATE,
                               token))
        arch_shared_fabric_stop();
}

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
        uintptr_t flags = atomic_begin();                                      \
        T value = *(const volatile T *)ptr;                                    \
        atomic_end(flags);                                                     \
        return value;                                                          \
    }                                                                          \
    void __atomic_store_##N(volatile void *ptr, T value, int order) {          \
        (void)order;                                                           \
        uintptr_t flags = atomic_begin();                                      \
        *(volatile T *)ptr = value;                                            \
        atomic_end(flags);                                                     \
    }                                                                          \
    T __atomic_exchange_##N(volatile void *ptr, T value, int order) {          \
        (void)order;                                                           \
        uintptr_t flags = atomic_begin();                                      \
        T old = *(volatile T *)ptr;                                            \
        *(volatile T *)ptr = value;                                            \
        atomic_end(flags);                                                     \
        return old;                                                            \
    }                                                                          \
    _Bool a20_compare_exchange_##N(volatile void *ptr, void *expected,         \
                                   T desired, int success, int failure) {      \
        (void)success;                                                         \
        (void)failure;                                                         \
        uintptr_t flags = atomic_begin();                                      \
        T old = *(volatile T *)ptr;                                            \
        bool equal = old == *(T *)expected;                                    \
        if (equal)                                                             \
            *(volatile T *)ptr = desired;                                      \
        else                                                                   \
            *(T *)expected = old;                                              \
        atomic_end(flags);                                                     \
        return equal;                                                          \
    }

#define DEFINE_FETCH(N, T, NAME, OP)                                           \
    T __atomic_fetch_##NAME##_##N(volatile void *ptr, T value, int order) {    \
        (void)order;                                                           \
        uintptr_t flags = atomic_begin();                                      \
        T old = *(volatile T *)ptr;                                            \
        *(volatile T *)ptr = (T)(old OP value);                                \
        atomic_end(flags);                                                     \
        return old;                                                            \
    }                                                                          \
    T __atomic_##NAME##_fetch_##N(volatile void *ptr, T value, int order) {    \
        (void)order;                                                           \
        uintptr_t flags = atomic_begin();                                      \
        T result = (T)(*(volatile T *)ptr OP value);                           \
        *(volatile T *)ptr = result;                                           \
        atomic_end(flags);                                                     \
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
