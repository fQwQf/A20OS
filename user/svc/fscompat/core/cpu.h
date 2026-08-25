/* fscompat/core/cpu.h — 隔离 defs.h 对 per-cpu 内核设施的引用。 */
#ifndef _CPU_H
#define _CPU_H

static inline unsigned int cpu_id(void)
{
    return 0;
}

static inline unsigned int arch_current_cpu_id(void)
{
    return 0;
}

#endif /* _CPU_H */
