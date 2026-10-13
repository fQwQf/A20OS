#ifndef SHARED_DEVICE_TEST_CPU_H
#define SHARED_DEVICE_TEST_CPU_H
extern _Thread_local unsigned test_cpu;
extern _Thread_local int test_irq;
void shared_device_poll(void);
void shared_test_pause(void);
static inline unsigned cpu_current_id(void) { return test_cpu; }
static inline int arch_irqs_enabled(void) { return test_irq; }
static inline void arch_local_irq_disable(void) { test_irq = 0; }
static inline void arch_local_irq_enable(void) { test_irq = 1; }
static inline void cpu_relax(void) { shared_device_poll(); shared_test_pause(); }
#endif
