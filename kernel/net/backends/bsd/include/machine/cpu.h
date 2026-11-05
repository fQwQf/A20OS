#if !defined(A20_BSD_PORTABLE_LP64)
#include_next <machine/cpu.h>
#else
#ifndef A20_BSD_CPU_H
#define A20_BSD_CPU_H
#include <machine/atomic.h>
#include <machine/cpufunc.h>
uint64_t ff_get_tsc_ns(void);
static inline uint64_t get_cyclecount(void) { return ff_get_tsc_ns(); }
#define cpu_spinwait() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define cpu_lock_delay() cpu_spinwait()
#endif
#endif
