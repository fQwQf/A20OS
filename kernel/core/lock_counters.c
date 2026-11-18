#include "core/lock_counters.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/stdio.h"
#include "core/kallsyms.h"
#include "core/string.h"
#include "core/errno.h"
#include "mm/slab.h"

/* Fixed registry of the locks the performance audit cares about.  Registration
 * stores the lock pointer and a name; the per-lock counters in spinlock_t
 * accumulate in place, so reads are cheap and race-free at u64 precision. */
/* Sized to hold every per-bucket vfile-table lock (128) plus the rest of
 * the registered set, so partial visibility never fakes a clean lock. */
#define LOCK_COUNTERS_MAX 192

typedef struct {
    spinlock_t *lock;
    const char *name;
} lock_counter_entry_t;

static spinlock_t g_lock_counters_lock = SPINLOCK_INIT;
static lock_counter_entry_t g_lock_counters[LOCK_COUNTERS_MAX];
static int g_lock_counter_count;
/*
 * Registrations refused because the table was full.
 *
 * The refusal used to be completely silent, which is the worst possible
 * behaviour for this subsystem: the lock keeps working, it just stops being
 * counted, and /proc/a20/lock_contention then reports a clean run for a lock
 * that was never observed.  This flag applied exactly to the socket-table
 * bucket locks (128 of them on the server profile against
 * a ceiling of 192), so the drop is now counted and rendered on the same
 * report that would otherwise have lied.
 */
static unsigned g_lock_counters_dropped;

void lock_counters_init(void) { /* Registry lock is statically initialized. */ }

unsigned lock_counters_count(void)
{
    lock_counters_init();
    /* Read under the registry lock, not with a relaxed atomic: the count is
     * maintained as a plain int under that lock, and a torn or stale read here
     * would be reported as a missing registration by the caller that compares
     * two of these. */
    uint64_t flags = spin_lock_irqsave(&g_lock_counters_lock);
    unsigned n = (unsigned)g_lock_counter_count;
    spin_unlock_irqrestore(&g_lock_counters_lock, flags);
    return n;
}

unsigned lock_counters_dropped(void)
{
    return __atomic_load_n(&g_lock_counters_dropped, __ATOMIC_ACQUIRE);
}

void lock_counters_register(spinlock_t *lock, const char *name)
{
    if (!lock || !name)
        return;
    lock_counters_init();
    uint64_t flags = spin_lock_irqsave(&g_lock_counters_lock);
    for (int i = 0; i < g_lock_counter_count; i++) {
        if (g_lock_counters[i].lock == lock) {
            if (!g_lock_counters[i].name)
                g_lock_counters[i].name = name;
            spin_unlock_irqrestore(&g_lock_counters_lock, flags);
            return;
        }
    }
    if (g_lock_counter_count < LOCK_COUNTERS_MAX) {
        g_lock_counters[g_lock_counter_count].lock = lock;
        g_lock_counters[g_lock_counter_count].name = name;
        g_lock_counter_count++;
    } else {
        __atomic_fetch_add(&g_lock_counters_dropped, 1, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&g_lock_counters_lock, flags);
}

/* Enable per-callsite contention sampling for one lock.  Allocates the fixed
 * sample table once; the contended path then records the caller's return
 * address so /proc/a20/lock_contention can attribute the hot call sites. */
void lock_counters_enable_callsite(spinlock_t *lock)
{
    if (!lock || lock->samples)
        return;
    lock_counters_init();
    lock_callsite_sample_t *arr = (lock_callsite_sample_t *)kcalloc(
        LOCK_CALLSITE_SAMPLES, sizeof(lock_callsite_sample_t));
    if (!arr)
        return;
    lock_callsite_sample_t *expected = NULL;
    if (!__atomic_compare_exchange_n(&lock->samples, &expected, arr, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED)) kfree(arr);
}

static size_t lock_counters_format_one(char *buf, size_t bufsz,
                                       spinlock_t *lock, const char *name,
                                       size_t off)
{
    uint64_t acq = __atomic_load_n(&lock->contended_acquires,
                                   __ATOMIC_RELAXED);
    uint64_t spn = __atomic_load_n(&lock->contended_spins,
                                   __ATOMIC_RELAXED);
    uint64_t mx = __atomic_load_n(&lock->contended_max_spins,
                                  __ATOMIC_RELAXED);
    int n = snprintf(buf + off, bufsz - off, "%s: %llu %llu max=%llu\n",
                     name ? name : "?", (unsigned long long)acq, (unsigned long long)spn,
                     (unsigned long long)mx);
    if (n < 0)
        return bufsz ? bufsz - 1 : 0;
    if ((size_t)n >= bufsz - off - 1)
        return bufsz ? bufsz - 1 : 0;
    off += (size_t)n;

    lock_callsite_sample_t *samples = __atomic_load_n(&lock->samples,
                                                      __ATOMIC_ACQUIRE);
    if (!samples)
        return off;
    for (int i = 0; i < LOCK_CALLSITE_SAMPLES; i++) {
        uintptr_t ra = __atomic_load_n(&samples[i].ra, __ATOMIC_RELAXED);
        uint64_t c = __atomic_load_n(&samples[i].contended, __ATOMIC_RELAXED);
        if (!c)
            continue;
        uint64_t s = __atomic_load_n(&samples[i].spins, __ATOMIC_RELAXED);
        uint64_t sm = __atomic_load_n(&samples[i].max_spins, __ATOMIC_RELAXED);
        uint64_t sym_off = 0;
        /* ra == 0 means the waiter had no recoverable return address, e.g. an
         * interrupt context.  Report it as "?" instead of dropping the sample,
         * so per-site spins still account for the lock's contended_spins rather
         * than silently losing whatever could not be symbolised. */
        const char *sym = ra ? kallsyms_lookup(ra, &sym_off) : NULL;
        n = snprintf(buf + off, bufsz - off,
                     "  [%s] %s+0x%lx: %llu %llu max=%llu\n",
                     name ? name : "?", sym ? sym : "?",
                     (unsigned long)sym_off, (unsigned long long)c,
                     (unsigned long long)s, (unsigned long long)sm);
        if (n < 0) return bufsz ? bufsz - 1 : 0;
        if ((size_t)n >= bufsz - off - 1) {
            off = bufsz ? bufsz - 1 : 0;
            break;
        }
        off += (size_t)n;
    }
    return off;
}

int lock_counters_format(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return -EINVAL;
    /* Registered locks/names have static kernel lifetime. Copy membership
     * under the registry lock; symbolize and render live atomic counters
     * outside it. This is a live snapshot, not a frozen perf window. */
    lock_counter_entry_t *entries = kmalloc(sizeof(g_lock_counters));
    if (!entries) return -ENOMEM;
    lock_counters_init();
    uint64_t flags = spin_lock_irqsave(&g_lock_counters_lock);
    unsigned held = g_lock_counter_count, dropped = g_lock_counters_dropped;
    memcpy(entries, g_lock_counters, held * sizeof(*entries));
    spin_unlock_irqrestore(&g_lock_counters_lock, flags);
    size_t off = 0;
    for (unsigned i = 0; i < held; i++) {
        off = lock_counters_format_one(buf, bufsz, entries[i].lock, entries[i].name, off);
        if (off >= bufsz - 1) goto full;
    }
    int n = snprintf(buf + off, bufsz - off,
                     "lock_counters: registered=%u capacity=%u dropped=%u\n",
                     held, (unsigned)LOCK_COUNTERS_MAX, dropped);
    if (n < 0 || (size_t)n >= bufsz - off - 1) goto full;
    off += (size_t)n;
    kfree(entries);
    return (int)off;
full:
    buf[bufsz - 1] = 0;
    kfree(entries);
    return (int)(bufsz - 1);
}

/*
 * Zero every registered lock's counters, keeping the registrations themselves.
 *
 * The counters are cumulative since boot with no way to narrow the window, so a
 * stress spike cannot be told apart from boot-time traffic.  docs/zh/status/current/server-readiness.md
 * records a wrong conclusion drawn from exactly that: an extreme per-acquire
 * spin count was first attributed to the stress window and had to be retracted
 * after the boot window was measured separately.  A gate can work around this by
 * reading the file before and after and subtracting, which is why the existing
 * gates do that rather than asserting a magnitude; but only the stress program
 * can subtract, so a reset entry point is what lets one process own the window.
 *
 * contended_max_spins is cleared too even though it is a high-water mark: after
 * a reset, "max over this window" is the useful question, and a stale boot-time
 * maximum would make every later window look as bad as the worst boot window.
 */
void lock_counters_reset(void)
{
    uint64_t flags = spin_lock_irqsave(&g_lock_counters_lock);
    for (int i = 0; i < g_lock_counter_count; i++) {
        spinlock_t *lock = g_lock_counters[i].lock;
        __atomic_store_n(&lock->contended_acquires, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&lock->contended_spins, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&lock->contended_max_spins, 0, __ATOMIC_RELAXED);
        lock_callsite_sample_t *samples =
            __atomic_load_n(&lock->samples, __ATOMIC_ACQUIRE);
        if (samples) {
            for (unsigned j = 0; j < LOCK_CALLSITE_SAMPLES; j++) {
                __atomic_store_n(&samples[j].ra, 0, __ATOMIC_RELAXED);
                __atomic_store_n(&samples[j].contended, 0, __ATOMIC_RELAXED);
                __atomic_store_n(&samples[j].spins, 0, __ATOMIC_RELAXED);
                __atomic_store_n(&samples[j].max_spins, 0, __ATOMIC_RELAXED);
            }
        }
    }
    spin_unlock_irqrestore(&g_lock_counters_lock, flags);
}
