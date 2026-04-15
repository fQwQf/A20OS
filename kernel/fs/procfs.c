/*
 * A20OS — procfs: Virtual /proc filesystem
 *
 * Provides process and system information via synthetic files.
 * Entries are generated on-demand during lookup and read.
 */

#include "procfs.h"
#include "proc.h"
#include "mm.h"
#include "timer.h"
#include "string.h"
#include "stdio.h"

extern task_t *proc_current(void);
extern task_t *proc_find(int pid);
extern size_t  frame_free_count(void);

typedef enum {
    PF_ROOT,
    PF_MEMINFO,
    PF_VERSION,
    PF_UPTIME,
    PF_CMDLINE,
    PF_CPUINFO,
    PF_MOUNTS,
    PF_LOADAVG,
    PF_PID_STAT,
    PF_PID_STATUS,
    PF_SELF,
    PF_FSTYPE,
} pf_type_t;

typedef struct pf_entry {
    char name[32];
    pf_type_t type;
    int pid;
    struct pf_entry *next;
} pf_entry_t;

static pf_entry_t *new_entry(const char *name, pf_type_t type, int pid) {
    pf_entry_t *e = (pf_entry_t *)kmalloc(sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->type = type;
    e->pid = pid;
    return e;
}

static void free_entries(pf_entry_t *e) {
    while (e) {
        pf_entry_t *n = e->next;
        kfree(e);
        e = n;
    }
}

static int is_pid_str(const char *s) {
    if (!s || !*s) return 0;
    while (*s) { if (*s < '0' || *s > '9') return 0; s++; }
    return 1;
}

static int generate_content(pf_type_t type, int pid, char *buf, size_t bufsz) {
    buf[0] = '\0';
    switch (type) {
    case PF_MEMINFO: {
        size_t free_frames = frame_free_count();
        size_t total_kb = (PHYS_MEMORY_END - PHYS_MEMORY_BASE) / 1024;
        size_t free_kb = free_frames * PAGE_SIZE / 1024;
        snprintf(buf, bufsz,
            "MemTotal:       %lu kB\n"
            "MemFree:        %lu kB\n"
            "MemAvailable:   %lu kB\n"
            "Buffers:           0 kB\n"
            "Cached:            0 kB\n",
            (unsigned long)total_kb, (unsigned long)free_kb, (unsigned long)free_kb);
        break;
    }
    case PF_VERSION:
        snprintf(buf, bufsz, "A20OS version 0.1 (riscv64)\n");
        break;
    case PF_UPTIME: {
        uint64_t ticks = timer_get_ticks();
        uint64_t sec = ticks / TICKS_PER_SEC;
        uint64_t frac = (ticks % TICKS_PER_SEC) * 100 / TICKS_PER_SEC;
        snprintf(buf, bufsz, "%lu.%02lu\n", (unsigned long)sec, (unsigned long)frac);
        break;
    }
    case PF_CMDLINE:
        snprintf(buf, bufsz, "console=ttyS0\n");
        break;
    case PF_CPUINFO:
        snprintf(buf, bufsz,
            "processor\t: 0\n"
            "hart\t\t: 0\n"
            "isa\t\t: rv64gc\n"
            "mmu\t\t: sv39\n\n");
        break;
    case PF_MOUNTS:
        snprintf(buf, bufsz,
            "none / ramfs rw 0 0\n"
            "/dev/vda /mnt vfat rw 0 0\n"
            "none /proc proc rw 0 0\n");
        break;
    case PF_LOADAVG:
        snprintf(buf, bufsz, "0.00 0.00 0.00 1/64 1\n");
        break;
    case PF_PID_STAT: {
        task_t *t = proc_find(pid);
        if (!t) { snprintf(buf, bufsz, "%d (unknown) S 0 0\n", pid); break; }
        snprintf(buf, bufsz,
            "%d (%s) S %d %d %d 0 0 0 0 0 0 0 0 %lu 0\n",
            t->pid, t->name, t->ppid, t->pgid, t->sid,
            (unsigned long)t->total_time);
        break;
    }
    case PF_PID_STATUS: {
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
    case PF_SELF: {
        task_t *t = proc_current();
        snprintf(buf, bufsz, "%d\n", t ? t->pid : 0);
        break;
    }
    case PF_FSTYPE:
        snprintf(buf, bufsz, "nodev\tproc\n\text4\n\tvfat\n\tramfs\n\ttmpfs\n");
        break;
    default:
        break;
    }
    return (int)strlen(buf);
}

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
    if (strcmp(name, "filesystems") == 0) return PF_FSTYPE;
    if (is_pid_str(name)) {
        *out_pid = atoi(name);
        return PF_ROOT;
    }
    if (strcmp(name, "stat") == 0) return PF_PID_STAT;
    if (strcmp(name, "status") == 0) return PF_PID_STATUS;
    return PF_ROOT;
}

typedef struct {
    pf_type_t type;
    int pid;
    char content[4096];
    size_t content_len;
} procfs_priv_t;

static procfs_priv_t *procfs_priv_create(pf_type_t type, int pid) {
    procfs_priv_t *p = (procfs_priv_t *)kmalloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->type = type;
    p->pid = pid;
    p->content_len = (size_t)generate_content(type, pid, p->content, sizeof(p->content));
    return p;
}

static int procfs_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    if (!name || !*name) return -ENOENT;

    int pid = 0;
    pf_type_t type = name_to_type(name, &pid);

    pf_entry_t *child = NULL;
    if (is_pid_str(name)) {
        child = new_entry(name, PF_ROOT, pid);
    } else if (type == PF_PID_STAT || type == PF_PID_STATUS) {
        procfs_priv_t *dp = (procfs_priv_t *)dir->fs_data;
        child = new_entry(name, type, dp ? dp->pid : 0);
    } else {
        child = new_entry(name, type, 0);
    }
    if (!child) return -ENOMEM;

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) { kfree(child); return -ENOMEM; }
    memset(vn, 0, sizeof(*vn));
    vn->ino = (uint64_t)(uintptr_t)child;
    vn->type = (type == PF_ROOT && is_pid_str(name)) ? VFS_FT_DIR : VFS_FT_REGULAR;
    vn->mode = (vn->type == VFS_FT_DIR) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    vn->ref_count = 1;
    vn->parent = dir;
    vn->ops = dir->ops;

    procfs_priv_t *priv = procfs_priv_create(child->type, child->pid);
    if (priv) vn->size = priv->content_len;
    vn->fs_data = priv;

    *out = vn;
    return 0;
}

static int procfs_stat(vnode_t *vn, kstat_t *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino = vn->ino;
    st->st_mode = vn->mode;
    st->st_size = vn->size;
    st->st_nlink = 1;
    return 0;
}

static void procfs_release(vnode_t *vn) {
    if (vn->fs_data) kfree(vn->fs_data);
    if (vn->ino) kfree((void *)(uintptr_t)vn->ino);
    kfree(vn);
}

static vnode_ops_t g_procfs_vnode_ops = {
    .lookup  = procfs_lookup,
    .stat    = procfs_stat,
    .release = procfs_release,
};

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

static int procfs_freaddir(vfile_t *vf, void *dirp, size_t count) {
    static const char *entries[] = {
        ".", "..", "meminfo", "version", "uptime", "cmdline",
        "cpuinfo", "mounts", "self", "loadavg", "filesystems", NULL
    };
    int idx = (int)(vf->offset);
    size_t total = 0;
    char *out = (char *)dirp;

    while (entries[idx] && total < count) {
        size_t namelen = strlen(entries[idx]);
        size_t reclen = (sizeof(linux_dirent64_t) + namelen + 1 + 7) & ~7UL;
        if (total + reclen > count) break;

        linux_dirent64_t *d = (linux_dirent64_t *)(out + total);
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

static int procfs_fclose(vfile_t *vf) {
    if (vf && vf->priv) { kfree(vf->priv); vf->priv = NULL; }
    return 0;
}

static vfile_ops_t g_procfs_fops = {
    .read    = procfs_fread,
    .write   = NULL,
    .lseek   = procfs_flseek,
    .readdir = procfs_freaddir,
    .close   = procfs_fclose,
};

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

vfile_t *procfs_open_vnode(vnode_t *vn, int flags) {
    (void)flags;
    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!vf) return NULL;
    memset(vf, 0, sizeof(*vf));
    vf->vnode = vn;
    vf->ops = &g_procfs_fops;
    vf->ref_count = 1;

    procfs_priv_t *vn_priv = (procfs_priv_t *)vn->fs_data;
    procfs_priv_t *priv = procfs_priv_create(vn_priv ? vn_priv->type : PF_ROOT, vn_priv ? vn_priv->pid : 0);
    if (!priv) { kfree(vf); return NULL; }
    vf->priv = priv;
    return vf;
}
