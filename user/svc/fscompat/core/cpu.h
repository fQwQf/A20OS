/* fscompat/core/cpu.h — shields defs.h from referencing per-cpu kernel facilities. */
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
