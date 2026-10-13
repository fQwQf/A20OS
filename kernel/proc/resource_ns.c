#include "proc/resource_ns.h"
#include "proc/proc.h"
#include "proc/userns.h"
#include "cg/cgroup_impl.h"
#include "core/string.h"
#include "core/stdio.h"
#include "mm/slab.h"

static resource_namespace_t initial[8];
static spinlock_t ns_lock = SPINLOCK_INIT;
static uint64_t next_ino = 4026600000ULL;
static const uint64_t ns_flags[8] = {
    [PROCNS_UTS] = LINUX_CLONE_NEWUTS,
    [PROCNS_IPC] = LINUX_CLONE_NEWIPC,
    [PROCNS_NET] = LINUX_CLONE_NEWNET,
    [PROCNS_CGROUP] = LINUX_CLONE_NEWCGROUP,
};
static int valid_kind(int kind) {
    return kind > 0 && kind < 8 && ns_flags[kind];
}
void resource_ns_init(void) {
    static const uint64_t inos[8] = {
        [PROCNS_UTS] = 4026531838ULL,
        [PROCNS_IPC] = 4026531839ULL,
        [PROCNS_NET] = 4026531841ULL,
        [PROCNS_CGROUP] = 4026531835ULL,
    };
    for (int i = 0; i < 8; i++) {
        initial[i].kind = i;
        initial[i].ino = inos[i];
        initial[i].owner = userns_init_ns();
        refcount_set(&initial[i].refs, 1);
        spin_init(&initial[i].lock);
        initial[i].loopback_up = 1;
        initial[i].loopback_mtu = 65536;
        strcpy(initial[i].hostname, "A20OS");
    }
}
int resource_ns_is_initial(const resource_namespace_t *ns) {
    return ns && valid_kind(ns->kind) && ns == &initial[ns->kind];
}
resource_namespace_t *resource_ns_get(resource_namespace_t *ns) {
    if (ns && !resource_ns_is_initial(ns))
        refcount_inc(&ns->refs);
    return ns;
}
void resource_ns_put(resource_namespace_t *ns) {
    if (!ns || resource_ns_is_initial(ns) || !refcount_dec_and_test(&ns->refs))
        return;
    if (ns->kind == PROCNS_IPC) {
        sysv_msg_namespace_destroy(ns->ino);
        sysv_sem_namespace_destroy(ns->ino);
        sysv_shm_namespace_destroy(ns->ino);
        posix_mq_namespace_destroy(ns->ino);
    }
    if (ns->cgroup_root)
        __atomic_fetch_sub(&ns->cgroup_root->namespace_refs, 1,
                           __ATOMIC_RELEASE);
    userns_put(ns->owner);
    kfree(ns);
}
resource_namespace_t *resource_ns_task_get(task_t *t, int kind) {
    if (!valid_kind(kind))
        return NULL;
    uint64_t irq = spin_lock_irqsave(&ns_lock);
    resource_namespace_t *ns = t ? t->resource_ns[kind] : NULL;
    if (!ns)
        ns = &initial[kind];
    resource_ns_get(ns);
    spin_unlock_irqrestore(&ns_lock, irq);
    return ns;
}
resource_namespace_t *resource_ns_current(int kind) {
    task_t *t = proc_current();
    return t && t->resource_ns[kind] ? t->resource_ns[kind] : &initial[kind];
}
uint64_t resource_ns_task_ino(task_t *t, int kind) {
    resource_namespace_t *ns = resource_ns_task_get(t, kind);
    uint64_t ino = ns ? ns->ino : 0;
    resource_ns_put(ns);
    return ino;
}
uint64_t resource_ns_ino(int kind) {
    return resource_ns_current(kind)->ino;
}
static resource_namespace_t *new_ns(task_t *t, int kind) {
    resource_namespace_t *ns = kcalloc(1, sizeof(*ns));
    if (!ns)
        return NULL;
    ns->kind = kind;
    ns->ino = __atomic_fetch_add(&next_ino, 1, __ATOMIC_RELAXED);
    refcount_set(&ns->refs, 1);
    spin_init(&ns->lock);
    ns->owner = userns_task_get(t);
    ns->loopback_mtu = 65536;
    if (kind == PROCNS_UTS) {
        resource_namespace_t *old = resource_ns_task_get(t, kind);
        uint64_t irq = spin_lock_irqsave(&old->lock);
        memcpy(ns->hostname, old->hostname, sizeof(ns->hostname));
        memcpy(ns->domainname, old->domainname, sizeof(ns->domainname));
        spin_unlock_irqrestore(&old->lock, irq);
        resource_ns_put(old);
    }
    if (kind == PROCNS_CGROUP) {
        ns->cgroup_root = t->cgroup ? t->cgroup : cg_root_node();
        if (ns->cgroup_root)
            __atomic_fetch_add(&ns->cgroup_root->namespace_refs, 1,
                               __ATOMIC_RELAXED);
    }
    return ns;
}
int resource_ns_join(task_t *t, resource_namespace_t *ns) {
    if (!t || !ns || !valid_kind(ns->kind))
        return -EINVAL;
    resource_ns_get(ns);
    uint64_t irq = spin_lock_irqsave(&ns_lock);
    resource_namespace_t *old = t->resource_ns[ns->kind];
    t->resource_ns[ns->kind] = ns;
    spin_unlock_irqrestore(&ns_lock, irq);
    resource_ns_put(old);
    return 0;
}
int resource_ns_unshare(task_t *t, uint64_t flags) {
    resource_namespace_t *prepared[8] = {0};
    for (int i = 0; i < 8; i++) {
        if (!(flags & ns_flags[i]))
            continue;
        prepared[i] = new_ns(t, i);
        if (!prepared[i]) {
            for (int j = 0; j < 8; j++)
                resource_ns_put(prepared[j]);
            return -ENOMEM;
        }
    }
    /* All allocations precede publication: a failed multi-domain request
     * leaves every membership intact. */
    for (int i = 0; i < 8; i++) {
        if (!prepared[i])
            continue;
        resource_ns_join(t, prepared[i]);
        resource_ns_put(prepared[i]);
    }
    return 0;
}
int resource_ns_fork(task_t *child, task_t *parent, uint64_t flags) {
    for (int i = 0; i < 8; i++) {
        if (!valid_kind(i))
            continue;
        child->resource_ns[i] = resource_ns_task_get(parent, i);
    }
    int r = resource_ns_unshare(child, flags);
    if (r < 0)
        resource_ns_release_task(child);
    return r;
}
void resource_ns_release_task(task_t *t) {
    if (!t)
        return;
    for (int i = 0; i < 8; i++) {
        uint64_t irq = spin_lock_irqsave(&ns_lock);
        resource_namespace_t *old = t->resource_ns[i];
        t->resource_ns[i] = NULL;
        spin_unlock_irqrestore(&ns_lock, irq);
        resource_ns_put(old);
    }
}
void resource_ns_cgroup_path(task_t *target, char *buf, size_t len) {
    resource_namespace_t *ns = resource_ns_current(PROCNS_CGROUP);
    cg_node_t *root = ns->cgroup_root;
    cg_node_t *node =
        target && target->cgroup ? target->cgroup : cg_root_node();
    cg_node_t *root_path[64], *target_path[64];
    int roots = 0, targets = 0;
    if (!root)
        root = cg_root_node();
    for (cg_node_t *p = root; p && roots < 64; p = p->parent)
        root_path[roots++] = p;
    for (cg_node_t *p = node; p && targets < 64; p = p->parent)
        target_path[targets++] = p;
    while (roots && targets &&
           root_path[roots - 1] == target_path[targets - 1]) {
        roots--;
        targets--;
    }
    size_t off = 0;
    if (!roots && !targets)
        off = snprintf(buf, len, "/");
    while (roots-- && off < len)
        off += snprintf(buf + off, len - off, "/..");
    while (targets && off < len)
        off +=
            snprintf(buf + off, len - off, "/%s", target_path[--targets]->name);
}
