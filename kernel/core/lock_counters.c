#include "core/lock_counters.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/stdio.h"
#include "core/kallsyms.h"
#include "mm/slab.h"

/* Fixed registry of the locks the BuildStorm audit cares about.  Registration
 * stores the lock pointer and a name; the per-lock counters in spinlock_t
 * accumulate in place, so reads are cheap and race-free at u64 precision. */
#define LOCK_COUNTERS_MAX 64

typedef struct {
    spinlock_t *lock;
    const char *name;
} lock_counter_entry_t;

static spinlock_t g_lock_counters_lock = SPINLOCK_INIT;
static lock_counter_entry_t g_lock_counters[LOCK_COUNTERS_MAX];
static int g_lock_counter_count;
static int g_lock_counters_initialized;

void lock_counters_init(void)
{
    if (g_lock_counters_initialized)
        return;
    spin_init(&g_lock_counters_lock);
    g_lock_counters_initialized = 1;
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
    __atomic_store_n(&lock->samples, arr, __ATOMIC_RELEASE);
}

static size_t lock_counters_format_one(char *buf, size_t bufsz,
                                       spinlock_t *lock, const char *name,
                                       size_t off)
{
    uint64_t acq = __atomic_load_n(&lock->contended_acquires,
                                   __ATOMIC_RELAXED);
    uint64_t spn = __atomic_load_n(&lock->contended_spins,
                                   __ATOMIC_RELAXED);
    int n = snprintf(buf + off, bufsz - off, "%s: %lu %lu\n",
                     name ? name : "?", (unsigned long)acq, (unsigned long)spn);
    if (n < 0)
        return bufsz ? bufsz - 1 : 0;
    if ((size_t)n >= bufsz - off)
        return bufsz ? bufsz - 1 : 0;
    off += (size_t)n;

    lock_callsite_sample_t *samples = __atomic_load_n(&lock->samples,
                                                      __ATOMIC_ACQUIRE);
    if (!samples)
        return off;
    for (int i = 0; i < LOCK_CALLSITE_SAMPLES; i++) {
        uintptr_t ra = __atomic_load_n(&samples[i].ra, __ATOMIC_RELAXED);
        uint64_t c = __atomic_load_n(&samples[i].contended, __ATOMIC_RELAXED);
        if (!ra || !c)
            continue;
        uint64_t s = __atomic_load_n(&samples[i].spins, __ATOMIC_RELAXED);
        uint64_t sym_off = 0;
        const char *sym = kallsyms_lookup(ra, &sym_off);
        n = snprintf(buf + off, bufsz - off, "  [%s] %s+0x%lx: %lu %lu\n",
                     name ? name : "?", sym ? sym : "?",
                     (unsigned long)sym_off, (unsigned long)c, (unsigned long)s);
        if (n < 0)
            break;
        if ((size_t)n >= bufsz - off) {
            off = bufsz ? bufsz - 1 : 0;
            break;
        }
        off += (size_t)n;
    }
    return off;
}

size_t lock_counters_format(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return 0;
    size_t off = 0;
    /* Also mark the collection live, like a20_perf_format(). */
    lock_counters_init();
    uint64_t flags = spin_lock_irqsave(&g_lock_counters_lock);
    for (int i = 0; i < g_lock_counter_count; i++) {
        off = lock_counters_format_one(buf, bufsz,
                                       g_lock_counters[i].lock,
                                       g_lock_counters[i].name, off);
        if (off >= bufsz)
            break;
    }
    spin_unlock_irqrestore(&g_lock_counters_lock, flags);
    return off;
}
