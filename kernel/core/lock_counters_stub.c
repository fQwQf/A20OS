#include "core/lock_counters.h"
#include "core/stdio.h"
void lock_counters_init(void) {}
void lock_counters_register(spinlock_t *lock, const char *name) { (void)lock; (void)name; }
void lock_counters_enable_callsite(spinlock_t *lock) { (void)lock; }
void lock_counters_reset(void) {}
unsigned lock_counters_count(void) { return 0; }
unsigned lock_counters_dropped(void) { return 0; }
int lock_counters_format(char *buffer, size_t size) {
    if (!size) return 0;
    int n = snprintf(buffer, size, "lock_metrics_compiled: 0\n");
    return n < 0 ? n : (size_t)n >= size ? (int)(size - 1) : n;
}
