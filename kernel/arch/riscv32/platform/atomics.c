#ifdef CONFIG_RISCV32

#include "core/types.h"
#include "cpu.h"

uint64_t __atomic_load_8(const volatile void *ptr, int memorder) {
    (void)memorder;
    uint32_t flags = arch_irq_save();
    const volatile uint64_t *p = (const volatile uint64_t *)ptr;
    uint64_t v = *p;
    arch_irq_restore(flags);
    return v;
}

void __atomic_store_8(volatile void *ptr, uint64_t val, int memorder) {
    (void)memorder;
    uint32_t flags = arch_irq_save();
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    *p = val;
    arch_irq_restore(flags);
}

uint64_t __atomic_fetch_add_8(volatile void *ptr, uint64_t val, int memorder) {
    (void)memorder;
    uint32_t flags = arch_irq_save();
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    uint64_t old = *p;
    *p = old + val;
    arch_irq_restore(flags);
    return old;
}

uint64_t __atomic_exchange_8(volatile void *ptr, uint64_t val, int memorder) {
    (void)memorder;
    uint32_t flags = arch_irq_save();
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    uint64_t old = *p;
    *p = val;
    arch_irq_restore(flags);
    return old;
}

_Bool __atomic_compare_exchange_8(volatile void *ptr, void *expected, uint64_t desired,
                                  _Bool weak, int success_memorder, int failure_memorder) {
    (void)weak;
    (void)success_memorder;
    (void)failure_memorder;
    uint32_t flags = arch_irq_save();
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    uint64_t *exp = (uint64_t *)expected;
    if (*p == *exp) {
        *p = desired;
        arch_irq_restore(flags);
        return 1;
    }
    *exp = *p;
    arch_irq_restore(flags);
    return 0;
}

#endif /* CONFIG_RISCV32 */
