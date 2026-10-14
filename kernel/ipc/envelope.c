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
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "net/socket.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "sys/usercopy.h"
#include "mm/slab.h"
#ifdef CONFIG_ENVELOPE_POLICY_RUST
#include "ipc/envelope_policy.h"
_Static_assert(A20_RIGHT_READ == 1ull,
               "Rust envelope policy READ bit must match the C ABI");
_Static_assert(A20_RIGHT_WRITE == 2ull,
               "Rust envelope policy WRITE bit must match the C ABI");
#endif

#define ENV_NS_PER_SEC 1000000000ull

#ifdef CONFIG_ENVELOPE_POLICY_RUST
typedef a20_env_shadow_state_t env_shadow_t;
_Static_assert(sizeof(env_shadow_t) == 24,
               "Rust envelope shadow size must match C");
_Static_assert(__builtin_offsetof(env_shadow_t, rights) == 16,
               "Rust envelope shadow rights offset must match C");
#else
typedef struct env_shadow {
    uint64_t vfile_identity; /* zero = free; stable through dup/fork aliases */
    uint8_t  obj_type;
    uint8_t  reserved[7];
    uint64_t rights;
} env_shadow_t;
_Static_assert(sizeof(env_shadow_t) == 24, "C envelope shadow ABI");
_Static_assert(__builtin_offsetof(env_shadow_t, rights) == 16,
               "C envelope shadow rights offset");
#endif

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
    task_t  *owner_task;     /* stable creator identity; pid may be reused */

    env_shadow_t shadows[A20_ENV_MAX_SHADOWS];

    /* Observable counters (relaxed; research telemetry, not control state). */
    a20_env_stats_t stats;
} a20_envelope_t;

/* ---- Registry ---------------------------------------------------------- */

#ifdef CONFIG_ENVELOPE_POLICY_RUST
typedef a20_env_registry_slot_t env_registry_slot_t;
#else
typedef struct env_registry_slot {
    a20_envelope_t *ptr;
    task_t *owner_task;
    uint32_t generation;
    int32_t owner_pid;
    uint8_t owner_live;
    uint8_t retired;
    uint16_t reserved;
} env_registry_slot_t;
_Static_assert(__builtin_offsetof(env_registry_slot_t, owner_live) ==
                   2 * sizeof(void *) + 8, "C envelope registry ABI");
_Static_assert(sizeof(env_registry_slot_t) ==
                   (((2 * sizeof(void *) + 12 + __alignof__(void *) - 1) /
                     __alignof__(void *)) * __alignof__(void *)),
               "C envelope registry slot size");
#endif
static env_registry_slot_t env_registry[A20_ENV_MAX_INSTANCES];
static spinlock_t env_registry_lock = SPINLOCK_INIT;

/* Registry entries are weak indexes. The creator owns the initial envelope
 * reference; attached tasks own additional references. C holds this lock for
 * every registry transition and pins an object before returning it. */
static int64_t env_registry_insert_locked(a20_envelope_t *e, task_t *owner)
{
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    return a20_envelope_registry_insert(env_registry, A20_ENV_MAX_INSTANCES,
                                        e, owner, owner->pid);
#else
    for (uint32_t i = 0; i < A20_ENV_MAX_INSTANCES; i++) {
        env_registry_slot_t *slot = &env_registry[i];
        if (!slot->ptr && !slot->retired) {
            slot->ptr = e;
            slot->owner_task = owner;
            slot->owner_pid = owner->pid;
            slot->owner_live = 1;
            return (int64_t)((slot->generation << 10) | i);
        }
    }
    return -EBUSY;
#endif
}

static a20_envelope_t *env_registry_lookup_locked(int64_t id,
                                                   bool require_owner)
{
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    return a20_envelope_registry_lookup(env_registry, A20_ENV_MAX_INSTANCES,
                                        id, require_owner ? 1 : 0);
#else
    if (id < 0 || (uint64_t)id > 0x7fffffffULL)
        return NULL;
    uint32_t raw = (uint32_t)id;
    uint32_t index = raw & (A20_ENV_MAX_INSTANCES - 1);
    env_registry_slot_t *slot = &env_registry[index];
    if (!slot->ptr || slot->generation != (raw >> 10) ||
        (require_owner && !slot->owner_live))
        return NULL;
    return slot->ptr;
#endif
}

static a20_envelope_t *env_registry_at_locked(uint32_t index)
{
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    return a20_envelope_registry_at(env_registry, A20_ENV_MAX_INSTANCES,
                                    index);
#else
    return index < A20_ENV_MAX_INSTANCES ? env_registry[index].ptr : NULL;
#endif
}

static a20_envelope_t *env_registry_close_owner_locked(task_t *owner)
{
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    return a20_envelope_registry_close_owner(env_registry,
                                             A20_ENV_MAX_INSTANCES,
                                             owner);
#else
    for (uint32_t i = 0; i < A20_ENV_MAX_INSTANCES; i++) {
        env_registry_slot_t *slot = &env_registry[i];
        if (slot->ptr && slot->owner_live && slot->owner_task == owner) {
            slot->owner_live = 0;
            return slot->ptr;
        }
    }
    return NULL;
#endif
}

static int env_registry_remove_locked(a20_envelope_t *e)
{
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    return a20_envelope_registry_remove(env_registry, A20_ENV_MAX_INSTANCES,
                                        e);
#else
    for (uint32_t i = 0; i < A20_ENV_MAX_INSTANCES; i++) {
        env_registry_slot_t *slot = &env_registry[i];
        if (slot->ptr == e) {
            if (slot->owner_live)
                return -EBUSY;
            slot->ptr = NULL;
            slot->owner_task = NULL;
            slot->owner_pid = 0;
            if (slot->generation == (0x7fffffffU >> 10))
                slot->retired = 1;
            else
                slot->generation++;
            return 0;
        }
    }
    return -ENOENT;
#endif
}

static int env_ns_to_ticks(uint64_t ns)
{
    if (ns == 0)
        return 0;
    return ns / (ENV_NS_PER_SEC / TICKS_PER_SEC) + 1;
}

static void env_free(a20_envelope_t *e)
{
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    /* Final-reference teardown is the only safe whole-table deletion point.
     * Per-numeric-fd deletion is ambiguous across shared task tables/aliases. */
    (void)a20_envelope_policy_shadow_clear(e->shadows,
                                           A20_ENV_MAX_SHADOWS);
#endif
    kfree(e);
}

static a20_envelope_t *env_get_ref(a20_envelope_t *e)
{
    refcount_inc(&e->refs);
    return e;
}

static void env_put_ref(a20_envelope_t *e)
{
    /* Serialize the zero transition with weak-index lookup/pinning. Otherwise
     * a lookup could increment a refcount that has already reached zero. */
    spin_lock(&env_registry_lock);
    bool destroy = refcount_dec_and_test(&e->refs);
    if (destroy)
        (void)env_registry_remove_locked(e);
    spin_unlock(&env_registry_lock);
    if (destroy)
        env_free(e);
}

/* Drop the creator reference for every envelope owned by an exiting task.
 * The temporary pin closes the weak-index race while the registry lock is
 * dropped; the two puts below release creator and temporary references. */
static void env_release_owner(task_t *owner)
{
    for (;;) {
        spin_lock(&env_registry_lock);
        a20_envelope_t *e = env_registry_close_owner_locked(owner);
        if (e)
            env_get_ref(e);
        spin_unlock(&env_registry_lock);
        if (!e)
            break;
        env_put_ref(e); /* creator reference */
        env_put_ref(e); /* temporary pin */
    }
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
     * under tasklist_lock (task-list membership) and signalled after release —
     * signal_send takes its own locks, and the documented lock order does
     * not include signal locks under tasklist_lock. */
    enum { KILL_BATCH = 32 };
    int cursor = -1;

    for (;;) {
        int pids[KILL_BATCH];
        int n = 0;
        uint64_t flags = spin_lock_irqsave(&tasklist_lock);
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
        spin_unlock_irqrestore(&tasklist_lock, flags);

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
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    if (a20_envelope_policy_expire(&e->expired, e->expire_tick,
                                   timer_get_ticks()) != 0)
        deny = -EACCES;
#else
    if (!e->expired && e->expire_tick != 0 &&
        timer_get_ticks() >= e->expire_tick)
        env_mark_expired_locked(e);
    if (e->expired)
        deny = -EACCES;
#endif
    spin_unlock(&e->lock);

    if (deny && (e->policy.flags & A20_ENV_F_KILL_ON_EXPIRE)) {
        if (__atomic_exchange_n(&e->kill_sent, 1, __ATOMIC_ACQ_REL) == 0)
            env_kill_tasks(e);
    }
    return deny;
}

/* Caller holds e->lock. Rows are keyed by stable vfile identity. The first
 * grant is immutable: reacquiring an alias cannot widen or narrow authority
 * already shared by dup/fork aliases. */
static int env_acquire_charge_install_locked(a20_envelope_t *e,
                                             uint8_t obj_type,
                                             uint64_t rights,
                                             uint64_t vfile_identity)
{
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    int rc = a20_envelope_policy_acquire_shadow(
        &e->remaining_ops, e->shadows, A20_ENV_MAX_SHADOWS,
        vfile_identity,
        obj_type, rights);
    if (rc == -EACCES)
        e->stats.use_deny_ops++;
    if (rc == 0)
        e->stats.acquire_ok++;
    return rc;
#else
    if (e->remaining_ops == 0) {
        e->stats.use_deny_ops++;
        return -EACCES;
    }
    e->remaining_ops--;

    int slot = -1;
    for (int i = 0; i < A20_ENV_MAX_SHADOWS; i++) {
        if (e->shadows[i].vfile_identity == vfile_identity) {
            e->stats.acquire_ok++;
            return 0;
        }
        if (slot < 0 && e->shadows[i].vfile_identity == 0)
            slot = i;
    }
    if (slot < 0)
        return -EMFILE;
    e->shadows[slot].vfile_identity = vfile_identity;
    e->shadows[slot].obj_type = obj_type;
    memset(e->shadows[slot].reserved, 0, sizeof(e->shadows[slot].reserved));
    e->shadows[slot].rights = rights;
    e->stats.acquire_ok++;
    return 0;
#endif
}

/* ---- Acquisition mediation --------------------------------------------- */

typedef struct env_acquire_vfile_context {
    a20_envelope_t *envelope;
    uint64_t requested_rights;
    int expected_kind; /* -1 derives from the pinned vfile */
    bool clamp_rights; /* transfer grants are intersected with the class cap */
} env_acquire_vfile_context_t;

static int env_charge_acquire_vfile_locked(vfile_t *vf, void *opaque)
{
    env_acquire_vfile_context_t *ctx = opaque;
    a20_envelope_t *e = ctx->envelope;
    uint8_t kind = env_kind_of_vfile(vf);
    if (ctx->expected_kind >= 0 && kind != (uint8_t)ctx->expected_kind)
        return -EBADF;

    int rc;
    spin_lock(&e->lock);

#ifdef CONFIG_ENVELOPE_POLICY_RUST
    if (a20_envelope_policy_expire(&e->expired, e->expire_tick,
                                   timer_get_ticks()) != 0) {
        rc = -EACCES;
        goto out;
    }
#else
    if (!e->expired && e->expire_tick != 0 &&
        timer_get_ticks() >= e->expire_tick)
        env_mark_expired_locked(e);
    if (e->expired) {
        rc = -EACCES;
        goto out;
    }
#endif

    if (!(e->policy.allowed_types & (1u << kind))) {
        e->stats.acquire_deny_type++;
        rc = -EPERM;
        goto out;
    }
    uint64_t cap = e->policy.rights_by_class[kind];
    uint64_t requested = ctx->requested_rights;
    if (ctx->clamp_rights && kind == A20_OBJ_SOCKET)
        requested |= A20_RIGHT_CONNECT | A20_RIGHT_ACCEPT;
    if (!ctx->clamp_rights && (requested & ~cap) != 0) {
        e->stats.acquire_deny_rights++;
        rc = -EACCES;
        goto out;
    }
    uint64_t granted = ctx->clamp_rights ? requested & cap : requested;
    if (ctx->clamp_rights && granted == 0) {
        e->stats.acquire_deny_rights++;
        rc = -EACCES;
        goto out;
    }
    rc = env_acquire_charge_install_locked(e, kind, granted, vf->identity);
out:
    spin_unlock(&e->lock);
    return rc;
}

static int env_mediate_acquire_pinned(int local_fd, vfile_t *pinned_vf,
                                      int expected_kind,
                                      uint64_t requested_rights,
                                      bool clamp_rights)
{
    if (local_fd < 0 || !pinned_vf)
        return -EBADF;
    task_t *t = proc_current();
    a20_envelope_t *e = __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
    if (!e)
        return 0;
    int rc = env_check_expired(e);
    if (rc)
        return rc;
    env_acquire_vfile_context_t ctx = {
        .envelope = e,
        .requested_rights = requested_rights,
        .expected_kind = expected_kind,
        .clamp_rights = clamp_rights,
    };
    rc = fdtable_with_current_vfile(local_fd, pinned_vf,
                                    env_charge_acquire_vfile_locked, &ctx);
    if (rc == -EACCES)
        (void)env_check_expired(e);
    return rc;
}

int env_mediate_acquire_vfile(int local_fd, struct vfile *pinned_vf,
                              uint64_t requested_rights)
{
    return env_mediate_acquire_pinned(local_fd, (vfile_t *)pinned_vf, -1,
                                      requested_rights, true);
}

int env_mediate_acquire(uint8_t obj_type, uint64_t rights, int gfd)
{
    if (obj_type == A20_OBJ_INVALID || obj_type >= 32)
        return -EINVAL;
    vfile_t *vf = fdtable_get_current_file_ref(gfd);
    if (!vf)
        return -EBADF;
    int rc = env_mediate_acquire_pinned(gfd, vf, obj_type, rights, false);
    vfs_put_file(vf);
    return rc;
}

/*
 * A6/A7 transfer acquisition: an authority created elsewhere enters this
 * task through SCM_RIGHTS receipt or pidfd_getfd.  The grant is clamped to
 * the policy class cap; an empty grant denies.
 */
int env_mediate_acquire_gfd(int gfd)
{
    vfile_t *vf = fdtable_get_current_file_ref(gfd);
    if (!vf)
        return -EBADF;
    const uint64_t base = A20_RIGHT_READ | A20_RIGHT_WRITE |
                          A20_RIGHT_STAT | A20_RIGHT_SEEK;
    int rc = env_mediate_acquire_pinned(gfd, vf, -1, base, true);
    vfs_put_file(vf);
    return rc;
}

int env_mediate_acquire_socket(uint64_t rights, int gfd)
{
    return env_mediate_acquire((uint8_t)A20_OBJ_SOCKET, rights, gfd);
}

/* A6 send side: authorize the exact local descriptor binding that supplied
 * the pinned vfile. fdtable_with_current_vfile holds files_struct.lock while
 * this callback checks the immutable vfile class and charges the envelope.
 * Keep this callback allocation-, sleep-, VFS-, and signal-free. */
typedef struct env_send_vfile_context {
    a20_envelope_t *envelope;
} env_send_vfile_context_t;

static int env_charge_send_vfile_locked(vfile_t *vf, void *opaque)
{
    env_send_vfile_context_t *ctx = opaque;
    a20_envelope_t *e = ctx->envelope;
    uint8_t kind = env_kind_of_vfile(vf);
    int rc;

    spin_lock(&e->lock);
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    rc = a20_envelope_policy_charge_propagation_at(
        &e->expired, e->expire_tick, timer_get_ticks(),
        e->policy.propagation_types, kind, &e->remaining_ops);
    if (rc != 0 && rc != -EPERM && !e->expired)
        e->stats.use_deny_ops++;
#else
    if (!e->expired && e->expire_tick != 0 &&
        timer_get_ticks() >= e->expire_tick)
        env_mark_expired_locked(e);
    if (e->expired) {
        rc = -EACCES;
        goto out;
    }
    if (!(e->policy.propagation_types & (1u << kind))) {
        rc = -EPERM;
        goto out;
    }
    if (e->remaining_ops == 0) {
        e->stats.use_deny_ops++;
        rc = -EACCES;
        goto out;
    }
    e->remaining_ops--;
    rc = 0;
#endif
#ifndef CONFIG_ENVELOPE_POLICY_RUST
out:
#endif
    spin_unlock(&e->lock);
    return rc;
}

int env_mediate_send_vfile(int local_fd, struct vfile *pinned_vf)
{
    if (local_fd < 0 || !pinned_vf)
        return -EBADF;
    task_t *t = proc_current();
    a20_envelope_t *e = __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
    if (!e)
        return 0;

    /* Fast preflight may run expiry side effects before taking files.lock. The
     * callback repeats the deadline check to close the preflight/charge race. */
    int rc = env_check_expired(e);
    if (rc)
        return rc;
    env_send_vfile_context_t ctx = { .envelope = e };
    rc = fdtable_with_current_vfile(local_fd, pinned_vf,
                                    env_charge_send_vfile_locked, &ctx);
    /* KILL_ON_EXPIRE can signal/scan tasks, so perform it only after the
     * fdtable callback has dropped files.lock and envelope.lock. */
    if (rc == -EACCES)
        (void)env_check_expired(e);
    return rc;
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
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    int charge = a20_envelope_policy_charge_propagation(
        e->policy.propagation_types, kind, &e->remaining_ops);
    if (charge != 0) {
        if (charge != -EPERM)
            e->stats.use_deny_ops++;
        spin_unlock(&e->lock);
        return charge;
    }
#else
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
#endif
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

typedef struct env_use_vfile_context {
    a20_envelope_t *envelope;
    size_t bytes;
    int want_write;
} env_use_vfile_context_t;

static int env_charge_use_vfile_locked(vfile_t *vf, void *opaque)
{
    env_use_vfile_context_t *ctx = opaque;
    a20_envelope_t *e = ctx->envelope;
    uint64_t identity = vf->identity;
    int rc = 0;
    spin_lock(&e->lock);

#ifdef CONFIG_ENVELOPE_POLICY_RUST
    if (a20_envelope_policy_expire(&e->expired, e->expire_tick,
                                   timer_get_ticks()) != 0) {
        rc = -EACCES;
        goto out;
    }
#else
    if (!e->expired && e->expire_tick != 0 &&
        timer_get_ticks() >= e->expire_tick)
        env_mark_expired_locked(e);
    if (e->expired) {
        rc = -EACCES;
        goto out;
    }
#endif

    bool tracked = false;
    uint64_t shadow_rights = 0;
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    int32_t rust_tracked = 0;
    if (a20_envelope_policy_shadow_lookup(e->shadows,
                                           A20_ENV_MAX_SHADOWS, identity,
                                           &shadow_rights,
                                           &rust_tracked) != 0) {
        rc = -EACCES;
        goto out;
    }
    tracked = rust_tracked != 0;
#else
    for (int i = 0; i < A20_ENV_MAX_SHADOWS; i++) {
        if (e->shadows[i].vfile_identity == identity) {
            tracked = true;
            shadow_rights = e->shadows[i].rights;
            break;
        }
    }
#endif

    /* Grandfathered authorities have no identity row and stay permitted but
     * budget-accounted. Tracked grants follow the open-file object through
     * aliases; a different object reusing the same local fd is never tracked. */
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    int decision = a20_envelope_policy_charge_use(
        &e->remaining_ops, &e->remaining_data, shadow_rights,
        tracked ? 1 : 0, ctx->want_write, (uint64_t)ctx->bytes);
    if (decision == A20_ENV_POLICY_DENY_RIGHTS) {
        e->stats.use_deny_rights++;
        rc = -EACCES;
        goto out;
    }
    if (decision == A20_ENV_POLICY_DENY_OPS) {
        e->stats.use_deny_ops++;
        rc = -EACCES;
        goto out;
    }
    if (decision == A20_ENV_POLICY_DENY_DATA) {
        e->stats.use_deny_data++;
        rc = -EACCES;
        goto out;
    }
    if (decision != A20_ENV_POLICY_DENY_NONE) {
        rc = -EACCES;
        goto out;
    }
#else
    if (tracked && ctx->want_write >= 0) {
        uint64_t need = ctx->want_write ? A20_RIGHT_WRITE : A20_RIGHT_READ;
        if (!(shadow_rights & need)) {
            e->stats.use_deny_rights++;
            rc = -EACCES;
            goto out;
        }
    }
    if (e->remaining_ops == 0) {
        e->stats.use_deny_ops++;
        rc = -EACCES;
        goto out;
    }
    e->remaining_ops--;
    if (ctx->bytes > 0) {
        if (e->remaining_data < ctx->bytes) {
            e->stats.use_deny_data++;
            rc = -EACCES;
            goto out;
        }
        e->remaining_data -= ctx->bytes;
    }
#endif
    e->stats.bytes_charged += ctx->bytes;
    e->stats.use_ok++;
out:
    spin_unlock(&e->lock);
    return rc;
}

int env_mediate_use_dir(int gfd, size_t bytes, int want_write)
{
    task_t *t = proc_current();
    a20_envelope_t *e = __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
    if (!e)
        return 0;
    int rc = env_check_expired(e);
    if (rc)
        return rc;
    vfile_t *vf = fdtable_get_current_file_ref(gfd);
    if (!vf)
        return -EBADF;
    env_use_vfile_context_t ctx = {
        .envelope = e,
        .bytes = bytes,
        .want_write = want_write,
    };
    rc = fdtable_with_current_vfile(gfd, vf,
                                    env_charge_use_vfile_locked, &ctx);
    if (rc == -EACCES)
        (void)env_check_expired(e);
    vfs_put_file(vf);
    return rc;
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

    vfile_t *vf = fdtable_get_current_file_ref(gfd);
    if (!vf)
        return 0;

    spin_lock(&e->lock);
    uint64_t rights = 0;
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    int32_t rust_tracked = 0;
    if (a20_envelope_policy_shadow_lookup(e->shadows,
                                           A20_ENV_MAX_SHADOWS,
                                           vf->identity,
                                           &rights, &rust_tracked) == 0)
        *tracked = rust_tracked;
#else
    for (int i = 0; i < A20_ENV_MAX_SHADOWS; i++) {
        if (e->shadows[i].vfile_identity == vf->identity) {
            *tracked = 1;
            rights = e->shadows[i].rights;
            break;
        }
    }
#endif
    spin_unlock(&e->lock);
    vfs_put_file(vf);
    return rights;
}

/* ---- Vfile-bound object classification --------------------------------- */

/* Tag while a vfile is still private to its creator, before fdtable install.
 * A vfile is the shared open-file identity across dup/fork aliases, while fd
 * numbers are task-local and reusable. The tag therefore lives for the
 * vfile's lifetime and is never cleared by closing one descriptor alias. */
int env_kind_tag_vfile(struct vfile *vf, uint8_t obj_type)
{
    if (!vf || obj_type <= A20_OBJ_INVALID || obj_type >= 32)
        return -EINVAL;
    uint8_t current = __atomic_load_n(&vf->envelope_kind, __ATOMIC_ACQUIRE);
    for (;;) {
        if (current == obj_type)
            return 0;
        if (current != 0)
            return -EEXIST;
        uint8_t expected = 0;
        if (__atomic_compare_exchange_n(&vf->envelope_kind, &expected,
                                       obj_type, false, __ATOMIC_RELEASE,
                                       __ATOMIC_ACQUIRE))
            return 0;
        current = expected;
    }
}

uint8_t env_kind_of_vfile(const struct vfile *vf)
{
    if (!vf)
        return (uint8_t)A20_OBJ_FILE;
    uint8_t kind = __atomic_load_n(&vf->envelope_kind, __ATOMIC_ACQUIRE);
    if (kind > A20_OBJ_INVALID && kind < 32)
        return kind;

    /* Socket vfiles are self-identifying by immutable ops/private state. Do
     * not resolve a descriptor number here: the caller already owns vf. */
    if (net_is_socket_vfile((struct vfile *)vf))
        return (uint8_t)A20_OBJ_SOCKET;
    return (uint8_t)A20_OBJ_FILE;
}

void env_kind_register(int gfd, uint8_t obj_type)
{
    /* Kept while syscall adapters migrate to constructor-owned tags. The
     * numeric slot may be task-local and recycled, so it cannot own class. */
    (void)gfd;
    (void)obj_type;
}

void env_kind_unregister(int gfd)
{
    /* No-op by design: closing one fd alias must not clear metadata on a
     * still-referenced vfile. Keep the legacy hook for fdtable callers. */
    (void)gfd;
}

uint8_t env_kind_of(int gfd)
{
    if (gfd < 0 || gfd >= MAX_FILES)
        return (uint8_t)A20_OBJ_FILE;
    vfile_t *vf = fdtable_get_current_file_ref(gfd);
    if (!vf)
        return (uint8_t)A20_OBJ_FILE;
    uint8_t kind = env_kind_of_vfile(vf);
    vfs_put_file(vf);
    return kind;
}

/* ---- Lifetime ----------------------------------------------------------- */

bool env_active(const struct task_t *t)
{
    return __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE) != NULL;
}

void env_inherit_task(task_t *child, const task_t *parent)
{
    spin_lock(&env_registry_lock);
    a20_envelope_t *e =
        __atomic_load_n(&parent->envelope, __ATOMIC_ACQUIRE);
    if (e)
        env_get_ref(e);
    spin_unlock(&env_registry_lock);
    if (!e)
        return;
    /* Shared-root inheritance (05 §2.3 v1): fork keeps one envelope with one
     * root budget; delegation-chain dissipation is W2 work.  Monotone decay
     * is preserved globally: children can only spend down the same budget. */
    __atomic_store_n(&child->envelope, e, __ATOMIC_RELEASE);
}

void env_release_task(task_t *t)
{
    a20_envelope_t *e =
        __atomic_exchange_n(&t->envelope, NULL, __ATOMIC_ACQ_REL);
    if (e)
        env_put_ref(e);
    env_release_owner(t);
}

void env_vfile_retire(struct vfile *vf)
{
    if (!vf || vf->identity == 0)
        return;
    uint64_t identity = vf->identity;

    /* Lock order: registry_lock -> envelope.lock. The registry is weak, but
     * its lock prevents a zero-ref envelope from being removed/freed while
     * its row is visited. A final vfile release occurs outside files.lock. */
    spin_lock(&env_registry_lock);
    for (uint32_t i = 0; i < A20_ENV_MAX_INSTANCES; i++) {
        a20_envelope_t *e = env_registry_at_locked(i);
        if (!e)
            continue;
        spin_lock(&e->lock);
#ifdef CONFIG_ENVELOPE_POLICY_RUST
        (void)a20_envelope_policy_shadow_remove(
            e->shadows, A20_ENV_MAX_SHADOWS, identity);
#else
        for (uint32_t j = 0; j < A20_ENV_MAX_SHADOWS; j++) {
            env_shadow_t *shadow = &e->shadows[j];
            if (shadow->vfile_identity != identity)
                continue;
            shadow->vfile_identity = 0;
            shadow->obj_type = 0;
            memset(shadow->reserved, 0, sizeof(shadow->reserved));
            shadow->rights = 0;
        }
#endif
        spin_unlock(&e->lock);
    }
    spin_unlock(&env_registry_lock);
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
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    if (a20_envelope_policy_shadow_init(e->shadows,
                                        A20_ENV_MAX_SHADOWS) != 0) {
        kfree(e);
        return -EINVAL;
    }
#else
    for (int i = 0; i < A20_ENV_MAX_SHADOWS; i++)
        e->shadows[i].vfile_identity = 0;
#endif
    e->owner_task = proc_current();
    e->owner_pid = proc_current()->pid;
    (void)flags;

    spin_lock(&env_registry_lock);
    int64_t env_id = env_registry_insert_locked(e, e->owner_task);
    spin_unlock(&env_registry_lock);

    if (env_id < 0) {
        kfree(e);
        return env_id;
    }
    return env_id;
}

int env_enter(int64_t env_id)
{
    if (env_id < 0 || (uint64_t)env_id > 0x7fffffffULL)
        return -EINVAL;
    spin_lock(&env_registry_lock);
    a20_envelope_t *e = env_registry_lookup_locked(env_id, true);
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
    if (env_id < 0 || (uint64_t)env_id > 0x7fffffffULL)
        return -EINVAL;
    task_t *caller = proc_current();
    bool denied = false;
    spin_lock(&env_registry_lock);
    a20_envelope_t *e = env_registry_lookup_locked(env_id, true);
    if (e && e->owner_task != caller && caller->cred.uid != 0) {
        denied = true;
        e = NULL;
    } else if (e) {
        env_get_ref(e);
    }
    spin_unlock(&env_registry_lock);
    if (!e)
        return denied ? -EPERM : -ENOENT;

    spin_lock(&e->lock);
    env_mark_expired_locked(e);
    spin_unlock(&e->lock);

    if (e->policy.flags & A20_ENV_F_KILL_ON_EXPIRE) {
        if (__atomic_exchange_n(&e->kill_sent, 1, __ATOMIC_ACQ_REL) == 0)
            env_kill_tasks(e);
    }
    env_put_ref(e);
    return 0;
}

int env_stats(int64_t env_id, a20_env_stats_t *out)
{
    if (env_id < 0 || (uint64_t)env_id > 0x7fffffffULL)
        return -EINVAL;
    if (!out)
        return -EFAULT;
    task_t *caller = proc_current();
    bool denied = false;
    spin_lock(&env_registry_lock);
    a20_envelope_t *e = env_registry_lookup_locked(env_id, true);
    if (e && e->owner_task != caller && caller->cred.uid != 0) {
        denied = true;
        e = NULL;
    } else if (e) {
        env_get_ref(e);
    }
    spin_unlock(&env_registry_lock);
    if (!e)
        return denied ? -EPERM : -ENOENT;

    a20_env_stats_t s;
    spin_lock(&e->lock);
    s = e->stats;
    s.expired = e->expired;
#ifdef CONFIG_ENVELOPE_POLICY_RUST
    int n = a20_envelope_policy_shadow_count(e->shadows,
                                              A20_ENV_MAX_SHADOWS);
    s.n_shadows = n < 0 ? 0 : n;
#else
    int n = 0;
    for (int i = 0; i < A20_ENV_MAX_SHADOWS; i++)
        if (e->shadows[i].vfile_identity != 0)
            n++;
    s.n_shadows = n;
#endif
    spin_unlock(&e->lock);

    env_put_ref(e);
    if (copy_to_user(out, &s, sizeof(s)) < 0)
        return -EFAULT;
    return 0;
}

/* ---- E8 runtime invariant audit ---------------------------------------- */

/* Caller holds env_registry_lock. */
static bool env_registry_contains_locked(const a20_envelope_t *e)
{
    for (int i = 0; i < A20_ENV_MAX_INSTANCES; i++)
        if (env_registry_at_locked((uint32_t)i) == e)
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
        a20_envelope_t *e = env_registry_at_locked((uint32_t)i);
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
            if (s->vfile_identity == 0)
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
    /* Attachment consistency: every task envelope pointer must name a
     * registered instance. Keep registry_lock while taking tasklist_lock so
     * weak-index membership cannot disappear between the check and use. The
     * global order is registry_lock -> tasklist_lock; no other path nests
     * these locks in the opposite order. */
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t;
         t = proc_next_task_locked(t)) {
        a20_envelope_t *evp =
            __atomic_load_n(&t->envelope, __ATOMIC_ACQUIRE);
        if (!evp)
            continue;
        r.n_attached_tasks++;
        if (!env_registry_contains_locked(evp))
            r.v_task_dangling++;
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    spin_unlock(&env_registry_lock);

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
