#ifndef _PROC_USERNS_H
#define _PROC_USERNS_H

#include "core/refcount.h"
#include "core/lock.h"

struct task_t;

/*
 * User namespaces.
 *
 * A user_namespace translates between ids as seen inside it and the global
 * (host) ids the rest of the kernel actually stores.  Credentials are kept
 * as global ids in proc_cred_t and translated at the boundary -- the set*
 * family on the way in, the get* family and /proc on the way out.  That is
 * what Linux does with kuid_t/kgid_t, and it means a uid comparison anywhere
 * inside the kernel stays a plain integer comparison.
 *
 * The initial namespace maps every id to itself, so a system with no user
 * namespaces sees no translation at all and every existing caller keeps both
 * its behaviour and its cost.
 *
 * Mapping representation: like Linux, a map is a small ordered list of
 * extents, each a (lower, parent_lower, count) triple.  Extents must be
 * appended in increasing `lower` order and must not overlap, which is what
 * lets a lookup be a short linear scan with no sorting at lookup time.  A
 * single extent covers the common "map my own uid" case; five covers the
 * containers-per-host layouts that need disjoint ranges.
 *
 * Reference model (identical to mntns/pidns so one mental model covers all
 * three):
 * - A namespace holds one reference on its parent, so walking ->parent from a
 *   referenced namespace can never dangle.
 * - task_t::user_ns holds a reference; NULL means the initial namespace,
 *   which is statically pinned.
 * - An open /proc/<pid>/ns/user file pins the namespace, so setns(2) keeps
 *   working after the target exits.
 * - userns_release_task() drops the task's reference at teardown.
 */
#define USERNS_INIT_INO       4026531837ULL
#define USERNS_MAP_MAX        5
/* Id reported to a process for a global uid its namespace cannot map --
 * Linux's (uid_t)-1, which is also the value the audit log uses. */
#define USERNS_OVERFLOW_UID   65534
#define USERNS_OVERFLOW_GID   65534

/* One contiguous run of the map: ids [lower, lower+count) inside the
 * namespace correspond to [parent_lower, parent_lower+count) outside it. */
typedef struct uid_gid_extent {
    uint32_t lower;
    uint32_t parent_lower;
    uint32_t count;
} uid_gid_extent_t;

typedef struct user_namespace {
    uint64_t    ino;              /* immutable id reported as user:[ino] */
    refcount_t  refs;
    int         level;            /* 0 == initial namespace */
    struct user_namespace *parent; /* one reference held by this namespace */
    spinlock_t  lock;
    uid_gid_extent_t uid_map[USERNS_MAP_MAX];
    int         uid_map_extents;
    uid_gid_extent_t gid_map[USERNS_MAP_MAX];
    int         gid_map_extents;
    /* Linux's setgroups(2) gate: a gid_map may only be written without
     * CAP_SETGID in the parent once setgroups has been set to "deny".
     * One-way and irreversible, which is why it is a flag rather than a
     * stored string. */
    int         setgroups_allowed;
    struct user_namespace *next;  /* registry link */
} user_namespace_t;

void                     userns_early_init(void);
/* The initial namespace: statically allocated, never freed, so callers that
 * must distinguish it from a heap namespace can compare against this. */
user_namespace_t        *userns_init_ns(void);

/* The namespace the task's credentials are expressed in. */
user_namespace_t        *userns_task_own(struct task_t *t);
user_namespace_t        *userns_current(void);
user_namespace_t        *userns_task_get(struct task_t *t);
user_namespace_t        *userns_get(user_namespace_t *ns);
void                     userns_put(user_namespace_t *ns);
uint64_t                 userns_task_ino(struct task_t *t);

/* ---- Id translation ----
 *
 * userns_to_kuid()/userns_to_kgid() convert a namespace-local id to the global
 * id the kernel stores.  An id the map does not cover yields -1 (EINVAL to
 * the caller), because silently substituting a global id would let a process
 * name a user its namespace has no authority over.
 *
 * userns_from_kuid() is the other direction and is lossy by design: a global
 * id with no mapping is reported as USERNS_OVERFLOW_UID, which is what a
 * container should see rather than the host's real uid. */
int                      userns_to_kuid(user_namespace_t *ns, int ns_uid);
int                      userns_to_kgid(user_namespace_t *ns, int ns_gid);
int                      userns_from_kuid(user_namespace_t *ns, int kuid);
int                      userns_from_kgid(user_namespace_t *ns, int kgid);

/* Is @kuid represented inside @ns at all?  File ownership checks need this
 * rather than the overflow id, because "no representation" and "uid 65534"
 * must not be conflated. */
int                      userns_kuid_mapped(user_namespace_t *ns, int kuid);

/* ---- Capability scoping ----
 *
 * Does @t hold @cap with respect to @ns?  One rule: walk from @ns up the
 * parent chain and grant only if the walk reaches @t's own namespace exactly,
 * where @t's capability set decides.  Running off the top without reaching it
 * denies -- a task holds nothing in a namespace above its own.
 *
 * That is what makes containment work.  unshare(CLONE_NEWUSER) hands the
 * caller a full capability set, and it is tempting to read that as authority
 * over the namespace it came from; it is not.  The creator's authority lives
 * in the namespace it created, and granting it more would let any user turn
 * `unshare -U` into host root by writing a map naming host uid 0.
 *
 * Authority over a descendant is not a separate case: it falls out of the
 * check being performed in the namespace that owns the object, so
 * "CAP_SYS_ADMIN in the parent governs the subtree" needs no walk here. */
int                      userns_capable(const struct task_t *t,
                                       user_namespace_t *ns, int cap);

/* ---- Map writing (the /proc/<pid>/{uid,gid}_map write path) ---- */
int                      userns_write_map(struct task_t *writer,
                                        user_namespace_t *ns, int is_gid,
                                        const uint32_t *extents, int count);
/* "deny" is irreversible and unprivileged; restoring "allow" needs
 * CAP_SETGID in the parent namespace. */
int                      userns_write_setgroups(struct task_t *writer,
                                              user_namespace_t *ns,
                                              int deny);

/* ---- Inheritance and membership changes ---- */
int                      userns_unshare(struct task_t *t);
int                      userns_fork(struct task_t *child,
                                    struct task_t *parent,
                                    uint64_t clone_flags);
int                      userns_join(struct task_t *t, user_namespace_t *ns);
void                     userns_release_task(struct task_t *t);

#endif /* _PROC_USERNS_H */