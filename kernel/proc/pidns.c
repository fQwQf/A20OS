/*
 * PID namespace objects: per-namespace id spaces layered on the initial
 * namespace that kernel/proc/pid.c already allocates.
 * See kernel/include/proc/pidns.h for the reference model.
 */
#include "proc/pidns.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/slab.h"
#include "core/errno.h"

static pid_namespace_t  g_init_ns;
static pid_namespace_t *g_ns_list;
static spinlock_t       g_pidns_lock = SPINLOCK_INIT;
static uint64_t         g_next_ino = PIDNS_INIT_INO + 1;

/* Free-id search over one namespace's bitmap.  Mirrors pid_bitmap_find_free()
 * in kernel/proc/pid.c, including the wrap, so a container behaves like the
 * initial namespace rather than like a fresh counter that never repeats. */
static int pidns_bitmap_find_free(pid_namespace_t *ns, int start)
{
    if (!ns->id_bitmap || ns->id_words == 0)
        return -1;
    unsigned word_start = (unsigned)(start - 1) / 64;

    for (unsigned w = 0; w < ns->id_words; w++) {
        unsigned wi = (word_start + w) % ns->id_words;
        if (ns->id_bitmap[wi] == ~0ULL)
            continue;
        unsigned bit_base = wi * 64;
        uint64_t inv = ~ns->id_bitmap[wi];
        if (wi == word_start) {
            unsigned start_bit = (unsigned)(start - 1) % 64;
            if (start_bit > 0)
                inv &= ~((1ULL << start_bit) - 1);
        }
        int first = 0;
        while (first < 64 && !((inv >> first) & 1ULL))
            first++;
        if (first == 64)
            continue;
        int candidate = (int)(bit_base + (unsigned)first + 1);
        if (candidate >= 1 && candidate <= PIDNS_CHILD_MAX)
            return candidate;
    }
    return -1;
}

static void pidns_bitmap_set(pid_namespace_t *ns, int pid)
{
    if (!ns->id_bitmap || pid < 1)
        return;
    unsigned idx = (unsigned)(pid - 1) / 64;
    if (idx >= ns->id_words)
        return;
    ns->id_bitmap[idx] |= 1ULL << ((unsigned)(pid - 1) % 64);
}

static void pidns_bitmap_clear(pid_namespace_t *ns, int pid)
{
    if (!ns->id_bitmap || pid < 1)
        return;
    unsigned idx = (unsigned)(pid - 1) / 64;
    if (idx >= ns->id_words)
        return;
    ns->id_bitmap[idx] &= ~(1ULL << ((unsigned)(pid - 1) % 64));
}

static int pidns_alloc_id_locked(pid_namespace_t *ns)
{
    int pid = pidns_bitmap_find_free(ns, ns->next_pid);
    if (pid < 0 && ns->next_pid > 1)
        pid = pidns_bitmap_find_free(ns, 1);
    if (pid < 1 || pid > PIDNS_CHILD_MAX)
        return -EAGAIN;

    pidns_bitmap_set(ns, pid);
    ns->next_pid = pid + 1;
    if (ns->next_pid > PIDNS_CHILD_MAX)
        ns->next_pid = 1;
    ns->nr_allocated++;
    return pid;
}

void pidns_early_init(void)
{
    memset(&g_init_ns, 0, sizeof(g_init_ns));
    g_init_ns.ino = PIDNS_INIT_INO;
    refcount_set(&g_init_ns.refs, 1);  /* pinned: never freed */
    g_init_ns.level = 0;
    spin_init(&g_init_ns.lock);
    g_init_ns.next_pid = 1;
    /* No bitmap: level 0 ids come from the global allocator in proc/pid.c, so
     * giving the initial namespace its own bitmap would let a container-local
     * id and a global id collide in a confusing way. */
    g_init_ns.id_bitmap = NULL;
    g_init_ns.id_words = 0;
    g_ns_list = &g_init_ns;
}

/* The initial namespace, for callers that need the root of the chain (mount
 * and capability policy anchored outside any container).  Note this is not the
 * same thing as pidns_current(): a task inside a container gets its own
 * namespace back from that. */
pid_namespace_t *pidns_init_ns(void)
{
    return &g_init_ns;
}

pid_namespace_t *pidns_current(void)
{
    /* The task's OWN membership, not the namespace it is spawning children
     * into.  Those differ between unshare(CLONE_NEWPID) and the next fork,
     * and a task that has unshared but not yet forked still reports its old
     * id -- returning the children namespace here would make getpid() report
     * 0 for a live process. */
    return pidns_task_own(proc_current());
}

pid_namespace_t *pidns_task_own(task_t *t)
{
    if (t) {
        pid_namespace_t *ns = (pid_namespace_t *)__atomic_load_n(
            &t->pid_ns, __ATOMIC_ACQUIRE);
        if (ns)
            return ns;
    }
    return &g_init_ns;
}

pid_namespace_t *pidns_get(pid_namespace_t *ns)
{
    if (!ns || ns == &g_init_ns)
        return &g_init_ns;
    uint64_t flags = spin_lock_irqsave(&g_pidns_lock);
    refcount_inc(&ns->refs);
    spin_unlock_irqrestore(&g_pidns_lock, flags);
    return ns;
}

pid_namespace_t *pidns_task_get(task_t *t)
{
    if (!t)
        return &g_init_ns;
    uint64_t flags = spin_lock_irqsave(&g_pidns_lock);
    pid_namespace_t *ns = (pid_namespace_t *)t->pid_ns;
    if (ns && ns != &g_init_ns)
        refcount_inc(&ns->refs);
    spin_unlock_irqrestore(&g_pidns_lock, flags);
    return ns ? ns : &g_init_ns;
}

uint64_t pidns_task_ino(task_t *t)
{
    if (!t)
        return PIDNS_INIT_INO;
    pid_namespace_t *ns = pidns_task_own(t);
    return ns->ino;
}

int task_pid_nr_ns(task_t *t, pid_namespace_t *ns)
{
    if (!t || !ns)
        return 0;
    if (ns->level == 0)
        return t->pid;
    /* Outside the target's own level the id does not exist, and the answer is
     * 0 -- Linux's "not visible in this namespace". */
    if (ns->level > t->pid_ns_level)
        return 0;
    /* A task one level deeper holds an id here, but only because this
     * namespace is on its chain.  A sibling namespace at the same level
     * allocates the same small numbers independently, so the id alone must not
     * be treated as membership. */
    if (!pidns_visible(ns, t))
        return 0;
    return t->ns_pid[ns->level];
}

int task_ppid_nr_ns(task_t *t, pid_namespace_t *ns)
{
    if (!t)
        return 0;
    task_t *p = t->parent;
    /* Linux uses real_parent for the visible ppid, because a ptraced child's
     * getppid() must not leak the tracer. */
    if (!p)
        return 0;
    /* A parent outside the caller's namespace is reported as 0, which falls
     * out of task_pid_nr_ns() finding no id for it here. */
    return task_pid_nr_ns(p, ns);
}

/* Is @target a member of @ns or of a namespace below it? */
int pidns_visible(pid_namespace_t *ns, task_t *target)
{
    if (!ns || !target)
        return 0;
    int tlevel = target->pid_ns_level;
    if (tlevel < ns->level)
        return 0;
    /* Walk the target's own namespace chain back up to @ns.  Every namespace
     * holds a reference on its parent, and the caller reached @ns either as
     * the initial namespace or through a reference it still holds, so this
     * walk cannot dangle. */
    pid_namespace_t *cur = pidns_task_own(target);
    for (int i = tlevel; i > ns->level && cur; i--)
        cur = cur->parent;
    return cur == ns;
}

/*
 * Resolve a pid the way the CALLER sees it.
 *
 * This is the entry point every user-facing pid lookup must go through: the
 * integer in a kill(2), a wait4(2) or a /proc/<pid> path is an id in the
 * caller's own namespace, so resolving it against the global table would let a
 * container address the host's processes by guessing ids.  Kernel-internal
 * callers that genuinely hold a global id keep using proc_find_get() directly.
 *
 * In the initial namespace this is exactly proc_find_get(), so every existing
 * caller keeps its behaviour and its cost.
 */
task_t *proc_find_get_user(int pid)
{
    if (pid <= 0)
        return NULL;
    return pidns_find_get(pidns_current(), pid);
}

task_t *pidns_find_get(pid_namespace_t *ns, int pid)
{
    if (!ns || pid < 1)
        return NULL;
    /* A container-local id is only meaningful against its own namespace, and
     * the initial namespace's ids live in the global table. */
    if (ns->level == 0)
        return proc_find_get(pid);

    int level = ns->level;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED)
            continue;
        if (t->pid_ns_level < level)
            continue;
        if (t->ns_pid[level] != pid)
            continue;
        /* Sibling namespaces at the same level allocate ids independently, so
         * a matching id alone does not mean the task is in THIS namespace --
         * two containers can both have a pid 5.  Confirm the task's chain
         * really contains @ns before resolving to it. */
        pid_namespace_t *cur = pidns_task_own(t);
        for (int i = t->pid_ns_level; i > level && cur; i--)
            cur = cur->parent;
        if (cur != ns)
            continue;
        if (!proc_get(t)) {
            spin_unlock_irqrestore(&proc_lock, flags);
            return NULL;
        }
        spin_unlock_irqrestore(&proc_lock, flags);
        return t;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return NULL;
}

task_t *pidns_next_visible(pid_namespace_t *ns, task_t **iter)
{
    if (!ns || !iter)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *t = *iter ? proc_next_task_locked(*iter) : proc_first_task_locked();
    while (t) {
        if (t->state == PROC_UNUSED || !pidns_visible(ns, t)) {
            t = proc_next_task_locked(t);
            continue;
        }
        if (!proc_get(t)) {
            t = proc_next_task_locked(t);
            continue;
        }
        *iter = t;
        spin_unlock_irqrestore(&proc_lock, flags);
        return t;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return NULL;
}

int pidns_nr_tasks(pid_namespace_t *ns)
{
    if (!ns)
        return 0;
    int n = 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED)
            continue;
        if (pidns_visible(ns, t))
            n++;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return n;
}

static pid_namespace_t *pidns_alloc(pid_namespace_t *parent)
{
    pid_namespace_t *ns = kmalloc(sizeof(*ns));
    if (!ns)
        return NULL;
    memset(ns, 0, sizeof(*ns));

    /* 512 ids covers PIDNS_CHILD_MAX; grown only if a caller raises it. */
    ns->id_words = (PIDNS_CHILD_MAX + 63) / 64;
    ns->id_bitmap = kmalloc(ns->id_words * sizeof(uint64_t));
    if (!ns->id_bitmap) {
        kfree(ns);
        return NULL;
    }
    memset(ns->id_bitmap, 0, ns->id_words * sizeof(uint64_t));

    /* The chain reference is taken under g_pidns_lock because pidns_put()
     * drops one under the same lock: incrementing outside it would let a
     * concurrent put observe the parent at zero references and free it while
     * this namespace still points at it. */
    refcount_set(&ns->refs, 1);          /* the creating reference */
    ns->level = parent->level + 1;
    spin_init(&ns->lock);
    ns->next_pid = 1;
    ns->parent = parent;

    uint64_t flags = spin_lock_irqsave(&g_pidns_lock);
    ns->ino = g_next_ino++;
    refcount_inc(&parent->refs);         /* the chain reference */
    spin_unlock_irqrestore(&g_pidns_lock, flags);
    return ns;
}

void pidns_put(pid_namespace_t *ns)
{
    if (!ns || ns == &g_init_ns)
        return;

    uint64_t flags = spin_lock_irqsave(&g_pidns_lock);
    if (!refcount_dec_and_test(&ns->refs)) {
        spin_unlock_irqrestore(&g_pidns_lock, flags);
        return;
    }
    pid_namespace_t **pp = &g_ns_list;
    while (*pp) {
        if (*pp == ns) {
            *pp = ns->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pid_namespace_t *parent = ns->parent;
    spin_unlock_irqrestore(&g_pidns_lock, flags);

    if (parent)
        pidns_put(parent);
    kfree(ns->id_bitmap);
    kfree(ns);
}

/*
 * Acquire the ids @t needs to be addressable in @ns and in every ancestor of
 * @ns down to level 1.  A task is visible from all of them, so it needs a
 * distinct id in each: the ancestor's id space has no way to know which of its
 * own members is which.  The ids are written to out[] indexed by level and the
 * task is left untouched -- the caller commits them.
 *
 * Acquiring before committing is what makes a failure non-destructive.  Once a
 * task's old ids were released the task could not be put back, because the id
 * it would get back is no longer its own.
 */
static int pidns_ids_acquire(pid_namespace_t *ns, int *out)
{
    pid_namespace_t *chain[PID_MAX_LEVELS];
    int ids[PID_MAX_LEVELS];
    int n = 0;

    for (int l = 0; l < PID_MAX_LEVELS; l++)
        out[l] = 0;

    for (pid_namespace_t *c = ns; c && c->level > 0; c = c->parent) {
        if (n >= PID_MAX_LEVELS)
            return -EUSERSRCH;
        chain[n] = c;
        ids[n] = 0;
        n++;
    }

    for (int i = 0; i < n; i++) {
        uint64_t f2 = spin_lock_irqsave(&chain[i]->lock);
        int id = pidns_alloc_id_locked(chain[i]);
        if (id > 0) {
            chain[i]->nr_allocated++;
            /* nr_tasks counts MEMBERS, so only the namespace the task is
             * actually joining counts it; the ancestor ids it takes on the
             * way up are addressability, not membership. */
            if (i == 0)
                chain[i]->nr_tasks++;
        }
        spin_unlock_irqrestore(&chain[i]->lock, f2);
        if (id < 0) {
            /* Unwind: a half-populated id space would leak ids forever. */
            for (int j = 0; j < i; j++) {
                uint64_t f3 = spin_lock_irqsave(&chain[j]->lock);
                pidns_bitmap_clear(chain[j], ids[j]);
                if (chain[j]->nr_allocated > 0)
                    chain[j]->nr_allocated--;
                if (j == 0 && chain[j]->nr_tasks > 0)
                    chain[j]->nr_tasks--;
                spin_unlock_irqrestore(&chain[j]->lock, f3);
            }
            return id;
        }
        ids[i] = id;
        out[chain[i]->level] = id;
    }
    return 0;
}

/* Release every id the task holds and clear its level bookkeeping.  Must run
 * before t->pid_ns is overwritten, since the chain is walked from there. */
static void pidns_ids_release(task_t *t)
{
    pid_namespace_t *ns = pidns_task_own(t);
    int own_level = t->pid_ns_level;

    for (int level = own_level; level >= 1 && ns; level--) {
        int id = t->ns_pid[level];
        if (id > 0) {
            uint64_t f2 = spin_lock_irqsave(&ns->lock);
            pidns_bitmap_clear(ns, id);
            if (ns->nr_allocated > 0)
                ns->nr_allocated--;
            if (level == own_level && ns->nr_tasks > 0)
                ns->nr_tasks--;
            spin_unlock_irqrestore(&ns->lock, f2);
        }
        t->ns_pid[level] = 0;
        ns = ns->parent;
    }
    t->pid_ns_level = 0;
}

/*
 * Move @t between namespaces.
 *
 * @own is the namespace the task becomes a member of; NULL leaves it alone.
 * @children is the namespace its children join; NULL leaves it alone.  Both
 * take their own reference and release whatever the field held before, so the
 * caller keeps whatever reference it arrived with.
 */
static int pidns_assign(task_t *t, pid_namespace_t *own, pid_namespace_t *children)
{
    pid_namespace_t *old_own = pidns_task_own(t);

    if (own && own != old_own) {
        if (own->level >= PID_MAX_LEVELS)
            return -EUSERSRCH;
        int ids[PID_MAX_LEVELS];
        int r = pidns_ids_acquire(own, ids);
        if (r < 0)
            return r;

        pidns_get(own);
        pidns_ids_release(t);
        for (int l = 1; l < PID_MAX_LEVELS; l++)
            t->ns_pid[l] = ids[l];
        t->pid_ns_level = own->level;
        __atomic_store_n(&t->pid_ns, own, __ATOMIC_RELEASE);
        pidns_put(old_own);
    }

    if (children) {
        pid_namespace_t *old = (pid_namespace_t *)__atomic_load_n(
            &t->pid_ns_for_children, __ATOMIC_ACQUIRE);
        if (children != old) {
            pidns_get(children);
            __atomic_store_n(&t->pid_ns_for_children, children, __ATOMIC_RELEASE);
            pidns_put(old);
        }
    }
    return 0;
}

int pidns_unshare(task_t *t)
{
    if (!t)
        return -ESRCH;

    /* The new namespace nests under the namespace the caller is a member of,
     * which is the one its children currently inherit. */
    pid_namespace_t *parent = pidns_task_own(t);
    if (parent->level + 1 >= PID_MAX_LEVELS)
        return -EUSERSRCH;

    pid_namespace_t *ns = pidns_alloc(parent);
    if (!ns)
        return -ENOMEM;

    uint64_t f2 = spin_lock_irqsave(&g_pidns_lock);
    ns->next = g_ns_list;
    g_ns_list = ns;
    ns->sibling = parent->children;
    parent->children = ns;
    spin_unlock_irqrestore(&g_pidns_lock, f2);

    /* unshare(CLONE_NEWPID) changes which namespace the caller's children
     * join and nothing else: the caller stays where it is, so its own getpid()
     * does not move.  Linux keeps this assignment until the task exits or
     * setns() replaces it -- every later fork lands in the new namespace too,
     * not just the first. */
    int r = pidns_assign(t, NULL, ns);
    /* Drop the creating reference pidns_alloc() handed us; the task field now
     * owns exactly one. */
    pidns_put(ns);
    return r;
}

int pidns_fork(task_t *child, task_t *parent, uint64_t clone_flags)
{
    if (!child || !parent)
        return -ESRCH;

    if (clone_flags & LINUX_CLONE_THREAD) {
        /* A thread shares its leader's namespaces: the same membership, and it
         * spawns into the same namespace the leader does.  Only its own id
         * differs, and pidns_assign() allocates one. */
        pid_namespace_t *own = pidns_task_own(parent);
        pid_namespace_t *kids = (pid_namespace_t *)__atomic_load_n(
            &parent->pid_ns_for_children, __ATOMIC_ACQUIRE);
        return pidns_assign(child, own, kids);
    }

    pid_namespace_t *target;
    int fresh = 0;
    if (clone_flags & LINUX_CLONE_NEWPID) {
        /* clone(CLONE_NEWPID): the child leads a brand new namespace and is
         * pid 1 inside it. */
        pid_namespace_t *base = pidns_task_own(parent);
        if (base->level + 1 >= PID_MAX_LEVELS)
            return -EUSERSRCH;
        target = pidns_alloc(base);
        if (!target)
            return -ENOMEM;
        uint64_t f2 = spin_lock_irqsave(&g_pidns_lock);
        target->next = g_ns_list;
        g_ns_list = target;
        target->sibling = base->children;
        base->children = target;
        spin_unlock_irqrestore(&g_pidns_lock, f2);
        fresh = 1;
    } else {
        target = (pid_namespace_t *)__atomic_load_n(
            &parent->pid_ns_for_children, __ATOMIC_ACQUIRE);
        if (!target)
            target = &g_init_ns;
    }

    /* The child joins the namespace the parent is spawning into, and spawns
     * into it in turn. */
    int r = pidns_assign(child, target, target);
    if (fresh)
        pidns_put(target);  /* drop the creating reference */
    return r;
}

int pidns_join(task_t *t, pid_namespace_t *ns)
{
    if (!t || !ns)
        return -EINVAL;
    if (ns->level >= PID_MAX_LEVELS)
        return -EUSERSRCH;
    /* setns(CLONE_NEWPID) changes which namespace the caller's children join;
     * it does not move the caller, exactly as in Linux. */
    return pidns_assign(t, NULL, ns);
}

void pidns_release_task(task_t *t)
{
    if (!t)
        return;

    pid_namespace_t *own = (pid_namespace_t *)__atomic_load_n(
        &t->pid_ns, __ATOMIC_ACQUIRE);
    pid_namespace_t *children = (pid_namespace_t *)__atomic_load_n(
        &t->pid_ns_for_children, __ATOMIC_ACQUIRE);

    /* Release the ids while pid_ns still points at the namespace to walk. */
    if (own && own != &g_init_ns)
        pidns_ids_release(t);

    __atomic_store_n(&t->pid_ns, NULL, __ATOMIC_RELAXED);
    __atomic_store_n(&t->pid_ns_for_children, NULL, __ATOMIC_RELAXED);
    pidns_put(own);
    pidns_put(children);
}
