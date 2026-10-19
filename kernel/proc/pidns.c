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

/* ---------------------------------------------------------------------------
 * PID-namespace bridges.
 *
 * Rust owns the per-namespace id allocator and every membership transaction
 * (kernel/proc/pidns_state.rs): one id per level with unwind, release, the
 * unshare/fork/join placement decisions and the visibility predicate.  C keeps
 * the namespace objects — allocation, refcounts, registry and child lists,
 * initial pinning — the per-namespace lock primitive and the task fields.
 * Everything below is a mechanical accessor over those live objects.
 * ------------------------------------------------------------------------- */
extern int a20_pidns_visible(const void *ns, void *target);
extern int a20_pidns_unshare(void *task);
extern int a20_pidns_fork(void *child, void *parent, uint64_t clone_flags);
extern int a20_pidns_join(void *task, void *ns);
extern void a20_pidns_release_task(void *task);

int a20_pidns_level(const void *ns)
{
    return ns ? ((const pid_namespace_t *)ns)->level : 0;
}

void *a20_pidns_parent(const void *ns)
{
    return ns ? ((pid_namespace_t *)ns)->parent : NULL;
}

uint64_t a20_pidns_lock(void *ns)
{
    return spin_lock_irqsave(&((pid_namespace_t *)ns)->lock);
}

void a20_pidns_unlock(void *ns, uint64_t flags)
{
    spin_unlock_irqrestore(&((pid_namespace_t *)ns)->lock, flags);
}

uint64_t *a20_pidns_bitmap(void *ns, uint32_t *words)
{
    pid_namespace_t *p = ns;
    if (words)
        *words = p ? p->id_words : 0;
    return p ? p->id_bitmap : NULL;
}

int *a20_pidns_next_pid_ref(void *ns)
{
    return &((pid_namespace_t *)ns)->next_pid;
}

int *a20_pidns_nr_allocated_ref(void *ns)
{
    return &((pid_namespace_t *)ns)->nr_allocated;
}

int *a20_pidns_nr_tasks_ref(void *ns)
{
    return &((pid_namespace_t *)ns)->nr_tasks;
}

void *a20_pidns_init_ns(void)
{
    return pidns_init_ns();
}

void *a20_pidns_get(void *ns)
{
    return pidns_get((pid_namespace_t *)ns);
}

void a20_pidns_put(void *ns)
{
    pidns_put((pid_namespace_t *)ns);
}

void *a20_pidns_task_own(void *task)
{
    return pidns_task_own((task_t *)task);
}

void *a20_pidns_task_children(void *task)
{
    return __atomic_load_n(&((task_t *)task)->pid_ns_for_children,
                           __ATOMIC_ACQUIRE);
}

int a20_pidns_task_level(void *task)
{
    return ((task_t *)task)->pid_ns_level;
}

void a20_pidns_task_set_level(void *task, int level)
{
    ((task_t *)task)->pid_ns_level = level;
}

int a20_pidns_task_id(void *task, int level)
{
    return ((task_t *)task)->ns_pid[level];
}

void a20_pidns_task_set_id(void *task, int level, int id)
{
    ((task_t *)task)->ns_pid[level] = id;
}

void a20_pidns_task_set_own(void *task, void *ns)
{
    __atomic_store_n(&((task_t *)task)->pid_ns, (pid_namespace_t *)ns,
                     __ATOMIC_RELEASE);
}

void a20_pidns_task_set_children(void *task, void *ns)
{
    __atomic_store_n(&((task_t *)task)->pid_ns_for_children,
                     (pid_namespace_t *)ns, __ATOMIC_RELEASE);
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
    return a20_pidns_visible(ns, target);
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
    /* E1/E2: tasklist_lock walks the global list; each candidate's ->state is
     * sampled under its own park_lock.  The namespace chain and ->ns_pid[] are
     * fixed once the task is linked, so they need no lock. */
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (proc_task_state_get(t) == PROC_UNUSED)
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
            spin_unlock_irqrestore(&tasklist_lock, flags);
            return NULL;
        }
        spin_unlock_irqrestore(&tasklist_lock, flags);
        return t;
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    return NULL;
}

task_t *pidns_next_visible(pid_namespace_t *ns, task_t **iter)
{
    if (!ns || !iter)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    task_t *t = *iter ? proc_next_task_locked(*iter) : proc_first_task_locked();
    while (t) {
        if (proc_task_state_get(t) == PROC_UNUSED || !pidns_visible(ns, t)) {
            t = proc_next_task_locked(t);
            continue;
        }
        if (!proc_get(t)) {
            t = proc_next_task_locked(t);
            continue;
        }
        *iter = t;
        spin_unlock_irqrestore(&tasklist_lock, flags);
        return t;
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    return NULL;
}

int pidns_nr_tasks(pid_namespace_t *ns)
{
    if (!ns)
        return 0;
    int n = 0;
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (proc_task_state_get(t) == PROC_UNUSED)
            continue;
        if (pidns_visible(ns, t))
            n++;
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
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

void *a20_pidns_alloc_linked(void *parent)
{
    pid_namespace_t *base = parent;
    pid_namespace_t *ns = pidns_alloc(base);
    if (!ns)
        return NULL;
    /* Publishing the fresh namespace is part of the transaction: the registry
     * list and the parent's child list are installed under the global lock,
     * exactly as the C path did before assigning the task. */
    uint64_t f = spin_lock_irqsave(&g_pidns_lock);
    ns->next = g_ns_list;
    g_ns_list = ns;
    ns->sibling = base->children;
    base->children = ns;
    spin_unlock_irqrestore(&g_pidns_lock, f);
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

int pidns_unshare(task_t *t)
{
    return a20_pidns_unshare(t);
}

int pidns_fork(task_t *child, task_t *parent, uint64_t clone_flags)
{
    return a20_pidns_fork(child, parent, clone_flags);
}

int pidns_join(task_t *t, pid_namespace_t *ns)
{
    return a20_pidns_join(t, ns);
}

void pidns_release_task(task_t *t)
{
    a20_pidns_release_task(t);
}
