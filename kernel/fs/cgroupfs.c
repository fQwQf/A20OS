#include "fs/vfs.h"
#include "fs/file.h"
#include "abi/linux/fcntl.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "cg/cgroup.h"
#include "cg/cgroup_impl.h"
#include "core/cpu.h"
#include "core/consts.h"
#include "core/klog.h"

extern task_t *proc_current(void);

typedef struct {
    cg_file_t type;
    cg_node_t *node;
    cg_sb_t *sb;
} cg_priv_t;

static cg_sb_t *cg_global_sb;

static const char *cg_file_name(cg_file_t f, cg_ver_t ver)
{
    switch (f) {
    case CF_TASKS: return "tasks";
    case CF_CGROUP_PROCS: return ver == CG_V1 ? "tasks" : "cgroup.procs";
    case CF_NOTIFY_ON_RELEASE: return "notify_on_release";
    case CF_RELEASE_AGENT: return "release_agent";
    case CF_CLONE_CHILDREN: return "cgroup.clone_children";
    case CF_EVENT_CONTROL: return "cgroup.event_control";
    case CF_CGROUP_CONTROLLERS: return "cgroup.controllers";
    case CF_CGROUP_SUBTREE_CONTROL: return "cgroup.subtree_control";
    case CF_CGROUP_KILL: return "cgroup.kill";
    case CF_CGROUP_TYPE: return "cgroup.type";
    case CF_MEMORY_USAGE: return "memory.usage_in_bytes";
    case CF_MEMORY_LIMIT: return "memory.limit_in_bytes";
    case CF_MEMORY_MAX_USAGE: return "memory.max_usage_in_bytes";
    case CF_MEMORY_STAT: return "memory.stat";
    case CF_MEMORY_SWAPPINESS: return "memory.swappiness";
    case CF_MEMORY_USE_HIERARCHY: return "memory.use_hierarchy";
    case CF_MEMORY_MEMSW_USAGE: return "memory.memsw.usage_in_bytes";
    case CF_MEMORY_MEMSW_LIMIT: return "memory.memsw.limit_in_bytes";
    case CF_MEMORY_KMEM_USAGE: return "memory.kmem.usage_in_bytes";
    case CF_MEMORY_KMEM_LIMIT: return "memory.kmem.limit_in_bytes";
    case CF_MEMORY_CURRENT: return "memory.current";
    case CF_MEMORY_MAX: return "memory.max";
    case CF_MEMORY_MIN: return "memory.min";
    case CF_MEMORY_LOW: return "memory.low";
    case CF_MEMORY_EVENTS: return "memory.events";
    case CF_MEMORY_SWAP_CURRENT: return "memory.swap.current";
    case CF_MEMORY_SWAP_MAX: return "memory.swap.max";
    case CF_CPUSET_CPUS: return "cpuset.cpus";
    case CF_CPUSET_MEMS: return "cpuset.mems";
    case CF_CPUSET_MEMORY_MIGRATE: return "cpuset.memory_migrate";
    case CF_CPU_CFS_QUOTA: return "cpu.cfs_quota_us";
    case CF_CPU_CFS_PERIOD: return "cpu.cfs_period_us";
    case CF_CPU_SHARES: return "cpu.shares";
    case CF_CPU_STAT: return "cpu.stat";
    case CF_CPU_MAX: return "cpu.max";
    default: return NULL;
    }
}

static int cg_file_writable(cg_file_t f)
{
    switch (f) {
    case CF_TASKS:
    case CF_CGROUP_PROCS:
    case CF_NOTIFY_ON_RELEASE:
    case CF_RELEASE_AGENT:
    case CF_CLONE_CHILDREN:
    case CF_EVENT_CONTROL:
    case CF_CGROUP_SUBTREE_CONTROL:
    case CF_CGROUP_KILL:
    case CF_MEMORY_LIMIT:
    case CF_MEMORY_SWAPPINESS:
    case CF_MEMORY_USE_HIERARCHY:
    case CF_MEMORY_MEMSW_LIMIT:
    case CF_MEMORY_KMEM_LIMIT:
    case CF_MEMORY_MAX:
    case CF_MEMORY_MIN:
    case CF_MEMORY_LOW:
    case CF_MEMORY_SWAP_MAX:
    case CF_CPUSET_CPUS:
    case CF_CPUSET_MEMS:
    case CF_CPUSET_MEMORY_MIGRATE:
    case CF_CPU_CFS_QUOTA:
    case CF_CPU_CFS_PERIOD:
    case CF_CPU_SHARES:
    case CF_CPU_MAX:
        return 1;
    default:
        return 0;
    }
}

static void cg_add_file(cg_node_t *n, cg_file_t f)
{
    if (n->file_count < CG_MAX_FILES)
        n->files[n->file_count++] = f;
}

static cg_node_t *cg_node_create(const char *name, cg_node_t *parent)
{
    cg_node_t *n = (cg_node_t *)kmalloc(sizeof(cg_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->parent = parent;
    spin_init(&n->lock);
    cg_mem_init(&n->res.mem);
    cg_cpu_init(&n->res.cpu);
    cg_cpuset_init(&n->res.cpuset, CONFIG_NR_CPUS);
    return n;
}

static void cg_node_populate_files(cg_node_t *n, cg_sb_t *sb)
{
    n->file_count = 0;
    if (sb->ver == CG_V1) {
        cg_add_file(n, CF_TASKS);
        cg_add_file(n, CF_CGROUP_PROCS);
        cg_add_file(n, CF_NOTIFY_ON_RELEASE);
        cg_add_file(n, CF_RELEASE_AGENT);
        cg_add_file(n, CF_CLONE_CHILDREN);
        cg_add_file(n, CF_EVENT_CONTROL);
        if (sb->controllers & CTRL_MEMORY) {
            cg_add_file(n, CF_MEMORY_USAGE);
            cg_add_file(n, CF_MEMORY_LIMIT);
            cg_add_file(n, CF_MEMORY_MAX_USAGE);
            cg_add_file(n, CF_MEMORY_STAT);
            cg_add_file(n, CF_MEMORY_SWAPPINESS);
            cg_add_file(n, CF_MEMORY_USE_HIERARCHY);
            cg_add_file(n, CF_MEMORY_MEMSW_USAGE);
            cg_add_file(n, CF_MEMORY_MEMSW_LIMIT);
            cg_add_file(n, CF_MEMORY_KMEM_USAGE);
            cg_add_file(n, CF_MEMORY_KMEM_LIMIT);
        }
        if (sb->controllers & CTRL_CPUSET) {
            cg_add_file(n, CF_CPUSET_CPUS);
            cg_add_file(n, CF_CPUSET_MEMS);
            cg_add_file(n, CF_CPUSET_MEMORY_MIGRATE);
        }
        if (sb->controllers & CTRL_CPU) {
            cg_add_file(n, CF_CPU_CFS_QUOTA);
            cg_add_file(n, CF_CPU_CFS_PERIOD);
            cg_add_file(n, CF_CPU_SHARES);
            cg_add_file(n, CF_CPU_STAT);
        }
    } else {
        cg_add_file(n, CF_CGROUP_PROCS);
        cg_add_file(n, CF_CGROUP_CONTROLLERS);
        cg_add_file(n, CF_CGROUP_SUBTREE_CONTROL);
        cg_add_file(n, CF_CGROUP_KILL);
        cg_add_file(n, CF_CGROUP_TYPE);
        cg_add_file(n, CF_MEMORY_CURRENT);
        cg_add_file(n, CF_MEMORY_MAX);
        cg_add_file(n, CF_MEMORY_MIN);
        cg_add_file(n, CF_MEMORY_LOW);
        cg_add_file(n, CF_MEMORY_EVENTS);
        cg_add_file(n, CF_MEMORY_STAT);
        cg_add_file(n, CF_MEMORY_SWAP_CURRENT);
        cg_add_file(n, CF_MEMORY_SWAP_MAX);
        cg_add_file(n, CF_CPU_MAX);
        cg_add_file(n, CF_CPUSET_CPUS);
        cg_add_file(n, CF_CPUSET_MEMS);
    }
}

static int cg_generate_content(cg_file_t f, cg_sb_t *sb, cg_node_t *node, char *buf, size_t bufsz)
{
    buf[0] = '\0';
    switch (f) {
    case CF_TASKS:
    case CF_CGROUP_PROCS: {
        int off = 0;
        if (node && node->is_root) {
            task_t *t;
            for (t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
                if (t->state != PROC_ZOMBIE) {
                    off += snprintf(buf + off, bufsz - off, "%d\n", t->pid);
                    if (off >= (int)bufsz - 16) break;
                }
            }
        } else if (node) {
            for (int i = 0; i < node->pid_count; i++) {
                off += snprintf(buf + off, bufsz - off, "%d\n", node->pids[i]);
                if (off >= (int)bufsz - 16) break;
            }
        }
        break;
    }
    case CF_NOTIFY_ON_RELEASE:
        snprintf(buf, bufsz, "0\n");
        break;
    case CF_RELEASE_AGENT:
        snprintf(buf, bufsz, "\n");
        break;
    case CF_CLONE_CHILDREN:
        snprintf(buf, bufsz, "0\n");
        break;
    case CF_CGROUP_CONTROLLERS:
        if (sb->ver == CG_V2) {
            buf[0] = '\0';
            if (sb->controllers & CTRL_MEMORY)  strcat(buf, "memory ");
            if (sb->controllers & CTRL_CPU)     strcat(buf, "cpu ");
            if (sb->controllers & CTRL_CPUSET)  strcat(buf, "cpuset ");
            if (sb->controllers & CTRL_CPUACCT) strcat(buf, "cpuacct ");
            strcat(buf, "\n");
        }
        break;
    case CF_CGROUP_SUBTREE_CONTROL:
        snprintf(buf, bufsz, "\n");
        break;
    case CF_CGROUP_TYPE:
        snprintf(buf, bufsz, "domain\n");
        break;

    case CF_MEMORY_USAGE:
    case CF_MEMORY_CURRENT:
    case CF_MEMORY_KMEM_USAGE: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t rss = node->res.mem.rss;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%lu\n", (unsigned long)(rss * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_MAX_USAGE: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t rss = node->res.mem.rss;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%lu\n", (unsigned long)(rss * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_LIMIT: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t lim = node->res.mem.limit;
        spin_unlock_irqrestore(&node->lock, flags);
        if (lim == SIZE_MAX)
            snprintf(buf, bufsz, "9223372036854771712\n");
        else
            snprintf(buf, bufsz, "%lu\n", (unsigned long)(lim * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_MEMSW_LIMIT: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t lim = node->res.mem.swap_limit;
        spin_unlock_irqrestore(&node->lock, flags);
        if (lim == SIZE_MAX)
            snprintf(buf, bufsz, "9223372036854771712\n");
        else
            snprintf(buf, bufsz, "%lu\n", (unsigned long)(lim * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_KMEM_LIMIT:
        snprintf(buf, bufsz, "9223372036854771712\n");
        break;
    case CF_MEMORY_STAT: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        cg_mem_state_t *m = &node->res.mem;
        int off = snprintf(buf, bufsz,
            "cache %lu\nrss %lu\nrss_huge 0\nmapped_file 0\nswap %lu\n"
            "pgpgin 0\npgpgout 0\ninactive_anon 0\nactive_anon %lu\n"
            "inactive_file 0\nactive_file 0\nunevictable 0\n"
            "hierarchical_memory_limit %lu\n"
            "hierarchical_memsw_limit %lu\n"
            "total_cache 0\ntotal_rss %lu\ntotal_swap %lu\n",
            (unsigned long)m->cache, (unsigned long)m->rss,
            (unsigned long)m->swap_usage,
            (unsigned long)m->rss,
            m->limit == SIZE_MAX ? 9223372036854771712UL
                                 : (unsigned long)(m->limit * PAGE_SIZE),
            m->swap_limit == SIZE_MAX ? 9223372036854771712UL
                                      : (unsigned long)(m->swap_limit * PAGE_SIZE),
            (unsigned long)m->rss, (unsigned long)m->swap_usage);
        spin_unlock_irqrestore(&node->lock, flags);
        (void)off;
        break;
    }
    case CF_MEMORY_SWAPPINESS:
        snprintf(buf, bufsz, "60\n");
        break;
    case CF_MEMORY_USE_HIERARCHY:
        snprintf(buf, bufsz, "1\n");
        break;
    case CF_MEMORY_MEMSW_USAGE: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t total = node->res.mem.rss + node->res.mem.swap_usage;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%lu\n", (unsigned long)(total * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_MAX: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t lim = node->res.mem.limit;
        spin_unlock_irqrestore(&node->lock, flags);
        if (lim == SIZE_MAX)
            snprintf(buf, bufsz, "max\n");
        else
            snprintf(buf, bufsz, "%lu\n", (unsigned long)(lim * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_SWAP_MAX: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t lim = node->res.mem.swap_limit;
        spin_unlock_irqrestore(&node->lock, flags);
        if (lim == SIZE_MAX)
            snprintf(buf, bufsz, "max\n");
        else
            snprintf(buf, bufsz, "%lu\n", (unsigned long)(lim * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_MIN:
    case CF_MEMORY_LOW:
        snprintf(buf, bufsz, "0\n");
        break;
    case CF_MEMORY_EVENTS: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        int off = snprintf(buf, bufsz,
            "low 0\nhigh 0\nmax %lu\noom %lu\noom_kill %d\n",
            (unsigned long)node->res.mem.failcnt,
            (unsigned long)node->res.mem.failcnt,
            node->res.mem.oom_kill_count);
        spin_unlock_irqrestore(&node->lock, flags);
        (void)off;
        break;
    }
    case CF_MEMORY_SWAP_CURRENT: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t sw = node->res.mem.swap_usage;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%lu\n", (unsigned long)(sw * PAGE_SIZE));
        break;
    }
    case CF_CPUSET_CPUS: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        uint32_t mask = node->res.cpuset.cpus_allowed;
        spin_unlock_irqrestore(&node->lock, flags);
        if (!mask) mask = (1U << CONFIG_NR_CPUS) - 1;
        int off = 0;
        int first = 1;
        for (unsigned i = 0; i < CONFIG_NR_CPUS && i < 32; i++) {
            if (mask & (1U << i)) {
                if (!first) off += snprintf(buf + off, bufsz - off, ",");
                off += snprintf(buf + off, bufsz - off, "%u", i);
                first = 0;
            }
        }
        off += snprintf(buf + off, bufsz - off, "\n");
        break;
    }
    case CF_CPUSET_MEMS:
        snprintf(buf, bufsz, "0\n");
        break;
    case CF_CPUSET_MEMORY_MIGRATE:
        snprintf(buf, bufsz, "0\n");
        break;
    case CF_CPU_CFS_QUOTA: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        uint64_t quota = node->res.cpu.quota;
        spin_unlock_irqrestore(&node->lock, flags);
        if (quota == CG_CPU_QUOTA_MAX)
            snprintf(buf, bufsz, "-1\n");
        else
            snprintf(buf, bufsz, "%lld\n", (long long)(quota / 1000));
        break;
    }
    case CF_CPU_CFS_PERIOD: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        uint64_t period = node->res.cpu.period;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%lld\n", (long long)(period / 1000));
        break;
    }
    case CF_CPU_SHARES:
        snprintf(buf, bufsz, "1024\n");
        break;
    case CF_CPU_STAT: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        cg_cpu_state_t *c = &node->res.cpu;
        int off = snprintf(buf, bufsz,
            "nr_periods %llu\nnr_throttled %u\nthrottled_time %llu\n",
            (unsigned long long)(c->total_runtime / (c->period ? c->period : 1)),
            c->nr_throttled,
            (unsigned long long)c->throttled_time);
        spin_unlock_irqrestore(&node->lock, flags);
        (void)off;
        break;
    }
    case CF_CPU_MAX: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        uint64_t quota = node->res.cpu.quota;
        uint64_t period = node->res.cpu.period;
        spin_unlock_irqrestore(&node->lock, flags);
        if (quota == CG_CPU_QUOTA_MAX)
            snprintf(buf, bufsz, "max %llu\n", (unsigned long long)(period / 1000));
        else
            snprintf(buf, bufsz, "%llu %llu\n",
                     (unsigned long long)(quota / 1000),
                     (unsigned long long)(period / 1000));
        break;
    }
    default:
        break;
    }
    return (int)strlen(buf);
}

static cg_file_t cg_find_file_by_name(const char *name, cg_node_t *node, cg_sb_t *sb)
{
    for (int i = 0; i < node->file_count; i++) {
        const char *fn = cg_file_name(node->files[i], sb->ver);
        if (fn && strcmp(name, fn) == 0)
            return node->files[i];
    }
    /* V1 cpuset without prefix */
    if (sb->ver == CG_V1 && (sb->controllers & CTRL_CPUSET)) {
        if (strcmp(name, "cpus") == 0) return CF_CPUSET_CPUS;
        if (strcmp(name, "mems") == 0) return CF_CPUSET_MEMS;
        if (strcmp(name, "memory_migrate") == 0) return CF_CPUSET_MEMORY_MIGRATE;
    }
    return CF_FILE_MAX;
}

static cg_node_t *cg_find_child(cg_node_t *parent, const char *name)
{
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] && strcmp(parent->children[i]->name, name) == 0)
            return parent->children[i];
    }
    return NULL;
}

static cg_sb_t *cg_get_sb(vnode_t *vn)
{
    if (!vn || !vn->mnt) return NULL;
    return (cg_sb_t *)vn->mnt->fs_data;
}

/* ---- vnode ops ---- */

static int cg_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    cg_sb_t *sb = cg_get_sb(dir);
    if (!sb) return -ENOENT;

    cg_node_t *node = (cg_node_t *)dir->fs_data;
    if (!node) return -ENOENT;

    if (strcmp(name, ".") == 0) { *out = dir; vnode_get(dir); return 0; }
    if (strcmp(name, "..") == 0) {
        if (node->parent) {
            /* find parent vnode — we just return dir for simplicity */
            *out = dir; vnode_get(dir); return 0;
        }
        *out = dir; vnode_get(dir); return 0;
    }

    /* check if it's a child directory */
    cg_node_t *child = cg_find_child(node, name);
    if (child) {
        vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
        if (!vn) return -ENOMEM;
        memset(vn, 0, sizeof(*vn));
        vn->ino = (uint64_t)(uintptr_t)child;
        vn->type = VFS_FT_DIR;
        vn->mode = S_IFDIR | 0755;
        vn->parent = dir;
        vnode_get(dir);
        vn->ops = dir->ops;
        vn->mnt = dir->mnt;
        vnode_ref_init(vn, 1);
        vn->fs_data = child;
        vn->uid = child->uid;
        vn->gid = child->gid;
        *out = vn;
        return 0;
    }

    /* check if it's a file */
    cg_file_t ft = cg_find_file_by_name(name, node, sb);
    if (ft != CF_FILE_MAX) {
        vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
        if (!vn) return -ENOMEM;
        memset(vn, 0, sizeof(*vn));
        vn->ino = 0;
        vn->type = VFS_FT_REGULAR;
        vn->mode = S_IFREG | (cg_file_writable(ft) ? 0644 : 0444);
        vn->parent = dir;
        vnode_get(dir);
        vn->ops = dir->ops;
        vn->mnt = dir->mnt;
        vnode_ref_init(vn, 1);
        char tmp[512];
        vn->size = (size_t)cg_generate_content(ft, sb, node, tmp, sizeof(tmp));
        vn->fs_data = node;
        vn->uid = node->uid;
        vn->gid = node->gid;
        /* Store file type in the upper bits of ino */
        vn->ino = (uint64_t)ft + 1;
        *out = vn;
        return 0;
    }

    return -ENOENT;
}

static int cg_create(vnode_t *dir, const char *name, int mode, vnode_t **out)
{
    (void)dir; (void)name; (void)mode; (void)out;
    return -EROFS;
}

static int cg_mkdir(vnode_t *dir, const char *name, int mode)
{
    (void)mode;
    cg_node_t *parent = (cg_node_t *)dir->fs_data;
    cg_sb_t *sb = cg_get_sb(dir);
    if (!parent || !sb) return -ENOENT;
    if (parent->child_count >= CG_MAX_CHILDREN) return -ENOSPC;
    if (cg_find_child(parent, name)) return -EEXIST;
    /* check it's not a file name */
    if (cg_find_file_by_name(name, parent, sb) != CF_FILE_MAX) return -EEXIST;

    cg_node_t *child = cg_node_create(name, parent);
    if (!child) return -ENOMEM;
    child->is_root = 0;
    cg_node_populate_files(child, sb);
    parent->children[parent->child_count++] = child;
    return 0;
}

static int cg_unlink(vnode_t *dir, const char *name)
{
    (void)dir; (void)name;
    return -EROFS;
}

static void cg_node_free_recursive(cg_node_t *node)
{
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) {
        cg_node_free_recursive(node->children[i]);
        node->children[i] = NULL;
    }
    node->child_count = 0;
    kfree(node);
}

static int cg_rmdir(vnode_t *dir, const char *name)
{
    cg_node_t *parent = (cg_node_t *)dir->fs_data;
    if (!parent) return -ENOENT;

    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] && strcmp(parent->children[i]->name, name) == 0) {
            cg_node_t *child = parent->children[i];
            cg_node_free_recursive(child);
            /* move last child into this slot */
            parent->children[i] = parent->children[parent->child_count - 1];
            parent->children[parent->child_count - 1] = NULL;
            parent->child_count--;
            return 0;
        }
    }
    return -ENOENT;
}

static int cg_rename(vnode_t *od, const char *on, vnode_t *nd, const char *nn)
{
    (void)od; (void)on; (void)nd; (void)nn;
    return -EROFS;
}

static int cg_stat(vnode_t *vn, kstat_t *st)
{
    cg_node_t *node = (cg_node_t *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    st->st_ino = vn->ino;
    st->st_mode = vn->mode;
    st->st_uid = node ? node->uid : 0;
    st->st_gid = node ? node->gid : 0;
    st->st_size = vn->size;
    st->st_nlink = 1;
    return 0;
}

static int cg_chown(vnode_t *vn, int uid, int gid)
{
    cg_node_t *node = (cg_node_t *)vn->fs_data;
    if (uid != -1) {
        vn->uid = (uint32_t)uid;
        if (node) node->uid = (uint32_t)uid;
    }
    if (gid != -1) {
        vn->gid = (uint32_t)gid;
        if (node) node->gid = (uint32_t)gid;
    }
    return 0;
}

static void cg_release(vnode_t *vn)
{
    if (vn->fs_data && vn->type == VFS_FT_DIR) {
        /* Only free non-root nodes */
        cg_node_t *node = (cg_node_t *)vn->fs_data;
        if (!node->is_root) {
            /* Don't kfree here — nodes live in the tree, freed on unmount */
        }
    }
    vnode_put(vn->parent);
    kfree(vn);
}

static vnode_ops_t g_cg_vnode_ops = {
    .lookup  = cg_lookup,
    .create  = cg_create,
    .mkdir   = cg_mkdir,
    .unlink  = cg_unlink,
    .rmdir   = cg_rmdir,
    .rename  = cg_rename,
    .stat    = cg_stat,
    .chown   = cg_chown,
    .release = cg_release,
};

/* ---- file ops ---- */

static int cg_fread(vfile_t *vf, char *buf, size_t count)
{
    cg_priv_t *p = (cg_priv_t *)vf->priv;
    if (!p) return -EBADF;

    char content[1024];
    int len = cg_generate_content(p->type, p->sb, p->node, content, sizeof(content));
    if ((int)vf->offset >= len) return 0;
    size_t avail = (size_t)len - vf->offset;
    size_t n = count < avail ? count : avail;
    memcpy(buf, content + vf->offset, n);
    vf->offset += n;
    return (int)n;
}

static long long cg_strtoll(const char *s, char **end)
{
    long long val = 0;
    int neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -val : val;
}

static long long cg_parse_ll(const char *buf, size_t count)
{
    char kbuf[32];
    size_t n = count < sizeof(kbuf) - 1 ? count : sizeof(kbuf) - 1;
    memcpy(kbuf, buf, n);
    kbuf[n] = '\0';
    long long val = 0;
    int neg = 0;
    size_t i = 0;
    if (i < n && kbuf[i] == '-') { neg = 1; i++; }
    for (; i < n; i++) {
        if (kbuf[i] >= '0' && kbuf[i] <= '9')
            val = val * 10 + (kbuf[i] - '0');
        else
            break;
    }
    return neg ? -val : val;
}

static int cg_parse_int(const char *buf, size_t count)
{
    return (int)cg_parse_ll(buf, count);
}

static int cg_fwrite(vfile_t *vf, const char *buf, size_t count)
{
    cg_priv_t *p = (cg_priv_t *)vf->priv;
    if (!p) return -EBADF;
    if (!cg_file_writable(p->type)) return -EINVAL;

    if ((p->type == CF_TASKS || p->type == CF_CGROUP_PROCS) && p->node && buf) {
        int pid = cg_parse_int(buf, count);
        if (pid > 0 && p->node->pid_count < CG_MAX_PIDS) {
            int dup = 0;
            for (int i = 0; i < p->node->pid_count; i++) {
                if (p->node->pids[i] == pid) { dup = 1; break; }
            }
            if (!dup) {
                p->node->pids[p->node->pid_count++] = pid;
                cg_attach_task(p->node, pid);
            }
        }
    }

    if (p->type == CF_CGROUP_KILL && p->node && buf) {
        int val = cg_parse_int(buf, count);
        if (val == 1) {
            for (int i = 0; i < p->node->pid_count; i++) {
                signal_send(p->node->pids[i], 9);
            }
            p->node->pid_count = 0;
        }
    }

    if (!p->node) return (int)count;

    if (p->type == CF_MEMORY_LIMIT || p->type == CF_MEMORY_MAX) {
        long long val = cg_parse_ll(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        if (val <= 0 || (val > 0 && strncmp(buf, "max", 3) == 0))
            p->node->res.mem.limit = SIZE_MAX;
        else
            p->node->res.mem.limit = (size_t)val / PAGE_SIZE;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }

    if (p->type == CF_MEMORY_MEMSW_LIMIT || p->type == CF_MEMORY_SWAP_MAX) {
        long long val = cg_parse_ll(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        if (val <= 0 || (val > 0 && strncmp(buf, "max", 3) == 0))
            p->node->res.mem.swap_limit = SIZE_MAX;
        else
            p->node->res.mem.swap_limit = (size_t)val / PAGE_SIZE;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }

    if (p->type == CF_CPU_CFS_QUOTA) {
        long long val = cg_parse_ll(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        if (val < 0)
            p->node->res.cpu.quota = CG_CPU_QUOTA_MAX;
        else
            p->node->res.cpu.quota = (uint64_t)val * 1000;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }

    if (p->type == CF_CPU_CFS_PERIOD) {
        long long val = cg_parse_ll(buf, count);
        if (val > 0) {
            uint64_t flags = spin_lock_irqsave(&p->node->lock);
            p->node->res.cpu.period = (uint64_t)val * 1000;
            spin_unlock_irqrestore(&p->node->lock, flags);
        }
    }

    if (p->type == CF_CPU_MAX) {
        char kbuf[64];
        size_t n = count < sizeof(kbuf) - 1 ? count : sizeof(kbuf) - 1;
        memcpy(kbuf, buf, n);
        kbuf[n] = '\0';
        long long quota_us = -1, period_us = 100000;
        if (strncmp(kbuf, "max", 3) == 0) {
            char *space = strchr(kbuf + 3, ' ');
            if (space) period_us = cg_strtoll(space + 1, NULL);
        } else {
            char *space = strchr(kbuf, ' ');
            if (space) {
                *space = '\0';
                quota_us = cg_strtoll(kbuf, NULL);
                period_us = cg_strtoll(space + 1, NULL);
            } else {
                quota_us = cg_strtoll(kbuf, NULL);
            }
        }
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        p->node->res.cpu.quota = quota_us < 0 ? CG_CPU_QUOTA_MAX : (uint64_t)quota_us * 1000;
        p->node->res.cpu.period = period_us > 0 ? (uint64_t)period_us * 1000 : 100000000ULL;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }

    if (p->type == CF_CPUSET_CPUS) {
        char kbuf[128];
        size_t n = count < sizeof(kbuf) - 1 ? count : sizeof(kbuf) - 1;
        memcpy(kbuf, buf, n);
        kbuf[n] = '\0';
        uint32_t mask = 0;
        char *tok = kbuf;
        while (*tok) {
            while (*tok == ',' || *tok == ' ') tok++;
            if (*tok == '\0' || *tok == '\n') break;
            long long cpu = cg_strtoll(tok, &tok);
            if (cpu >= 0 && cpu < (long long)CONFIG_NR_CPUS && cpu < 32)
                mask |= (1U << (unsigned)cpu);
            if (*tok == '-') {
                long long end = cg_strtoll(tok + 1, &tok);
                for (long long c = cpu; c <= end && c < (long long)CONFIG_NR_CPUS && c < 32; c++)
                    if (c >= 0) mask |= (1U << (unsigned)c);
            }
        }
        if (!mask) mask = (1U << CONFIG_NR_CPUS) - 1;
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        p->node->res.cpuset.cpus_allowed = mask;
        cg_cpuset_update_effective(p->node, CONFIG_NR_CPUS);
        spin_unlock_irqrestore(&p->node->lock, flags);
        for (int i = 0; i < p->node->pid_count; i++) {
            task_t *t = proc_find(p->node->pids[i]);
            if (t) t->cpus_allowed = mask;
        }
    }

    return (int)count;
}

static long cg_flseek(vfile_t *vf, long offset, int whence)
{
    cg_priv_t *p = (cg_priv_t *)vf->priv;
    if (!p) return -EBADF;
    char content[1024];
    long clen = (long)cg_generate_content(p->type, p->sb, p->node, content, sizeof(content));
    long new_off;
    switch (whence) {
    case SEEK_SET: new_off = offset; break;
    case SEEK_CUR: new_off = (long)vf->offset + offset; break;
    case SEEK_END: new_off = clen + offset; break;
    default: return -EINVAL;
    }
    if (new_off < 0) new_off = 0;
    vf->offset = (size_t)new_off;
    return new_off;
}

static int cg_freaddir(vfile_t *vf, void *dirp, size_t count)
{
    cg_priv_t *p = (cg_priv_t *)vf->priv;
    if (!p) return -EBADF;
    cg_node_t *node = p->node;
    cg_sb_t *sb = p->sb;

    int idx = (int)vf->offset;
    size_t total = 0;
    char *out = (char *)dirp;

    while (total < count) {
        const char *name = NULL;
        int is_dir = 0;

        if (idx == 0) { name = "."; is_dir = 1; }
        else if (idx == 1) { name = ".."; is_dir = 1; }
        else if (idx - 2 < node->child_count) {
            name = node->children[idx - 2]->name;
            is_dir = 1;
        } else {
            int fidx = idx - 2 - node->child_count;
            if (fidx < node->file_count) {
                name = cg_file_name(node->files[fidx], sb->ver);
                is_dir = 0;
                /* V1 cpuset alias check */
                if (!name && sb->ver == CG_V1 && (sb->controllers & CTRL_CPUSET)) {
                    cg_file_t f = node->files[fidx];
                    if (f == CF_CPUSET_CPUS) name = "cpuset.cpus";
                    else if (f == CF_CPUSET_MEMS) name = "cpuset.mems";
                    else if (f == CF_CPUSET_MEMORY_MIGRATE) name = "cpuset.memory_migrate";
                }
            }
        }

        if (!name) break;
        size_t namelen = strlen(name);
        size_t reclen = (sizeof(vfs_dirent64_t) + namelen + 1 + 7) & ~7UL;
        if (total + reclen > count) break;

        vfs_dirent64_t *d = (vfs_dirent64_t *)(out + total);
        d->d_ino = (uint64_t)idx;
        d->d_off = (int64_t)(total + reclen);
        d->d_reclen = (uint16_t)reclen;
        d->d_type = is_dir ? 4 : 8;
        memcpy(d->d_name, name, namelen + 1);
        total += reclen;
        idx++;
    }
    vf->offset = (size_t)idx;
    return (int)total;
}

static int cg_fclose(vfile_t *vf)
{
    if (vf && vf->priv) { kfree(vf->priv); vf->priv = NULL; }
    return 0;
}

static vfile_ops_t g_cg_fops = {
    .read    = cg_fread,
    .write   = cg_fwrite,
    .lseek   = cg_flseek,
    .readdir = cg_freaddir,
    .close   = cg_fclose,
};

vfile_t *cgroupfs_open_vnode(vnode_t *vn, int flags)
{
    vfile_t *vf = vfile_alloc();
    if (!vf) return NULL;
    vf->vnode = vn;
    vf->flags = flags;
    vnode_get(vn);
    vf->ops = &g_cg_fops;
    refcount_set(&vf->ref_count, 1);

    cg_sb_t *sb = cg_get_sb(vn);
    cg_node_t *node = (cg_node_t *)vn->fs_data;

    cg_priv_t *priv = (cg_priv_t *)kmalloc(sizeof(cg_priv_t));
    if (!priv) { vfile_free(vf); return NULL; }
    memset(priv, 0, sizeof(*priv));
    priv->sb = sb ? sb : NULL;
    priv->node = node;

    if (vn->type == VFS_FT_REGULAR && vn->ino > 0) {
        priv->type = (cg_file_t)(vn->ino - 1);
    } else {
        priv->type = CF_FILE_MAX;
    }
    vf->priv = priv;
    return vf;
}

static uint32_t cg_parse_controllers(const char *opts)
{
    uint32_t ctrl = 0;
    if (!opts || !*opts) return 0;
    char buf[256];
    strncpy(buf, opts, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        if (strcmp(tok, "memory") == 0)  ctrl |= CTRL_MEMORY;
        else if (strcmp(tok, "cpu") == 0)     ctrl |= CTRL_CPU;
        else if (strcmp(tok, "cpuset") == 0)  ctrl |= CTRL_CPUSET;
        else if (strcmp(tok, "cpuacct") == 0) ctrl |= CTRL_CPUACCT;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return ctrl;
}

vnode_t *cgroupfs_mount(int is_v2, const char *opts, void **out_sb)
{
    cg_sb_t *sb = (cg_sb_t *)kmalloc(sizeof(cg_sb_t));
    if (!sb) return NULL;
    memset(sb, 0, sizeof(*sb));
    sb->ver = is_v2 ? CG_V2 : CG_V1;

    if (is_v2) {
        sb->controllers = CTRL_MEMORY | CTRL_CPU | CTRL_CPUSET | CTRL_CPUACCT;
    } else {
        sb->controllers = cg_parse_controllers(opts);
        if (sb->controllers == 0) {
            /* If no specific controller from opts, try the src as controller name */
            sb->controllers = CTRL_MEMORY | CTRL_CPU | CTRL_CPUSET | CTRL_CPUACCT;
        }
    }

    cg_node_t *root = cg_node_create("", NULL);
    if (!root) { kfree(sb); return NULL; }
    root->is_root = 1;
    cg_node_populate_files(root, sb);
    sb->root = root;

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) { kfree(root); kfree(sb); return NULL; }
    memset(vn, 0, sizeof(*vn));
    vn->ino = 0;
    vn->type = VFS_FT_DIR;
    vn->mode = S_IFDIR | 0755;
    vnode_ref_init(vn, 1);
    vn->ops = &g_cg_vnode_ops;
    vn->fs_data = root;
    if (out_sb) *out_sb = sb;
    cg_global_sb = sb;
    return vn;
}

void cgroupfs_unmount(vnode_t *root)
{
    if (!root) return;
    cg_node_t *node = (cg_node_t *)root->fs_data;
    if (node) {
        cg_node_free_recursive(node);
    }
    if (root->mnt && root->mnt->fs_data) {
        kfree(root->mnt->fs_data);
        root->mnt->fs_data = NULL;
    }
    kfree(root);
}

struct cg_node *cg_root_node(void)
{
    if (!cg_global_sb || !cg_global_sb->root)
        return NULL;
    return (struct cg_node *)cg_global_sb->root;
}

void cg_attach_task(struct cg_node *cg, int pid)
{
    task_t *t = proc_find(pid);
    if (!t) return;
    t->cgroup = cg;
    uint64_t flags = spin_lock_irqsave(&((cg_node_t *)cg)->lock);
    t->cpus_allowed = ((cg_node_t *)cg)->res.cpuset.cpus_allowed;
    spin_unlock_irqrestore(&((cg_node_t *)cg)->lock, flags);
    if (!t->cpus_allowed)
        t->cpus_allowed = (1U << CONFIG_NR_CPUS) - 1;
}

void cg_detach_task(struct cg_node *cg, int pid)
{
    (void)cg;
    task_t *t = proc_find(pid);
    if (!t) return;
    t->cgroup = NULL;
    t->cg_throttled = 0;
}
