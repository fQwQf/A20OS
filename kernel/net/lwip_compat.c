#include "core/types.h"

long strtol(const char *nptr, char **endptr, int base) {
    const char *p = nptr;
    long sign = 1;
    long value = 0;

    while (*p == ' ' || *p == '\t' || *p == '\n' ||
           *p == '\r' || *p == '\f' || *p == '\v')
        p++;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
        p++;
    } else if (base == 0) {
        base = 10;
    }

    while (*p) {
        int digit;
        if (*p >= '0' && *p <= '9')
            digit = *p - '0';
        else if (*p >= 'a' && *p <= 'z')
            digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z')
            digit = *p - 'A' + 10;
        else
            break;
        if (digit >= base)
            break;
        value = value * base + digit;
        p++;
    }

    if (endptr)
        *endptr = (char *)p;
    return value * sign;
}


#include "core/lock.h"
#include "arch/cc.h"

#if CONFIG_NET_LANES > 1
/* SYS_ARCH_PROTECT may nest inside lwIP helpers, so only the outermost
 * protection section takes the SMP lock. Local IRQ masking prevents a same-CPU
 * interrupt from observing an in-progress depth transition. */
static spinlock_t lwip_protect_lock = SPINLOCK_INIT;
static unsigned lwip_protect_depth[CONFIG_NR_CPUS];

/* The heap lock is deliberately separate from SYS_ARCH_PROTECT. Heap code
 * updates statistics while holding this lock, establishing heap -> protect
 * ordering; no path may enter the heap while SYS_ARCH_PROTECT is held. */
static spinlock_t lwip_heap_lock = SPINLOCK_INIT;
static uint8_t lwip_heap_irq_was_enabled[CONFIG_NR_CPUS];

void a20_lwip_heap_lock(void)
{
    uint8_t restore_irqs = arch_irqs_enabled() ? 1 : 0;
    arch_local_irq_disable();
    unsigned cpu = cpu_current_id();
    if (lwip_protect_depth[cpu] != 0)
        panic("lwIP heap lock acquired inside SYS_ARCH_PROTECT");
    lwip_heap_irq_was_enabled[cpu] = restore_irqs;
    spin_lock(&lwip_heap_lock);
}

void a20_lwip_heap_unlock(void)
{
    arch_local_irq_disable();
    unsigned cpu = cpu_current_id();
    uint8_t restore_irqs = lwip_heap_irq_was_enabled[cpu];

    spin_unlock(&lwip_heap_lock);
    if (restore_irqs)
        arch_local_irq_enable();
}
#endif

sys_prot_t sys_arch_protect(void) {
    uint64_t flags = arch_irqs_enabled() ? 1 : 0;
    arch_local_irq_disable();
#if CONFIG_NET_LANES > 1
    unsigned cpu = cpu_current_id();
    if (lwip_protect_depth[cpu]++ == 0)
        spin_lock(&lwip_protect_lock);
#endif
    return flags;
}

void sys_arch_unprotect(sys_prot_t pval) {
#if CONFIG_NET_LANES > 1
    unsigned cpu = cpu_current_id();
    if (lwip_protect_depth[cpu] == 0)
        panic("lwIP SYS_ARCH_UNPROTECT without matching protect");
    if (--lwip_protect_depth[cpu] == 0)
        spin_unlock(&lwip_protect_lock);
#endif
    if (pval)
        arch_local_irq_enable();
}
