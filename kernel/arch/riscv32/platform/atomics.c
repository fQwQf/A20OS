#ifdef CONFIG_RISCV32

#include "core/types.h"
#include "cpu.h"
static volatile uint32_t atomic64_lock;
static uint32_t atomic64_acquire(void)
{
    uint32_t flags = arch_irq_save();
    uint32_t busy;
    do {
        __asm__ volatile("amoswap.w.aq %0, %2, (%1)"
                         : "=r"(busy) : "r"(&atomic64_lock), "r"(1) : "memory");
    } while (busy);
    return flags;
}
static void atomic64_release(uint32_t flags)
{
    __asm__ volatile("amoswap.w.rl zero, zero, (%0)"
                     :: "r"(&atomic64_lock) : "memory");
    arch_irq_restore(flags);
}

uint64_t __atomic_load_8(const volatile void *ptr, int memorder) {
    (void)memorder;
    uint32_t flags = atomic64_acquire();
    const volatile uint64_t *p = (const volatile uint64_t *)ptr;
    uint64_t v = *p;
    atomic64_release(flags);
    return v;
}

void __atomic_store_8(volatile void *ptr, uint64_t val, int memorder) {
    (void)memorder;
    uint32_t flags = atomic64_acquire();
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    *p = val;
    atomic64_release(flags);
}

uint64_t __atomic_fetch_add_8(volatile void *ptr, uint64_t val, int memorder) {
    (void)memorder;
    uint32_t flags = atomic64_acquire();
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    uint64_t old = *p;
    *p = old + val;
    atomic64_release(flags);
    return old;
}

uint64_t __atomic_fetch_sub_8(volatile void *ptr, uint64_t val, int memorder) {
    (void)memorder;
    uint32_t flags = atomic64_acquire();
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    uint64_t old = *p;
    *p = old - val;
    atomic64_release(flags);
    return old;
}

uint64_t __atomic_exchange_8(volatile void *ptr, uint64_t val, int memorder) {
    (void)memorder;
    uint32_t flags = atomic64_acquire();
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    uint64_t old = *p;
    *p = val;
    atomic64_release(flags);
    return old;
}

_Bool __atomic_compare_exchange_8(volatile void *ptr, void *expected, uint64_t desired,
                                  _Bool weak, int success_memorder, int failure_memorder) {
    (void)weak;
    (void)success_memorder;
    (void)failure_memorder;
    uint32_t flags = atomic64_acquire();
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    uint64_t *exp = (uint64_t *)expected;
    if (*p == *exp) {
        *p = desired;
        atomic64_release(flags);
        return 1;
    }
    *exp = *p;
    atomic64_release(flags);
    return 0;
}

#endif /* CONFIG_RISCV32 */
