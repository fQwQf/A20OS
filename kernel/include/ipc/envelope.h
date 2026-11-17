/*
 * A20OS Linux ABI — Capability Envelope mediator (docs/zh/research/05).
 *
 * A capability envelope attaches a budgeted-capability policy (type x rights
 * x time x ops x data, docs/zh/research/03) to an unmodified Linux-ABI process.
 * The kernel mediates every resource acquisition (open/socket/...) and every
 * resource use (read/write/...) through env_mediate_* entry points; a NULL
 * envelope pointer on the task is the zero-cost fast path.
 *
 * ENVELOPE_MEDIATION_CONTRACT:
 *   - Every event that grants the task a new usable authority instance must
 *     pass through env_mediate_acquire_*() BEFORE the fd is installed.
 *   - Every event that consumes an authority (I/O) passes through
 *     env_mediate_use().
 *   - Envelopes survive fork (shared root budget, refcounted) and execve
 *     (a process cannot shed its envelope by changing its identity).
 *   - Expiry is atomic: once expired (root budget exhausted or revoked),
 *     every subsequent mediated operation fails; with A20_ENV_F_KILL_ON_EXPIRE
 *     attached tasks are killed at their next mediation point.
 *
 * Escape-surface semantics that v1 implements are documented in
 * docs/zh/research/05 §2.5; unmediated surfaces remaining in W2 are listed
 * there as A4-A10 follow-ups.
 */
#ifndef _ABI_LINUX_ENVELOPE_H
#define _ABI_LINUX_ENVELOPE_H

#include <stdint.h>
#include "core/defs.h"

struct task_t;
struct vfile;

/* Policy flags. */
#define A20_ENV_F_KILL_ON_EXPIRE  (1u << 0) /* kill attached tasks on expiry */
#define A20_ENV_F_MONITOR         (1u << 1) /* count/log grandfathered fds   */

/* Registry limits.  Instance slots are freed only on envelope destroy
 * (refcount zero); a supervisor that wraps many launches (CI runner,
 * corpus evaluation) holds one ref per live launch, so the cap must
 * cover a session's concurrent envelopes, not a handful. */
#define A20_ENV_MAX_INSTANCES 1024
#define A20_ENV_MAX_SHADOWS   128

/* Object types reused from the Native object model (ipc/ipc.h); the policy
 * bitmask indexes them directly: bit (1 << A20_OBJ_FILE) etc. */
#define A20_ENV_TYPE_MASK_ALL 0xFFFFFFFFu

/*
 * a20_env_policy — deployment-time policy snapshot (docs/zh/research/05 §2.1).
 * Zero budget fields mean "no limit" for that dimension.
 */
typedef struct a20_env_policy {
    uint32_t allowed_types;              /* bitmask of (1 << a20_object_type_t) */
    uint64_t rights_by_class[32];        /* per-object-type rights cap          */
    uint64_t time_budget_ns;             /* envelope lifetime                   */
    uint64_t op_budget;                  /* total mediated resource operations  */
    uint64_t data_budget;                /* bytes through read/write            */
    uint32_t propagation_types;          /* types allowed out via SCM_RIGHTS    */
    uint32_t flags;                      /* A20_ENV_F_*                         */
} a20_env_policy_t;

/* Observable counters (sys_a20_envelope_stats). */
typedef struct a20_env_stats {
    uint64_t acquire_ok;
    uint64_t acquire_deny_type;
    uint64_t acquire_deny_rights;
    uint64_t use_ok;
    uint64_t use_deny_ops;
    uint64_t use_deny_data;
    uint64_t use_deny_rights;
    uint64_t use_deny_expired;
    uint64_t bytes_charged;
    int      expired;
    int      n_shadows;
} a20_env_stats_t;

/* Runtime invariant audit (E8, docs/zh/research/10 §E8 / 08 §2.4):
 * walks every registered envelope and re-verifies the safety invariants
 * that TLA+ model checks statically (TypeAllowed, RightsSubCap, budget
 * bounds) plus task-attachment consistency. */
typedef struct a20_env_audit {
    uint32_t n_envelopes;      /* registered envelopes walked            */
    uint32_t n_attached_tasks; /* tasks holding an envelope pointer      */
    uint32_t n_shadows;        /* total live shadow entries              */
    uint32_t violations;       /* total failed checks                    */
    uint32_t v_type_allowed;   /* shadow type outside policy mask        */
    uint32_t v_rights_sub_cap; /* shadow rights exceed class cap         */
    uint32_t v_budget_nonneg;  /* counter below zero / above initial     */
    uint32_t v_task_dangling;  /* attached task whose envelope is gone   */
} a20_env_audit_t;

/* Aggregate audit over all envelopes; writes *out unless out == NULL
 * (NULL still performs the walk so violations hit klog). */
int64_t env_audit(struct a20_env_audit *out);

/* ---- Mediation fast paths (called from Linux syscall layer) ---- */

/*
 * Acquisition of a NEWLY CREATED authority (open/socket/pipe/memfd/...).
 * The object's class is carried by its vfile (ordinary files default to
 * FILE), then this runs the policy checks (type x rights x time x ops) and
 * installs a shadow entry. New callers should keep their vfile pinned and use
 * env_mediate_acquire_vfile() so a close/reuse cannot authorize a replacement.
 */
/* Legacy gfd-named entry points take a task-local descriptor index into the
 * current files_struct; they do not take a global descriptor or vfile ID. */
int env_mediate_acquire(uint8_t obj_type, uint64_t rights, int local_fd);
int env_mediate_acquire_socket(uint64_t rights, int local_fd);

/* Transfer acquisition for SCM_RIGHTS and pidfd_getfd. @pinned_vf is a
 * caller-held reference to the exact transferred object, @local_fd is the
 * receiving task's installed local descriptor, and @requested_rights is the
 * legacy base grant (READ|WRITE|STAT|SEEK); CONNECT|ACCEPT are added only for
 * socket vfiles. The fdtable callback verifies the slot still names @pinned_vf
 * while charging and recording its stable vfile identity. Within one envelope,
 * the first grant for that vfile identity is immutable: later reacquisition
 * still consumes an op but cannot broaden or narrow authority shared by its
 * aliases. Reopen paths must intersect requested rights with the source grant
 * before creating a distinct vfile identity. This is not per-fd attenuation. */
int env_mediate_acquire_vfile(int local_fd, struct vfile *pinned_vf,
                              uint64_t requested_rights);

/*
 * Acquisition of an authority created elsewhere entering this task via
 * descriptor transfer (A6 SCM_RIGHTS receive, A7 pidfd_getfd). Classification
 * follows the acquired vfile identity; successful checks install a shadow
 * keyed by that identity, shared across dup/fork aliases and retired when the
 * final vfile reference is released.
 */
int env_mediate_acquire_gfd(int local_fd);

/*
 * Propagation check (A6 send side): the fd's class must appear in
 * policy.propagation_types for it to leave the task via SCM_RIGHTS.
 * Charges one op.  Returns 0 or negative errno.
 */
int env_mediate_send_fd(int gfd);
int env_mediate_send_vfile(int local_fd, struct vfile *pinned_vf);

/*
 * Resource-class-only mediation for authorities without an fd identity
 * (A5 shmat attaches a memory segment and yields an address): expiry +
 * type membership + op charge, no shadow registration.
 */
int env_mediate_class(uint8_t obj_type);

/*
 * Use: expiry check + op/data accounting + DIRECTION enforcement against
 * the shadow rights of tracked authorities (grandfathered fds are exempt).
 * want_write: 0 = read, 1 = write, -1 = no direction requirement.
 */
int env_mediate_use_dir(int local_fd, size_t bytes, int want_write);
int env_mediate_use(int local_fd, size_t bytes); /* dir = -1 */

/*
 * A8 support: rights carried by the shadow for gfd in the CURRENT task's
 * envelope; *tracked is cleared when no shadow exists (grandfathered fd).
 */
uint64_t env_shadow_rights_of_gfd(int local_fd, int *tracked);

/* Called from the final vfile-free path after its last reference reaches zero.
 * This reclaims every envelope's identity shadow before identity storage can
 * be reused; caller holds no fdtable or VFS lock. */
void env_vfile_retire(struct vfile *vf);

/* ---- Vfile-bound object classification ---- */

/* Tag a newly constructed vfile before publishing it into an fdtable. The
 * class then follows dup/fork aliases and cannot be confused by local-fd
 * reuse. Ordinary files need no explicit tag. */
int env_kind_tag_vfile(struct vfile *vf, uint8_t obj_type);
uint8_t env_kind_of_vfile(const struct vfile *vf);

/* Transitional compatibility for syscall creation sites. New objects are
 * tagged in their vfile constructor before fdtable publication; this legacy
 * numeric-fd hook intentionally does not mutate classification. */
void env_kind_register(int gfd, uint8_t obj_type);

/* Compatibility hook called by descriptor close paths. Classification lives
 * on the vfile, so closing one alias must not clear metadata for other aliases. */
void env_kind_unregister(int gfd);
uint8_t env_kind_of(int gfd);

/* True when current task runs inside an envelope (hot-path guard). */
bool env_active(const struct task_t *t);

/* ---- Lifetime (called from proc/fork.c and proc/task.c) ---- */

/* Fork inheritance: child shares the parent's envelope (refcounted root). */
void env_inherit_task(struct task_t *child, const struct task_t *parent);

/* Task teardown: drop its attached reference and release creator references
 * for envelopes owned by this task. A revoked envelope remains addressable
 * until owner teardown, while attached tasks retain their own references. */
void env_release_task(struct task_t *t);

#endif /* _ABI_LINUX_ENVELOPE_H */
