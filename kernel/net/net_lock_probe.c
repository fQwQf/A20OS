#include "net/socket_internal.h"

#include "core/cpu.h"
#include "core/panic.h"
#include "core/stdio.h"

#if CONFIG_NET_LOCK_ASSERT

/*
 * The net-lock half of the lock contract, made executable.
 *
 * The lwIP half got its probe in 7d217d3fd and is described in
 * docs/net/network-lock-contract.md ("核心锁断言").  This is the other half.
 * Until now the two rules this file checks were prose that only a reader could
 * enforce, and both are exactly the kind a future edit breaks silently:
 *
 *   1. net_sock_lock2() orders its two sockets by address, and a critical
 *      section holds at most two socket locks.  Break the order and two CPUs
 *      taking the same pair in opposite directions deadlock; break the depth and
 *      a path that never meant to nest three locks starts doing so.
 *   2. A bucket lock is never taken while a socket lock is held.  The bucket is
 *      the outer lock, so socket-inside-bucket (net_bucket_scan(),
 *      net_register_socket_locked(), net_socket_unregister()) is the sanctioned
 *      direction and bucket-inside-socket is the ABBA.
 *
 * Both are checked against a per-CPU set of the net locks this CPU holds.
 * socket_internal.h carries the argument for why a per-CPU set is the right
 * granularity here (every net lock is irqsave/irqrestore, so a holder can be
 * neither preempted nor migrated while it holds one).  Do not weaken that
 * argument without re-deriving it: if a net lock ever becomes a plain
 * spin_lock()/spin_unlock() pair, this table stops being authoritative.
 *
 * What the probe cannot see, stated so nobody reads a clean run as more than it
 * is: it only knows which CPU took which lock, not which task.  It cannot
 * therefore catch a lock that is taken in one order on CPU 0 and released in
 * another, or a path that drops one of a pair's locks early.  It catches the
 * class of bug where a *set* of locks violates the contract, which is the class
 * that produced every ABBA this tree has had.
 */

typedef struct {
    const void *lock;
    uint8_t     kind;
} net_lock_held_t;

/*
 * One slot of slack over the contract's own ceiling (two socket locks, one
 * bucket lock): if a path really does nest a third socket lock the probe must
 * say so rather than overwrite an entry and report a clean set.
 */
static net_lock_held_t g_net_lock_held[CONFIG_NR_CPUS][NET_LOCK_PROBE_MAX];
static unsigned         g_net_lock_held_n[CONFIG_NR_CPUS];

static unsigned    g_net_lock_violations;
static const void *g_net_lock_sites[NET_LOCK_PROBE_SITES];
static unsigned    g_net_lock_nsites;

/* Armed at the end of net_init().  Before that the registry locks do not exist
 * and no net lock may be judged; see g_lwip_lock_armed for the same reasoning on
 * the lwIP side. */
static volatile int g_net_lock_armed;
static unsigned g_net_lock_short;

unsigned net_lock_probe_violations(void)
{
    return __atomic_load_n(&g_net_lock_violations, __ATOMIC_RELAXED);
}

void net_lock_probe_arm(void)
{
    __atomic_store_n(&g_net_lock_armed, 1, __ATOMIC_RELEASE);
}

static void net_lock_note_site(const void *site)
{
    for (unsigned i = 0; i < NET_LOCK_PROBE_SITES; i++) {
        if (__atomic_load_n(&g_net_lock_sites[i], __ATOMIC_RELAXED) == site)
            return;
        if (__atomic_load_n(&g_net_lock_sites[i], __ATOMIC_RELAXED) == NULL) {
            __atomic_store_n(&g_net_lock_sites[i], site, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_net_lock_nsites, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}

/*
 * `why` is a short fixed string rather than a number because the panic line is
 * read by someone who does not have this file open; the site address is what
 * actually locates the offending call.
 */
static void net_lock_violation(const char *why, const void *lock, int kind,
                               const void *site, unsigned cpu,
                               unsigned held_n)
{
    __atomic_fetch_add(&g_net_lock_violations, 1, __ATOMIC_RELAXED);
    net_lock_note_site(site);
    if (CONFIG_NET_LOCK_ASSERT == 1)
        panic("net lock contract: %s lock=%lx kind=%s site=%lx cpu=%u "
              "held=%u violations=%u",
              why, (unsigned long)(uintptr_t)lock,
              kind == NET_LOCK_PROBE_BUCKET ? "bucket" : "socket",
              (unsigned long)(uintptr_t)site, cpu, held_n,
              net_lock_probe_violations());
}

void net_lock_probe_acquire(const void *lock, int kind, const void *site)
{
    if (!__atomic_load_n(&g_net_lock_armed, __ATOMIC_ACQUIRE))
        return;
    unsigned cpu = cpu_current_id();
    net_lock_held_t *set = g_net_lock_held[cpu];
    unsigned n = g_net_lock_held_n[cpu];

    if (n >= NET_LOCK_PROBE_MAX) {
        /* Cannot happen while the contract holds; reported rather than
         * truncated so the count stays honest. */
        net_lock_violation("held-set overflow", lock, kind, site, cpu, n);
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        /* Re-entering a lock this CPU already holds is a self-deadlock that the
         * spinlock would otherwise report with no explanation. */
        if (set[i].lock == lock) {
            net_lock_violation("recursive acquire", lock, kind, site, cpu, n);
            return;
        }
    }

    if (kind == NET_LOCK_PROBE_BUCKET) {
        for (unsigned i = 0; i < n; i++) {
            /* Bucket under socket: the ABBA.  net_bucket_scan() and the
             * register/unregister pair take the socket inside the bucket, which
             * is why this direction and not the other one is the violation. */
            if (set[i].kind == NET_LOCK_PROBE_SOCKET) {
                net_lock_violation("bucket lock under socket lock", lock, kind,
                                   site, cpu, n);
                return;
            }
        }
        if (n > 0) {
            /* net_bucket_lock2() was deleted in 7c7a4d7c8, so at most one bucket
             * lock may ever be held. */
            net_lock_violation("second bucket lock", lock, kind, site, cpu, n);
            return;
        }
    } else {
        unsigned sock = 0;
        for (unsigned i = 0; i < n; i++) {
            if (set[i].kind != NET_LOCK_PROBE_SOCKET)
                continue;
            sock++;
            /* Ascending order, checked as a property of the held set rather than
             * re-derived from the comparison net_sock_lock2() already made to
             * order its own pair: this catches a third socket arriving below
             * the two already held. */
            if ((uintptr_t)set[i].lock > (uintptr_t)lock) {
                net_lock_violation("socket lock out of address order", lock,
                                   kind, site, cpu, n);
                return;
            }
        }
        if (sock >= 2) {
            net_lock_violation("third socket lock", lock, kind, site, cpu, n);
            return;
        }
    }

    set[n].lock = lock;
    set[n].kind = (uint8_t)kind;
    __atomic_store_n(&g_net_lock_held_n[cpu], n + 1, __ATOMIC_RELAXED);
}

void net_lock_probe_release(const void *lock, int kind)
{
    if (!__atomic_load_n(&g_net_lock_armed, __ATOMIC_ACQUIRE))
        return;
    unsigned cpu = cpu_current_id();
    net_lock_held_t *set = g_net_lock_held[cpu];
    unsigned n = g_net_lock_held_n[cpu];

    for (unsigned i = n; i > 0; i--) {
        if (set[i - 1].lock != lock)
            continue;
        /* Release from the end: net_sock_unlock2() drops hi then lo, and the set
         * is order-independent for every check the acquire path makes, but
         * keeping the tail contiguous means an unmatched release leaves one hole
         * rather than a hole in the middle. */
        for (unsigned j = i - 1; j + 1 < n; j++)
            set[j] = set[j + 1];
        __atomic_store_n(&g_net_lock_held_n[cpu], n - 1, __ATOMIC_RELAXED);
        return;
    }
    /* Releasing a lock this CPU does not hold means the acquire never recorded
     * it (arm raced, or a path bypasses the wrapper).  Counted, not fatal: it is
     * a bookkeeping defect rather than a lock-order one, and it must not be
     * allowed to mask the checks above. */
    net_lock_violation("release of an unheld lock", lock, kind,
                       __builtin_return_address(0), cpu, n);
}

int net_lock_probe_format(char *buf, size_t bufsz)
{
    int off = 0;
    int n = snprintf(buf, bufsz,
                     "\nnet_lock: armed=%d violations=%u sites=%u "
                     "held_cpu0=%u lockcounters_short=%u\n",
                     g_net_lock_armed ? 1 : 0, net_lock_probe_violations(),
                     __atomic_load_n(&g_net_lock_nsites, __ATOMIC_RELAXED),
                     __atomic_load_n(&g_net_lock_held_n[0], __ATOMIC_RELAXED),
                     __atomic_load_n(&g_net_lock_short, __ATOMIC_RELAXED));
    if (n > 0)
        off = n;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;

    for (unsigned i = 0; i < NET_LOCK_PROBE_SITES; i++) {
        const void *site =
            __atomic_load_n(&g_net_lock_sites[i], __ATOMIC_RELAXED);
        if (!site)
            break;
        n = snprintf(buf + off, bufsz - (size_t)off, "net_lock_site%u: %lx\n", i,
                     (unsigned long)(uintptr_t)site);
        if (n < 0)
            break;
        off += n;
        if ((size_t)off >= bufsz)
            return (int)bufsz - 1;
    }
    return off;
}

/*
 * The socket-table bucket locks did not all make it into the contention
 * registry.  This is a *silent coverage loss*, not a lock bug: the locks are
 * acquired exactly as before, they are simply never counted, so
 * /proc/a20/lock_contention reports a clean run for locks it never saw.
 *
 * Counted and rendered alongside the rest of this file's diagnostics, and fatal
 * under CONFIG_NET_LOCK_ASSERT == 1 for the same reason the lock-order
 * violations are: a partially instrumented data plane is worse than an
 * uninstrumented one, because the report still looks trustworthy.
 */
void net_lockcounters_short(unsigned got, unsigned want, unsigned dropped)
{
    g_net_lock_short++;
    printf("[NET] lock_counters: only %u of %u net bucket locks registered "
           "(dropped=%u) -- the contention audit is now partially blind\n",
           got, want, dropped);
#if CONFIG_NET_LOCK_ASSERT == 1
    panic("net bucket locks: only %u of %u registered, dropped=%u", got, want,
          dropped);
#endif
}

#else /* CONFIG_NET_LOCK_ASSERT */

/*
 * Stubs, so that a20_lwip_format_status() can render "net_lock: not checked"
 * without an #ifdef of its own and a reader never sees a build that silently
 * omits the row.  None of these is reachable from the lock helpers in
 * socket_internal.h: every one of those calls sits behind #if
 * CONFIG_NET_LOCK_ASSERT, which is why the default build pays nothing.
 */
void net_lock_probe_acquire(const void *lock, int kind, const void *site)
{
    (void)lock;
    (void)kind;
    (void)site;
}
void net_lock_probe_release(const void *lock, int kind)
{
    (void)lock;
    (void)kind;
}
void net_lock_probe_arm(void) {}
unsigned net_lock_probe_violations(void) { return 0; }
void net_lockcounters_short(unsigned got, unsigned want, unsigned dropped)
{
    /* Without the assert switch there is no net-side check to run, but the
     * contention registry's own dropped counter (lock_counters.c) still records
     * the refusal and still renders on /proc/a20/lock_contention, so a build
     * that cannot afford the probe is not left blind. */
    printf("[NET] lock_counters: only %u of %u net bucket locks registered "
           "(dropped=%u)\n",
           got, want, dropped);
}

int net_lock_probe_format(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return 0;
    return snprintf(buf, bufsz,
                    "\nnet_lock: not checked (CONFIG_NET_LOCK_ASSERT=0)\n");
}

#endif /* CONFIG_NET_LOCK_ASSERT */