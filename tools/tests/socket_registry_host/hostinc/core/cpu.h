#ifndef A20_HOST_REGISTRY_CORE_CPU_H
#define A20_HOST_REGISTRY_CORE_CPU_H
/*
 * Host stand-in for core/cpu.h, used only by tools/tests/socket_registry_host.
 *
 * The production socket_registry.c takes the registry's spinlocks, and
 * spin_lock_irqsave() reaches the architecture's IRQ predicates.  On the host
 * those are `cli`/`sti`, so they are declared here as ordinary functions that
 * the harness implements as no-ops.  The overlay is deliberately minimal: it
 * shadows core/cpu.h and nothing else, so every other kernel header in the
 * include chain stays the production one.
 */
#include <stdint.h>

#ifndef CONFIG_NR_CPUS
#define CONFIG_NR_CPUS 1
#endif

unsigned cpu_current_id(void);
int arch_irqs_enabled(void);
void arch_local_irq_disable(void);
void arch_local_irq_enable(void);
void arch_cpu_relax(void);

static inline void cpu_relax(void)
{
    arch_cpu_relax();
}

#endif /* A20_HOST_REGISTRY_CORE_CPU_H */
