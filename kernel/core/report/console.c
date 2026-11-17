#include "core/klog.h"
#include "core/lock.h"
#include "core/timer.h"
#include "proc/proc.h"
extern spinlock_t klog_console_lock;
extern void console_emit_locked(const char *, size_t);

/* Process-context replay: bound each console critical section to 64 bytes.
 * Release interrupts and yield between chunks; never mask interrupts for a
 * complete multi-kilobyte report. Normal logging may interleave at boundaries. */
void klog_console_replay(const char *buffer, size_t length) {
    while (length && klog_console_enabled()) {
        uint64_t flags;
        if (spin_trylock_irqsave(&klog_console_lock, &flags)) {
            size_t chunk = length < 64 ? length : 64;
            console_emit_locked(buffer, chunk);
            spin_unlock_irqrestore(&klog_console_lock, flags);
            buffer += chunk;
            length -= chunk;
        }
        if (length) proc_sleep_until(timer_get_ticks() + 1);
    }
}
