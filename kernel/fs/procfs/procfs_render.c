#include "fs/procfs.h"
#include "fs/procfs_internal.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/block_cache.h"
#include "fs/page_cache.h"
#include "ipc/objstats.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/lifetime.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "mm/oom.h"
#include "mm/swap.h"
#include "core/timer.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/version.h"
#include "net/socket.h"
#include "net/net_config.h"

#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
#include "drivers/core/driver_lifecycle_test.h"
#endif

extern size_t  frame_free_count(void);
extern int     vfs_mount_count(void);
extern struct mount *vfs_mount_at(int index);

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

static void appendf(char *buf, size_t bufsz, size_t *off, const char *fmt, ...) {
    if (*off >= bufsz) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *off, bufsz - *off, fmt, args);
    va_end(args);
    if (n < 0) return;
    size_t wrote = (size_t)n;
    if (wrote >= bufsz - *off)
        *off = bufsz - 1;
    else
        *off += wrote;
}

static void append_vma_flags(char *buf, size_t bufsz, size_t *off,
                             uint64_t vm_flags) {
    appendf(buf, bufsz, off, "VmFlags:");
    if (vm_flags & VM_READ) appendf(buf, bufsz, off, " rd");
    if (vm_flags & VM_WRITE) appendf(buf, bufsz, off, " wr");
    if (vm_flags & VM_EXEC) appendf(buf, bufsz, off, " ex");
    appendf(buf, bufsz, off, " mr mw me ac");
    if (vm_flags & VM_STACK) appendf(buf, bufsz, off, " gd");
    if (vm_flags & VM_HUGEPAGE) appendf(buf, bufsz, off, " hg");
    if (vm_flags & VM_NOHUGEPAGE) appendf(buf, bufsz, off, " nh");
    appendf(buf, bufsz, off, "\n");
}

typedef struct vma_smaps_stats {
    size_t rss_pages;
    size_t shared_clean_pages;
    size_t shared_dirty_pages;
    size_t private_clean_pages;
    size_t private_dirty_pages;
    size_t anonymous_pages;
    size_t anon_huge_pages;
    size_t shmem_pmd_pages;
    size_t file_pmd_pages;
} vma_smaps_stats_t;

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

typedef struct procfs_vma_snapshot {
    uint64_t start;
    uint64_t end;
    uint64_t vm_flags;
    uint64_t file_offset;
    unsigned long ino;
    int thp_eligible;
    char name[MAX_PATH_LEN];
    vma_smaps_stats_t stats;
} procfs_vma_snapshot_t;

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
    int thp_disabled = task->policy.thp_disabled;
    mm_struct_t *mm = proc_task_get_mm(task);
    proc_put(task);
    if (!mm)
        return -ESRCH;

    procfs_vma_snapshot_t *records = NULL;
    size_t capacity = 0;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&mm->lock);
        size_t needed = 0;
        for (vm_area_t *v = mm->mmap; v; v = v->next)
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
        for (vm_area_t *v = mm->mmap; v; v = v->next) {
            if (count >= capacity) {
                retry = 1;
                break;
            }
            procfs_vma_snapshot_t *rec = &records[count++];
            memset(rec, 0, sizeof(*rec));
            rec->start = v->start;
            rec->end = v->end;
            rec->vm_flags = v->vm_flags;
            rec->file_offset = v->file_offset;
            rec->thp_eligible = !thp_disabled &&
                !(v->vm_flags & VM_NOHUGEPAGE) &&
                (v->vm_flags & (VM_HUGEPAGE | VM_ANON)) &&
                (v->end - v->start) >= (2UL * 1024 * 1024);
            if (v->vm_flags & VM_STACK) {
                strncpy(rec->name, "[stack]", sizeof(rec->name) - 1);
            } else if (v->start >= mm->start_brk && v->start < mm->brk) {
                strncpy(rec->name, "[heap]", sizeof(rec->name) - 1);
            } else if (v->file_fd >= 0) {
                vfile_t *vf = vfs_get_file_ref(v->file_fd);
                if (vf) {
                    if (vf->path[0])
                        strncpy(rec->name, vf->path,
                                sizeof(rec->name) - 1);
                    if (vf->vnode)
                        rec->ino = (unsigned long)vf->vnode->ino;
                    vfs_put_file_ref(v->file_fd, vf);
                }
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
    buf[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        procfs_vma_snapshot_t *rec = &records[i];
        char r = (rec->vm_flags & VM_READ) ? 'r' : '-';
        char w = (rec->vm_flags & VM_WRITE) ? 'w' : '-';
        char x = (rec->vm_flags & VM_EXEC) ? 'x' : '-';
        char s = (rec->vm_flags & VM_SHARED) ? 's' : 'p';
        size_t kb = (size_t)(rec->end - rec->start) / 1024;
        size_t rss_kb = rec->stats.rss_pages * PAGE_SIZE / 1024;

        if (rec->name[0]) {
            appendf(buf, bufsz, &off,
                    "%08lx-%08lx %c%c%c%c %08lx 00:00 %lu %s\n",
                    (unsigned long)rec->start, (unsigned long)rec->end,
                    r, w, x, s, (unsigned long)rec->file_offset,
                    rec->ino, rec->name);
        } else {
            appendf(buf, bufsz, &off,
                    "%08lx-%08lx %c%c%c%c %08lx 00:00 %lu\n",
                    (unsigned long)rec->start, (unsigned long)rec->end,
                    r, w, x, s, (unsigned long)rec->file_offset,
                    rec->ino);
        }
        if (!smaps)
            continue;
        vma_smaps_stats_t *st = &rec->stats;
        appendf(buf, bufsz, &off,
                 "Size:           %8lu kB\n"
                "KernelPageSize: %8lu kB\n"
                "MMUPageSize:    %8lu kB\n"
                "Rss:            %8lu kB\n"
                "Pss:            %8lu kB\n"
                "Shared_Clean:   %8lu kB\n"
                "Shared_Dirty:   %8lu kB\n"
                "Private_Clean:  %8lu kB\n"
                "Private_Dirty:  %8lu kB\n"
                "Referenced:     %8lu kB\n"
                "Anonymous:      %8lu kB\n"
                "AnonHugePages:  %8lu kB\n"
                "ShmemPmdMapped: %8lu kB\n"
                "FilePmdMapped:  %8lu kB\n"
                "THPeligible:    %8d\n",
                (unsigned long)kb,
                (unsigned long)(PAGE_SIZE / 1024),
                (unsigned long)(PAGE_SIZE / 1024),
                (unsigned long)rss_kb,
                (unsigned long)rss_kb,
                (unsigned long)(st->shared_clean_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->shared_dirty_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->private_clean_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->private_dirty_pages * PAGE_SIZE / 1024),
                (unsigned long)rss_kb,
                (unsigned long)(st->anonymous_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->anon_huge_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->shmem_pmd_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->file_pmd_pages * PAGE_SIZE / 1024),
                rec->thp_eligible);
        append_vma_flags(buf, bufsz, &off, rec->vm_flags);
    }
    kfree(records);
    *buf_out = buf;
    *len_out = off;
    return 0;
}

// 生成 procfs 文件的内容
int generate_content(pf_type_t type, int pid, char *buf, size_t bufsz) {
    buf[0] = '\0';
    switch (type) {
    case PF_MEMINFO: {
        size_t free_frames = frame_free_count();
        size_t total_kb = pfa.total_frames * PAGE_SIZE / 1024;
        size_t free_kb = free_frames * PAGE_SIZE / 1024;
        slab_stats_t slab;
        bcache_stats_t bc;
        proc_vm_stats_t vmstats;
        pfa_huge_stats_t huge;
        slab_get_stats(&slab);
        bcache_get_stats(&bc);
        proc_get_vm_stats(&vmstats);
        pfa_get_huge_stats(&huge);
        size_t buffers_kb = bc.block_pool_bytes / 1024;
        size_t cached_kb = bc.valid_pages * PCACHE_PAGE_SIZE / 1024;
        size_t dirty_kb = (bc.dirty_blocks * BCACHE_BLOCK_SIZE +
                           bc.dirty_pages * PCACHE_PAGE_SIZE) / 1024;
        size_t slab_kb = slab.total_bytes / 1024;
        size_t sreclaim_kb = slab.reclaimable_bytes / 1024;
        size_t sunreclaim_kb = slab_kb > sreclaim_kb ? slab_kb - sreclaim_kb : 0;
        size_t available_kb = free_kb + cached_kb + sreclaim_kb;
#ifdef CONFIG_SWAP
        size_t swap_total_kb = total_swap_pages * PAGE_SIZE / 1024;
        size_t swap_free_kb = nr_swap_pages * PAGE_SIZE / 1024;
#else
        size_t swap_total_kb = 0;
        size_t swap_free_kb = 0;
#endif
        if (available_kb > total_kb)
            available_kb = total_kb;
        snprintf(buf, bufsz,
            "MemTotal:       %lu kB\n"
            "MemFree:        %lu kB\n"
            "MemAvailable:   %lu kB\n"
            "Buffers:        %lu kB\n"
            "Cached:         %lu kB\n"
            "SwapTotal:      %lu kB\n"
            "SwapFree:       %lu kB\n"
            "Shmem:          0 kB\n"
            "Dirty:          %lu kB\n"
            "Slab:           %lu kB\n"
            "SReclaimable:   %lu kB\n"
            "SUnreclaim:     %lu kB\n"
            "AnonHugePages:  %lu kB\n"
            "ShmemHugePages: %lu kB\n"
            "FileHugePages:  %lu kB\n"
            "HugePages_Total: %lu\n"
            "HugePages_Free:  %lu\n"
            "Hugepagesize:   2048 kB\n",
            (unsigned long)total_kb,
            (unsigned long)free_kb,
            (unsigned long)available_kb,
            (unsigned long)buffers_kb,
            (unsigned long)cached_kb,
            (unsigned long)swap_total_kb,
            (unsigned long)swap_free_kb,
            (unsigned long)dirty_kb,
            (unsigned long)slab_kb,
            (unsigned long)sreclaim_kb,
            (unsigned long)sunreclaim_kb,
            (unsigned long)(vmstats.anon_huge_pages * PAGE_SIZE / 1024),
            (unsigned long)(vmstats.shmem_huge_pages * PAGE_SIZE / 1024),
            (unsigned long)(vmstats.file_huge_pages * PAGE_SIZE / 1024),
            (unsigned long)huge.total_huge_pages,
            (unsigned long)huge.free_huge_pages);
        break;
    }
    case PF_SWAPS: {
        size_t off = 0;
        appendf(buf, bufsz, &off,
                "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n");
#ifdef CONFIG_SWAP
        for (int type = 0; type < MAX_SWAPFILES; type++) {
            swap_info_struct *si = &swap_info[type];
            if (!si->active)
                continue;
            appendf(buf, bufsz, &off, "%-40s\tpartition\t%llu\t%lu\t-2\n",
                    si->name ? si->name : "",
                    (unsigned long long)si->pages,
                    (unsigned long)si->inuse_pages);
        }
#endif
        break;
    }
    case PF_VERSION:
        snprintf(buf, bufsz, "Linux version %s (%s) (%s)\n",
                 LINUX_ABI_RELEASE, ARCH_NAME, LINUX_ABI_VERSION);
        break;
    case PF_UPTIME: {  // 生成运行时间
        uint64_t ticks = timer_get_ticks();
        uint64_t sec = ticks / TICKS_PER_SEC;
        uint64_t frac = (ticks % TICKS_PER_SEC) * 100 / TICKS_PER_SEC;
        snprintf(buf, bufsz, "%lu.%02lu\n", (unsigned long)sec, (unsigned long)frac);
        break;
    }
    case PF_CMDLINE:
        snprintf(buf, bufsz, "console=ttyS0\n");
        break;
    case PF_CPUINFO:  // 生成 CPU 信息
        snprintf(buf, bufsz,
            "processor\t: 0\n"
            "hart\t\t: 0\n"
            "isa\t\t: rv64gc\n"
            "mmu\t\t: sv39\n\n");
        break;
    case PF_MOUNTS: {
        buf[0] = '\0';
        int pos = 0;
        for (int i = 0; i < vfs_mount_count(); i++) {
            struct mount *m = vfs_mount_at(i);
            if (!m || !m->path[0]) continue;
            const char *fstype = m->fstype[0] ? m->fstype : "unknown";
            const char *dev = m->dev[0] ? m->dev : "none";
            const char *opts = m->opts[0] ? m->opts : "rw";
            int n = snprintf(buf + pos, bufsz - pos,
                "%s %s %s %s 0 0\n", dev, m->path, fstype, opts);
            if (n < 0 || (size_t)n >= bufsz - pos) break;
            pos += n;
        }
        break;
    }
    case PF_LOADAVG:  // 生成负载平均值
        snprintf(buf, bufsz, "0.00 0.00 0.00 1/64 1\n");
        break;
    case PF_NET:
        net_format_status(buf, bufsz);
        break;
    case PF_NET_STATUS:
        net_format_status(buf, bufsz);
        break;
    case PF_NET_CONFIG:
        a20_net_config_format(buf, bufsz);
        break;
    case PF_CONFIG_GZ: {
        size_t n = sizeof(g_proc_config_gz) < bufsz ? sizeof(g_proc_config_gz) : bufsz;
        memcpy(buf, g_proc_config_gz, n);
        return (int)n;
    }
    case PF_A20_SCHED_BASE_SLICE:
        snprintf(buf, bufsz, "%d\n", g_sched_base_slice_ms);
        return (int)strlen(buf);
    case PF_A20_BCACHE: {
        bcache_stats_t bc;
        bcache_get_stats(&bc);
        snprintf(buf, bufsz,
            "caches: %lu\n"
            "block_pool_bytes: %lu\n"
            "page_pool_bytes: %lu\n"
            "valid_blocks: %lu\n"
            "dirty_blocks: %lu\n"
            "valid_pages: %lu\n"
            "dirty_pages: %lu\n",
            (unsigned long)bc.caches,
            (unsigned long)bc.block_pool_bytes,
            (unsigned long)bc.page_pool_bytes,
            (unsigned long)bc.valid_blocks,
            (unsigned long)bc.dirty_blocks,
            (unsigned long)bc.valid_pages,
            (unsigned long)bc.dirty_pages);
        break;
    }
    case PF_A20_PAGE_CACHE: {
        page_cache_stats_t pc;
        page_cache_get_stats(&pc);
        snprintf(buf, bufsz,
            "capacity: %lu\n"
            "bytes: %lu\n"
            "valid: %lu\n"
            "dirty: %lu\n"
            "pinned: %lu\n",
            (unsigned long)pc.capacity,
            (unsigned long)pc.bytes,
            (unsigned long)pc.valid,
            (unsigned long)pc.dirty,
            (unsigned long)pc.pinned);
        break;
    }
    case PF_A20_OOM: {
        oom_stats_t os;
        oom_get_stats(&os);
        snprintf(buf, bufsz,
            "kills: %lu\n"
            "last_kill_tick: %lu\n"
            "last_victim_pid: %d\n"
            "last_victim_score: %d\n"
            "free_pages_at_kill: %lu\n"
            "free_pages_now: %lu\n"
            "in_progress: %d\n",
            os.kills,
            os.last_kill_tick,
            os.last_victim_pid,
            os.last_victim_score,
            os.free_pages_at_kill,
            os.free_pages_now,
            os.in_progress);
        break;
    }
    case PF_A20_TASK_LIFETIME:
        return (int)proc_lifetime_format(buf, bufsz);
    case PF_A20_OBJECTS:
        snprintf(buf, bufsz,
            "handles: %lu\n"
            "channel_eps: %lu\n"
            "eventqs: %lu\n"
            "vmos: %lu\n"
            "vmo_pages: %lu\n"
            "irq_bindings: %lu\n",
            (unsigned long)__atomic_load_n(&g_a20_objstats.handles, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.channel_eps, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.eventqs, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.vmos, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.vmo_pages, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.irq_bindings, __ATOMIC_RELAXED));
        break;
    case PF_A20_DRIVER_LIFECYCLE:
        buf[0] = '\0';
        return 0;
    case PF_PID_STAT: {  // 生成进程 stat 信息
        task_t *t = proc_find_get(pid);
        if (!t) { snprintf(buf, bufsz, "%d (unknown) S 0 0\n", pid); break; }
        snprintf(buf, bufsz,
            "%d (%s) %c %d %d %d 0 0 0 0 0 0 0 0 %lu 0\n",
            t->pid, t->name, procfs_task_state_char(t),
            t->ppid, t->pgid, t->sid,
            (unsigned long)t->total_time);
        proc_put(t);
        break;
    }
    case PF_PID_STATUS: {  // 生成进程 status 信息
        task_t *t = proc_find_get(pid);
        if (!t) { snprintf(buf, bufsz, "Name:\tunknown\nPid:\t%d\n", pid); break; }
        const char *state = procfs_task_state_text(t);
        char groups[160];
        size_t glen = 0;
        groups[0] = '\0';
        for (int i = 0; i < t->cred.ngroups && i < MAX_GROUPS; i++) {
            int n = snprintf(groups + glen, sizeof(groups) - glen, "%s%d",
                             i ? " " : "", t->cred.groups[i]);
            if (n < 0 || (size_t)n >= sizeof(groups) - glen)
                break;
            glen += (size_t)n;
        }

        size_t rss_kb = 0;
        size_t vmlck_kb = 0;
        size_t vmdata_kb = 0;
        if (t->mm) {
            rss_kb = t->mm->rss * PAGE_SIZE / 1024;
            vmlck_kb = t->mm->locked_vm / 1024;
            vm_area_t *vma = t->mm->mmap;
            while (vma) {
                if ((vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_STACK)) {
                    vmdata_kb += (vma->end - vma->start) / 1024;
                }
                vma = vma->next;
            }
        }

        snprintf(buf, bufsz,
            "Name:\t%s\n"
            "Pid:\t%d\n"
            "PPid:\t%d\n"
            "PGid:\t%d\n"
            "Sid:\t%d\n"
            "Tgid:\t%d\n"
            "Ngid:\t0\n"
            "State:\t%s\n"
            "Uid:\t%d\t%d\t%d\t%d\n"
            "Gid:\t%d\t%d\t%d\t%d\n"
            "Groups:\t%s\n"
            "CapInh:\t%016lx\n"
            "CapPrm:\t%016lx\n"
            "CapEff:\t%016lx\n"
            "CapBnd:\t%016lx\n"
            "Threads:\t1\n"
            "Rss:\t%lu kB\n"
            "VmLck:\t%lu kB\n"
            "VmData:\t%lu kB\n",
            t->name, t->pid, t->ppid, t->pgid, t->sid, t->pid, state,
            t->cred.uid, t->cred.euid, t->cred.suid, t->cred.fsuid,
            t->cred.gid, t->cred.egid, t->cred.sgid, t->cred.fsgid,
            groups,
            (unsigned long)t->cred.cap_inheritable,
            (unsigned long)t->cred.cap_permitted,
            (unsigned long)t->cred.cap_effective,
            (unsigned long)t->cred.cap_bounding,
            (unsigned long)rss_kb,
            (unsigned long)vmlck_kb,
            (unsigned long)vmdata_kb);
        proc_put(t);
        break;
    }
    case PF_PID_STATM: {
        task_t *t = proc_find_get(pid);
        size_t total = t && t->mm ? t->mm->total_vm : 0;
        size_t rss = t && t->mm ? t->mm->rss : 0;
        snprintf(buf, bufsz, "%lu %lu 0 0 0 0 0\n",
                 (unsigned long)total, (unsigned long)rss);
        proc_put(t);
        break;
    }
    case PF_PID_MAPS: {
        buf[0] = '\0';
        break;
    }
    case PF_PID_SMAPS: {
        buf[0] = '\0';
        break;
    }
    case PF_PID_OOM_SCORE_ADJ: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%d\n", t ? t->policy.oom_score_adj : 0);
        proc_put(t);
        break;
    }
    case PF_PID_OOM_SCORE: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%d\n", t ? (t->policy.oom_score_adj >= 0 ? t->policy.oom_score_adj : 0) : 0);
        proc_put(t);
        break;
    }
    case PF_PID_CGROUP:
        snprintf(buf, bufsz,
            "0::/init.scope\n");
        break;
    case PF_PID_CMDLINE: {
        task_t *t = proc_find_get(pid);
        if (!t || !t->exec_path[0]) {
            buf[0] = '\0';
            proc_put(t);
            return 1;
        }
        size_t len = strlen(t->exec_path);
        if (len + 1 > bufsz) len = bufsz - 1;
        memcpy(buf, t->exec_path, len);
        buf[len] = '\0';
        proc_put(t);
        return (int)(len + 1);
    }
    case PF_PID_COMM: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%s\n", t ? t->name : "unknown");
        proc_put(t);
        break;
    }
    case PF_PID_EXE: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%s\n", t && t->exec_path[0] ? t->exec_path : "/sbin/init");
        proc_put(t);
        break;
    }
    case PF_PID_CWD: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%s\n", t ? t->fs.cwd : "/");
        proc_put(t);
        break;
    }
    case PF_PID_ENVIRON:
        buf[0] = '\0';
        return 0;
    case PF_PID_IO: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz,
            "rchar: %lu\n"
            "wchar: %lu\n"
            "syscr: 0\nsyscw: 0\n"
            "read_bytes: 0\nwrite_bytes: 0\n"
            "cancelled_write_bytes: 0\n",
            (unsigned long)(t ? t->total_time : 0),
            (unsigned long)(t ? t->child_stime : 0));
        proc_put(t);
        break;
    }
    case PF_PID_LOGINUID:
        snprintf(buf, bufsz, "4294967295\n");
        break;
    case PF_PID_SESSIONID: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%d\n", t ? t->sid : 0);
        proc_put(t);
        break;
    }
    case PF_PID_NS_PID:
        snprintf(buf, bufsz, "pid:[0]\n");
        break;
    case PF_PID_NS_UTS:
        snprintf(buf, bufsz, "uts:[0]\n");
        break;
    case PF_PID_NS_USER:
        snprintf(buf, bufsz, "user:[0]\n");
        break;
    case PF_PID_NS_IPC:
        snprintf(buf, bufsz, "ipc:[0]\n");
        break;
    case PF_PID_NS_MNT:
        snprintf(buf, bufsz, "mnt:[0]\n");
        break;
    case PF_PID_NS_NET:
        snprintf(buf, bufsz, "net:[0]\n");
        break;
    case PF_PID_NS_CGROUP:
        snprintf(buf, bufsz, "cgroup:[0]\n");
        break;
    case PF_PID_MOUNTINFO: {
        buf[0] = '\0';
        int pos = 0;
        for (int i = 0; i < vfs_mount_count(); i++) {
            struct mount *m = vfs_mount_at(i);
            if (!m || !m->path[0]) continue;
            const char *fstype = m->fstype[0] ? m->fstype : "unknown";
            const char *dev = m->dev[0] ? m->dev : "none";
            const char *opts = m->opts[0] ? m->opts : "rw";
            int n = snprintf(buf + pos, bufsz - pos,
                "%d %d 0:%d / %s %s - %s %s %s\n",
                i + 1, i + 1, i + 1, m->path, opts, fstype, dev, opts);
            if (n < 0 || (size_t)n >= bufsz - pos) break;
            pos += n;
        }
        break;
    }
    case PF_SYS_KERNEL_PID_MAX:
        snprintf(buf, bufsz, "%d\n", proc_pid_max());
        break;
    case PF_SYS_KERNEL_OSRELEASE:
        snprintf(buf, bufsz, "%s\n", LINUX_ABI_RELEASE);
        break;
    case PF_SYS_KERNEL_PIDMAP:
        proc_format_pidmap(buf, bufsz);
        break;
    case PF_SYS_KERNEL_TAINTED:
        snprintf(buf, bufsz, "0\n");
        break;
    case PF_SYS_KERNEL_SCHED_AUTOGROUP:
        snprintf(buf, bufsz, "0\n");
        break;
    case PF_SYS_KERNEL_CORE_PATTERN:
        snprintf(buf, bufsz, "core\n");
        break;
    case PF_SYS_KERNEL_IO_URING_DISABLED:
        snprintf(buf, bufsz, "0\n");
        break;
    case PF_SYS_FS_PIPE_MAX_SIZE:
        snprintf(buf, bufsz, "%d\n", g_procfs_pipe_max_size);
        break;
    case PF_SYS_FS_LEASE_BREAK_TIME:
        snprintf(buf, bufsz, "%d\n", g_procfs_lease_break_time);
        break;
    case PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS:
        snprintf(buf, bufsz, "16384\n");
        break;
    case PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES:
        snprintf(buf, bufsz, "128\n");
        break;
    case PF_SYS_VM_DROP_CACHES:
        snprintf(buf, bufsz, "0\n");
        break;
    case PF_INTERRUPTS:
        snprintf(buf, bufsz,
            "           CPU0\n"
            "  0:         %lu   IO-APIC   2-edge   timer\n"
            "  1:         %lu   IO-APIC   1-edge   i8042\n"
            "RES:         %lu   Rescheduling interrupts\n"
            "CAL:         %lu   Function call interrupts\n",
            (unsigned long)0, (unsigned long)0,
            (unsigned long)0, (unsigned long)0);
        break;
    case PF_SELF: {  // 生成当前进程的 pid
        task_t *t = proc_current();
        snprintf(buf, bufsz, "%d\n", t ? t->pid : 0);
        break;
    }
    case PF_FSTYPE:
        snprintf(buf, bufsz, "nodev\tproc\nnodev\tcgroup\nnodev\tcgroup2\n\text4\n\tvfat\n\tramfs\n\ttmpfs\n");
        break;
    case PF_CGROUPS:
        snprintf(buf, bufsz,
            "#subsys_name\thierarchy\tnum_cgroups\tenabled\n"
            "cpuset\t1\t1\t1\n"
            "cpu\t1\t1\t1\n"
            "cpuacct\t1\t1\t1\n"
            "memory\t1\t1\t1\n");
        break;
    case PF_PID_PAGEMAP:
        buf[0] = '\0';
        return 0;
    default:
        break;
    }
    return (int)strlen(buf);
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
    int gfd = -1;
    int cloexec = 0;
    vfile_t *target = fdtable_get_file_ref(task, fd, &gfd, &cloexec);
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
    int len = snprintf(buf, bufsz,
                       "pos:\t%lu\nflags:\t0%o\nino:\t%lu\n",
                       (unsigned long)pos, open_flags, ino);
    vfs_put_file_ref(gfd, target);
    return len < 0 ? 0 : len;
}


