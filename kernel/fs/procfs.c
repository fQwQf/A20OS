/*
 * A20OS — procfs: Virtual /proc filesystem
 *
 * Provides process and system information via synthetic files.
 * Entries are generated on-demand during lookup and read.
 */

#include "fs/procfs.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "core/timer.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/version.h"
#include "net/socket.h"

extern task_t *proc_current(void);
extern task_t *proc_find(int pid);
extern size_t  frame_free_count(void);

// procfs 文件类型枚举
typedef enum {
    PF_ROOT,          // 根目录或进程目录
    PF_MEMINFO,       // /proc/meminfo - 内存信息
    PF_VERSION,       // /proc/version - 内核版本
    PF_UPTIME,        // /proc/uptime - 运行时间
    PF_CMDLINE,       // /proc/cmdline - 启动参数
    PF_CPUINFO,       // /proc/cpuinfo - CPU 信息
    PF_MOUNTS,       // /proc/mounts - 挂载信息
    PF_LOADAVG,      // /proc/loadavg - 负载平均值
    PF_NET,          // /proc/net - 网络协议栈状态
    PF_PID_STAT,     // /proc/[pid]/stat - 进程状态
    PF_PID_STATUS,   // /proc/[pid]/status - 进程状态详情
    PF_PID_STATM,
    PF_PID_MAPS,
    PF_PID_SMAPS,
    PF_PID_OOM_SCORE_ADJ,
    PF_SYS,
    PF_SYS_FS,
    PF_SYS_FS_PIPE_MAX_SIZE,
    PF_SYS_FS_LEASE_BREAK_TIME,
    PF_SYS_KERNEL,
    PF_SYS_KERNEL_PID_MAX,
    PF_SYS_KERNEL_SCHED_AUTOGROUP,
    PF_SYS_KERNEL_TAINTED,
    PF_SELF,         // /proc/self - 当前进程的 pid
    PF_FSTYPE,       // /proc/filesystems - 支持的文件系统
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

static size_t vma_resident_pages(mm_struct_t *mm, vm_area_t *vma) {
    size_t pages = 0;
    if (!mm || !mm->pgdir || !vma) return 0;
    for (uint64_t va = vma->start; va < vma->end; ) {
        uint64_t base = 0;
        size_t size = 0;
        uint64_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, &base, &size);
        if (pte && (*pte & PTE_V) && arch_pte_is_leaf(*pte)) {
            pages += size / PAGE_SIZE;
            va = base + size;
        } else {
            va += PAGE_SIZE;
        }
    }
    return pages;
}

static size_t vma_huge_pages(mm_struct_t *mm, vm_area_t *vma) {
    size_t pages = 0;
    if (!mm || !mm->pgdir || !vma) return 0;
    for (uint64_t va = vma->start; va < vma->end; ) {
        int level = 0;
        uint64_t base = 0;
        size_t size = 0;
        uint64_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
        if (pte && (*pte & PTE_V) && arch_pte_is_leaf(*pte)) {
            if (level > 0)
                pages += size / PAGE_SIZE;
            va = base + size;
        } else {
            va += PAGE_SIZE;
        }
    }
    return pages;
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
        size_t rss_kb = vma_resident_pages(t->mm, v) * PAGE_SIZE / 1024;
        size_t huge_kb = vma_huge_pages(t->mm, v) * PAGE_SIZE / 1024;
        int thp_eligible = !t->thp_disabled && !(v->vm_flags & VM_NOHUGEPAGE) &&
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
                "Shared_Clean:          0 kB\n"
                "Shared_Dirty:          0 kB\n"
                "Private_Clean:         0 kB\n"
                "Private_Dirty:  %8lu kB\n"
                "Referenced:     %8lu kB\n"
                "Anonymous:      %8lu kB\n"
                "AnonHugePages:  %8lu kB\n"
                "ShmemPmdMapped:        0 kB\n"
                "FilePmdMapped:         0 kB\n"
                "THPeligible:    %8d\n",
                (unsigned long)kb,
                (unsigned long)(PAGE_SIZE / 1024),
                (unsigned long)(PAGE_SIZE / 1024),
                (unsigned long)rss_kb,
                (unsigned long)rss_kb,
                (unsigned long)rss_kb,
                (unsigned long)rss_kb,
                (unsigned long)((v->vm_flags & VM_ANON) ? rss_kb : 0),
                (unsigned long)huge_kb,
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
        snprintf(buf, bufsz,
            "MemTotal:       %lu kB\n"
            "MemFree:        %lu kB\n"
            "MemAvailable:   %lu kB\n"
            "Buffers:           0 kB\n"
            "Cached:            0 kB\n"
            "AnonHugePages:     0 kB\n"
            "ShmemHugePages:    0 kB\n"
            "FileHugePages:     0 kB\n"
            "HugePages_Total:   0\n"
            "HugePages_Free:    0\n"
            "Hugepagesize:   2048 kB\n",
            (unsigned long)total_kb, (unsigned long)free_kb, (unsigned long)free_kb);
        break;
    }
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
    case PF_MOUNTS:  // 生成挂载信息
        snprintf(buf, bufsz,
            "none / ramfs rw 0 0\n"
            "/dev/vda /mnt vfat rw 0 0\n"
            "none /proc proc rw 0 0\n");
        break;
    case PF_LOADAVG:  // 生成负载平均值
        snprintf(buf, bufsz, "0.00 0.00 0.00 1/64 1\n");
        break;
    case PF_NET:
        net_format_status(buf, bufsz);
        break;
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
        snprintf(buf, bufsz,
            "Name:\t%s\n"
            "Pid:\t%d\n"
            "PPid:\t%d\n"
            "PGid:\t%d\n"
            "Sid:\t%d\n"
            "State:\t%s\n"
            "Threads:\t1\n",
            t->name, t->pid, t->ppid, t->pgid, t->sid, state);
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
        snprintf(buf, bufsz, "%d\n", t ? t->oom_score_adj : 0);
        break;
    }
    case PF_SYS_KERNEL_PID_MAX:
        snprintf(buf, bufsz, "%d\n", proc_pid_max());
        break;
    case PF_SYS_KERNEL_SCHED_AUTOGROUP:
        snprintf(buf, bufsz, "0\n");
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
    case PF_FSTYPE:  // 生成支持的文件系统列表
        snprintf(buf, bufsz, "nodev\tproc\n\text4\n\tvfat\n\tramfs\n\ttmpfs\n");
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
    if (strcmp(name, "cmdline") == 0) return PF_CMDLINE;
    if (strcmp(name, "cpuinfo") == 0) return PF_CPUINFO;
    if (strcmp(name, "mounts") == 0) return PF_MOUNTS;
    if (strcmp(name, "self") == 0) return PF_SELF;
    if (strcmp(name, "loadavg") == 0) return PF_LOADAVG;
    if (strcmp(name, "net") == 0) return PF_NET;
    if (strcmp(name, "filesystems") == 0) return PF_FSTYPE;
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
    return PF_ROOT;
}

// procfs 私有数据结构
typedef struct {
    pf_type_t type;         // 文件类型
    int pid;               // 进程 ID
    char content[4096];    // 文件内容
    size_t content_len;    // 内容长度
} procfs_priv_t;

// 创建 procfs 私有数据并生成内容
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
    procfs_priv_t *dp = (procfs_priv_t *)dir->fs_data;

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
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "sched_autogroup_enabled") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_SCHED_AUTOGROUP, 0);
        type = PF_SYS_KERNEL_SCHED_AUTOGROUP;
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "tainted") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_TAINTED, 0);
        type = PF_SYS_KERNEL_TAINTED;
    } else if (type == PF_SELF) {
        task_t *cur = proc_current();
        child = new_entry(name, PF_ROOT, cur ? cur->pid : 0);
    } else if (is_pid_str(name)) {
        child = new_entry(name, PF_ROOT, pid);
    } else if (type == PF_PID_STAT || type == PF_PID_STATUS ||
               type == PF_PID_STATM || type == PF_PID_MAPS ||
               type == PF_PID_SMAPS || type == PF_PID_OOM_SCORE_ADJ) {
        child = new_entry(name, type, dp ? dp->pid : 0);
    } else {
        child = new_entry(name, type, 0);
    }
    if (!child) return -ENOMEM;

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) { kfree(child); return -ENOMEM; }
    memset(vn, 0, sizeof(*vn));
    vn->ino = (uint64_t)(uintptr_t)child;
    vn->type = ((type == PF_ROOT && is_pid_str(name)) || type == PF_SELF ||
                type == PF_SYS || type == PF_SYS_FS || type == PF_SYS_KERNEL) ?
               VFS_FT_DIR : VFS_FT_REGULAR;
    vn->mode = (vn->type == VFS_FT_DIR) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    if (type == PF_PID_OOM_SCORE_ADJ || type == PF_SYS_KERNEL_SCHED_AUTOGROUP ||
        type == PF_SYS_FS_PIPE_MAX_SIZE || type == PF_SYS_FS_LEASE_BREAK_TIME)
        vn->mode = S_IFREG | 0644;
    vn->ref_count = 1;
    vn->parent = dir;
    dir->ref_count++;
    vn->ops = dir->ops;

    procfs_priv_t *priv = procfs_priv_create(child->type, child->pid);
    if (priv) vn->size = priv->content_len;
    vn->fs_data = priv;

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
        if (t) t->oom_score_adj = value;
        return (int)count;
    }
    if (p->type == PF_SYS_KERNEL_SCHED_AUTOGROUP)
        return (int)count;
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
    if (!vf || !vf->vnode || !vf->vnode->fs_data) return -EBADF;
    procfs_priv_t *p = (procfs_priv_t *)vf->vnode->fs_data;
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
        "cpuinfo", "mounts", "self", "loadavg", "net", "filesystems", NULL
    };
    static const char *pid_entries[] = {
        ".", "..", "stat", "status", "statm", "maps", "smaps", NULL
    };
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    const char **entries = (p && p->type == PF_ROOT && p->pid > 0) ?
                           pid_entries : root_entries;
    int idx = (int)(vf->offset);
    size_t total = 0;
    char *out = (char *)dirp;

    while (entries[idx] && total < count) {
        size_t namelen = strlen(entries[idx]);
        size_t reclen = (sizeof(a20_dirent64_t) + namelen + 1 + 7) & ~7UL;
        if (total + reclen > count) break;

        a20_dirent64_t *d = (a20_dirent64_t *)(out + total);
        d->d_ino = (uint64_t)idx;
        d->d_off = (int64_t)(total + reclen);
        d->d_reclen = (uint16_t)reclen;
        d->d_type = (idx <= 1) ? VFS_FT_DIR : VFS_FT_REGULAR;
        memcpy(d->d_name, entries[idx], namelen + 1);
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
    root->ref_count = 1;
    root->ops = &g_procfs_vnode_ops;
    root->size = 0;

    procfs_priv_t *priv = procfs_priv_create(PF_ROOT, 0);
    root->fs_data = priv;
    return root;
}

// 打开 procfs vnode
vfile_t *procfs_open_vnode(vnode_t *vn, int flags) {
    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!vf) return NULL;
    memset(vf, 0, sizeof(*vf));
    vf->vnode = vn;
    vf->flags = flags;
    vn->ref_count++;
    vf->ops = &g_procfs_fops;
    vf->ref_count = 1;

    procfs_priv_t *vn_priv = (procfs_priv_t *)vn->fs_data;
    procfs_priv_t *priv = procfs_priv_create(vn_priv ? vn_priv->type : PF_ROOT, vn_priv ? vn_priv->pid : 0);
    if (!priv) { kfree(vf); return NULL; }
    vf->priv = priv;
    return vf;
}
