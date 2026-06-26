#include "fs/vfs.h"
#include "fs/file.h"
#include "core/fcntl.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "cg/cgroup.h"
#include "cg/cgroup_impl.h"
#include "core/cpu.h"
#include "core/consts.h"

typedef struct {
    cg_file_t type;
    cg_node_t *node;
    cg_sb_t *sb;
} cg_priv_t;

void a20_cgroupfs_spin_init(spinlock_t *lock)
{
    spin_init(lock);
}

uint64_t a20_cgroupfs_spin_lock_irqsave(spinlock_t *lock)
{
    return spin_lock_irqsave(lock);
}

void a20_cgroupfs_spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    spin_unlock_irqrestore(lock, flags);
}

static const char *cg_file_name_local(cg_file_t f, cg_ver_t ver)
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

static int cg_file_writable_local(cg_file_t f)
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

static int cg_generate_content_local(cg_file_t f, cg_sb_t *sb, cg_node_t *node, char *buf, size_t bufsz)
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
        snprintf(buf, bufsz, "%d\n", node->clone_children);
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
        if (lim == SIZE_MAX) snprintf(buf, bufsz, "9223372036854771712\n");
        else snprintf(buf, bufsz, "%lu\n", (unsigned long)(lim * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_MEMSW_LIMIT: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t lim = node->res.mem.swap_limit;
        spin_unlock_irqrestore(&node->lock, flags);
        if (lim == SIZE_MAX) snprintf(buf, bufsz, "9223372036854771712\n");
        else snprintf(buf, bufsz, "%lu\n", (unsigned long)(lim * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_KMEM_LIMIT:
        snprintf(buf, bufsz, "9223372036854771712\n");
        break;
    case CF_MEMORY_STAT: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        cg_mem_state_t *m = &node->res.mem;
        snprintf(buf, bufsz,
            "cache %lu\nrss %lu\nrss_huge 0\nmapped_file 0\nswap %lu\n"
            "pgpgin 0\npgpgout 0\ninactive_anon 0\nactive_anon %lu\n"
            "inactive_file 0\nactive_file 0\nunevictable 0\n"
            "hierarchical_memory_limit %lu\n"
            "hierarchical_memsw_limit %lu\n"
            "total_cache 0\ntotal_rss %lu\ntotal_swap %lu\n",
            (unsigned long)m->cache, (unsigned long)m->rss,
            (unsigned long)m->swap_usage,
            (unsigned long)m->rss,
            m->limit == SIZE_MAX ? 9223372036854771712UL : (unsigned long)(m->limit * PAGE_SIZE),
            m->swap_limit == SIZE_MAX ? 9223372036854771712UL : (unsigned long)(m->swap_limit * PAGE_SIZE),
            (unsigned long)m->rss, (unsigned long)m->swap_usage);
        spin_unlock_irqrestore(&node->lock, flags);
        break;
    }
    case CF_MEMORY_SWAPPINESS: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        unsigned sw = node->res.mem.swappiness;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%u\n", sw ? sw : 60);
        break;
    }
    case CF_MEMORY_USE_HIERARCHY: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        int hier = node->res.mem.hierarchy;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%d\n", hier);
        break;
    }
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
        if (lim == SIZE_MAX) snprintf(buf, bufsz, "max\n");
        else snprintf(buf, bufsz, "%lu\n", (unsigned long)(lim * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_SWAP_MAX: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t lim = node->res.mem.swap_limit;
        spin_unlock_irqrestore(&node->lock, flags);
        if (lim == SIZE_MAX) snprintf(buf, bufsz, "max\n");
        else snprintf(buf, bufsz, "%lu\n", (unsigned long)(lim * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_MIN: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t mv = node->res.mem.min_val;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%lu\n", (unsigned long)(mv * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_LOW: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        size_t lv = node->res.mem.low_val;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%lu\n", (unsigned long)(lv * PAGE_SIZE));
        break;
    }
    case CF_MEMORY_EVENTS: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        snprintf(buf, bufsz, "low 0\nhigh 0\nmax %lu\noom %lu\noom_kill %d\n",
                 (unsigned long)node->res.mem.failcnt,
                 (unsigned long)node->res.mem.failcnt,
                 node->res.mem.oom_kill_count);
        spin_unlock_irqrestore(&node->lock, flags);
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
        snprintf(buf + off, bufsz - off, "\n");
        break;
    }
    case CF_CPUSET_MEMS:
        snprintf(buf, bufsz, "0\n");
        break;
    case CF_CPUSET_MEMORY_MIGRATE: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        int mig = node->res.cpuset.memory_migrate;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%d\n", mig);
        break;
    }
    case CF_CPU_CFS_QUOTA: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        uint64_t quota = node->res.cpu.quota;
        spin_unlock_irqrestore(&node->lock, flags);
        if (quota == CG_CPU_QUOTA_MAX) snprintf(buf, bufsz, "-1\n");
        else snprintf(buf, bufsz, "%lld\n", (long long)(quota / 1000));
        break;
    }
    case CF_CPU_CFS_PERIOD: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        uint64_t period = node->res.cpu.period;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%lld\n", (long long)(period / 1000));
        break;
    }
    case CF_CPU_SHARES: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        uint64_t sh = node->res.cpu.shares;
        spin_unlock_irqrestore(&node->lock, flags);
        snprintf(buf, bufsz, "%llu\n", sh ? (unsigned long long)sh : 1024ULL);
        break;
    }
    case CF_CPU_STAT: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        cg_cpu_state_t *c = &node->res.cpu;
        snprintf(buf, bufsz,
                 "nr_periods %llu\nnr_throttled %u\nthrottled_time %llu\n",
                 (unsigned long long)(c->total_runtime / (c->period ? c->period : 1)),
                 c->nr_throttled,
                 (unsigned long long)c->throttled_time);
        spin_unlock_irqrestore(&node->lock, flags);
        break;
    }
    case CF_CPU_MAX: {
        uint64_t flags = spin_lock_irqsave(&node->lock);
        uint64_t quota = node->res.cpu.quota;
        uint64_t period = node->res.cpu.period;
        spin_unlock_irqrestore(&node->lock, flags);
        if (quota == CG_CPU_QUOTA_MAX) snprintf(buf, bufsz, "max %llu\n", (unsigned long long)(period / 1000));
        else snprintf(buf, bufsz, "%llu %llu\n", (unsigned long long)(quota / 1000), (unsigned long long)(period / 1000));
        break;
    }
    default:
        break;
    }
    return (int)strlen(buf);
}

static long long cg_strtoll_local(const char *s, char **end)
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

static long long cg_parse_ll_local(const char *buf, size_t count)
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
        if (kbuf[i] >= '0' && kbuf[i] <= '9') val = val * 10 + (kbuf[i] - '0');
        else break;
    }
    return neg ? -val : val;
}

static int cg_parse_int_local(const char *buf, size_t count)
{
    return (int)cg_parse_ll_local(buf, count);
}

static int cg_fread_local(vfile_t *vf, char *buf, size_t count)
{
    cg_priv_t *p = (cg_priv_t *)vf->priv;
    if (!p) return -EBADF;
    char content[1024];
    int len = cg_generate_content_local(p->type, p->sb, p->node, content, sizeof(content));
    if ((int)vf->offset >= len) return 0;
    size_t avail = (size_t)len - vf->offset;
    size_t n = count < avail ? count : avail;
    memcpy(buf, content + vf->offset, n);
    vf->offset += n;
    return (int)n;
}

static int cg_fwrite_local(vfile_t *vf, const char *buf, size_t count)
{
    cg_priv_t *p = (cg_priv_t *)vf->priv;
    if (!p) return -EBADF;
    if (!cg_file_writable_local(p->type)) return -EINVAL;

    if ((p->type == CF_TASKS || p->type == CF_CGROUP_PROCS) && p->node && buf) {
        int pid = cg_parse_int_local(buf, count);
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
        int val = cg_parse_int_local(buf, count);
        if (val == 1) {
            for (int i = 0; i < p->node->pid_count; i++) signal_send(p->node->pids[i], 9);
            p->node->pid_count = 0;
        }
    }

    if (!p->node) return (int)count;

    if (p->type == CF_MEMORY_LIMIT || p->type == CF_MEMORY_MAX) {
        long long val = cg_parse_ll_local(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        if (val <= 0 || (val > 0 && strncmp(buf, "max", 3) == 0)) p->node->res.mem.limit = SIZE_MAX;
        else p->node->res.mem.limit = (size_t)val / PAGE_SIZE;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }
    if (p->type == CF_MEMORY_MEMSW_LIMIT || p->type == CF_MEMORY_SWAP_MAX) {
        long long val = cg_parse_ll_local(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        if (val <= 0 || (val > 0 && strncmp(buf, "max", 3) == 0)) p->node->res.mem.swap_limit = SIZE_MAX;
        else p->node->res.mem.swap_limit = (size_t)val / PAGE_SIZE;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }
    if (p->type == CF_CPU_CFS_QUOTA) {
        long long val = cg_parse_ll_local(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        p->node->res.cpu.quota = val < 0 ? CG_CPU_QUOTA_MAX : (uint64_t)val * 1000;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }
    if (p->type == CF_CPU_CFS_PERIOD) {
        long long val = cg_parse_ll_local(buf, count);
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
            if (space) period_us = cg_strtoll_local(space + 1, NULL);
        } else {
            char *space = strchr(kbuf, ' ');
            if (space) {
                *space = '\0';
                quota_us = cg_strtoll_local(kbuf, NULL);
                period_us = cg_strtoll_local(space + 1, NULL);
            } else {
                quota_us = cg_strtoll_local(kbuf, NULL);
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
            long long cpu = cg_strtoll_local(tok, &tok);
            if (cpu >= 0 && cpu < (long long)CONFIG_NR_CPUS && cpu < 32) mask |= (1U << (unsigned)cpu);
            if (*tok == '-') {
                long long end = cg_strtoll_local(tok + 1, &tok);
                for (long long c = cpu; c <= end && c < (long long)CONFIG_NR_CPUS && c < 32; c++) if (c >= 0) mask |= (1U << (unsigned)c);
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
    if (p->type == CF_MEMORY_USE_HIERARCHY) {
        int val = cg_parse_int_local(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        p->node->res.mem.hierarchy = val ? 1 : 0;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }
    if (p->type == CF_MEMORY_SWAPPINESS) {
        long long val = cg_parse_ll_local(buf, count);
        if (val >= 0 && val <= 100) {
            uint64_t flags = spin_lock_irqsave(&p->node->lock);
            p->node->res.mem.swappiness = (unsigned)val;
            spin_unlock_irqrestore(&p->node->lock, flags);
        }
    }
    if (p->type == CF_MEMORY_MIN) {
        long long val = cg_parse_ll_local(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        p->node->res.mem.min_val = (val < 0 || strncmp(buf, "max", 3) == 0) ? 0 : (size_t)val / PAGE_SIZE;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }
    if (p->type == CF_MEMORY_LOW) {
        long long val = cg_parse_ll_local(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        p->node->res.mem.low_val = (val < 0 || strncmp(buf, "max", 3) == 0) ? 0 : (size_t)val / PAGE_SIZE;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }
    if (p->type == CF_CPU_SHARES) {
        long long val = cg_parse_ll_local(buf, count);
        if (val >= 2 && val <= 262144) {
            uint64_t flags = spin_lock_irqsave(&p->node->lock);
            p->node->res.cpu.shares = (uint64_t)val;
            spin_unlock_irqrestore(&p->node->lock, flags);
        }
    }
    if (p->type == CF_CPUSET_MEMS) {
        long long val = cg_parse_ll_local(buf, count);
        if (val >= 0) {
            uint64_t flags = spin_lock_irqsave(&p->node->lock);
            p->node->res.cpuset.mems_allowed = (uint32_t)(1U << (unsigned)val);
            spin_unlock_irqrestore(&p->node->lock, flags);
        }
    }
    if (p->type == CF_CPUSET_MEMORY_MIGRATE) {
        int val = cg_parse_int_local(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        p->node->res.cpuset.memory_migrate = val ? 1 : 0;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }
    if (p->type == CF_CLONE_CHILDREN) {
        int val = cg_parse_int_local(buf, count);
        uint64_t flags = spin_lock_irqsave(&p->node->lock);
        p->node->clone_children = val ? 1 : 0;
        spin_unlock_irqrestore(&p->node->lock, flags);
    }
    if (p->type == CF_CGROUP_SUBTREE_CONTROL) {
        char kbuf[256];
        size_t n = count < sizeof(kbuf) - 1 ? count : sizeof(kbuf) - 1;
        memcpy(kbuf, buf, n);
        kbuf[n] = '\0';
        uint32_t ctrl = p->sb ? p->sb->controllers : 0;
        char *tok = kbuf;
        while (*tok) {
            while (*tok == ' ' || *tok == '\t' || *tok == '\n') tok++;
            if (*tok == '\0') break;
            int enable = 1;
            if (*tok == '+') { enable = 1; tok++; }
            else if (*tok == '-') { enable = 0; tok++; }
            char *start = tok;
            while (*tok && *tok != ' ' && *tok != '\t' && *tok != '\n') tok++;
            char saved = *tok;
            *tok = '\0';
            if (strcmp(start, "memory") == 0) { if (enable) ctrl |= CTRL_MEMORY; else ctrl &= ~CTRL_MEMORY; }
            else if (strcmp(start, "cpu") == 0) { if (enable) ctrl |= CTRL_CPU; else ctrl &= ~CTRL_CPU; }
            else if (strcmp(start, "cpuset") == 0) { if (enable) ctrl |= CTRL_CPUSET; else ctrl &= ~CTRL_CPUSET; }
            *tok = saved;
        }
        if (p->sb) p->sb->controllers = ctrl;
    }
    return (int)count;
}

static long cg_flseek_local(vfile_t *vf, long offset, int whence)
{
    cg_priv_t *p = (cg_priv_t *)vf->priv;
    if (!p) return -EBADF;
    char content[1024];
    long clen = (long)cg_generate_content_local(p->type, p->sb, p->node, content, sizeof(content));
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

static int cg_freaddir_local(vfile_t *vf, void *dirp, size_t count)
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
        else if (idx - 2 < node->child_count) { name = node->children[idx - 2]->name; is_dir = 1; }
        else {
            int fidx = idx - 2 - node->child_count;
            if (fidx < node->file_count) {
                name = cg_file_name_local(node->files[fidx], sb->ver);
                is_dir = 0;
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

static int cg_fclose_local(vfile_t *vf)
{
    if (vf && vf->priv) { kfree(vf->priv); vf->priv = NULL; }
    return 0;
}

static vfile_ops_t g_cg_fops_local = {
    .read = cg_fread_local,
    .write = cg_fwrite_local,
    .lseek = cg_flseek_local,
    .readdir = cg_freaddir_local,
    .close = cg_fclose_local,
};

const char *a20_cgroupfs_file_name(cg_file_t f, cg_ver_t ver)
{
    return cg_file_name_local(f, ver);
}

int a20_cgroupfs_file_writable(cg_file_t f)
{
    return cg_file_writable_local(f);
}

int a20_cgroupfs_file_size(cg_file_t f, cg_sb_t *sb, cg_node_t *node)
{
    char tmp[1024];
    return cg_generate_content_local(f, sb, node, tmp, sizeof(tmp));
}

vfile_t *a20_cgroupfs_open_vnode(vnode_t *vn, int flags)
{
    vfile_t *vf = vfile_alloc();
    if (!vf) return NULL;
    vf->vnode = vn;
    vf->flags = flags;
    vnode_get(vn);
    vf->ops = &g_cg_fops_local;
    vfile_ref_init(vf, 1);

    cg_sb_t *sb = (!vn || !vn->mnt) ? NULL : (cg_sb_t *)vn->mnt->fs_data;
    cg_node_t *node = (cg_node_t *)vn->fs_data;
    cg_priv_t *priv = (cg_priv_t *)kmalloc(sizeof(cg_priv_t));
    if (!priv) { vnode_put(vn); vfile_free(vf); return NULL; }
    memset(priv, 0, sizeof(*priv));
    priv->sb = sb;
    priv->node = node;
    priv->type = vn->type == VFS_FT_REGULAR ? (cg_file_t)((vn->ino & 0xFFFF) - 1) : CF_FILE_MAX;
    vf->priv = priv;
    return vf;
}
