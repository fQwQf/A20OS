#include "mm/process_vm.h"
#include "proc/resource_ns.h"
#include "fs/procfs.h"
#include "fs/vfs/mount.h"
#include "net/netfilter.h"
#include "fs/procfs_internal.h"
#include "fs/procfs_node_state.h"
#include "mm/pt.h"
#include "hyp/hyp.h"
#include "fs/vfs/mntns.h"
#include "proc/pidns.h"
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
#include "abi/linux/pidfd.h"
#endif
#include "core/bootargs.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/block_cache.h"
#include "fs/ext4_journal.h"
#include "fs/page_cache.h"
#include "ipc/objstats.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/lifetime.h"
#include "proc/coredump.h"
/* Unconditional on purpose: without CONFIG_XLATOR the header supplies
 * stubs, so this does not pull the channel into a cut-down build. */
#include "proc/xlator.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "mm/oom.h"
#include "core/psi.h"
#include "mm/swap.h"
#include "core/timer.h"
#include "core/timekeeping.h"
#include "core/smp.h"
#include "core/perf.h"
#include "core/lock_counters.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/version.h"
#include "net/socket.h"
#include "net/socket/socket_internal.h"
#include "net/net_config.h"
#include "net/stack.h"
#include "drivers/core/riscv_iommu.h"
#include "drivers/bus/pci_msix.h"


#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
#include "drivers/core/driver_lifecycle_test.h"
#endif

extern size_t  frame_free_count(void);
extern int     vfs_mount_count(void);
extern struct mount *vfs_mount_at(int index);

__attribute__((weak)) int arch_cpuinfo_fields(char *buf, size_t size)
{
    return snprintf(buf, size, "model name\t: %s\n", ARCH_NAME);
}

/*
 * On-demand content generation for the synthetic /proc files.  Split out of
 * fs/procfs.c so the vnode/file machinery and the text renderers stay
 * independently readable.  All functions are pure renderers: they take a
 * snapshot under the appropriate locks and format text into caller-owned
 * buffers; they never mutate procfs state.
 */

static const uint8_t g_proc_config_gz[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x03, 0x7d, 0x8f, 0x31, 0x0e, 0x02, 0x21,
    0x10, 0x45, 0x7b, 0x4f, 0x41, 0xe2, 0x11, 0x4c,
    0xec, 0x2c, 0x60, 0x58, 0x56, 0x02, 0x2c, 0x84,
    0x19, 0xd6, 0x58, 0x4d, 0x65, 0x61, 0xa3, 0xc5,
    0x6e, 0xe3, 0xed, 0x4d, 0x58, 0x13, 0x2d, 0xc0,
    0x6e, 0xfe, 0x7f, 0xaf, 0xf8, 0x03, 0x71, 0x32,
    0x76, 0xe4, 0x10, 0xca, 0xe9, 0xb5, 0x83, 0x2d,
    0xa4, 0x1c, 0x81, 0x0d, 0x7e, 0x0b, 0x0a, 0xa9,
    0xc6, 0xbd, 0xf8, 0x14, 0x0a, 0x75, 0xb5, 0x06,
    0x44, 0x96, 0x00, 0x24, 0xee, 0x8b, 0x78, 0x3c,
    0x57, 0xb1, 0xdc, 0xd6, 0xbe, 0xc4, 0xf3, 0xa1,
    0xe9, 0x91, 0x44, 0x87, 0x24, 0x09, 0x9b, 0x14,
    0xc6, 0x1c, 0x4b, 0xea, 0x30, 0x83, 0xac, 0xe4,
    0xa4, 0x2f, 0x56, 0xd3, 0xb9, 0x69, 0x58, 0xb7,
    0x1d, 0x7f, 0x61, 0x5d, 0xd9, 0x36, 0xd2, 0x7c,
    0x64, 0x04, 0x4a, 0x4d, 0xda, 0x05, 0x21, 0xea,
    0xe2, 0x87, 0xf6, 0x66, 0x27, 0xbd, 0xc7, 0x6b,
    0xe8, 0x3c, 0x14, 0x43, 0x92, 0xc4, 0x2a, 0xbb,
    0x5f, 0xfc, 0x06, 0x92, 0x96, 0xf1, 0x8c, 0xa4,
    0x01, 0x00, 0x00,
};

static char procfs_task_state_char(const task_t *task) {
    if (!task) return 'X';
    switch (task->state) {
    case PROC_READY:
    case PROC_RUNNING:
        return 'R';
    case PROC_BLOCKED:
        return 'S';
    case PROC_STOPPED:
        return 'T';
    case PROC_ZOMBIE:
        return 'Z';
    case PROC_UNUSED:
    default:
        return 'X';
    }
}

static const char *procfs_task_state_text(const task_t *task) {
    switch (procfs_task_state_char(task)) {
    case 'R': return "R (running)";
    case 'S': return "S (sleeping)";
    case 'T': return "T (stopped)";
    case 'Z': return "Z (zombie)";
    default:  return "X (dead)";
    }
}

typedef struct a20_procfs_map_stats vma_smaps_stats_t;

static void vma_collect_smaps_stats(mm_struct_t *mm, uint64_t start,
                                    uint64_t end, uint64_t vm_flags,
                                    vma_smaps_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    if (!mm || !mm->pgdir) return;

    for (uint64_t va = start; va < end; ) {
        mm_leaf_info_t leaf;
        uint64_t lock_flags = spin_lock_irqsave(&mm->lock);
        int present = mm_query_leaf(mm->pgdir, va, &leaf);
        spin_unlock_irqrestore(&mm->lock, lock_flags);
        if (present) {
            size_t pages = leaf.size / PAGE_SIZE;
            int shared = (vm_flags & VM_SHARED) != 0;
            int dirty = leaf.dirty;
            int anon = (vm_flags & VM_ANON) != 0;

            stats->rss_pages += pages;
            if (shared) {
                if (dirty) stats->shared_dirty_pages += pages;
                else stats->shared_clean_pages += pages;
            } else {
                if (dirty) stats->private_dirty_pages += pages;
                else stats->private_clean_pages += pages;
            }
            if (anon)
                stats->anonymous_pages += pages;
            if (leaf.level > 0) {
                if (anon && shared)
                    stats->shmem_pmd_pages += pages;
                else if (anon)
                    stats->anon_huge_pages += pages;
                else
                    stats->file_pmd_pages += pages;
            }
            uint64_t next = leaf.base + leaf.size;
            va = next > va ? next : va + PAGE_SIZE;
        } else {
            va += PAGE_SIZE;
        }
    }
}

typedef struct a20_procfs_map_record procfs_vma_snapshot_t;
_Static_assert(sizeof(((procfs_vma_snapshot_t *)0)->name) == MAX_PATH_LEN,
               "procfs map snapshot name ABI");

static int snapshot_pid_maps(int pid, int smaps,
                             procfs_vma_snapshot_t **records_out,
                             size_t *count_out)
{
    task_t *task = proc_find_get(pid);
    if (!task)
        return -ESRCH;
    if (!proc_task_may_access(proc_current(), task)) {
        proc_put(task);
        return -EACCES;
    }
#ifndef ARCH_NO_PMD_LEAF
    int thp_disabled = task->policy.thp_disabled;
#endif
    mm_struct_t *mm = proc_task_get_mm(task);
    proc_put(task);
    if (!mm)
        return -ESRCH;

    procfs_vma_snapshot_t *records = NULL;
    size_t capacity = 0;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&mm->lock);
        size_t needed = 0;
        for (mm_seg_t *v = mm->mmap; v; v = v->next)
            needed++;
        spin_unlock_irqrestore(&mm->lock, flags);

        if (needed > capacity) {
            if (needed > SIZE_MAX / sizeof(*records)) {
                kfree(records);
                mm_destroy(mm);
                return -ENOMEM;
            }
            procfs_vma_snapshot_t *new_records =
                krealloc(records, needed * sizeof(*records));
            if (!new_records && needed) {
                kfree(records);
                mm_destroy(mm);
                return -ENOMEM;
            }
            records = new_records;
            capacity = needed;
        }

        flags = spin_lock_irqsave(&mm->lock);
        size_t count = 0;
        int retry = 0;
        for (mm_seg_t *v = mm->mmap; v; v = v->next) {
            if (count >= capacity) {
                retry = 1;
                break;
            }
            procfs_vma_snapshot_t *rec = &records[count++];
            memset(rec, 0, sizeof(*rec));
            rec->start = v->start;
            rec->end = v->end;
            rec->vm_flags = v->vm_flags;
            rec->file_offset = v->backing_offset;
#ifdef ARCH_NO_PMD_LEAF
            rec->thp_eligible = 0;
#else
            rec->thp_eligible = !thp_disabled &&
                !(v->vm_flags & VM_NOHUGEPAGE) &&
                (v->vm_flags & (VM_HUGEPAGE | VM_ANON)) &&
                (v->end - v->start) >= (2UL * 1024 * 1024);
#endif
            if (v->vm_flags & VM_STACK) {
                strncpy(rec->name, "[stack]", sizeof(rec->name) - 1);
            } else if (v->start >= mm->start_brk && v->start < mm->brk) {
                strncpy(rec->name, "[heap]", sizeof(rec->name) - 1);
            } else if ((v->vm_flags & VM_FILE) && v->file) {
                vfile_t *vf = v->file;
                if (vf->path[0])
                    strncpy(rec->name, vf->path,
                            sizeof(rec->name) - 1);
                if (vf->vnode)
                    rec->ino = (unsigned long)vf->vnode->ino;
            }
        }
        spin_unlock_irqrestore(&mm->lock, flags);
        if (!retry) {
            if (smaps) {
                for (size_t i = 0; i < count; i++) {
                    procfs_vma_snapshot_t *rec = &records[i];
                    vma_collect_smaps_stats(mm, rec->start, rec->end,
                                            rec->vm_flags, &rec->stats);
                }
            }
            mm_destroy(mm);
            *records_out = records;
            *count_out = count;
            return 0;
        }
    }
}

int generate_pid_maps_alloc(int pid, int smaps, char **buf_out,
                                   size_t *len_out)
{
    procfs_vma_snapshot_t *records = NULL;
    size_t count = 0;
    int ret = snapshot_pid_maps(pid, smaps, &records, &count);
    if (ret < 0)
        return ret;

    size_t per_record = MAX_PATH_LEN + (smaps ? 1024 : 128);
    if (count > (SIZE_MAX - 1) / per_record) {
        kfree(records);
        return -ENOMEM;
    }
    size_t bufsz = count * per_record + 1;
    char *buf = kmalloc(bufsz);
    if (!buf) {
        kfree(records);
        return -ENOMEM;
    }
    int rendered = a20_procfs_render_maps(records, count, smaps, buf, bufsz);
    kfree(records);
    if (rendered < 0) {
        kfree(buf);
        return rendered;
    }
    *buf_out = buf;
    *len_out = (size_t)rendered;
    return 0;
}

/*
 * /proc/net/{tcp,udp,unix} rows.
 *
 * net_socket_table_walk() hands each socket over while holding g_net_lock, so
 * a row is built in a fixed local buffer and copied out with a running offset
 * instead of formatted straight into the destination; once the destination is
 * full the walk keeps running but every visitor bails before formatting.
 */
/* Raw socket rows are collected while the socket's own locks are held.  Rust
 * formats them only after this bounded snapshot has been taken. */
typedef struct procfs_net_snapshot_ctx {
    struct a20_procfs_net_row *rows;
    size_t capacity;
    size_t count;
    int overflow;
} procfs_net_snapshot_ctx_t;

static unsigned procfs_net_rx_queued(const net_socket_t *s)
{
    size_t total = 0;
    for (const net_msg_t *m = s->rx_head; m; m = m->next)
        total += (s->type == SOCK_STREAM) ? (m->len - m->off) : m->len;
    return (unsigned)total;
}

static void procfs_net_count_row(net_socket_t *s, void *arg)
{
    (void)s;
    size_t *count = arg;
    (*count)++;
}

static void procfs_net_collect_row(net_socket_t *s, void *arg)
{
    procfs_net_snapshot_ctx_t *ctx = arg;
    if (ctx->count >= ctx->capacity) {
        ctx->overflow = 1;
        return;
    }
    struct a20_procfs_net_row *r = &ctx->rows[ctx->count++];
    memset(r, 0, sizeof(*r));
    r->reg_idx = s->reg_idx;
    r->domain = s->domain;
    r->type = s->type;
    r->tcp_flags = (s->listening ? 1 : 0) |
                   (s->tcp_connecting ? 2 : 0) |
                   (s->connected ? 4 : 0) |
                   (s->peer_closed ? 8 : 0);
    r->local_len = (int32_t)(s->local_len < A20_PROCFS_NET_ADDR_MAX
                                ? s->local_len : A20_PROCFS_NET_ADDR_MAX);
    r->peer_len = (int32_t)(s->peer_len < A20_PROCFS_NET_ADDR_MAX
                                ? s->peer_len : A20_PROCFS_NET_ADDR_MAX);
    if (r->local_len > 0)
        memcpy(r->local, s->local, (size_t)r->local_len);
    if (r->peer_len > 0)
        memcpy(r->peer, s->peer_addr, (size_t)r->peer_len);
    r->rx_queued = procfs_net_rx_queued(s);
    r->owner_uid = s->owner_uid;
    r->recv_timeout_ticks = s->recv_timeout_ticks;
}

static int gen_net_snapshot(int type, struct a20_procfs_net_row **out,
                            size_t *count_out)
{
    if (!out || !count_out)
        return -EINVAL;
    *out = NULL;
    *count_out = 0;
    net_table_kind_t table;
    int family;
    switch (type) {
    case PF_NET_TCP: table = NET_TABLE_TCP; family = AF_INET; break;
    case PF_NET_UDP: table = NET_TABLE_UDP; family = AF_INET; break;
    case PF_NET_TCP6: table = NET_TABLE_TCP; family = AF_INET6; break;
    case PF_NET_UDP6: table = NET_TABLE_UDP; family = AF_INET6; break;
    case PF_NET_UNIX: table = NET_TABLE_UNIX; family = AF_UNSPEC; break;
    default: return -EINVAL;
    }

    /* Creation/close may race the two walks.  Recount and retry when the
     * second pass grew; never write past the snapshot allocation. */
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        size_t capacity = 0;
        int walked = net_socket_table_walk(table, family, procfs_net_count_row,
                                           &capacity);
        if (walked < 0)
            return walked;
        if (!capacity)
            return 0;
        if (capacity > SIZE_MAX / sizeof(**out))
            return -ENOMEM;
        struct a20_procfs_net_row *rows = kmalloc(capacity * sizeof(*rows));
        if (!rows)
            return -ENOMEM;
        procfs_net_snapshot_ctx_t ctx = { rows, capacity, 0, 0 };
        walked = net_socket_table_walk(table, family, procfs_net_collect_row,
                                      &ctx);
        if (walked < 0) {
            kfree(rows);
            return walked;
        }
        if (!ctx.overflow) {
            *out = rows;
            *count_out = ctx.count;
            return 0;
        }
        kfree(rows);
    }
    return -EAGAIN;
}

static void gen_net_snapshot_free(struct a20_procfs_net_row *rows)
{
    kfree(rows);
}

/* Mechanical bridge for the Rust content-generator engine.  Every accessor is
 * one read; the retained C generator above stays as the shrinking fallback. */
static const char *gen_sysname(void) { return A20OS_SYSNAME; }
static const char *gen_release(void) { return A20OS_RELEASE; }
static const char *gen_arch_name(void) { return ARCH_NAME; }
static const char *gen_version_full(void) { return A20OS_VERSION_FULL; }
static uint64_t gen_ticks(void) { return timer_get_ticks(); }
static uint64_t gen_ticks_per_sec(void) { return TICKS_PER_SEC; }
static const char *gen_bootargs(void) { return bootargs_get() ? bootargs_get() : ""; }
static void gen_loadavg(uint64_t *a1, uint64_t *a5, uint64_t *a15,
                        unsigned *running, unsigned *total, int *max_pid)
{
    proc_loadavg_snapshot(a1, a5, a15, running, total, max_pid);
}
static uint32_t gen_anonprov_max(void) { return mm_pt_anon_prov_max(); }
static int gen_sched_base_slice(void) { return g_sched_base_slice_ms; }
static int gen_pid_max(void) { return proc_pid_max(); }
static int gen_current_pid(void)
{
    task_t *t = proc_current();
    return t ? t->pid : 0;
}
static int gen_pipe_max_size(void) { return g_procfs_pipe_max_size; }
static int gen_lease_break_time(void) { return g_procfs_lease_break_time; }
static int gen_max_files(void) { return MAX_FILES; }
static void gen_meminfo_raw(struct a20_meminfo_raw *m)
{
    slab_stats_t slab;
    bcache_stats_t bc;
    page_cache_stats_t pc;
    proc_vm_stats_t vm;
    pfa_huge_stats_t huge;
    slab_get_stats(&slab);
    bcache_get_stats(&bc);
    page_cache_get_stats(&pc);
    proc_get_vm_stats(&vm);
    pfa_get_huge_stats(&huge);
    m->total_frames = (uint64_t)pfa.total_frames;
    m->free_frames = (uint64_t)frame_free_count();
    m->slab_total_bytes = (uint64_t)slab.total_bytes;
    m->slab_reclaim_bytes = (uint64_t)slab.reclaimable_bytes;
    m->bcache_pool_bytes = (uint64_t)bc.block_pool_bytes;
    m->bcache_valid_pages = (uint64_t)bc.valid_pages;
    m->bcache_dirty_blocks = (uint64_t)bc.dirty_blocks;
    m->bcache_dirty_pages = (uint64_t)bc.dirty_pages;
    m->pcache_valid = (uint64_t)pc.valid;
    m->pcache_dirty = (uint64_t)pc.dirty;
    m->vm_anon_huge = (uint64_t)vm.anon_huge_pages;
    m->vm_shmem_huge = (uint64_t)vm.shmem_huge_pages;
    m->vm_file_huge = (uint64_t)vm.file_huge_pages;
    m->huge_total = (uint64_t)huge.total_huge_pages;
    m->huge_free = (uint64_t)huge.free_huge_pages;
#ifdef CONFIG_SWAP
    m->swap_total_pages = (uint64_t)total_swap_pages;
    m->swap_free_pages = (uint64_t)nr_swap_pages;
#else
    m->swap_total_pages = 0;
    m->swap_free_pages = 0;
#endif
    m->page_size = (uint64_t)PAGE_SIZE;
    m->pcache_page_size = (uint64_t)PCACHE_PAGE_SIZE;
    m->bcache_block_size = (uint64_t)BCACHE_BLOCK_SIZE;
}

/* Copy one target namespace while its table lock is held. Formatting must
 * not retain vfs_mount_at()'s unowned pointer across a concurrent removal. */
static int gen_mount_snapshot(int pid, struct a20_mount_ent **out,
                              size_t *count, char *root, size_t root_size)
{
    *out = NULL;
    *count = 0;
    task_t *t = pid > 0 ? proc_find_get(pid) : proc_get(proc_current());
    if (!t)
        return -ESRCH;
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    snprintf(root, root_size, "%s", t->fs.root_path[0] ? t->fs.root_path : "/");
    spin_unlock_irqrestore(&t->park_lock, flags);
    mnt_namespace_t *ns = mntns_task_get(t);
    proc_put(t);
    struct a20_mount_ent *rows = kmalloc(MNTNS_MAX_MOUNTS * sizeof(*rows));
    if (!rows) {
        mntns_put(ns);
        return -ENOMEM;
    }
    _Static_assert(sizeof(rows[0].path) >= MAX_PATH_LEN,
                   "procfs mount paths must not truncate VFS paths");
    size_t n = 0;
    flags = mntns_mount_table_lock();
    for (int i = 0; i < ns->nmounts; i++) {
        mount_t *m = ns->mounts[i];
        if (!m || !m->publish_ready || m->teardown_running ||
            m->detached || !m->attached || !m->path[0])
            continue;
        struct a20_mount_ent *e = &rows[n++];
        memset(e, 0, sizeof(*e));
        snprintf(e->dev, sizeof(e->dev), "%s", m->dev[0] ? m->dev : "none");
        snprintf(e->path, sizeof(e->path), "%s", m->path);
        snprintf(e->fstype, sizeof(e->fstype), "%s",
                 m->fstype[0] ? m->fstype : "unknown");
        snprintf(e->opts, sizeof(e->opts), "%s", m->opts[0] ? m->opts : "rw");
        e->mnt_id = (uint32_t)m->mnt_id;
        e->parent_id = (uint32_t)vfs_mount_parent_id(m);
    }
    mntns_mount_table_unlock(flags);
    mntns_put(ns);
    *out = rows;
    *count = n;
    return 0;
}

static void gen_mount_snapshot_free(struct a20_mount_ent *rows) { kfree(rows); }

static int gen_nr_cpus(void) { return CONFIG_NR_CPUS; }

static int gen_cpu_times(int cpu, uint64_t *u, uint64_t *s, uint64_t *idl)
{
    proc_get_cpu_times((unsigned)cpu, u, s, idl);
    return 0;
}
static void gen_realtime(uint64_t *out) { timekeeping_get_realtime(out); }
static void gen_monotonic(uint64_t *out) { timekeeping_get_monotonic(out); }
static uint64_t gen_tasks_created(void)
{
    proc_lifetime_stats_t lt;
    proc_lifetime_snapshot(&lt);
    return (uint64_t)lt.tasks_created;
}
static uint64_t gen_runq_load_sum(void) { return proc_runq_load_sum(); }
static int gen_swap_get(int index, struct a20_swap_ent *out)
{
#ifdef CONFIG_SWAP
    swap_listing_t sl;
    if (index < 0 || index >= MAX_SWAPFILES)
        return 0;
    if (swap_list_area(index, &sl) != 0)
        return 0;
    snprintf(out->name, sizeof(out->name), "%s", sl.name);
    out->pages = sl.pages;
    out->inuse_pages = (uint64_t)sl.inuse_pages;
    return 1;
#else
    (void)index;
    (void)out;
    return 0;
#endif
}
static uint32_t gen_online_cpu_mask(void) { return smp_online_cpu_mask(); }
static int gen_cpuinfo_fields(char *buf, size_t size)
{
    return arch_cpuinfo_fields(buf, size);
}

static void gen_bcache_raw(struct a20_bcache_raw *o)
{
    bcache_stats_t bc;
    bcache_get_stats(&bc);
    o->caches = (uint64_t)bc.caches;
    o->block_pool_bytes = (uint64_t)bc.block_pool_bytes;
    o->page_pool_bytes = (uint64_t)bc.page_pool_bytes;
    o->valid_blocks = (uint64_t)bc.valid_blocks;
    o->dirty_blocks = (uint64_t)bc.dirty_blocks;
    o->valid_pages = (uint64_t)bc.valid_pages;
    o->dirty_pages = (uint64_t)bc.dirty_pages;
}

static void gen_pagecache_raw(struct a20_pagecache_raw *o)
{
    page_cache_stats_t pc;
    page_cache_get_stats(&pc);
    o->capacity = (uint64_t)pc.capacity;
    o->allocated = (uint64_t)pc.allocated;
    o->bytes = (uint64_t)pc.bytes;
    o->valid = (uint64_t)pc.valid;
    o->dirty = (uint64_t)pc.dirty;
    o->pinned = (uint64_t)pc.pinned;
}

static void gen_oom_raw(struct a20_oom_raw *o)
{
    oom_stats_t os;
    oom_get_stats(&os);
    o->kills = (uint64_t)os.kills;
    o->last_kill_tick = (uint64_t)os.last_kill_tick;
    o->last_victim_pid = (int32_t)os.last_victim_pid;
    o->last_victim_score = (int32_t)os.last_victim_score;
    o->free_pages_at_kill = (uint64_t)os.free_pages_at_kill;
    o->free_pages_now = (uint64_t)os.free_pages_now;
    o->in_progress = (int32_t)os.in_progress;
    o->kswapd_passes = (uint64_t)os.kswapd_passes;
    o->kswapd_pages_freed = (uint64_t)os.kswapd_pages_freed;
    o->kswapd_last_pass_tick = (uint64_t)os.kswapd_last_pass_tick;
}

static void gen_objstats_raw(struct a20_objstats_raw *o)
{
    o->handles = (uint64_t)__atomic_load_n(&g_a20_objstats.handles, __ATOMIC_RELAXED);
    o->channel_eps = (uint64_t)__atomic_load_n(&g_a20_objstats.channel_eps, __ATOMIC_RELAXED);
    o->eventqs = (uint64_t)__atomic_load_n(&g_a20_objstats.eventqs, __ATOMIC_RELAXED);
    o->vmos = (uint64_t)__atomic_load_n(&g_a20_objstats.vmos, __ATOMIC_RELAXED);
    o->vmo_pages = (uint64_t)__atomic_load_n(&g_a20_objstats.vmo_pages, __ATOMIC_RELAXED);
    o->irq_bindings = (uint64_t)__atomic_load_n(&g_a20_objstats.irq_bindings, __ATOMIC_RELAXED);
    o->vfiles = (uint64_t)__atomic_load_n(&g_a20_objstats.vfiles, __ATOMIC_RELAXED);
    o->vmo_dirty_frames = (uint64_t)__atomic_load_n(&g_a20_objstats.vmo_dirty_frames, __ATOMIC_RELAXED);
}

static void gen_iommu_raw(struct a20_iommu_raw *o)
{
    riscv_iommu_stats_t st;
    riscv_iommu_get_stats(&st);
    o->enabled = (int32_t)st.enabled;
    o->domains_claimed = (uint64_t)st.domains_claimed;
    o->domains_released = (uint64_t)st.domains_released;
    o->maps = (uint64_t)st.maps;
    o->map_failures = (uint64_t)st.map_failures;
    o->unmaps = (uint64_t)st.unmaps;
    o->mapped_pages = (uint64_t)st.mapped_pages;
    o->fault_records = (uint64_t)st.fault_records;
    o->faults = (uint64_t)st.faults;
    o->blocked_events = (uint64_t)st.blocked_events;
    o->last_fault_devid = (uint32_t)st.last_fault_devid;
    o->last_fault_cause = (uint64_t)st.last_fault_cause;
    o->last_fault_iova = (uint64_t)st.last_fault_iova;
    o->last_fault_owner = (int32_t)st.last_fault_owner;
}

static void gen_sysvipc_raw(struct a20_sysvipc_raw *o)
{
    extern int sysv_msg_count(void);
    extern int sysv_sem_count(void);
    extern int sysv_shm_count(void);
    o->msg = (int32_t)sysv_msg_count();
    o->sem = (int32_t)sysv_sem_count();
    o->shm = (int32_t)sysv_shm_count();
}

static void gen_config_gz(const uint8_t **data, size_t *len)
{
    *data = (const uint8_t *)g_proc_config_gz;
    *len = sizeof(g_proc_config_gz);
}

static int gen_userns_overflow_id(int gid)
{
    return userns_overflow_id(gid);
}
static const char *gen_hostname(void)
{
    extern const char *linux_kernel_hostname(void);
    return linux_kernel_hostname();
}
static const char *gen_domainname(void)
{
    extern const char *linux_kernel_domainname(void);
    return linux_kernel_domainname();
}
static int gen_psi(int kind, char *buf, size_t size)
{
    switch (kind) {
    case 1:  psi_render_mem(buf, size); break;
    case 2:  psi_render_io(buf, size); break;
    default: psi_render_cpu(buf, size); break;
    }
    return (int)strlen(buf);
}
static int gen_pidmap(char *buf, size_t size)
{
    proc_format_pidmap(buf, size);
    return (int)strlen(buf);
}

static int gen_pid_raw(int pid, struct a20_pid_raw *o)
{
    task_t *t = proc_find_get(pid);
    if (!t)
        return 0;
    memset(o, 0, sizeof(*o));
    o->pid = t->pid;
    o->ppid = t->ppid;
    o->pgid = t->pgid;
    o->sid = t->sid;
    snprintf(o->name, sizeof(o->name), "%s", t->name);
    o->state_char = procfs_task_state_char(t);
    snprintf(o->state_text, sizeof(o->state_text), "%s", procfs_task_state_text(t));
    o->utime_ticks = (uint64_t)t->utime_ticks;
    o->stime_ticks = (uint64_t)t->stime_ticks;
    o->child_utime = (int64_t)t->child_utime;
    o->child_stime = (int64_t)t->child_stime;
    o->priority = t->priority;
    o->start_jiffies = (uint64_t)t->start_jiffies;
    o->vsize = t->mm ? (uint64_t)t->mm->total_vm * PAGE_SIZE : 0;
    o->rss_pages = (int64_t)mm_rss_get(t->mm);
    proc_task_sched_state_t sched;
    proc_task_sched_state_snapshot(t, &sched);
    o->processor = sched.task_cpu_id;
    o->uid = t->cred.uid;
    o->euid = t->cred.euid;
    o->suid = t->cred.suid;
    o->fsuid = t->cred.fsuid;
    o->gid = t->cred.gid;
    o->egid = t->cred.egid;
    o->sgid = t->cred.sgid;
    o->fsgid = t->cred.fsgid;
    o->cap_inh = (uint64_t)t->cred.cap_inheritable;
    o->cap_prm = (uint64_t)t->cred.cap_permitted;
    o->cap_eff = (uint64_t)t->cred.cap_effective;
    o->cap_bnd = (uint64_t)t->cred.cap_bounding;
    uint32_t ng = 0;
    for (int i = 0; i < t->cred.ngroups && i < MAX_GROUPS && ng < 64; i++)
        o->groups[ng++] = (int32_t)t->cred.groups[i];
    o->ngroups = ng;
    if (t->mm) {
        o->rss_kb = mm_rss_get(t->mm) * PAGE_SIZE / 1024;
        o->vmlck_kb = t->mm->locked_vm / 1024;
        uint64_t vd = 0;
        mm_seg_t *vma = t->mm->mmap;
        while (vma) {
            if ((vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_STACK))
                vd += (vma->end - vma->start) / 1024;
            vma = vma->next;
        }
        o->vmdata_kb = vd;
    }
    proc_put(t);
    return 1;
}

static int gen_pid_str(int pid, int which, char *out, size_t size)
{
    task_t *t = proc_find_get(pid);
    const char *s;
    if (which == 1)
        s = (t && t->exec_path[0]) ? t->exec_path : "/sbin/init";
    else if (which == 2)
        s = t ? t->fs.cwd : "/";
    else if (which == 3)
        s = (t && t->exec_path[0]) ? t->exec_path : "";
    else
        s = t ? t->name : "unknown";
    int n = snprintf(out, size, "%s", s);
    if (t)
        proc_put(t);
    return n < 0 ? 0 : n;
}

static int gen_pid_io(int pid, struct a20_pid_io_raw *o)
{
    memset(o, 0, sizeof(*o));
    task_t *t = proc_find_get(pid);
    if (!t)
        return 0;
    o->rchar = (uint64_t)__atomic_load_n(&t->io_rchar, __ATOMIC_RELAXED);
    o->wchar = (uint64_t)__atomic_load_n(&t->io_wchar, __ATOMIC_RELAXED);
    o->syscr = (uint64_t)__atomic_load_n(&t->io_syscr, __ATOMIC_RELAXED);
    o->syscw = (uint64_t)__atomic_load_n(&t->io_syscw, __ATOMIC_RELAXED);
    o->read_bytes = (uint64_t)__atomic_load_n(&t->io_read_bytes, __ATOMIC_RELAXED);
    o->write_bytes = (uint64_t)__atomic_load_n(&t->io_write_bytes, __ATOMIC_RELAXED);
    proc_put(t);
    return 1;
}

static int gen_pid_sessionid(int pid)
{
    task_t *t = proc_find_get(pid);
    int s = t ? t->sid : 0;
    if (t)
        proc_put(t);
    return s;
}

static uint64_t gen_pid_ns_ino(int pid, int kind)
{
    task_t *t = proc_find_get(pid);
    uint64_t ino;
    switch (kind) {
    case 0:
        ino = t ? pidns_task_ino(t) : PIDNS_INIT_INO;
        break;
    case 1: {
        uint64_t i = PIDNS_INIT_INO;
        if (t) {
            pid_namespace_t *ns = (pid_namespace_t *)__atomic_load_n(
                &t->pid_ns_for_children, __ATOMIC_ACQUIRE);
            i = ns ? ns->ino : PIDNS_INIT_INO;
        }
        ino = i;
        break;
    }
    case 2: ino = resource_ns_task_ino(t, PROCNS_UTS); break;
    case 3: ino = userns_task_ino(t); break;
    case 4: ino = resource_ns_task_ino(t, PROCNS_IPC); break;
    case 5: ino = mntns_task_ino(t); break;
    case 6: ino = resource_ns_task_ino(t, PROCNS_NET); break;
    default: ino = resource_ns_task_ino(t, PROCNS_CGROUP); break;
    }
    if (t)
        proc_put(t);
    return ino;
}

static int gen_format_helper(int which, char *buf, size_t size)
{
    switch (which) {
    case 1:
        coredump_get_pattern(buf, size);
        return (int)strlen(buf);
    case 2:
        return xlator_render(buf, size);
    case 3:
        return (int)proc_lifetime_format(buf, size);
    case 4:
        return (int)a20_perf_format(buf, size);
    case 5:
        return (int)lock_counters_format(buf, size);
    case 6:
        netfilter_format(buf, size);
        return (int)strlen(buf);
    case 7:
        return net_stack_format_memory(buf, size);
    case 8:
        return ext4_journal_crash_points_format(buf, size);
    case 9: {
        extern int drvmod_list(char *buf, size_t sz);
        int n = drvmod_list(buf, size);
        return n < 0 ? 0 : n;
    }
    case 10: {
        extern int drvmod_drivers(char *buf, size_t sz);
        int n = drvmod_drivers(buf, size);
        return n < 0 ? 0 : n;
    }
    case 11:
        if (current_board && current_board->format_interrupts) {
            current_board->format_interrupts(buf, size);
        } else {
            snprintf(buf, size,
                "           CPU0\n"
                "  0:         %lu   platform   2-edge   timer\n"
                "  1:         %lu   platform   1-edge   i8042\n"
                "RES:         %lu   Rescheduling interrupts\n"
                "CAL:         %lu   Function call interrupts\n",
                (unsigned long)0, (unsigned long)0, (unsigned long)0,
                (unsigned long)0);
        }
        return (int)strlen(buf);
    case 12:
        a20_net_config_format(buf, size);
        return (int)strlen(buf);
    case 13:
        net_stack_format_devices(buf, size);
        return (int)strlen(buf);
    case 14:
        net_stack_format_routes(buf, size);
        return (int)strlen(buf);
    default:
        net_format_status(buf, size);
        return (int)strlen(buf);
    }
}

static int gen_hyp_selftest(void)
{
    return hyp_selftest();
}

static int gen_msix_cpu_count(void)
{
    return (int)arch_irq_msix_cpu_count();
}

static uint32_t gen_msix_snapshot(struct a20_msix_raw *out)
{
    pci_msix_affinity_entry_t entries[PCI_MSIX_AFFINITY_VECTORS];
    unsigned shown = sizeof(entries) / sizeof(entries[0]);
    unsigned total = pci_msix_affinity_snapshot(entries, shown);
    if (shown > 64)
        shown = 64;
    out->total = (uint32_t)total;
    out->n = (uint32_t)shown;
    for (unsigned i = 0; i < shown; i++) {
        snprintf(out->ent[i].name, sizeof(out->ent[i].name), "%s",
                 entries[i].name ? entries[i].name : "");
        out->ent[i].index = (uint32_t)entries[i].index;
        out->ent[i].vector = entries[i].vector;
        out->ent[i].cpu = (int32_t)entries[i].cpu;
    }
    return (uint32_t)total;
}

static int gen_pid_oom(int pid, int *adj)
{
    task_t *t = proc_find_get(pid);
    if (!t) {
        *adj = 0;
        return 0;
    }
    *adj = t->policy.oom_score_adj;
    proc_put(t);
    return 1;
}

static int gen_pid_cgroup_path(int pid, char *out, size_t size)
{
    task_t *t = proc_find_get(pid);
    resource_ns_cgroup_path(t, out, size);
    if (t)
        proc_put(t);
    return (int)strlen(out);
}

static int gen_userns_map(int pid, int is_gid, struct a20_map_raw *out)
{
    task_t *t = (pid > 0) ? proc_find_get(pid) : proc_current();
    if (!t) {
        out->n = 0;
        return 0;
    }
    user_namespace_t *ns = userns_task_own(t);
    uint64_t f = spin_lock_irqsave(&ns->lock);
    const uid_gid_extent_t *map = is_gid ? ns->gid_map : ns->uid_map;
    int n = is_gid ? ns->gid_map_extents : ns->uid_map_extents;
    if (n < 0)
        n = 0;
    if (n > 16)
        n = 16;
    for (int i = 0; i < n; i++) {
        out->ent[i].lower = (uint32_t)map[i].lower;
        out->ent[i].parent_lower = (uint32_t)map[i].parent_lower;
        out->ent[i].count = (uint32_t)map[i].count;
    }
    out->n = (uint32_t)n;
    spin_unlock_irqrestore(&ns->lock, f);
    if (pid > 0)
        proc_put(t);
    return n;
}

static int gen_userns_setgroups_allowed(int pid, int *allowed)
{
    task_t *t = (pid > 0) ? proc_find_get(pid) : proc_current();
    if (!t) {
        *allowed = 0;
        return 0;
    }
    user_namespace_t *ns = userns_task_own(t);
    uint64_t f = spin_lock_irqsave(&ns->lock);
    *allowed = ns->setgroups_allowed;
    spin_unlock_irqrestore(&ns->lock, f);
    if (pid > 0)
        proc_put(t);
    return 1;
}

static int gen_pid_exists(int pid)
{
    task_t *t = proc_find_get(pid);
    if (t)
        proc_put(t);
    return t ? 1 : 0;
}

static int gen_pid_limits(int pid, struct a20_limits_raw *o)
{
    task_t *t = proc_find_get(pid);
    if (!t)
        return 0;
    o->nofile = (uint64_t)t->limits.nofile;
    o->memlock = (uint64_t)t->limits.memlock;
    proc_put(t);
    return 1;
}

static int gen_pid_wchan(int pid, char *out, size_t size)
{
    task_t *t = proc_find_get(pid);
    if (!t)
        return -ESRCH;
    const char *w = "0";
    if (t->state == PROC_BLOCKED)
        w = "do_wait";
    else if (t->state == PROC_STOPPED)
        w = "do_signal_stop";
    snprintf(out, size, "%s", w);
    proc_put(t);
    return (int)strlen(out);
}

static int gen_pid_auxv(int pid, unsigned char *buf, uint32_t cap)
{
    task_t *t = proc_find_get(pid);
    if (!t)
        return 0;
    mm_struct_t *mm = proc_task_get_mm(t);
    if (!mm) {
        proc_put(t);
        return 0;
    }
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    uint32_t n = mm->auxv_n;
    if (n > A20_AUXV_MAX_PAIRS)
        n = A20_AUXV_MAX_PAIRS;
    size_t pair_size = 2 * sizeof(unsigned long);
    size_t len = (size_t)n * pair_size;
    if (len > cap) {
        len = 0; /* never publish a silently truncated vector */
    } else {
        for (uint32_t i = 0; i < n; i++) {
            unsigned long pair[2] = {
                (unsigned long)mm->auxv[i][0],
                (unsigned long)mm->auxv[i][1],
            };
            memcpy(buf + (size_t)i * pair_size, pair, pair_size);
        }
    }
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_destroy(mm);
    proc_put(t);
    return (int)len;
}

static const struct a20_procfs_gen g_procfs_gen = {
    .sysname = gen_sysname,
    .release = gen_release,
    .arch_name = gen_arch_name,
    .version_full = gen_version_full,
    .ticks = gen_ticks,
    .ticks_per_sec = gen_ticks_per_sec,
    .bootargs = gen_bootargs,
    .loadavg = gen_loadavg,
    .anonprov_max = gen_anonprov_max,
    .sched_base_slice = gen_sched_base_slice,
    .pid_max = gen_pid_max,
    .current_pid = gen_current_pid,
    .pipe_max_size = gen_pipe_max_size,
    .lease_break_time = gen_lease_break_time,
    .max_files = gen_max_files,
    .meminfo_raw = gen_meminfo_raw,
    .mount_snapshot = gen_mount_snapshot,
    .mount_snapshot_free = gen_mount_snapshot_free,
    .nr_cpus = gen_nr_cpus,
    .cpu_times = gen_cpu_times,
    .realtime = gen_realtime,
    .monotonic = gen_monotonic,
    .tasks_created = gen_tasks_created,
    .runq_load_sum = gen_runq_load_sum,
    .swap_get = gen_swap_get,
    .online_cpu_mask = gen_online_cpu_mask,
    .cpuinfo_fields = gen_cpuinfo_fields,
    .bcache_raw = gen_bcache_raw,
    .pagecache_raw = gen_pagecache_raw,
    .oom_raw = gen_oom_raw,
    .objstats_raw = gen_objstats_raw,
    .iommu_raw = gen_iommu_raw,
    .sysvipc_raw = gen_sysvipc_raw,
    .config_gz = gen_config_gz,
    .userns_overflow_id = gen_userns_overflow_id,
    .hostname = gen_hostname,
    .domainname = gen_domainname,
    .psi_render = gen_psi,
    .pidmap = gen_pidmap,
    .pid_raw = gen_pid_raw,
    .pid_str = gen_pid_str,
    .pid_io = gen_pid_io,
    .pid_sessionid = gen_pid_sessionid,
    .pid_ns_ino = gen_pid_ns_ino,
    .format_helper = gen_format_helper,
    .hyp_selftest = gen_hyp_selftest,
    .pid_oom = gen_pid_oom,
    .pid_cgroup_path = gen_pid_cgroup_path,
    .userns_map = gen_userns_map,
    .userns_setgroups_allowed = gen_userns_setgroups_allowed,
    .pid_exists = gen_pid_exists,
    .pid_limits = gen_pid_limits,
    .pid_wchan = gen_pid_wchan,
    .pid_auxv = gen_pid_auxv,
    .msix_cpu_count = gen_msix_cpu_count,
    .msix_snapshot = gen_msix_snapshot,
    .net_snapshot = gen_net_snapshot,
    .net_snapshot_free = gen_net_snapshot_free,
};
_Static_assert(sizeof(g_procfs_gen) / sizeof(void *) == 58,
               "procfs content mirror: bridge table field count");

int generate_content(pf_type_t type, int pid, char *buf, size_t bufsz)
{
    if (type == PF_PID_ENVIRON) {
        task_t *task = proc_find_get(pid);
        if (!task) return -ESRCH;
        if (!proc_task_may_access(proc_current(), task)) {
            proc_put(task); return -EACCES;
        }
        mm_struct_t *mm = proc_task_get_mm(task);
        proc_put(task);
        if (!mm) return 0;
        size_t length = mm->env_end >= mm->env_start ? mm->env_end - mm->env_start : 0;
        if (length >= bufsz) length = bufsz ? bufsz - 1 : 0;
        long result = length ? process_vm_read_mm_kernel(mm,
            (const void *)(uintptr_t)mm->env_start, buf, length) : 0;
        mm_destroy(mm);
        return (int)result;
    }
    return a20_procfs_generate(&g_procfs_gen, (int)type, pid, buf, bufsz);
}

int generate_pid_fdinfo(int pid, int fd, char *buf, size_t bufsz)
{
    task_t *task = proc_find_get(pid);
    if (!task)
        return -ESRCH;
    if (!proc_task_may_access(proc_current(), task)) {
        proc_put(task);
        return -EACCES;
    }
    int cloexec = 0;
    vfile_t *target = fdtable_get_file_ref(task, fd, &cloexec);
    proc_put(task);
    if (!target)
        return -ENOENT;

    mutex_lock(&target->offset_lock);
    size_t pos = target->offset;
    mutex_unlock(&target->offset_lock);
    unsigned int open_flags = (unsigned int)target->flags;
    if (cloexec)
        open_flags |= O_CLOEXEC;
    unsigned long ino = target->vnode ?
        (unsigned long)target->vnode->ino : 0;
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    int pid_target = linux_pidfd_vfile_pid(target);
#else
    int pid_target = -1;
#endif
    int len = a20_procfs_render_fdinfo(pos, open_flags, ino, pid_target, buf,
                                       bufsz);
    vfs_put_file(target);
    return len < 0 ? 0 : len;
}
