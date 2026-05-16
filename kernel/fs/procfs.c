/*
 * A20OS — procfs: Virtual /proc filesystem
 *
 * Provides process and system information via synthetic files.
 * Entries are generated on-demand during lookup and read.
 */

#include "fs/procfs.h"
#include "fs/file.h"
#include "fs/block_cache.h"
#include "fs/page_cache.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "core/timer.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/version.h"
#include "net/socket.h"

extern task_t *proc_current(void);
extern task_t *proc_find(int pid);
extern size_t  frame_free_count(void);
extern int     vfs_mount_count(void);
extern struct mount *vfs_mount_at(int index);

// procfs 文件类型枚举
typedef enum {
    PF_ROOT,
    PF_MEMINFO,
    PF_VERSION,
    PF_UPTIME,
    PF_CMDLINE,
    PF_CPUINFO,
    PF_MOUNTS,
    PF_LOADAVG,
    PF_NET,
    PF_CONFIG_GZ,
    PF_PID_STAT,
    PF_PID_STATUS,
    PF_PID_STATM,
    PF_PID_MAPS,
    PF_PID_SMAPS,
    PF_PID_OOM_SCORE_ADJ,
    PF_PID_OOM_SCORE,
    PF_PID_CGROUP,
    PF_PID_CMDLINE,
    PF_PID_COMM,
    PF_PID_EXE,
    PF_PID_CWD,
    PF_PID_FD,
    PF_PID_ENVIRON,
    PF_PID_IO,
    PF_PID_LOGINUID,
    PF_PID_SESSIONID,
    PF_PID_NS,
    PF_PID_FDINFO,
    PF_PID_MOUNTINFO,
    PF_SYS,
    PF_SYS_FS,
    PF_SYS_FS_PIPE_MAX_SIZE,
    PF_SYS_FS_LEASE_BREAK_TIME,
    PF_SYS_KERNEL,
    PF_SYS_KERNEL_PID_MAX,
    PF_SYS_KERNEL_PIDMAP,
    PF_SYS_KERNEL_TAINTED,
    PF_A20,
    PF_A20_BCACHE,
    PF_A20_PAGE_CACHE,
    PF_CGROUPS,
    PF_SELF,
    PF_FSTYPE,
    PF_SWAPS,
} pf_type_t;

// procfs 目录项结构
typedef struct pf_entry {
    char name[32];           // 文件名
    pf_type_t type;         // 文件类型
    int pid;                // 进程 ID（仅对进程相关文件有效）
    struct pf_entry *next;
} pf_entry_t;

static int g_procfs_pipe_max_size = 1048576;
static int g_procfs_lease_break_time = 45;

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

// 创建一个新的目录项
static pf_entry_t *new_entry(const char *name, pf_type_t type, int pid) {
    pf_entry_t *e = (pf_entry_t *)kmalloc(sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->type = type;
    e->pid = pid;
    return e;
}

// 判断字符串是否为纯数字（进程 ID）
static int is_pid_str(const char *s) {
    if (!s || !*s) return 0;
    while (*s) { if (*s < '0' || *s > '9') return 0; s++; }
    return 1;
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

static const char *vma_name(vm_area_t *vma) {
    if (vma->vm_flags & VM_STACK) return "[stack]";
    if (vma->vm_flags & VM_ANON) return "";
    return "";
}

static void append_vma_flags(char *buf, size_t bufsz, size_t *off, vm_area_t *vma) {
    appendf(buf, bufsz, off, "VmFlags:");
    if (vma->vm_flags & VM_READ) appendf(buf, bufsz, off, " rd");
    if (vma->vm_flags & VM_WRITE) appendf(buf, bufsz, off, " wr");
    if (vma->vm_flags & VM_EXEC) appendf(buf, bufsz, off, " ex");
    appendf(buf, bufsz, off, " mr mw me ac");
    if (vma->vm_flags & VM_STACK) appendf(buf, bufsz, off, " gd");
    if (vma->vm_flags & VM_HUGEPAGE) appendf(buf, bufsz, off, " hg");
    if (vma->vm_flags & VM_NOHUGEPAGE) appendf(buf, bufsz, off, " nh");
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

static void vma_collect_smaps_stats(mm_struct_t *mm, vm_area_t *vma,
                                    vma_smaps_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    if (!mm || !mm->pgdir || !vma) return;

    for (uint64_t va = vma->start; va < vma->end; ) {
        int level = 0;
        uint64_t base = 0;
        size_t size = 0;
        uint64_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
        if (pte && (*pte & PTE_V) && arch_pte_is_leaf(*pte)) {
            size_t pages = size / PAGE_SIZE;
            int shared = (vma->vm_flags & VM_SHARED) != 0;
            int dirty = (*pte & PTE_D) != 0;
            int anon = (vma->vm_flags & VM_ANON) != 0;

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
            if (level > 0) {
                if (anon && shared)
                    stats->shmem_pmd_pages += pages;
                else if (anon)
                    stats->anon_huge_pages += pages;
                else
                    stats->file_pmd_pages += pages;
            }
            va = base + size;
        } else {
            va += PAGE_SIZE;
        }
    }
}

static void generate_pid_maps(task_t *t, char *buf, size_t bufsz, int smaps) {
    size_t off = 0;
    if (!t || !t->mm) return;
    for (vm_area_t *v = t->mm->mmap; v && off + 128 < bufsz; v = v->next) {
        char r = (v->vm_flags & VM_READ) ? 'r' : '-';
        char w = (v->vm_flags & VM_WRITE) ? 'w' : '-';
        char x = (v->vm_flags & VM_EXEC) ? 'x' : '-';
        char s = (v->vm_flags & VM_SHARED) ? 's' : 'p';
        size_t kb = (size_t)(v->end - v->start) / 1024;
        vma_smaps_stats_t st;
        vma_collect_smaps_stats(t->mm, v, &st);
        size_t rss_kb = st.rss_pages * PAGE_SIZE / 1024;
        int thp_eligible = !t->policy.thp_disabled && !(v->vm_flags & VM_NOHUGEPAGE) &&
                           (v->vm_flags & (VM_HUGEPAGE | VM_ANON)) &&
                           (v->end - v->start) >= (2UL * 1024 * 1024);

        appendf(buf, bufsz, &off, "%012lx-%012lx %c%c%c%c 00000000 00:00 0 %s\n",
                (unsigned long)v->start, (unsigned long)v->end,
                r, w, x, s, vma_name(v));
        if (!smaps) continue;
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
                (unsigned long)(st.shared_clean_pages * PAGE_SIZE / 1024),
                (unsigned long)(st.shared_dirty_pages * PAGE_SIZE / 1024),
                (unsigned long)(st.private_clean_pages * PAGE_SIZE / 1024),
                (unsigned long)(st.private_dirty_pages * PAGE_SIZE / 1024),
                (unsigned long)rss_kb,
                (unsigned long)(st.anonymous_pages * PAGE_SIZE / 1024),
                (unsigned long)(st.anon_huge_pages * PAGE_SIZE / 1024),
                (unsigned long)(st.shmem_pmd_pages * PAGE_SIZE / 1024),
                (unsigned long)(st.file_pmd_pages * PAGE_SIZE / 1024),
                thp_eligible);
        append_vma_flags(buf, bufsz, &off, v);
    }
}

// 生成 procfs 文件的内容
static int generate_content(pf_type_t type, int pid, char *buf, size_t bufsz) {
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
        if (available_kb > total_kb)
            available_kb = total_kb;
        snprintf(buf, bufsz,
            "MemTotal:       %lu kB\n"
            "MemFree:        %lu kB\n"
            "MemAvailable:   %lu kB\n"
            "Buffers:        %lu kB\n"
            "Cached:         %lu kB\n"
            "SwapTotal:      0 kB\n"
            "SwapFree:       0 kB\n"
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
    case PF_SWAPS:
        snprintf(buf, bufsz,
            "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n");
        break;
    case PF_VERSION:
        snprintf(buf, bufsz, "A20OS version " VERSION " (" ARCH_NAME ")\n"); 
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
    case PF_CONFIG_GZ: {
        size_t n = sizeof(g_proc_config_gz) < bufsz ? sizeof(g_proc_config_gz) : bufsz;
        memcpy(buf, g_proc_config_gz, n);
        return (int)n;
    }
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
    case PF_PID_STAT: {  // 生成进程 stat 信息
        task_t *t = proc_find(pid);
        if (!t) { snprintf(buf, bufsz, "%d (unknown) S 0 0\n", pid); break; }
        snprintf(buf, bufsz,
            "%d (%s) S %d %d %d 0 0 0 0 0 0 0 0 %lu 0\n",
            t->pid, t->name, t->ppid, t->pgid, t->sid,
            (unsigned long)t->total_time);
        break;
    }
    case PF_PID_STATUS: {  // 生成进程 status 信息
        task_t *t = proc_find(pid);
        if (!t) { snprintf(buf, bufsz, "Name:\tunknown\nPid:\t%d\n", pid); break; }
        const char *state = "S (sleeping)";
        if (t->state == PROC_RUNNING) state = "R (running)";
        else if (t->state == PROC_BLOCKED) state = "S (sleeping)";
        else if (t->state == PROC_ZOMBIE) state = "Z (zombie)";
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
        snprintf(buf, bufsz,
            "Name:\t%s\n"
            "Pid:\t%d\n"
            "PPid:\t%d\n"
            "PGid:\t%d\n"
            "Sid:\t%d\n"
            "State:\t%s\n"
            "Uid:\t%d\t%d\t%d\t%d\n"
            "Gid:\t%d\t%d\t%d\t%d\n"
            "Groups:\t%s\n"
            "CapInh:\t%016lx\n"
            "CapPrm:\t%016lx\n"
            "CapEff:\t%016lx\n"
            "CapBnd:\t%016lx\n"
            "Threads:\t1\n",
            t->name, t->pid, t->ppid, t->pgid, t->sid, state,
            t->cred.uid, t->cred.euid, t->cred.suid, t->cred.fsuid,
            t->cred.gid, t->cred.egid, t->cred.sgid, t->cred.fsgid,
            groups,
            (unsigned long)t->cred.cap_inheritable,
            (unsigned long)t->cred.cap_permitted,
            (unsigned long)t->cred.cap_effective,
            (unsigned long)t->cred.cap_bounding);
        break;
    }
    case PF_PID_STATM: {
        task_t *t = proc_find(pid);
        size_t total = t && t->mm ? t->mm->total_vm : 0;
        size_t rss = t && t->mm ? t->mm->rss : 0;
        snprintf(buf, bufsz, "%lu %lu 0 0 0 0 0\n",
                 (unsigned long)total, (unsigned long)rss);
        break;
    }
    case PF_PID_MAPS: {
        task_t *t = proc_find(pid);
        generate_pid_maps(t, buf, bufsz, 0);
        break;
    }
    case PF_PID_SMAPS: {
        task_t *t = proc_find(pid);
        generate_pid_maps(t, buf, bufsz, 1);
        break;
    }
    case PF_PID_OOM_SCORE_ADJ: {
        task_t *t = proc_find(pid);
        snprintf(buf, bufsz, "%d\n", t ? t->policy.oom_score_adj : 0);
        break;
    }
    case PF_PID_OOM_SCORE: {
        task_t *t = proc_find(pid);
        snprintf(buf, bufsz, "%d\n", t ? (t->policy.oom_score_adj >= 0 ? t->policy.oom_score_adj : 0) : 0);
        break;
    }
    case PF_PID_CGROUP:
        snprintf(buf, bufsz,
            "0::/init.scope\n");
        break;
    case PF_PID_CMDLINE: {
        task_t *t = proc_find(pid);
        if (!t || !t->exec_path[0]) {
            buf[0] = '\0';
            return 1;
        }
        size_t len = strlen(t->exec_path);
        if (len + 1 > bufsz) len = bufsz - 1;
        memcpy(buf, t->exec_path, len);
        buf[len] = '\0';
        return (int)(len + 1);
    }
    case PF_PID_COMM: {
        task_t *t = proc_find(pid);
        snprintf(buf, bufsz, "%s\n", t ? t->name : "unknown");
        break;
    }
    case PF_PID_EXE: {
        task_t *t = proc_find(pid);
        snprintf(buf, bufsz, "%s\n", t && t->exec_path[0] ? t->exec_path : "/sbin/init");
        break;
    }
    case PF_PID_CWD: {
        task_t *t = proc_find(pid);
        snprintf(buf, bufsz, "%s\n", t ? t->fs.cwd : "/");
        break;
    }
    case PF_PID_ENVIRON:
        buf[0] = '\0';
        return 0;
    case PF_PID_IO: {
        task_t *t = proc_find(pid);
        snprintf(buf, bufsz,
            "rchar: %lu\n"
            "wchar: %lu\n"
            "syscr: 0\nsyscw: 0\n"
            "read_bytes: 0\nwrite_bytes: 0\n"
            "cancelled_write_bytes: 0\n",
            (unsigned long)(t ? t->total_time : 0),
            (unsigned long)(t ? t->child_stime : 0));
        break;
    }
    case PF_PID_LOGINUID:
        snprintf(buf, bufsz, "4294967295\n");
        break;
    case PF_PID_SESSIONID: {
        task_t *t = proc_find(pid);
        snprintf(buf, bufsz, "%d\n", t ? t->sid : 0);
        break;
    }
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
    case PF_SYS_KERNEL_PIDMAP:
        proc_format_pidmap(buf, bufsz);
        break;
    case PF_SYS_KERNEL_TAINTED:
        snprintf(buf, bufsz, "0\n");
        break;
    case PF_SYS_FS_PIPE_MAX_SIZE:
        snprintf(buf, bufsz, "%d\n", g_procfs_pipe_max_size);
        break;
    case PF_SYS_FS_LEASE_BREAK_TIME:
        snprintf(buf, bufsz, "%d\n", g_procfs_lease_break_time);
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
    default:
        break;
    }
    return (int)strlen(buf);
}

// 根据名称解析文件类型（如果需要同时解析出 pid）
static pf_type_t name_to_type(const char *name, int *out_pid) {
    *out_pid = 0;
    if (strcmp(name, "meminfo") == 0) return PF_MEMINFO;
    if (strcmp(name, "version") == 0) return PF_VERSION;
    if (strcmp(name, "uptime") == 0) return PF_UPTIME;
    if (strcmp(name, "cpuinfo") == 0) return PF_CPUINFO;
    if (strcmp(name, "mounts") == 0) return PF_MOUNTS;
    if (strcmp(name, "self") == 0) return PF_SELF;
    if (strcmp(name, "loadavg") == 0) return PF_LOADAVG;
    if (strcmp(name, "net") == 0) return PF_NET;
    if (strcmp(name, "config.gz") == 0) return PF_CONFIG_GZ;
    if (strcmp(name, "filesystems") == 0) return PF_FSTYPE;
    if (strcmp(name, "cgroups") == 0) return PF_CGROUPS;
    if (strcmp(name, "swaps") == 0) return PF_SWAPS;
    if (strcmp(name, "pidmap") == 0) return PF_SYS_KERNEL_PIDMAP;
    if (strcmp(name, "a20") == 0) return PF_A20;
    if (strcmp(name, "bcache") == 0) return PF_A20_BCACHE;
    if (strcmp(name, "page_cache") == 0) return PF_A20_PAGE_CACHE;
    if (strcmp(name, "cmdline") == 0) return PF_CMDLINE;
    if (is_pid_str(name)) {
        *out_pid = atoi(name);
        return PF_ROOT;
    }
    if (strcmp(name, "stat") == 0) return PF_PID_STAT;
    if (strcmp(name, "status") == 0) return PF_PID_STATUS;
    if (strcmp(name, "statm") == 0) return PF_PID_STATM;
    if (strcmp(name, "maps") == 0) return PF_PID_MAPS;
    if (strcmp(name, "smaps") == 0) return PF_PID_SMAPS;
    if (strcmp(name, "oom_score_adj") == 0) return PF_PID_OOM_SCORE_ADJ;
    if (strcmp(name, "oom_score") == 0) return PF_PID_OOM_SCORE;
    if (strcmp(name, "cgroup") == 0) return PF_PID_CGROUP;
    if (strcmp(name, "cmdline") == 0) return PF_PID_CMDLINE;
    if (strcmp(name, "comm") == 0) return PF_PID_COMM;
    if (strcmp(name, "exe") == 0) return PF_PID_EXE;
    if (strcmp(name, "cwd") == 0) return PF_PID_CWD;
    if (strcmp(name, "fd") == 0) return PF_PID_FD;
    if (strcmp(name, "environ") == 0) return PF_PID_ENVIRON;
    if (strcmp(name, "io") == 0) return PF_PID_IO;
    if (strcmp(name, "loginuid") == 0) return PF_PID_LOGINUID;
    if (strcmp(name, "sessionid") == 0) return PF_PID_SESSIONID;
    if (strcmp(name, "ns") == 0) return PF_PID_NS;
    if (strcmp(name, "fdinfo") == 0) return PF_PID_FDINFO;
    if (strcmp(name, "mountinfo") == 0) return PF_PID_MOUNTINFO;
    return PF_ROOT;
}

// Lightweight metadata stored in vnode->fs_data (no content buffer)
typedef struct {
    pf_type_t type;
    int pid;
    size_t content_len;
} procfs_meta_t;

// Full state for open files (includes content buffer)
typedef struct {
    pf_type_t type;
    int pid;
    size_t content_len;
    char content[4096];
} procfs_priv_t;

static procfs_meta_t *procfs_meta_create(pf_type_t type, int pid) {
    procfs_meta_t *m = (procfs_meta_t *)kmalloc(sizeof(*m));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->pid = pid;
    char tmp[4096];
    m->content_len = (size_t)generate_content(type, pid, tmp, sizeof(tmp));
    return m;
}

static procfs_priv_t *procfs_priv_create(pf_type_t type, int pid) {
    procfs_priv_t *p = (procfs_priv_t *)kmalloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->type = type;
    p->pid = pid;
    p->content_len = (size_t)generate_content(type, pid, p->content, sizeof(p->content));
    return p;
}

// procfs 的 lookup 操作（查找目录项）
static int procfs_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    if (!name || !*name) return -ENOENT;

    int pid = 0;
    pf_type_t type = name_to_type(name, &pid);
    procfs_meta_t *dp = (procfs_meta_t *)dir->fs_data;

    pf_entry_t *child = NULL;
    if (dp && dp->type == PF_ROOT && dp->pid == 0 && strcmp(name, "sys") == 0) {
        child = new_entry(name, PF_SYS, 0);
        type = PF_SYS;
    } else if (dp && dp->type == PF_SYS && strcmp(name, "fs") == 0) {
        child = new_entry(name, PF_SYS_FS, 0);
        type = PF_SYS_FS;
    } else if (dp && dp->type == PF_SYS_FS && strcmp(name, "pipe-max-size") == 0) {
        child = new_entry(name, PF_SYS_FS_PIPE_MAX_SIZE, 0);
        type = PF_SYS_FS_PIPE_MAX_SIZE;
    } else if (dp && dp->type == PF_SYS_FS && strcmp(name, "lease-break-time") == 0) {
        child = new_entry(name, PF_SYS_FS_LEASE_BREAK_TIME, 0);
        type = PF_SYS_FS_LEASE_BREAK_TIME;
    } else if (dp && dp->type == PF_SYS && strcmp(name, "kernel") == 0) {
        child = new_entry(name, PF_SYS_KERNEL, 0);
        type = PF_SYS_KERNEL;
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "pid_max") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_PID_MAX, 0);
        type = PF_SYS_KERNEL_PID_MAX;
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "pidmap") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_PIDMAP, 0);
        type = PF_SYS_KERNEL_PIDMAP;
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "tainted") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_TAINTED, 0);
        type = PF_SYS_KERNEL_TAINTED;
    } else if (dp && dp->type == PF_ROOT && dp->pid == 0 && strcmp(name, "a20") == 0) {
        child = new_entry(name, PF_A20, 0);
        type = PF_A20;
    } else if (dp && dp->type == PF_A20 && strcmp(name, "bcache") == 0) {
        child = new_entry(name, PF_A20_BCACHE, 0);
        type = PF_A20_BCACHE;
    } else if (dp && dp->type == PF_A20 && strcmp(name, "page_cache") == 0) {
        child = new_entry(name, PF_A20_PAGE_CACHE, 0);
        type = PF_A20_PAGE_CACHE;
    } else if (type == PF_SELF) {
        task_t *cur = proc_current();
        child = new_entry(name, PF_ROOT, cur ? cur->pid : 0);
    } else if (is_pid_str(name)) {
        child = new_entry(name, PF_ROOT, pid);
    } else if (type == PF_PID_STAT || type == PF_PID_STATUS ||
               type == PF_PID_STATM || type == PF_PID_MAPS ||
               type == PF_PID_SMAPS || type == PF_PID_OOM_SCORE_ADJ ||
               type == PF_PID_OOM_SCORE || type == PF_PID_CGROUP ||
               type == PF_PID_CMDLINE || type == PF_PID_COMM ||
               type == PF_PID_EXE || type == PF_PID_CWD ||
               type == PF_PID_FD || type == PF_PID_ENVIRON ||
               type == PF_PID_IO || type == PF_PID_LOGINUID ||
               type == PF_PID_SESSIONID || type == PF_PID_NS ||
               type == PF_PID_FDINFO || type == PF_PID_MOUNTINFO) {
        child = new_entry(name, type, dp ? dp->pid : 0);
    } else if (dp && dp->type == PF_ROOT && dp->pid > 0 &&
               strcmp(name, "cmdline") == 0) {
        child = new_entry(name, PF_PID_CMDLINE, dp->pid);
    } else if (dp && dp->type == PF_ROOT && dp->pid > 0 &&
               strcmp(name, "mounts") == 0) {
        /* /proc/<pid>/mounts — same as /proc/mounts */
        child = new_entry(name, PF_MOUNTS, 0);
    } else if (dp->type == PF_PID_NS || dp->type == PF_PID_FD ||
               dp->type == PF_PID_FDINFO) {
        return -ENOENT;
    } else if (dp && dp->type == PF_ROOT && dp->pid > 0) {
        return -ENOENT;
    } else {
        child = new_entry(name, type, 0);
    }
    if (!child) return -ENOMEM;

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) { kfree(child); return -ENOMEM; }
    memset(vn, 0, sizeof(*vn));
    vn->ino = (uint64_t)(uintptr_t)child;
    vn->type = ((type == PF_ROOT && is_pid_str(name)) || type == PF_SELF ||
                type == PF_SYS || type == PF_SYS_FS || type == PF_SYS_KERNEL ||
                type == PF_A20 || type == PF_PID_FD || type == PF_PID_NS ||
                type == PF_PID_FDINFO) ?
               VFS_FT_DIR : VFS_FT_REGULAR;
    vn->mode = (vn->type == VFS_FT_DIR) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    if (type == PF_PID_OOM_SCORE_ADJ ||
        type == PF_SYS_FS_PIPE_MAX_SIZE || type == PF_SYS_FS_LEASE_BREAK_TIME)
        vn->mode = S_IFREG | 0644;
    vnode_ref_init(vn, 1);
    vn->parent = dir;
    vnode_get(dir);
    vn->ops = dir->ops;

    procfs_meta_t *meta = procfs_meta_create(child->type, child->pid);
    if (meta) vn->size = meta->content_len;
    vn->fs_data = meta;

    *out = vn;
    return 0;
}

// procfs 的 stat 操作（获取文件状态）
static int procfs_stat(vnode_t *vn, kstat_t *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino = vn->ino;
    st->st_mode = vn->mode;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_size = vn->size;
    st->st_nlink = 1;
    return 0;
}

// procfs 的 release 操作（释放 vnode）
static void procfs_release(vnode_t *vn) {
    if (vn->fs_data) kfree(vn->fs_data);
    if (vn->ino) kfree((void *)(uintptr_t)vn->ino);
    vnode_put(vn->parent);
    kfree(vn);
}

// procfs vnode 操作表
static vnode_ops_t g_procfs_vnode_ops = {
    .lookup  = procfs_lookup,
    .stat    = procfs_stat,
    .release = procfs_release,
};

// procfs 的 read 操作（读取文件内容）
static int procfs_fread(vfile_t *vf, char *buf, size_t count) {
    if (!vf || !vf->priv) return -EBADF;
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    if (vf->offset >= p->content_len) return 0;
    size_t avail = p->content_len - vf->offset;
    size_t n = count < avail ? count : avail;
    memcpy(buf, p->content + vf->offset, n);
    vf->offset += n;
    return (int)n;
}

static int procfs_fwrite(vfile_t *vf, const char *buf, size_t count) {
    if (!vf || !vf->priv) return -EBADF;
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    if (p->type == PF_PID_OOM_SCORE_ADJ) {
        char tmp[32];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        int value = atoi(tmp);
        if (value < -1000 || value > 1000)
            return -EINVAL;
        task_t *t = proc_find(p->pid);
        if (!t) t = proc_current();
        if (t) t->policy.oom_score_adj = value;
        return (int)count;
    }
    if (p->type == PF_SYS_KERNEL_PID_MAX) {
        char tmp[32];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        int value = atoi(tmp);
        int r = proc_set_pid_max(value);
        return r < 0 ? r : (int)count;
    }
    if (p->type == PF_SYS_FS_PIPE_MAX_SIZE || p->type == PF_SYS_FS_LEASE_BREAK_TIME) {
        char tmp[32];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        int value = atoi(tmp);
        if (value < 0)
            return -EINVAL;
        if (p->type == PF_SYS_FS_PIPE_MAX_SIZE)
            g_procfs_pipe_max_size = value;
        else
            g_procfs_lease_break_time = value;
        return (int)count;
    }
    return -EINVAL;
}

// procfs 的 lseek 操作（设置文件偏移）
static long procfs_flseek(vfile_t *vf, long offset, int whence) {
    if (!vf || !vf->priv) return -EBADF;
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    long new_off;
    switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)vf->offset + offset; break;
        case SEEK_END: new_off = (long)p->content_len + offset; break;
        default: return -EINVAL;
    }
    if (new_off < 0) new_off = 0;
    vf->offset = (size_t)new_off;
    return new_off;
}

// procfs 的 readdir 操作（读取目录项）
static int procfs_freaddir(vfile_t *vf, void *dirp, size_t count) {
    static const char *root_entries[] = {
        ".", "..", "meminfo", "version", "uptime", "cmdline",
        "cpuinfo", "mounts", "self", "loadavg", "net", "config.gz",
        "filesystems", "cgroups", "swaps", "pidmap", "sys", "a20", NULL
    };
    static const char *pid_entries[] = {
        ".", "..", "stat", "status", "statm", "maps", "smaps",
        "oom_score", "oom_score_adj", "cgroup", "cmdline", "comm", "exe", "cwd",
        "fd", "environ", "io", "loginuid", "sessionid", "ns", "fdinfo",
        "mountinfo", "mounts", NULL
    };
    static const char *sys_entries[] = {
        ".", "..", "fs", "kernel", NULL
    };
    static const char *sys_fs_entries[] = {
        ".", "..", "pipe-max-size", "lease-break-time", NULL
    };
    static const char *sys_kernel_entries[] = {
        ".", "..", "pid_max", "pidmap", "tainted", NULL
    };
    static const char *a20_entries[] = {
        ".", "..", "bcache", "page_cache", NULL
    };
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    const char **entries = root_entries;
    int is_root = (p && p->type == PF_ROOT && p->pid == 0);
    if (p && p->type == PF_ROOT && p->pid > 0)
        entries = pid_entries;
    else if (p && p->type == PF_SYS)
        entries = sys_entries;
    else if (p && p->type == PF_SYS_FS)
        entries = sys_fs_entries;
    else if (p && p->type == PF_SYS_KERNEL)
        entries = sys_kernel_entries;
    else if (p && p->type == PF_A20)
        entries = a20_entries;
    int idx = (int)(vf->offset);
    size_t total = 0;
    char *out = (char *)dirp;

    while (total < count) {
        const char *name = NULL;
        char pidbuf[16];

        if (is_root) {
            int static_count = 0;
            for (int i = 0; root_entries[i]; i++) static_count = i + 1;
            if (idx < static_count) {
                name = root_entries[idx];
            } else {
                int pid_idx = idx - static_count;
                uint64_t flags = spin_lock_irqsave(&proc_lock);
                int cur_idx = 0;
                task_t *t;
                for (t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
                    if (t->state == PROC_UNUSED || t->pid <= 1)
                        continue;
                    if (cur_idx == pid_idx)
                        break;
                    cur_idx++;
                }
                if (t) {
                    snprintf(pidbuf, sizeof(pidbuf), "%d", t->pid);
                    name = pidbuf;
                }
                spin_unlock_irqrestore(&proc_lock, flags);
            }
        } else {
            name = entries[idx];
        }

        if (!name) break;
        size_t namelen = strlen(name);
        size_t reclen = (sizeof(vfs_dirent64_t) + namelen + 1 + 7) & ~7UL;
        if (total + reclen > count) break;

        vfs_dirent64_t *d = (vfs_dirent64_t *)(out + total);
        d->d_ino = (uint64_t)idx;
        d->d_off = (int64_t)(total + reclen);
        d->d_reclen = (uint16_t)reclen;
        int is_dir = (idx <= 1);
        if (is_root && is_pid_str(name))
            is_dir = 1;
        if (!is_root && (strcmp(name, "fd") == 0 || strcmp(name, "ns") == 0 ||
                         strcmp(name, "fdinfo") == 0))
            is_dir = 1;
        d->d_type = is_dir ? 4 : 8; /* DT_DIR=4, DT_REG=8 per Linux getdents64 */
        memcpy(d->d_name, name, namelen + 1);
        total += reclen;
        idx++;
    }
    vf->offset = (size_t)idx;
    return (int)total;
}

// procfs 的 close 操作（关闭文件）
static int procfs_fclose(vfile_t *vf) {
    if (vf && vf->priv) { kfree(vf->priv); vf->priv = NULL; }
    return 0;
}

// procfs vfile 操作表
static vfile_ops_t g_procfs_fops = {
    .read    = procfs_fread,
    .write   = procfs_fwrite,
    .lseek   = procfs_flseek,
    .readdir = procfs_freaddir,
    .close   = procfs_fclose,
};

// 挂载 procfs 文件系统
vnode_t *procfs_mount(void) {
    vnode_t *root = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!root) return NULL;
    memset(root, 0, sizeof(*root));
    root->ino = 0;  /* 0 = no pf_entry_t to free in release */
    root->type = VFS_FT_DIR;
    root->mode = S_IFDIR | 0555;
    vnode_ref_init(root, 1);
    root->ops = &g_procfs_vnode_ops;
    root->size = 0;

    procfs_meta_t *meta = procfs_meta_create(PF_ROOT, 0);
    root->fs_data = meta;
    return root;
}

// 打开 procfs vnode
vfile_t *procfs_open_vnode(vnode_t *vn, int flags) {
    vfile_t *vf = vfile_alloc();
    if (!vf) return NULL;
    vf->vnode = vn;
    vf->flags = flags;
    vnode_get(vn);
    vf->ops = &g_procfs_fops;
    refcount_set(&vf->ref_count, 1);

    procfs_meta_t *meta = (procfs_meta_t *)vn->fs_data;
    procfs_priv_t *priv = procfs_priv_create(meta ? meta->type : PF_ROOT, meta ? meta->pid : 0);
    if (!priv) { vfile_free(vf); return NULL; }
    vf->priv = priv;
    return vf;
}
