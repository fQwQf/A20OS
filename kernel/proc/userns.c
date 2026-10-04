/*
 * User namespace objects: id translation between a container's numbering and
 * the global ids the kernel stores.
 * See kernel/include/proc/userns.h for the object model.
 */
#include "proc/userns.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/nsclone.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/slab.h"
#include "core/errno.h"

static user_namespace_t  g_init_userns;
static user_namespace_t *g_userns_list;
static spinlock_t        g_userns_lock = SPINLOCK_INIT;
static uint64_t          g_userns_next_ino = USERNS_INIT_INO + 1;

void userns_early_init(void)
{
    memset(&g_init_userns, 0, sizeof(g_init_userns));
    g_init_userns.ino = USERNS_INIT_INO;
    refcount_set(&g_init_userns.refs, 1);  /* pinned: never freed */
    g_init_userns.level = 0;
    spin_init(&g_init_userns.lock);
    /* The initial namespace maps every id to itself, so a kernel that never
     * creates a user namespace pays nothing for translation and every id
     * comparison against task_t::cred stays valid. */
    g_init_userns.uid_map[0].lower = 0;
    g_init_userns.uid_map[0].parent_lower = 0;
    g_init_userns.uid_map[0].count = 0xFFFFFFFFU;
    g_init_userns.uid_map_extents = 1;
    g_init_userns.gid_map[0] = g_init_userns.uid_map[0];
    g_init_userns.gid_map_extents = 1;
    g_init_userns.setgroups_allowed = 1;
    g_userns_list = &g_init_userns;
}

user_namespace_t *userns_init_ns(void)
{
    return &g_init_userns;
}

user_namespace_t *userns_task_own(task_t *t)
{
    if (t) {
        user_namespace_t *ns = (user_namespace_t *)__atomic_load_n(
            &t->user_ns, __ATOMIC_ACQUIRE);
        if (ns)
            return ns;
    }
    return &g_init_userns;
}

user_namespace_t *userns_current(void)
{
    return userns_task_own(proc_current());
}

user_namespace_t *userns_get(user_namespace_t *ns)
{
    if (!ns || ns == &g_init_userns)
        return &g_init_userns;
    uint64_t flags = spin_lock_irqsave(&g_userns_lock);
    refcount_inc(&ns->refs);
    spin_unlock_irqrestore(&g_userns_lock, flags);
    return ns;
}

user_namespace_t *userns_task_get(task_t *t)
{
    if (!t)
        return &g_init_userns;
    user_namespace_t *ns = userns_task_own(t);
    if (ns != &g_init_userns) {
        uint64_t flags = spin_lock_irqsave(&g_userns_lock);
        refcount_inc(&ns->refs);
        spin_unlock_irqrestore(&g_userns_lock, flags);
    }
    return ns;
}

uint64_t userns_task_ino(task_t *t)
{
    if (!t)
        return USERNS_INIT_INO;
    return userns_task_own(t)->ino;
}

static user_namespace_t *userns_alloc(user_namespace_t *parent)
{
    user_namespace_t *ns = kmalloc(sizeof(*ns));
    if (!ns)
        return NULL;
    memset(ns, 0, sizeof(*ns));

    refcount_set(&ns->refs, 1);          /* the creating reference */
    ns->level = parent->level + 1;
    spin_init(&ns->lock);
    ns->parent = parent;
    /* An empty map means "nothing is represented yet", which is exactly the
     * state a fresh namespace must start in: until someone writes uid_map the
     * creator sees the overflow id for everything, rather than silently
     * inheriting the parent's numbering. */
    ns->uid_map_extents = 0;
    ns->gid_map_extents = 0;
    ns->setgroups_allowed = 1;

    uint64_t flags = spin_lock_irqsave(&g_userns_lock);
    ns->ino = g_userns_next_ino++;
    refcount_inc(&parent->refs);         /* the chain reference */
    ns->next = g_userns_list;
    g_userns_list = ns;
    spin_unlock_irqrestore(&g_userns_lock, flags);
    return ns;
}

void userns_put(user_namespace_t *ns)
{
    if (!ns || ns == &g_init_userns)
        return;

    uint64_t flags = spin_lock_irqsave(&g_userns_lock);
    if (!refcount_dec_and_test(&ns->refs)) {
        spin_unlock_irqrestore(&g_userns_lock, flags);
        return;
    }
    user_namespace_t **pp = &g_userns_list;
    while (*pp) {
        if (*pp == ns) {
            *pp = ns->next;
            break;
        }
        pp = &(*pp)->next;
    }
    user_namespace_t *parent = ns->parent;
    spin_unlock_irqrestore(&g_userns_lock, flags);

    if (parent)
        userns_put(parent);
    kfree(ns);
}

/* ---- Id translation ---- */

/* Find the extent whose NAMESPACE-RELATIVE range covers @id, or -1.  Caller
 * holds no lock: the map only ever grows by appending, so a reader that
 * misses sees a prefix that is still correct. */
static int map_lookup(const uid_gid_extent_t *map, int extents, uint32_t id)
{
    for (int i = 0; i < extents; i++) {
        uint32_t lo = map[i].lower;
        uint32_t cnt = map[i].count;
        /* Written as a subtraction so a 32-bit range cannot wrap at the top of
         * the space. */
        if (id >= lo && (uint32_t)(id - lo) < cnt)
            return i;
    }
    return -1;
}

/* The same scan keyed on parent_lower, i.e. over GLOBAL ids.  The two differ
 * exactly when the namespace renumbers, which is the case a rootless creator
 * produces ("0 1000 1"): searching the global side against `lower` there
 * would answer "uid 1000 is unmapped" for a map that plainly contains it. */
static int map_lookup_parent(const uid_gid_extent_t *map, int extents,
                             uint32_t id)
{
    for (int i = 0; i < extents; i++) {
        uint32_t lo = map[i].parent_lower;
        uint32_t cnt = map[i].count;
        if (id >= lo && (uint32_t)(id - lo) < cnt)
            return i;
    }
    return -1;
}

static int map_to_global(const uid_gid_extent_t *map, int extents,
                         int ns_id, uint32_t *out)
{
    if (ns_id < 0)
        return -1;
    int i = map_lookup(map, extents, (uint32_t)ns_id);
    if (i < 0)
        return -1;
    uint32_t parent = map[i].parent_lower + ((uint32_t)ns_id - map[i].lower);
    *out = parent;
    return 0;
}

static int map_from_global(const uid_gid_extent_t *map, int extents,
                           uint32_t global, uint32_t *out)
{
    int i = map_lookup_parent(map, extents, global);
    if (i < 0)
        return -1;
    *out = map[i].lower + (global - map[i].parent_lower);
    return 0;
}

int userns_to_kuid(user_namespace_t *ns, int ns_uid)
{
    if (!ns) return -1;
    uint32_t k = 0;
    if (map_to_global(ns->uid_map, ns->uid_map_extents, ns_uid, &k) < 0)
        return -1;
    return (int)k;
}

int userns_to_kgid(user_namespace_t *ns, int ns_gid)
{
    if (!ns) return -1;
    uint32_t k = 0;
    if (map_to_global(ns->gid_map, ns->gid_map_extents, ns_gid, &k) < 0)
        return -1;
    return (int)k;
}

int userns_from_kuid(user_namespace_t *ns, int kuid)
{
    if (!ns || kuid < 0) return USERNS_OVERFLOW_UID;
    uint32_t local = 0;
    if (map_from_global(ns->uid_map, ns->uid_map_extents,
                        (uint32_t)kuid, &local) < 0)
        return USERNS_OVERFLOW_UID;
    return (int)local;
}

int userns_from_kgid(user_namespace_t *ns, int kgid)
{
    if (!ns || kgid < 0) return USERNS_OVERFLOW_GID;
    uint32_t local = 0;
    if (map_from_global(ns->gid_map, ns->gid_map_extents,
                        (uint32_t)kgid, &local) < 0)
        return USERNS_OVERFLOW_GID;
    return (int)local;
}

int userns_kuid_mapped(user_namespace_t *ns, int kuid)
{
    if (!ns || kuid < 0)
        return 0;
    uint32_t local = 0;
    return map_from_global(ns->uid_map, ns->uid_map_extents,
                           (uint32_t)kuid, &local) == 0;
}

/* ---- Capability scoping ---- */

/*
 * userns_capable(t, ns, cap) -- may @t exercise @cap with respect to @ns?
 *
 * This is the single function every namespace-scoped permission check goes
 * through, so its shape is the whole security argument.  The rule has one
 * clause: walk from @ns up the parent chain and grant only if the walk lands
 * exactly on @t's own namespace, where @t's capability set decides.
 *
 * Both halves of that are load-bearing:
 *
 *  - Landing on @own means @ns is @t's own namespace or a descendant of it.
 *    unshare(CLONE_NEWUSER) fills @t's capability set, which is why a process
 *    becomes root inside a namespace it just made -- and why it can then
 *    administer namespaces nested inside that one, since those are further
 *    down the same chain.
 *
 *  - Running off the top without landing is the containment property.  @t
 *    holds nothing in a namespace above its own, no matter what it did to
 *    create the namespaces below.  Walking the other way -- from @t's own
 *    namespace up towards the initial one -- gets this exactly backwards and
 *    is the mistake worth naming: it would let any unprivileged process call
 *    unshare(CLONE_NEWUSER) and immediately acquire authority over the
 *    initial namespace, i.e. over every mount and pid namespace on the
 *    system.  Granting a namespace's creator authority over the namespace it
 *    came from has the same result by another route.
 *
 * Note what is deliberately absent: no case for "the creator may administer
 * what it created".  It needs none.  Authority over a descendant arrives
 * because the check is performed in the namespace that owns the object being
 * administered, and that namespace is the descendant -- so the walk finds
 * @own and the creator's capability set answers.  The rule that looks like it
 * needs an exception is just the rule, applied at the right level.
 */
int userns_capable(const task_t *t, user_namespace_t *ns, int cap)
{
    if (!t || !ns)
        return 0;
    if (cap < 0 || cap >= 64)
        return 0;

    user_namespace_t *own = userns_task_own((task_t *)t);

    /* Walk from @ns up the parent chain towards the task's own namespace.
     * Authority is granted only if the walk lands exactly on @own, and then
     * only for capabilities the task actually holds there.
     *
     * Running off the top of @own without hitting it is the containment
     * property: a task has no capabilities in a namespace above its own, no
     * matter what it did to create the namespaces below.  The extra `level`
     * test stops a malformed chain from turning this into a spin; levels
     * strictly decrease on the way up, so it is also the loop's exit. */
    for (user_namespace_t *p = ns; p; p = p->parent) {
        if (p == own)
            return proc_has_cap(t, cap);
        if (p->level <= own->level)
            return 0;
    }
    return 0;
}

/* ---- Map writing ---- */

/*
 * May @writer install this map without holding CAP_SETUID/CAP_SETGID in the
 * parent namespace?
 *
 * The narrow unprivileged case is what makes user namespaces usable without
 * privilege, and it is deliberately tiny: exactly ONE extent of length ONE
 * that maps the writer's OWN id to namespace id 0.  That is what
 * `unshare -U; echo $USER > /proc/self/uid_map` does, and it is the most a
 * process without privilege can ever be allowed to grant itself: it maps the
 * single id it already owns, and grants no authority over any id it does not
 * already hold.  Anything wider -- a second extent, a longer one, or someone
 * else's id -- is refused, so an unprivileged process cannot use a user
 * namespace to obtain ids it did not already have.
 */
static int unprivileged_map_allowed(const uint32_t *extents, int count,
                                    uint32_t own_id)
{
    if (count != 1)
        return 0;
    if (extents[2] != 1)
        return 0;
    if (extents[1] != own_id)
        return 0;
    /* lower is the id INSIDE the namespace; any value is harmless for a
     * single-id map because it cannot reach another id. */
    return 1;
}

/*
 * Parse "lower parent_lower count" triples and install them.
 *
 * Permission rules, matching Linux:
 *  - writing uid_map requires CAP_SETUID in the PARENT namespace, or the
 *    single-own-id exception above;
 *  - writing gid_map requires CAP_SETGID in the parent, unless setgroups has
 *    already been set to "deny" (that is the whole point of the setgroups
 *    file: an unprivileged creator disowns the ability to gain group authority
 *    before it hands out any gids);
 *  - every extent must lie inside the parent namespace's own map, so a
 *    namespace can only hand out ids that are representable outside it.
 */
int userns_write_map(task_t *writer, user_namespace_t *ns, int is_gid,
                     const uint32_t *extents, int count)
{
    if (!writer || !ns || !extents || count <= 0)
        return -EINVAL;
    if (count > USERNS_MAP_MAX)
        return -EINVAL;

    user_namespace_t *parent = ns->parent;
    if (!parent)
        return -EINVAL;

    /* The writer acts on a namespace it does not necessarily belong to (the
     * creator writes the child's maps), so authority is measured in the
     * PARENT namespace the map draws from, not in the writer's own. */
    int privileged;
    if (is_gid) {
        uint64_t f = spin_lock_irqsave(&ns->lock);
        int sg = ns->setgroups_allowed;
        spin_unlock_irqrestore(&ns->lock, f);
        privileged = userns_capable(writer, parent, CAP_SETGID);
        /* While setgroups is still allowed, handing out a gid also grants
         * group authority, so it needs the capability.  Once it has been set
         * to "deny" the creator has already given that up, and the unprivileged
         * single-id exception becomes reachable. */
        if (sg && !privileged)
            return -EPERM;
    } else {
        privileged = userns_capable(writer, parent, CAP_SETUID);
    }
    if (!privileged &&
        !unprivileged_map_allowed(extents, count,
                                  is_gid ? (uint32_t)writer->cred.egid
                                         : (uint32_t)writer->cred.euid))
        return -EPERM;

    /* Every extent must be representable in the parent, and must fit in 32
     * bits: a mapping that runs off the end would let lower + count wrap and
     * alias ids that were never granted. */
    for (int i = 0; i < count; i++) {
        uint32_t lower = extents[i * 3];
        uint32_t plower = extents[i * 3 + 1];
        uint32_t cnt = extents[i * 3 + 2];
        if (cnt == 0)
            return -EINVAL;
        if (cnt - 1U > 0xFFFFFFFFU - lower)
            return -EINVAL;
        if (cnt - 1U > 0xFFFFFFFFU - plower)
            return -EINVAL;
        uint32_t resolved = 0;
        int mapped = is_gid
            ? map_to_global(parent->gid_map, parent->gid_map_extents,
                            (int)plower, &resolved) == 0
            : map_to_global(parent->uid_map, parent->uid_map_extents,
                            (int)plower, &resolved) == 0;
        if (!mapped) {
            return -EPERM;
        }
    }

    uint64_t flags = spin_lock_irqsave(&ns->lock);
    /* Extents are appended in increasing lower order and must not overlap,
     * which is what keeps lookup a plain scan with no reordering. */
    uint32_t highest = 0;
    int have = 0;
    uid_gid_extent_t *map = is_gid ? ns->gid_map : ns->uid_map;
    int nextents = is_gid ? ns->gid_map_extents : ns->uid_map_extents;
    for (int i = 0; i < nextents; i++) {
        uint32_t top = map[i].lower + map[i].count;
        if (top > highest) {
            highest = top;
            have = 1;
        }
    }
    for (int i = 0; i < count; i++) {
        uint32_t lower = extents[i * 3];
        uint32_t cnt = extents[i * 3 + 2];
        if (have && lower < highest)
            goto fail;                     /* would overlap existing extents */
        uint32_t top = lower + cnt;
        if (top > 0xFFFFFFFFU)
            goto fail;
        highest = top;
        have = 1;
    }
    for (int i = 0; i < count; i++) {
        map[nextents + i].lower = extents[i * 3];
        map[nextents + i].parent_lower = extents[i * 3 + 1];
        map[nextents + i].count = extents[i * 3 + 2];
    }
    if (is_gid)
        ns->gid_map_extents = nextents + count;
    else
        ns->uid_map_extents = nextents + count;
    spin_unlock_irqrestore(&ns->lock, flags);
    return 0;

fail:
    spin_unlock_irqrestore(&ns->lock, flags);
    return -EINVAL;
}

/*
 * Handle a write to /proc/<pid>/setgroups.
 *
 * Only one transition exists -- "allow" to "deny" -- and it is irreversible.
 * Making it one-way is the security property: a creator that has disowned
 * group authority must not be able to take it back after handing gids out,
 * because code running inside the namespace may already have observed the
 * window in which it was allowed.
 */
int userns_write_setgroups(task_t *writer, user_namespace_t *ns, int deny)
{
    if (!writer || !ns || !ns->parent)
        return -EINVAL;

    user_namespace_t *parent = ns->parent;
    if (deny) {
        /* Going to "deny" never needs privilege: it only removes authority. */
        uint64_t f = spin_lock_irqsave(&ns->lock);
        ns->setgroups_allowed = 0;
        spin_unlock_irqrestore(&ns->lock, f);
        return 0;
    }

    uint64_t f = spin_lock_irqsave(&ns->lock);
    int currently = ns->setgroups_allowed;
    spin_unlock_irqrestore(&ns->lock, f);
    if (!currently)
        return -EPERM;   /* already denied; the transition is one-way */
    /* Going back to "allow" would restore authority the namespace may already
     * have delegated, so it needs privilege over the parent. */
    if (!userns_capable(writer, parent, CAP_SETGID))
        return -EPERM;
    return 0;
}

/* ---- Inheritance and membership changes ---- */

int userns_unshare(task_t *t)
{
    if (!t)
        return -ESRCH;
    if (userns_task_own(t)->level + 1 >= 64)
        return -EUSERSRCH;

    user_namespace_t *parent = userns_task_own(t);
    user_namespace_t *ns = userns_alloc(parent);
    if (!ns)
        return -ENOMEM;

    /* unshare(CLONE_NEWUSER) is unprivileged in Linux, and that is the point:
     * it is how an unprivileged process gets a namespace it is root in.  The
     * privilege it gains is scoped to the new namespace, so the full
     * capability set here grants nothing outside it. */
    t->cred.cap_permitted = ~0ULL;
    t->cred.cap_effective = ~0ULL;
    t->cred.cap_inheritable = ~0ULL;
    t->cred.cap_bounding = ~0ULL;

    user_namespace_t *old = (user_namespace_t *)__atomic_load_n(
        &t->user_ns, __ATOMIC_ACQUIRE);
    userns_get(ns);
    __atomic_store_n(&t->user_ns, ns, __ATOMIC_RELEASE);
    /* Drop the creating reference; the task field now owns exactly one. */
    userns_put(ns);
    if (old)
        userns_put(old);
    return 0;
}

int userns_fork(task_t *child, task_t *parent, uint64_t clone_flags)
{
    if (!child || !parent)
        return -ESRCH;

    /* Exactly one reference must be handed to the child's user_ns field.
     * Both paths below end up with the field owning exactly one, which is the
     * same invariant pidns_fork() and mntns_fork() maintain. */
    user_namespace_t *target;
    int fresh = 0;
    if (clone_flags & LINUX_CLONE_NEWUSER) {
        /* clone(CLONE_NEWUSER): the child is the first member of a brand new
         * namespace and is fully capable inside it -- and nowhere else.
         * userns_alloc() returns the creating reference, which is the one the
         * field takes over, so nothing else is taken here. */
        target = userns_alloc(userns_task_own(parent));
        if (!target)
            return -ENOMEM;
        fresh = 1;
    } else {
        /* Same namespace as the parent.  The reference is taken below, at the
         * single point where the field is assigned -- taking one here as well
         * would leave the namespace pinned by a count nobody will ever drop,
         * which is exactly how a namespace outlives its last task. */
        target = userns_task_own(parent);
    }

    /* A thread shares its leader's namespace, like every other namespace. */
    if (fresh) {
        /* Same grant as unshare(CLONE_NEWUSER): the child is root inside the
         * namespace it was just made the first member of, and nowhere else --
         * userns_capable() is what keeps that from meaning anything outside. */
        child->cred.cap_permitted = ~0ULL;
        child->cred.cap_effective = ~0ULL;
        child->cred.cap_inheritable = ~0ULL;
        child->cred.cap_bounding = ~0ULL;
    }
    user_namespace_t *old = (user_namespace_t *)__atomic_load_n(
        &child->user_ns, __ATOMIC_ACQUIRE);
    if (target != old) {
        if (!fresh)
            userns_get(target);
        __atomic_store_n(&child->user_ns, target, __ATOMIC_RELEASE);
        if (old)
            userns_put(old);
    } else if (fresh) {
        /* Cannot happen today (a fresh namespace has no members yet), but if it
         * ever did the creating reference would have no owner. */
        userns_put(target);
    }
    return 0;
}

int userns_join(task_t *t, user_namespace_t *ns)
{
    if (!t || !ns)
        return -EINVAL;
    /* setns(CLONE_NEWUSER) moves the caller's credentials into @ns.  The
     * authority to do so was already checked by the caller (ns_capable in
     * sys_setns); here we only move the pointer. */
    user_namespace_t *old = (user_namespace_t *)__atomic_load_n(
        &t->user_ns, __ATOMIC_ACQUIRE);
    if (ns == old)
        return 0;
    userns_get(ns);
    __atomic_store_n(&t->user_ns, ns, __ATOMIC_RELEASE);
    if (old)
        userns_put(old);
    return 0;
}

void userns_release_task(task_t *t)
{
    if (!t)
        return;
    user_namespace_t *ns = (user_namespace_t *)__atomic_load_n(
        &t->user_ns, __ATOMIC_ACQUIRE);
    if (!ns)
        return;
    __atomic_store_n(&t->user_ns, NULL, __ATOMIC_RELAXED);
    userns_put(ns);
}