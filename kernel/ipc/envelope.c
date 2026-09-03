/*
 * A20OS Linux ABI — Capability Envelope mediator core (docs/research/05).
 *
 * Implements the budgeted-capability enforcement object ("envelope") that is
 * attached to unmodified Linux-ABI processes.  See abi/linux/envelope.h for
 * the mediation contract.  This file owns:
 *   - envelope registry + lifetime (refcounted, shared across fork),
 *   - acquisition mediation (type x rights x time x ops) + shadow table,
 *   - use mediation (ops/data accounting, lazy expiry),
 *   - active revocation (env_revoke) — authority dies at the next mediated
 *     operation of every attached task.
 *
 * Locking: one spinlock per envelope protects policy budgets and the shadow
 * table.  The per-task ->envelope pointer is only swapped atomically and
 * never written after attach except by release (exchange to NULL), so the
 * syscall fast path is a single acquire-load plus branch.
 */
#include "ipc/envelope.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/klog.h"
#include "core/errno.h"
#include "ipc/ipc.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "sys/usercopy.h"
#include "mm/slab.h"

#define ENV_NS_PER_SEC 1000000000ull

typedef struct env_shadow {
    int      gfd;            /* global fd this shadow authorizes; -1 = free */
    uint8_t  obj_type;       /* a20_object_type_t                           */
    uint64_t rights;         /* granted rights (subset of class cap)        */
} env_shadow_t;

typedef struct a20_envelope {
    refcount_t      refs;
    spinlock_t      lock;
    a20_env_policy_t policy;

    uint64_t expire_tick;    /* 0 = no time limit */
    uint64_t remaining_ops;
    uint64_t remaining_data;
    int      expired;
    int      kill_sent;

    int      owner_pid;      /* supervisor that created the envelope */

    env_shadow_t shadows[A20_ENV_MAX_SHADOWS];

    /* Observable counters (relaxed; research telemetry, not control state). */
    a20_env_stats_t stats;
} a20_envelope_t;

/* ---- Registry ---------------------------------------------------------- */

static a20_envelope_t *env_registry[A20_ENV_MAX_INSTANCES];
static spinlock_t env_registry_lock = SPINLOCK_INIT;

static int env_ns_to_ticks(uint64_t ns)
{
    if (ns == 0)
        return 0;
    return ns / (ENV_NS_PER_SEC / TICKS_PER_SEC) + 1;
}

static void env_destroy(a20_envelope_t *e)
{
    for (int i = 0; i < A20_ENV_MAX_INSTANCES; i++) {
        /* Clear our own registry slot under the registry lock. */
        spin_lock(&env_registry_lock);
        if (env_registry[i] == e)
            env_registry[i] = NULL;
        spin_unlock(&env_registry_lock);
    }
    kfree(e);
}

static a20_envelope_t *env_get_ref(a20_envelope_t *e)
{
    refcount_inc(&e->refs);
    return e;
}

static void env_put_ref(a20_envelope_t *e)
{
    if (refcount_dec_and_test(&e->refs))
        env_destroy(e);
}

/* ---- Expiry ------------------------------------------------------------ */

/* Caller holds e->lock. */
static void env_mark_expired_locked(a20_envelope_t *e)
{
    if (e->expired)
        return;
    e->expired = 1;
}

static void env_kill_tasks(a20_envelope_t *e)
{
    /* KILL_ON_EXPIRE: SIGKILL every task still attached.  Pids are collected
     * under proc_lock (task-list membership) and signalled after release —
     * signal_send takes its own locks, and the documented lock order does
     * not include signal locks under proc_lock. */
    enum { KILL_BATCH = 32 };
    int cursor = -1;

    for (;;) {
        int pids[KILL_BATCH];
        int n = 0;
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        for (task_t *t = proc_first_task_locked(); t;
             t = proc_next_task_locked(t)) {
            if (__atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE) != (void *)e ||
                t->pid <= cursor)
                continue;

            if (n == KILL_BATCH && t->pid >= pids[KILL_BATCH - 1])
                continue;
            int pos = n < KILL_BATCH ? n++ : KILL_BATCH - 1;
            while (pos > 0 && pids[pos - 1] > t->pid) {
                if (pos < KILL_BATCH)
                    pids[pos] = pids[pos - 1];
                pos--;
            }
            pids[pos] = t->pid;
        }
        spin_unlock_irqrestore(&proc_lock, flags);

        if (n == 0)
            break;
        for (int i = 0; i < n; i++)
            signal_send(pids[i], SIGKILL);
        cursor = pids[n - 1];
    }
}

/* Lazy expiry check shared by all mediation entry points.
 * Returns negative errno when the envelope must deny, 0 otherwise. */
static int env_check_expired(a20_envelope_t *e)
{
    int deny = 0;
    spin_lock(&e->lock);
    if (!e->expired && e->expire_tick != 0 &&
        timer_get_ticks() >= e->expire_tick)
        env_mark_expired_locked(e);
    if (e->expired)
        deny = -EACCES;
    spin_unlock(&e->lock);

    if (deny && (e->policy.flags & A20_ENV_F_KILL_ON_EXPIRE)) {
        if (__atomic_exchange_n(&e->kill_sent, 1, __ATOMIC_ACQ_REL) == 0)
            env_kill_tasks(e);
    }
    return deny;
}

/* Caller holds e->lock.  Charges one op and installs the shadow entry
 * keyed by gfd (overwriting a stale slot from a closed/re-mediated fd). */
static int env_acquire_charge_install_locked(a20_envelope_t *e,
                                             uint8_t obj_type,
                                             uint64_t rights, int gfd)
{
    if (e->remaining_ops == 0) {
        e->stats.use_deny_ops++;
        return -EACCES;
    }
    e->remaining_ops--;

    int slot = -1;
    for (int i = 0; i < A20_ENV_MAX_SHADOWS; i++) {
        if (e->shadows[i].gfd == gfd) {
            slot = i;
            break;
        }
        if (slot < 0 && e->shadows[i].gfd < 0)
            slot = i;
    }
    if (slot < 0)
        return -EMFILE;
    e->shadows[slot].gfd = gfd;
    e->shadows[slot].obj_type = obj_type;
    e->shadows[slot].rights = rights;
    e->stats.acquire_ok++;
    return 0;
}

/* ---- Acquisition mediation --------------------------------------------- */

int env_mediate_acquire(uint8_t obj_type, uint64_t rights, int gfd)
{
    task_t *t = proc_current();
    a20_envelope_t *e =
        __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
    if (!e)
        return 0; /* not enveloped: zero-cost pass-through */

    int rc = env_check_expired(e);
    if (rc)
        return rc;

    spin_lock(&e->lock);

    if (!(e->policy.allowed_types & (1u << obj_type))) {
        e->stats.acquire_deny_type++;
        spin_unlock(&e->lock);
        return -EPERM;
    }
    uint64_t cap = e->policy.rights_by_class[obj_type];
    if ((rights & ~cap) != 0) {
        e->stats.acquire_deny_rights++;
        spin_unlock(&e->lock);
        return -EACCES;
    }
    int r = env_acquire_charge_install_locked(e, obj_type, rights & cap, gfd);
    spin_unlock(&e->lock);
    return r;
}

/*
 * A6/A7 transfer acquisition: an authority created elsewhere enters this
 * task through SCM_RIGHTS receipt or pidfd_getfd.  The grant is clamped to
 * the policy class cap; an empty grant denies.
 */
int env_mediate_acquire_gfd(int gfd)
{
    task_t *t = proc_current();
    a20_envelope_t *e =
        __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
    if (!e)
        return 0;

    int rc = env_check_expired(e);
    if (rc)
        return rc;

    uint8_t kind = env_kind_of(gfd);
    uint64_t want = A20_RIGHT_READ | A20_RIGHT_WRITE |
                    A20_RIGHT_STAT | A20_RIGHT_SEEK;
    if (kind == A20_OBJ_SOCKET)
        want |= A20_RIGHT_CONNECT | A20_RIGHT_ACCEPT;

    spin_lock(&e->lock);

    if (!(e->policy.allowed_types & (1u << kind))) {
        e->stats.acquire_deny_type++;
        spin_unlock(&e->lock);
        return -EPERM;
    }
    uint64_t granted = want & e->policy.rights_by_class[kind];
    if (granted == 0) {
        e->stats.acquire_deny_rights++;
        spin_unlock(&e->lock);
        return -EACCES;
    }
    int r = env_acquire_charge_install_locked(e, kind, granted, gfd);
    spin_unlock(&e->lock);
    return r;
}

int env_mediate_acquire_socket(uint64_t rights, int gfd)
{
    return env_mediate_acquire((uint8_t)A20_OBJ_SOCKET, rights, gfd);
}

/* A6 send side: propagation_types gates every authority leaving the task. */
int env_mediate_send_fd(int gfd)
{
    task_t *t = proc_current();
    a20_envelope_t *e =
        __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
    if (!e)
        return 0;

    int rc = env_check_expired(e);
    if (rc)
        return rc;

    uint8_t kind = env_kind_of(gfd);
    spin_lock(&e->lock);
    if (!(e->policy.propagation_types & (1u << kind))) {
        spin_unlock(&e->lock);
        return -EPERM;
    }
    if (e->remaining_ops == 0) {
        e->stats.use_deny_ops++;
        spin_unlock(&e->lock);
        return -EACCES;
    }
    e->remaining_ops--;
    spin_unlock(&e->lock);
    return 0;
}

/* A5: authorities without an fd identity (shm segments attach by address). */
int env_mediate_class(uint8_t obj_type)
{
    task_t *t = proc_current();
    a20_envelope_t *e =
        __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
    if (!e)
        return 0;

    int rc = env_check_expired(e);
    if (rc)
        return rc;

    spin_lock(&e->lock);
    if (!(e->policy.allowed_types & (1u << obj_type))) {
        e->stats.acquire_deny_type++;
        spin_unlock(&e->lock);
        return -EPERM;
    }
    if (e->remaining_ops == 0) {
        e->stats.use_deny_ops++;
        spin_unlock(&e->lock);
        return -EACCES;
    }
    e->remaining_ops--;
    spin_unlock(&e->lock);
    return 0;
}

/* ---- Use mediation ------------------------------------------------------ */

int env_mediate_use_dir(int gfd, size_t bytes, int want_write)
{
    task_t *t = proc_current();
    a20_envelope_t *e =
        __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
    if (!e)
        return 0;

    int rc = env_check_expired(e);
    if (rc)
        return rc;

    spin_lock(&e->lock);

    bool tracked = false;
    uint64_t shadow_rights = 0;
    for (int i = 0; i < A20_ENV_MAX_SHADOWS; i++) {
        if (e->shadows[i].gfd == gfd) {
            tracked = true;
            shadow_rights = e->shadows[i].rights;
            break;
        }
    }

    /*
     * Grandfathered authorities (inherited fds from before enter()) are not
     * in the shadow table: they stay allowed but budget-accounted, and no
     * direction requirement can be applied to them.  Tracked authorities
     * must hold the direction bit (05 §2.5.1 A8 keeps downgraded reopens
     * from being written).
     */
    if (tracked && want_write >= 0) {
        uint64_t need =
            want_write == 1 ? A20_RIGHT_WRITE : A20_RIGHT_READ;
        if (!(shadow_rights & need)) {
            e->stats.use_deny_rights++;
            spin_unlock(&e->lock);
            return -EACCES;
        }
    }

    if (e->remaining_ops == 0) {
        e->stats.use_deny_ops++;
        spin_unlock(&e->lock);
        return -EACCES;
    }
    e->remaining_ops--;

    if (bytes > 0) {
        if (e->remaining_data < bytes) {
            e->stats.use_deny_data++;
            spin_unlock(&e->lock);
            return -EACCES;
        }
        e->remaining_data -= bytes;
        e->stats.bytes_charged += bytes;
    }
    e->stats.use_ok++;
    spin_unlock(&e->lock);
    return 0;
}

int env_mediate_use(int gfd, size_t bytes)
{
    return env_mediate_use_dir(gfd, bytes, -1);
}

/* A8: rights carried by the shadow for gfd; *tracked cleared when absent. */
uint64_t env_shadow_rights_of_gfd(int gfd, int *tracked)
{
    *tracked = 0;
    task_t *t = proc_current();
    a20_envelope_t *e =
        __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
    if (!e)
        return 0;

    spin_lock(&e->lock);
    uint64_t rights = 0;
    for (int i = 0; i < A20_ENV_MAX_SHADOWS; i++) {
        if (e->shadows[i].gfd == gfd) {
            *tracked = 1;
            rights = e->shadows[i].rights;
            break;
        }
    }
    spin_unlock(&e->lock);
    return rights;
}

/* ---- Global fd-kind registry -------------------------------------------- */

/*
 * One byte per global fd, recorded at every creation site regardless of
 * envelope state, so transferred descriptors (SCM_RIGHTS/pidfd_getfd) can
 * be classified even when sender and receiver live in different envelopes.
 * Zero means unregistered and classifies as FILE (conservative default).
 */
static uint8_t env_gfd_kind[MAX_FILES];

void env_kind_register(int gfd, uint8_t obj_type)
{
    if (gfd >= 0 && gfd < MAX_FILES)
        __atomic_store_n(&env_gfd_kind[gfd], obj_type, __ATOMIC_RELEASE);
}

uint8_t env_kind_of(int gfd)
{
    if (gfd >= 0 && gfd < MAX_FILES) {
        uint8_t k = __atomic_load_n(&env_gfd_kind[gfd], __ATOMIC_ACQUIRE);
        if (k)
            return k;
    }
    /*
     * Unregistered descriptor: SCM_RIGHTS delivery allocates a brand-new
     * global fd for the receiver, so the creation-site registration lives
     * under the sender's number.  Probe the socket table before falling
     * back to the conservative FILE default.
     */
    extern void *net_socket_from_file(int gfd);
    if (gfd >= 0 && net_socket_from_file(gfd))
        return (uint8_t)A20_OBJ_SOCKET;
    return (uint8_t)A20_OBJ_FILE;
}

/* ---- Lifetime ----------------------------------------------------------- */

bool env_active(const struct task_t *t)
{
    return __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE) != NULL;
}

void env_inherit_task(task_t *child, const task_t *parent)
{
    a20_envelope_t *e =
        __atomic_load_n(&parent->envelope, __ATOMIC_ACQUIRE);
    if (!e)
        return;
    /* Shared-root inheritance (05 §2.3 v1): fork keeps one envelope with one
     * root budget; delegation-chain dissipation is W2 work.  Monotone decay
     * is preserved globally: children can only spend down the same budget. */
    env_get_ref(e);
    __atomic_store_n(&child->envelope, e, __ATOMIC_RELEASE);
}

void env_release_task(task_t *t)
{
    a20_envelope_t *e =
        __atomic_exchange_n(&t->envelope, NULL, __ATOMIC_ACQ_REL);
    if (e)
        env_put_ref(e);
}

/* ---- Control plane (sys_a20_bridge.c calls these) ----------------------- */

int64_t env_create(const a20_env_policy_t *policy, uint32_t flags)
{
    if (!policy)
        return -EFAULT;
    a20_env_policy_t p;
    if (copy_from_user(&p, policy, sizeof(p)) < 0)
        return -EFAULT;

    /* Sanity: unknown type bits beyond the enum range are rejected so a
     * buggy supervisor cannot pre-authorize unmapped classes. */
    if ((uint64_t)p.allowed_types >> 17)
        return -EINVAL;

    a20_envelope_t *e = kmalloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    memset(e, 0, sizeof(*e));
    refcount_set(&e->refs, 1);
    e->lock = (spinlock_t)SPINLOCK_INIT;
    e->policy = p;
    e->expire_tick = env_ns_to_ticks(p.time_budget_ns)
                         ? timer_get_ticks() + env_ns_to_ticks(p.time_budget_ns)
                         : 0;
    e->remaining_ops = p.op_budget;
    e->remaining_data = p.data_budget;
    for (int i = 0; i < A20_ENV_MAX_SHADOWS; i++)
        e->shadows[i].gfd = -1;
    e->owner_pid = proc_current()->pid;
    (void)flags;

    spin_lock(&env_registry_lock);
    int slot = -1;
    for (int i = 0; i < A20_ENV_MAX_INSTANCES; i++) {
        if (!env_registry[i]) {
            slot = i;
            break;
        }
    }
    if (slot >= 0)
        env_registry[slot] = e;
    spin_unlock(&env_registry_lock);

    if (slot < 0) {
        kfree(e);
        return -EBUSY;
    }
    return slot; /* env_id */
}

int env_enter(int64_t env_id)
{
    if (env_id < 0 || env_id >= A20_ENV_MAX_INSTANCES)
        return -EINVAL;
    spin_lock(&env_registry_lock);
    a20_envelope_t *e = env_registry[env_id];
    if (e)
        env_get_ref(e);
    spin_unlock(&env_registry_lock);
    if (!e)
        return -ENOENT;

    if (env_check_expired(e)) {
        env_put_ref(e);
        return -EACCES;
    }

    task_t *t = proc_current();
    void *expected = NULL;
    if (!__atomic_compare_exchange_n(&t->envelope, &expected, e, false,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        env_put_ref(e);
        return -EINVAL; /* already enveloped: envelopes are monotone */
    }
    /* Close the revoke/expiry race between the pre-attach check and CAS.
     * A task never returns successfully while attached to an expired root. */
    if (env_check_expired(e)) {
        __atomic_store_n(&t->envelope, NULL, __ATOMIC_RELEASE);
        env_put_ref(e);
        return -EACCES;
    }
    return 0;
}

int env_revoke(int64_t env_id)
{
    if (env_id < 0 || env_id >= A20_ENV_MAX_INSTANCES)
        return -EINVAL;
    spin_lock(&env_registry_lock);
    a20_envelope_t *e = env_registry[env_id];
    spin_unlock(&env_registry_lock);
    if (!e)
        return -ENOENT;
    if (e->owner_pid != proc_current()->pid &&
        proc_current()->cred.uid != 0)
        return -EPERM;

    spin_lock(&e->lock);
    env_mark_expired_locked(e);
    spin_unlock(&e->lock);

    if (e->policy.flags & A20_ENV_F_KILL_ON_EXPIRE) {
        if (__atomic_exchange_n(&e->kill_sent, 1, __ATOMIC_ACQ_REL) == 0)
            env_kill_tasks(e);
    }
    return 0;
}

int env_stats(int64_t env_id, a20_env_stats_t *out)
{
    if (env_id < 0 || env_id >= A20_ENV_MAX_INSTANCES)
        return -EINVAL;
    if (!out)
        return -EFAULT;
    spin_lock(&env_registry_lock);
    a20_envelope_t *e = env_registry[env_id];
    spin_unlock(&env_registry_lock);
    if (!e)
        return -ENOENT;
    if (e->owner_pid != proc_current()->pid &&
        proc_current()->cred.uid != 0)
        return -EPERM;

    a20_env_stats_t s;
    spin_lock(&e->lock);
    s = e->stats;
    s.expired = e->expired;
    int n = 0;
    for (int i = 0; i < A20_ENV_MAX_SHADOWS; i++)
        if (e->shadows[i].gfd >= 0)
            n++;
    s.n_shadows = n;
    spin_unlock(&e->lock);

    if (copy_to_user(out, &s, sizeof(s)) < 0)
        return -EFAULT;
    return 0;
}

/* ---- E8 runtime invariant audit ---------------------------------------- */

/* Caller holds env_registry_lock. */
static bool env_registry_contains(const a20_envelope_t *e)
{
    for (int i = 0; i < A20_ENV_MAX_INSTANCES; i++)
        if (env_registry[i] == e)
            return true;
    return false;
}

/* Re-verifies the TLA+-checked invariants (TypeAllowed, RightsSubCap,
 * budget bounds) plus task-attachment consistency across every live
 * envelope.  Violations are aggregated in *r and logged. */
int64_t env_audit(struct a20_env_audit *out)
{
    struct a20_env_audit r;
    memset(&r, 0, sizeof(r));

    spin_lock(&env_registry_lock);
    for (int i = 0; i < A20_ENV_MAX_INSTANCES; i++) {
        a20_envelope_t *e = env_registry[i];
        if (!e)
            continue;
        spin_lock(&e->lock);

        r.n_envelopes++;
        if (e->policy.op_budget && e->remaining_ops > e->policy.op_budget)
            r.v_budget_nonneg++;
        if (e->policy.data_budget &&
            e->remaining_data > e->policy.data_budget)
            r.v_budget_nonneg++;

        for (int j = 0; j < A20_ENV_MAX_SHADOWS; j++) {
            const env_shadow_t *s = &e->shadows[j];
            if (s->gfd < 0)
                continue;
            r.n_shadows++;
            if (!(e->policy.allowed_types & (1u << s->obj_type))) {
                r.v_type_allowed++;
                continue;
            }
            if (s->rights & ~e->policy.rights_by_class[s->obj_type])
                r.v_rights_sub_cap++;
        }
        spin_unlock(&e->lock);
    }
    spin_unlock(&env_registry_lock);

    /* Attachment consistency: every task envelope pointer must name a
     * registered instance. */
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t;
         t = proc_next_task_locked(t)) {
        a20_envelope_t *evp =
            __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
        if (!evp)
            continue;
        r.n_attached_tasks++;
        if (!env_registry_contains(evp))
            r.v_task_dangling++;
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    r.violations = r.v_type_allowed + r.v_rights_sub_cap +
                   r.v_budget_nonneg + r.v_task_dangling;

    if (out && copy_to_user(out, &r, sizeof(r)) < 0)
        return -EFAULT;
    if (r.violations)
        kerr("ENV_AUDIT: %u violations (%u type, %u rights, %u budget, "
             "%u dangling)\n", r.violations, r.v_type_allowed,
             r.v_rights_sub_cap, r.v_budget_nonneg, r.v_task_dangling);
    return 0;
}
