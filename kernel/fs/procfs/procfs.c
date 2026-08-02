/*
 * A20OS — procfs: Virtual /proc filesystem
 *
 * Provides process and system information via synthetic files.
 * Entries are generated on-demand during lookup and read.
 */

#include "fs/procfs.h"
#include "fs/procfs_internal.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/block_cache.h"
#include "fs/page_cache.h"
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

// procfs 文件类型枚举

// procfs 目录项结构
typedef struct pf_entry {
    char name[32];           // 文件名
    pf_type_t type;         // 文件类型
    int pid;                // 进程 ID（仅对进程相关文件有效）
    struct pf_entry *next;
} pf_entry_t;

int g_procfs_pipe_max_size = 1048576;
int g_procfs_lease_break_time = 45;


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

static int parse_fd_name(const char *name, int *fd)
{
    if (!name || !fd || *name < '0' || *name > '9')
        return -ENOENT;
    int value = 0;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9' || value > (MAX_FILES - 1) / 10)
            return -ENOENT;
        value = value * 10 + (*p - '0');
        if (value >= MAX_FILES)
            return -ENOENT;
    }
    *fd = value;
    return 0;
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
    if (strcmp(name, "interrupts") == 0) return PF_INTERRUPTS;
    if (strcmp(name, "pidmap") == 0) return PF_SYS_KERNEL_PIDMAP;
    if (strcmp(name, "a20") == 0) return PF_A20;
    if (strcmp(name, "bcache") == 0) return PF_A20_BCACHE;
    if (strcmp(name, "page_cache") == 0) return PF_A20_PAGE_CACHE;
    if (strcmp(name, "oom") == 0) return PF_A20_OOM;
    if (strcmp(name, "task_lifetime") == 0) return PF_A20_TASK_LIFETIME;
    if (strcmp(name, "driver_lifecycle") == 0) return PF_A20_DRIVER_LIFECYCLE;
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
    if (strcmp(name, "pid") == 0) return PF_PID_NS_PID;
    if (strcmp(name, "uts") == 0) return PF_PID_NS_UTS;
    if (strcmp(name, "user") == 0) return PF_PID_NS_USER;
    if (strcmp(name, "ipc") == 0) return PF_PID_NS_IPC;
    if (strcmp(name, "mnt") == 0) return PF_PID_NS_MNT;
    if (strcmp(name, "net") == 0) return PF_PID_NS_NET;
    if (strcmp(name, "cgroup") == 0) return PF_PID_NS_CGROUP;
    if (strcmp(name, "fdinfo") == 0) return PF_PID_FDINFO;
    if (strcmp(name, "mountinfo") == 0) return PF_PID_MOUNTINFO;
    if (strcmp(name, "pagemap") == 0) return PF_PID_PAGEMAP;
    return PF_ROOT;
}

static int procfs_root_file_type(pf_type_t type)
{
    switch (type) {
    case PF_MEMINFO:
    case PF_VERSION:
    case PF_UPTIME:
    case PF_CMDLINE:
    case PF_CPUINFO:
    case PF_MOUNTS:
    case PF_LOADAVG:
    case PF_NET:
    case PF_CONFIG_GZ:
    case PF_FSTYPE:
    case PF_CGROUPS:
    case PF_SWAPS:
    case PF_INTERRUPTS:
    case PF_SYS_KERNEL_PIDMAP:
        return 1;
    default:
        return 0;
    }
}

// Lightweight metadata stored in vnode->fs_data (no content buffer)
typedef struct {
    pf_type_t type;
    int pid;
    int fd;
    size_t content_len;
} procfs_meta_t;

// Full state for open files (includes content buffer)
typedef struct {
    pf_type_t type;
    int pid;
    size_t content_len;
    char *content;
} procfs_priv_t;

static procfs_meta_t *procfs_meta_create(pf_type_t type, int pid, int fd) {
    procfs_meta_t *m = (procfs_meta_t *)kmalloc(sizeof(*m));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->pid = pid;
    m->fd = fd;
    int real_pid = pid;
    if (pid == -1) {
        task_t *cur = proc_current();
        real_pid = cur ? cur->pid : 0;
    }
    if (type == PF_PID_PAGEMAP || type == PF_PID_MAPS ||
        type == PF_PID_SMAPS || type == PF_PID_FDINFO_ENTRY) {
        m->content_len = (256ULL * 1024 * 1024 * 1024 / PAGE_SIZE) * 8;
        if (type != PF_PID_PAGEMAP)
            m->content_len = 0;
    } else {
        char tmp[4096];
        m->content_len = (size_t)generate_content(type, real_pid, tmp, sizeof(tmp));
    }
    return m;
}

static procfs_priv_t *procfs_priv_create(pf_type_t type, int pid, int fd) {
    procfs_priv_t *p = (procfs_priv_t *)kmalloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->type = type;
    int real_pid = pid;
    if (pid == -1) {
        task_t *cur = proc_current();
        real_pid = cur ? cur->pid : 0;
    }
    p->pid = real_pid;
    if (type == PF_PID_PAGEMAP) {
        p->content_len = (256ULL * 1024 * 1024 * 1024 / PAGE_SIZE) * 8;
    } else if (type == PF_PID_MAPS || type == PF_PID_SMAPS) {
        int ret = generate_pid_maps_alloc(real_pid, type == PF_PID_SMAPS,
                                          &p->content, &p->content_len);
        if (ret < 0) {
            kfree(p);
            return NULL;
        }
    } else {
        p->content = kmalloc(4096);
        if (!p->content) {
            kfree(p);
            return NULL;
        }
        int len = type == PF_PID_FDINFO_ENTRY ?
            generate_pid_fdinfo(real_pid, fd, p->content, 4096) :
            generate_content(type, real_pid, p->content, 4096);
        if (len < 0) {
            kfree(p->content);
            kfree(p);
            return NULL;
        }
        p->content_len = (size_t)len;
    }
    return p;
}

// procfs 的 lookup 操作（查找目录项）
static int procfs_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    if (!name || !*name) return -ENOENT;

    int pid = 0;
    pf_type_t type = name_to_type(name, &pid);
    procfs_meta_t *dp = (procfs_meta_t *)dir->fs_data;

    pf_entry_t *child = NULL;
    int fd_entry = -1;
    int fd_symlink = 0;
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
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "osrelease") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_OSRELEASE, 0);
        type = PF_SYS_KERNEL_OSRELEASE;
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "pid_max") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_PID_MAX, 0);
        type = PF_SYS_KERNEL_PID_MAX;
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "pidmap") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_PIDMAP, 0);
        type = PF_SYS_KERNEL_PIDMAP;
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "tainted") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_TAINTED, 0);
        type = PF_SYS_KERNEL_TAINTED;
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "sched_autogroup_enabled") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_SCHED_AUTOGROUP, 0);
        type = PF_SYS_KERNEL_SCHED_AUTOGROUP;
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "core_pattern") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_CORE_PATTERN, 0);
        type = PF_SYS_KERNEL_CORE_PATTERN;
    } else if (dp && dp->type == PF_SYS_KERNEL && strcmp(name, "io_uring_disabled") == 0) {
        child = new_entry(name, PF_SYS_KERNEL_IO_URING_DISABLED, 0);
        type = PF_SYS_KERNEL_IO_URING_DISABLED;
    } else if (dp && dp->type == PF_SYS && strcmp(name, "vm") == 0) {
        child = new_entry(name, PF_SYS_VM, 0);
        type = PF_SYS_VM;
    } else if (dp && dp->type == PF_SYS_VM && strcmp(name, "drop_caches") == 0) {
        child = new_entry(name, PF_SYS_VM_DROP_CACHES, 0);
        type = PF_SYS_VM_DROP_CACHES;
    } else if (dp && dp->type == PF_SYS_FS && strcmp(name, "inotify") == 0) {
        child = new_entry(name, PF_SYS_FS_INOTIFY, 0);
        type = PF_SYS_FS_INOTIFY;
    } else if (dp && dp->type == PF_SYS_FS_INOTIFY && strcmp(name, "max_queued_events") == 0) {
        child = new_entry(name, PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS, 0);
        type = PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS;
    } else if (dp && dp->type == PF_SYS_FS_INOTIFY && strcmp(name, "max_user_instances") == 0) {
        child = new_entry(name, PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES, 0);
        type = PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES;
    } else if (dp && dp->type == PF_SYS && strcmp(name, "net") == 0) {
        child = new_entry(name, PF_SYS_NET, 0);
        type = PF_SYS_NET;
    } else if (dp && dp->type == PF_NET && strcmp(name, "status") == 0) {
        child = new_entry(name, PF_NET_STATUS, 0);
        type = PF_NET_STATUS;
    } else if (dp && dp->type == PF_NET && strcmp(name, "config") == 0) {
        child = new_entry(name, PF_NET_CONFIG, 0);
        type = PF_NET_CONFIG;
    } else if (dp && dp->type == PF_ROOT && dp->pid == 0 && strcmp(name, "a20") == 0) {
        child = new_entry(name, PF_A20, 0);
        type = PF_A20;
    } else if (dp && dp->type == PF_A20 && strcmp(name, "bcache") == 0) {
        child = new_entry(name, PF_A20_BCACHE, 0);
        type = PF_A20_BCACHE;
    } else if (dp && dp->type == PF_A20 && strcmp(name, "page_cache") == 0) {
        child = new_entry(name, PF_A20_PAGE_CACHE, 0);
        type = PF_A20_PAGE_CACHE;
    } else if (dp && dp->type == PF_A20 && strcmp(name, "oom") == 0) {
        child = new_entry(name, PF_A20_OOM, 0);
        type = PF_A20_OOM;
    } else if (dp && dp->type == PF_A20 &&
               strcmp(name, "task_lifetime") == 0) {
        child = new_entry(name, PF_A20_TASK_LIFETIME, 0);
        type = PF_A20_TASK_LIFETIME;
    } else if (dp && dp->type == PF_A20 && strcmp(name, "driver_lifecycle") == 0) {
        child = new_entry(name, PF_A20_DRIVER_LIFECYCLE, 0);
        type = PF_A20_DRIVER_LIFECYCLE;
    } else if (dp && dp->type == PF_ROOT && dp->pid == 0 && strcmp(name, "interrupts") == 0) {
        child = new_entry(name, PF_INTERRUPTS, 0);
        type = PF_INTERRUPTS;
    } else if (dp && dp->type == PF_PID_FD) {
        if (parse_fd_name(name, &fd_entry) < 0)
            return -ENOENT;
        int real_pid = dp->pid;
        if (real_pid == -1) {
            task_t *cur = proc_current();
            real_pid = cur ? cur->pid : 0;
        }
        task_t *task = proc_find_get(real_pid);
        if (task && !proc_task_may_access(proc_current(), task)) {
            proc_put(task);
            return -EACCES;
        }
        int gfd = -1;
        vfile_t *target = task ?
            fdtable_get_file_ref(task, fd_entry, &gfd, NULL) : NULL;
        proc_put(task);
        if (!target)
            return -ENOENT;
        vfs_put_file_ref(gfd, target);
        child = new_entry(name, PF_PID_FD, dp->pid);
        type = PF_PID_FD;
        fd_symlink = 1;
    } else if (dp && dp->type == PF_PID_FDINFO) {
        if (parse_fd_name(name, &fd_entry) < 0)
            return -ENOENT;
        int real_pid = dp->pid;
        if (real_pid == -1) {
            task_t *cur = proc_current();
            real_pid = cur ? cur->pid : 0;
        }
        task_t *task = proc_find_get(real_pid);
        if (task && !proc_task_may_access(proc_current(), task)) {
            proc_put(task);
            return -EACCES;
        }
        int gfd = -1;
        vfile_t *target = task ?
            fdtable_get_file_ref(task, fd_entry, &gfd, NULL) : NULL;
        proc_put(task);
        if (!target)
            return -ENOENT;
        vfs_put_file_ref(gfd, target);
        child = new_entry(name, PF_PID_FDINFO_ENTRY, dp->pid);
        type = PF_PID_FDINFO_ENTRY;
    } else if (dp && dp->type == PF_PID_NS) {
        if (strcmp(name, "pid") == 0)
            type = PF_PID_NS_PID;
        else if (strcmp(name, "uts") == 0)
            type = PF_PID_NS_UTS;
        else if (strcmp(name, "user") == 0)
            type = PF_PID_NS_USER;
        else if (strcmp(name, "ipc") == 0)
            type = PF_PID_NS_IPC;
        else if (strcmp(name, "mnt") == 0)
            type = PF_PID_NS_MNT;
        else if (strcmp(name, "net") == 0)
            type = PF_PID_NS_NET;
        else if (strcmp(name, "cgroup") == 0)
            type = PF_PID_NS_CGROUP;
        else
            return -ENOENT;
        child = new_entry(name, type, dp->pid);
    } else if (dp && dp->type == PF_ROOT && dp->pid == 0 &&
               type == PF_SELF) {
        child = new_entry(name, PF_ROOT, -1);
    } else if (dp && dp->type == PF_ROOT && dp->pid == 0 &&
               is_pid_str(name)) {
        task_t *task = proc_find_get(pid);
        if (!task)
            return -ENOENT;
        proc_put(task);
        child = new_entry(name, PF_ROOT, pid);
    } else if (dp && dp->type == PF_ROOT && (dp->pid > 0 || dp->pid == -1) &&
               strcmp(name, "cmdline") == 0) {
        child = new_entry(name, PF_PID_CMDLINE, dp->pid);
    } else if (dp && dp->type == PF_ROOT && (dp->pid > 0 || dp->pid == -1) &&
               strcmp(name, "mounts") == 0) {
        /* /proc/<pid>/mounts — same as /proc/mounts */
        child = new_entry(name, PF_MOUNTS, 0);
    } else if (dp && dp->type == PF_ROOT &&
               (dp->pid > 0 || dp->pid == -1) &&
               strcmp(name, "cgroup") == 0) {
        type = PF_PID_CGROUP;
        child = new_entry(name, type, dp->pid);
    } else if (dp && dp->type == PF_ROOT && (dp->pid > 0 || dp->pid == -1)) {
        if (type == PF_PID_STAT || type == PF_PID_STATUS ||
            type == PF_PID_STATM || type == PF_PID_MAPS ||
            type == PF_PID_SMAPS || type == PF_PID_OOM_SCORE_ADJ ||
            type == PF_PID_OOM_SCORE || type == PF_PID_CGROUP ||
            type == PF_PID_COMM || type == PF_PID_EXE ||
            type == PF_PID_CWD || type == PF_PID_FD ||
            type == PF_PID_ENVIRON || type == PF_PID_IO ||
            type == PF_PID_LOGINUID || type == PF_PID_SESSIONID ||
            type == PF_PID_NS || type == PF_PID_FDINFO ||
            type == PF_PID_MOUNTINFO || type == PF_PID_PAGEMAP) {
            child = new_entry(name, type, dp->pid);
        } else {
            return -ENOENT;
        }
    } else {
        if (!dp || dp->type != PF_ROOT || dp->pid != 0)
            return -ENOENT;
        if (!procfs_root_file_type(type))
            return -ENOENT;
        child = new_entry(name, type, 0);
    }
    if (!child) return -ENOMEM;

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) { kfree(child); return -ENOMEM; }
    memset(vn, 0, sizeof(*vn));
    vn->ino = (uint64_t)(uintptr_t)child;
    vn->type = fd_symlink ? VFS_FT_SYMLINK :
               ((type == PF_ROOT && is_pid_str(name)) || type == PF_SELF ||
                type == PF_SYS || type == PF_SYS_FS || type == PF_SYS_KERNEL ||
                type == PF_SYS_NET || type == PF_SYS_VM ||
                type == PF_SYS_FS_INOTIFY || type == PF_NET ||
                type == PF_A20 || type == PF_PID_FD ||
                type == PF_PID_NS || type == PF_PID_FDINFO) ?
               VFS_FT_DIR : VFS_FT_REGULAR;
    vn->mode = vn->type == VFS_FT_SYMLINK ? (S_IFLNK | 0777) :
               (vn->type == VFS_FT_DIR ? (S_IFDIR | 0555) : (S_IFREG | 0444));
    if (type == PF_A20_DRIVER_LIFECYCLE)
        vn->mode = S_IFREG | 0200;
    else if (type == PF_PID_OOM_SCORE_ADJ ||
        type == PF_SYS_FS_PIPE_MAX_SIZE || type == PF_SYS_FS_LEASE_BREAK_TIME ||
        type == PF_SYS_KERNEL_SCHED_AUTOGROUP ||
        type == PF_SYS_KERNEL_CORE_PATTERN ||
        type == PF_SYS_VM_DROP_CACHES ||
        type == PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS ||
        type == PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES)
        vn->mode = S_IFREG | 0644;
    vnode_ref_init(vn, 1);
    vn->parent = dir;
    vnode_get(dir);
    vn->mnt = dir->mnt;
    vn->ops = dir->ops;

    procfs_meta_t *meta = procfs_meta_create(child->type, child->pid,
                                              fd_entry);
    if (!meta) {
        vnode_put(dir);
        kfree(vn);
        kfree(child);
        return -ENOMEM;
    }
    vn->size = meta->content_len;
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

static int procfs_readlink(vnode_t *vn, char *buf, size_t sz)
{
    if (!vn || !buf || sz == 0)
        return -EINVAL;
    procfs_meta_t *meta = (procfs_meta_t *)vn->fs_data;
    pf_entry_t *entry = (pf_entry_t *)(uintptr_t)vn->ino;
    if (!meta || !entry || meta->type != PF_PID_FD)
        return -EINVAL;

    int fd;
    if (parse_fd_name(entry->name, &fd) < 0)
        return -ENOENT;
    int pid = meta->pid;
    if (pid == -1) {
        task_t *cur = proc_current();
        pid = cur ? cur->pid : 0;
    }
    task_t *task = proc_find_get(pid);
    if (task && !proc_task_may_access(proc_current(), task)) {
        proc_put(task);
        return -EACCES;
    }
    int gfd = -1;
    vfile_t *target = task ?
        fdtable_get_file_ref(task, fd, &gfd, NULL) : NULL;
    if (!target) {
        proc_put(task);
        return -ENOENT;
    }
    if (!target->path[0]) {
        vfs_put_file_ref(gfd, target);
        proc_put(task);
        return -ENOENT;
    }
    size_t len = strlen(target->path);
    if (len > sz)
        len = sz;
    memcpy(buf, target->path, len);
    vfs_put_file_ref(gfd, target);
    proc_put(task);
    return (int)len;
}

// procfs 的 release 操作（释放 vnode）
static void procfs_release(vnode_t *vn) {
    if (vn->fs_data) kfree(vn->fs_data);
    if (vn->ino) kfree((void *)(uintptr_t)vn->ino);
    vnode_put(vn->parent);
    kfree(vn);
}

static vfile_t *procfs_open_vnode(vnode_t *vn, int flags);

// procfs vnode 操作表
static vnode_ops_t g_procfs_vnode_ops = {
    .lookup  = procfs_lookup,
    .readlink = procfs_readlink,
    .stat    = procfs_stat,
    .open    = procfs_open_vnode,
    .release = procfs_release,
};

// procfs 的 read 操作（读取文件内容）
static int procfs_fread(vfile_t *vf, char *buf, size_t count) {
    if (!vf || !vf->priv) return -EBADF;
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    if (p->type == PF_PID_PAGEMAP) {
        task_t *t = proc_find_get(p->pid);
        if (!t) return -ESRCH;
        if (!t->mm || !t->mm->pgdir) {
            proc_put(t);
            return 0;
        }

        size_t read_bytes = 0;
        while (read_bytes < count && vf->offset < p->content_len) {
            uint64_t entry_idx = vf->offset / 8;
            uint64_t entry_offset = vf->offset % 8;

            uint64_t va = entry_idx * PAGE_SIZE;
            uint64_t entry_val = mm_pagemap_entry(t->mm->pgdir, va);

            size_t chunk = 8 - entry_offset;
            if (chunk > count - read_bytes) {
                chunk = count - read_bytes;
            }
            if (chunk > p->content_len - vf->offset) {
                chunk = p->content_len - vf->offset;
            }

            char *entry_bytes = (char *)&entry_val;
            memcpy(buf + read_bytes, entry_bytes + entry_offset, chunk);

            read_bytes += chunk;
            vf->offset += chunk;
        }
        proc_put(t);
        return (int)read_bytes;
    }
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
        task_t *t = proc_find_get(p->pid);
        task_t *target = t ? t : proc_current();
        if (target) target->policy.oom_score_adj = value;
        proc_put(t);
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
    if (p->type == PF_SYS_KERNEL_SCHED_AUTOGROUP) {
        char tmp[32];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        int value = atoi(tmp);
        if (value != 0 && value != 1)
            return -EINVAL;
        return (int)count;
    }
    if (p->type == PF_SYS_KERNEL_CORE_PATTERN ||
        p->type == PF_SYS_KERNEL_IO_URING_DISABLED ||
        p->type == PF_SYS_VM_DROP_CACHES ||
        p->type == PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS ||
        p->type == PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES) {
        return (int)count;
    }
#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
    if (p->type == PF_A20_DRIVER_LIFECYCLE) {
        /* Permission check: require CAP_SYS_ADMIN (root has all caps). */
        task_t *cur = proc_current();
        if (!proc_has_cap(cur, CAP_SYS_ADMIN))
            return -EPERM;
        driver_lifecycle_test_run();
        return (int)count;
    }
#endif
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

static int procfs_fd_readdir(vfile_t *vf, procfs_priv_t *p,
                             void *dirp, size_t count)
{
    task_t *task = proc_find_get(p->pid);
    if (!task)
        return -ENOENT;
    if (!proc_task_may_access(proc_current(), task)) {
        proc_put(task);
        return -EACCES;
    }

    int cursor = (int)vf->offset;
    size_t total = 0;
    char *out = (char *)dirp;

    while (total < count) {
        const char *name;
        char fd_name[16];
        int fd = -1;
        int next_cursor;

        if (cursor == 0) {
            name = ".";
            next_cursor = 1;
        } else if (cursor == 1) {
            name = "..";
            next_cursor = 2;
        } else {
            fd = cursor - 2;
            while (fd < MAX_FILES) {
                int gfd = -1;
                vfile_t *target = fdtable_get_file_ref(task, fd, &gfd, NULL);
                if (target) {
                    vfs_put_file_ref(gfd, target);
                    break;
                }
                fd++;
            }
            if (fd >= MAX_FILES)
                break;
            snprintf(fd_name, sizeof(fd_name), "%d", fd);
            name = fd_name;
            next_cursor = fd + 3;
        }

        size_t namelen = strlen(name);
        size_t reclen = (offsetof(vfs_dirent64_t, d_name) + namelen + 1 + 7) & ~7UL;
        if (total + reclen > count)
            break;

        vfs_dirent64_t *d = (vfs_dirent64_t *)(out + total);
        d->d_ino = fd >= 0 ? (uint64_t)fd + 1 : (uint64_t)cursor + 1;
        d->d_off = next_cursor;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = fd < 0 ? 4 : (p->type == PF_PID_FD ? 10 : 8);
        memcpy(d->d_name, name, namelen + 1);

        total += reclen;
        cursor = next_cursor;
    }

    vf->offset = (size_t)cursor;
    proc_put(task);
    return (int)total;
}

// procfs 的 readdir 操作（读取目录项）
static int procfs_freaddir(vfile_t *vf, void *dirp, size_t count) {
    static const char *root_entries[] = {
        ".", "..", "meminfo", "version", "uptime", "cmdline",
        "cpuinfo", "mounts", "self", "loadavg", "net", "config.gz",
        "filesystems", "cgroups", "swaps", "interrupts", "pidmap", "sys", "a20", NULL
    };
    static const char *pid_entries[] = {
        ".", "..", "stat", "status", "statm", "maps", "smaps",
        "oom_score", "oom_score_adj", "cgroup", "cmdline", "comm", "exe", "cwd",
        "fd", "environ", "io", "loginuid", "sessionid", "ns", "fdinfo",
        "mountinfo", "mounts", "pagemap", NULL
    };
    static const char *sys_entries[] = {
        ".", "..", "fs", "kernel", "vm", "net", NULL
    };
    static const char *sys_fs_entries[] = {
        ".", "..", "pipe-max-size", "lease-break-time", "inotify", NULL
    };
    static const char *sys_kernel_entries[] = {
        ".", "..", "osrelease", "pid_max", "pidmap", "tainted", "sched_autogroup_enabled",
        "core_pattern", "io_uring_disabled", NULL
    };
    static const char *sys_net_entries[] = {
        ".", "..", NULL
    };
    static const char *net_entries[] = {
        ".", "..", "status", "config", NULL
    };
    static const char *sys_vm_entries[] = {
        ".", "..", "drop_caches", NULL
    };
    static const char *sys_fs_inotify_entries[] = {
        ".", "..", "max_queued_events", "max_user_instances", NULL
    };
    static const char *a20_entries[] = {
        ".", "..", "bcache", "page_cache", "oom", "task_lifetime",
        "driver_lifecycle", NULL
    };
    static const char *ns_entries[] = {
        ".", "..", "pid", "uts", "user", "ipc", "mnt", "net", "cgroup", NULL
    };
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    if (p && (p->type == PF_PID_FD || p->type == PF_PID_FDINFO))
        return procfs_fd_readdir(vf, p, dirp, count);

    const char **entries = root_entries;
    int is_root = (p && p->type == PF_ROOT && p->pid == 0);
    if (p && p->type == PF_ROOT && (p->pid > 0 || p->pid == -1))
        entries = pid_entries;
    else if (p && p->type == PF_SYS)
        entries = sys_entries;
    else if (p && p->type == PF_SYS_FS)
        entries = sys_fs_entries;
    else if (p && p->type == PF_SYS_KERNEL)
        entries = sys_kernel_entries;
    else if (p && p->type == PF_SYS_NET)
        entries = sys_net_entries;
    else if (p && p->type == PF_NET)
        entries = net_entries;
    else if (p && p->type == PF_SYS_VM)
        entries = sys_vm_entries;
    else if (p && p->type == PF_SYS_FS_INOTIFY)
        entries = sys_fs_inotify_entries;
    else if (p && p->type == PF_A20)
        entries = a20_entries;
    else if (p && p->type == PF_PID_NS)
        entries = ns_entries;
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
                    if (t->state == PROC_UNUSED || t->pid <= 0)
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
        if (p && p->type == PF_SYS && strcmp(name, "fs") == 0)
            is_dir = 1;
        if (p && p->type == PF_SYS && strcmp(name, "kernel") == 0)
            is_dir = 1;
        if (p && p->type == PF_SYS && strcmp(name, "vm") == 0)
            is_dir = 1;
        if (p && p->type == PF_SYS && strcmp(name, "net") == 0)
            is_dir = 1;
        if (p && p->type == PF_SYS_FS && strcmp(name, "inotify") == 0)
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
    if (vf && vf->priv) {
        procfs_priv_t *p = (procfs_priv_t *)vf->priv;
        kfree(p->content);
        kfree(p);
        vf->priv = NULL;
    }
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

    procfs_meta_t *meta = procfs_meta_create(PF_ROOT, 0, -1);
    root->fs_data = meta;
    return root;
}

// 打开 procfs vnode
static vfile_t *procfs_open_vnode(vnode_t *vn, int flags) {
    vfile_t *vf = vfile_alloc();
    if (!vf) return NULL;
    vf->vnode = vn;
    vf->flags = flags;
    vf->ops = &g_procfs_fops;
    vfile_ref_init(vf, 1);

    procfs_meta_t *meta = (procfs_meta_t *)vn->fs_data;
    procfs_priv_t *priv = procfs_priv_create(meta ? meta->type : PF_ROOT,
                                              meta ? meta->pid : 0,
                                              meta ? meta->fd : -1);
    if (!priv) { vfile_free(vf); return NULL; }
    vnode_get(vn);
    vf->priv = priv;
    return vf;
}
