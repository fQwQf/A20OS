#include "proc/resource_ns.h"
/*
 * A20OS — procfs: Virtual /proc filesystem
 *
 * Provides process and system information via synthetic files.
 * Entries are generated on-demand during lookup and read.
 */

#include "fs/procfs.h"
#include "fs/procfs_state.h"
#include "fs/procfs_node_state.h"
#include "fs/ext4_journal.h"
#include "net/netfilter.h"
#include "net/net_config.h"
#include "net/lwip_stack.h"
#include "fs/procfs_internal.h"
#include "drivers/bus/pci_msix.h"
#include "mm/pt.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/perf.h"
#include "core/fcntl.h"
#include "core/lock_counters.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/block_cache.h"
#include "fs/page_cache.h"
#include "fs/ext4.h"
#include "fs/uxfs.h"
#include "fs/vfs/dcache.h"
#include "fs/vfs/mntns.h"
#include "proc/pidns.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/coredump.h"
/* Unconditional on purpose: without CONFIG_XLATOR the header supplies
 * stubs, so this does not pull the channel into a cut-down build. */
#include "proc/xlator.h"
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

/* The mirrored pf_type numbering must match the real enum.  That enum is
 * flat (no member is behind a conditional), so these hold for every
 * profile that builds procfs.c. */
_Static_assert(PF_ROOT == A20_PF_ROOT, "pf_type mirror drifted: PF_ROOT");
_Static_assert(PF_MEMINFO == A20_PF_MEMINFO, "pf_type mirror drifted: PF_MEMINFO");
_Static_assert(PF_STAT == A20_PF_STAT, "pf_type mirror drifted: PF_STAT");
_Static_assert(PF_VERSION == A20_PF_VERSION, "pf_type mirror drifted: PF_VERSION");
_Static_assert(PF_UPTIME == A20_PF_UPTIME, "pf_type mirror drifted: PF_UPTIME");
_Static_assert(PF_CMDLINE == A20_PF_CMDLINE, "pf_type mirror drifted: PF_CMDLINE");
_Static_assert(PF_CPUINFO == A20_PF_CPUINFO, "pf_type mirror drifted: PF_CPUINFO");
_Static_assert(PF_MOUNTS == A20_PF_MOUNTS, "pf_type mirror drifted: PF_MOUNTS");
_Static_assert(PF_LOADAVG == A20_PF_LOADAVG, "pf_type mirror drifted: PF_LOADAVG");
_Static_assert(PF_NET == A20_PF_NET, "pf_type mirror drifted: PF_NET");
_Static_assert(PF_NET_STATUS == A20_PF_NET_STATUS, "pf_type mirror drifted: PF_NET_STATUS");
_Static_assert(PF_NET_CONFIG == A20_PF_NET_CONFIG, "pf_type mirror drifted: PF_NET_CONFIG");
_Static_assert(PF_NET_DEV == A20_PF_NET_DEV, "pf_type mirror drifted: PF_NET_DEV");
_Static_assert(PF_NET_TCP == A20_PF_NET_TCP, "pf_type mirror drifted: PF_NET_TCP");
_Static_assert(PF_NET_UDP == A20_PF_NET_UDP, "pf_type mirror drifted: PF_NET_UDP");
_Static_assert(PF_NET_TCP6 == A20_PF_NET_TCP6, "pf_type mirror drifted: PF_NET_TCP6");
_Static_assert(PF_NET_UDP6 == A20_PF_NET_UDP6, "pf_type mirror drifted: PF_NET_UDP6");
_Static_assert(PF_NET_UNIX == A20_PF_NET_UNIX, "pf_type mirror drifted: PF_NET_UNIX");
_Static_assert(PF_CONFIG_GZ == A20_PF_CONFIG_GZ, "pf_type mirror drifted: PF_CONFIG_GZ");
_Static_assert(PF_PID_STAT == A20_PF_PID_STAT, "pf_type mirror drifted: PF_PID_STAT");
_Static_assert(PF_PID_STATUS == A20_PF_PID_STATUS, "pf_type mirror drifted: PF_PID_STATUS");
_Static_assert(PF_PID_STATM == A20_PF_PID_STATM, "pf_type mirror drifted: PF_PID_STATM");
_Static_assert(PF_PID_MAPS == A20_PF_PID_MAPS, "pf_type mirror drifted: PF_PID_MAPS");
_Static_assert(PF_PID_SMAPS == A20_PF_PID_SMAPS, "pf_type mirror drifted: PF_PID_SMAPS");
_Static_assert(PF_PID_OOM_SCORE_ADJ == A20_PF_PID_OOM_SCORE_ADJ, "pf_type mirror drifted: PF_PID_OOM_SCORE_ADJ");
_Static_assert(PF_PID_OOM_SCORE == A20_PF_PID_OOM_SCORE, "pf_type mirror drifted: PF_PID_OOM_SCORE");
_Static_assert(PF_PID_CGROUP == A20_PF_PID_CGROUP, "pf_type mirror drifted: PF_PID_CGROUP");
_Static_assert(PF_PID_CMDLINE == A20_PF_PID_CMDLINE, "pf_type mirror drifted: PF_PID_CMDLINE");
_Static_assert(PF_PID_COMM == A20_PF_PID_COMM, "pf_type mirror drifted: PF_PID_COMM");
_Static_assert(PF_PID_EXE == A20_PF_PID_EXE, "pf_type mirror drifted: PF_PID_EXE");
_Static_assert(PF_PID_CWD == A20_PF_PID_CWD, "pf_type mirror drifted: PF_PID_CWD");
_Static_assert(PF_PID_FD == A20_PF_PID_FD, "pf_type mirror drifted: PF_PID_FD");
_Static_assert(PF_PID_ENVIRON == A20_PF_PID_ENVIRON, "pf_type mirror drifted: PF_PID_ENVIRON");
_Static_assert(PF_PID_AUXV == A20_PF_PID_AUXV, "pf_type mirror drifted: PF_PID_AUXV");
_Static_assert(PF_PID_IO == A20_PF_PID_IO, "pf_type mirror drifted: PF_PID_IO");
_Static_assert(PF_PID_LOGINUID == A20_PF_PID_LOGINUID, "pf_type mirror drifted: PF_PID_LOGINUID");
_Static_assert(PF_PID_SESSIONID == A20_PF_PID_SESSIONID, "pf_type mirror drifted: PF_PID_SESSIONID");
_Static_assert(PF_PID_TASK == A20_PF_PID_TASK, "pf_type mirror drifted: PF_PID_TASK");
_Static_assert(PF_PID_NS == A20_PF_PID_NS, "pf_type mirror drifted: PF_PID_NS");
_Static_assert(PF_PID_NS_PID == A20_PF_PID_NS_PID, "pf_type mirror drifted: PF_PID_NS_PID");
_Static_assert(PF_PID_NS_PID_FOR_CHILDREN == A20_PF_PID_NS_PID_FOR_CHILDREN, "pf_type mirror drifted: PF_PID_NS_PID_FOR_CHILDREN");
_Static_assert(PF_PID_NS_UTS == A20_PF_PID_NS_UTS, "pf_type mirror drifted: PF_PID_NS_UTS");
_Static_assert(PF_PID_NS_USER == A20_PF_PID_NS_USER, "pf_type mirror drifted: PF_PID_NS_USER");
_Static_assert(PF_PID_NS_IPC == A20_PF_PID_NS_IPC, "pf_type mirror drifted: PF_PID_NS_IPC");
_Static_assert(PF_PID_NS_MNT == A20_PF_PID_NS_MNT, "pf_type mirror drifted: PF_PID_NS_MNT");
_Static_assert(PF_PID_NS_NET == A20_PF_PID_NS_NET, "pf_type mirror drifted: PF_PID_NS_NET");
_Static_assert(PF_PID_NS_CGROUP == A20_PF_PID_NS_CGROUP, "pf_type mirror drifted: PF_PID_NS_CGROUP");
_Static_assert(PF_PID_FDINFO == A20_PF_PID_FDINFO, "pf_type mirror drifted: PF_PID_FDINFO");
_Static_assert(PF_PID_FDINFO_ENTRY == A20_PF_PID_FDINFO_ENTRY, "pf_type mirror drifted: PF_PID_FDINFO_ENTRY");
_Static_assert(PF_PID_MOUNTINFO == A20_PF_PID_MOUNTINFO, "pf_type mirror drifted: PF_PID_MOUNTINFO");
_Static_assert(PF_PID_PAGEMAP == A20_PF_PID_PAGEMAP, "pf_type mirror drifted: PF_PID_PAGEMAP");
_Static_assert(PF_SYS == A20_PF_SYS, "pf_type mirror drifted: PF_SYS");
_Static_assert(PF_SYS_FS == A20_PF_SYS_FS, "pf_type mirror drifted: PF_SYS_FS");
_Static_assert(PF_SYS_FS_PIPE_MAX_SIZE == A20_PF_SYS_FS_PIPE_MAX_SIZE, "pf_type mirror drifted: PF_SYS_FS_PIPE_MAX_SIZE");
_Static_assert(PF_SYS_FS_LEASE_BREAK_TIME == A20_PF_SYS_FS_LEASE_BREAK_TIME, "pf_type mirror drifted: PF_SYS_FS_LEASE_BREAK_TIME");
_Static_assert(PF_SYS_KERNEL == A20_PF_SYS_KERNEL, "pf_type mirror drifted: PF_SYS_KERNEL");
_Static_assert(PF_SYS_KERNEL_OSRELEASE == A20_PF_SYS_KERNEL_OSRELEASE, "pf_type mirror drifted: PF_SYS_KERNEL_OSRELEASE");
_Static_assert(PF_SYS_KERNEL_PID_MAX == A20_PF_SYS_KERNEL_PID_MAX, "pf_type mirror drifted: PF_SYS_KERNEL_PID_MAX");
_Static_assert(PF_SYS_KERNEL_PIDMAP == A20_PF_SYS_KERNEL_PIDMAP, "pf_type mirror drifted: PF_SYS_KERNEL_PIDMAP");
_Static_assert(PF_SYS_KERNEL_TAINTED == A20_PF_SYS_KERNEL_TAINTED, "pf_type mirror drifted: PF_SYS_KERNEL_TAINTED");
_Static_assert(PF_SYS_KERNEL_SCHED_AUTOGROUP == A20_PF_SYS_KERNEL_SCHED_AUTOGROUP, "pf_type mirror drifted: PF_SYS_KERNEL_SCHED_AUTOGROUP");
_Static_assert(PF_SYS_KERNEL_CORE_PATTERN == A20_PF_SYS_KERNEL_CORE_PATTERN, "pf_type mirror drifted: PF_SYS_KERNEL_CORE_PATTERN");
_Static_assert(PF_SYS_KERNEL_IO_URING_DISABLED == A20_PF_SYS_KERNEL_IO_URING_DISABLED, "pf_type mirror drifted: PF_SYS_KERNEL_IO_URING_DISABLED");
_Static_assert(PF_SYS_KERNEL_HOSTNAME == A20_PF_SYS_KERNEL_HOSTNAME, "pf_type mirror drifted: PF_SYS_KERNEL_HOSTNAME");
_Static_assert(PF_SYS_KERNEL_DOMAINNAME == A20_PF_SYS_KERNEL_DOMAINNAME, "pf_type mirror drifted: PF_SYS_KERNEL_DOMAINNAME");
_Static_assert(PF_SYS_KERNEL_OVERFLOW_UID == A20_PF_SYS_KERNEL_OVERFLOW_UID, "pf_type mirror drifted: PF_SYS_KERNEL_OVERFLOW_UID");
_Static_assert(PF_SYS_KERNEL_OVERFLOW_GID == A20_PF_SYS_KERNEL_OVERFLOW_GID, "pf_type mirror drifted: PF_SYS_KERNEL_OVERFLOW_GID");
_Static_assert(PF_BOOT_ID == A20_PF_BOOT_ID, "pf_type mirror drifted: PF_BOOT_ID");
_Static_assert(PF_CAP_LAST_CAP == A20_PF_CAP_LAST_CAP, "pf_type mirror drifted: PF_CAP_LAST_CAP");
_Static_assert(PF_NR_OPEN == A20_PF_NR_OPEN, "pf_type mirror drifted: PF_NR_OPEN");
_Static_assert(PF_PRESSURE == A20_PF_PRESSURE, "pf_type mirror drifted: PF_PRESSURE");
_Static_assert(PF_PRESSURE_CPU == A20_PF_PRESSURE_CPU, "pf_type mirror drifted: PF_PRESSURE_CPU");
_Static_assert(PF_PRESSURE_MEM == A20_PF_PRESSURE_MEM, "pf_type mirror drifted: PF_PRESSURE_MEM");
_Static_assert(PF_PRESSURE_IO == A20_PF_PRESSURE_IO, "pf_type mirror drifted: PF_PRESSURE_IO");
_Static_assert(PF_UID_MAP == A20_PF_UID_MAP, "pf_type mirror drifted: PF_UID_MAP");
_Static_assert(PF_GID_MAP == A20_PF_GID_MAP, "pf_type mirror drifted: PF_GID_MAP");
_Static_assert(PF_SETGROUPS == A20_PF_SETGROUPS, "pf_type mirror drifted: PF_SETGROUPS");
_Static_assert(PF_SYSVIPC == A20_PF_SYSVIPC, "pf_type mirror drifted: PF_SYSVIPC");
_Static_assert(PF_DEVICES == A20_PF_DEVICES, "pf_type mirror drifted: PF_DEVICES");
_Static_assert(PF_PARTITIONS == A20_PF_PARTITIONS, "pf_type mirror drifted: PF_PARTITIONS");
_Static_assert(PF_DISKSTATS == A20_PF_DISKSTATS, "pf_type mirror drifted: PF_DISKSTATS");
_Static_assert(PF_MODULES == A20_PF_MODULES, "pf_type mirror drifted: PF_MODULES");
_Static_assert(PF_MISC == A20_PF_MISC, "pf_type mirror drifted: PF_MISC");
_Static_assert(PF_IOMEM == A20_PF_IOMEM, "pf_type mirror drifted: PF_IOMEM");
_Static_assert(PF_IOPORTS == A20_PF_IOPORTS, "pf_type mirror drifted: PF_IOPORTS");
_Static_assert(PF_SOFTIRQS == A20_PF_SOFTIRQS, "pf_type mirror drifted: PF_SOFTIRQS");
_Static_assert(PF_ROUTE == A20_PF_ROUTE, "pf_type mirror drifted: PF_ROUTE");
_Static_assert(PF_ARP == A20_PF_ARP, "pf_type mirror drifted: PF_ARP");
_Static_assert(PF_TTY == A20_PF_TTY, "pf_type mirror drifted: PF_TTY");
_Static_assert(PF_LDISCS == A20_PF_LDISCS, "pf_type mirror drifted: PF_LDISCS");
_Static_assert(PF_DRIVERS == A20_PF_DRIVERS, "pf_type mirror drifted: PF_DRIVERS");
_Static_assert(PF_PID_LIMITS == A20_PF_PID_LIMITS, "pf_type mirror drifted: PF_PID_LIMITS");
_Static_assert(PF_PID_WCHAN == A20_PF_PID_WCHAN, "pf_type mirror drifted: PF_PID_WCHAN");
_Static_assert(PF_PID_STACK == A20_PF_PID_STACK, "pf_type mirror drifted: PF_PID_STACK");
_Static_assert(PF_THREAD_SELF == A20_PF_THREAD_SELF, "pf_type mirror drifted: PF_THREAD_SELF");
_Static_assert(PF_SYS_VM == A20_PF_SYS_VM, "pf_type mirror drifted: PF_SYS_VM");
_Static_assert(PF_SYS_VM_DROP_CACHES == A20_PF_SYS_VM_DROP_CACHES, "pf_type mirror drifted: PF_SYS_VM_DROP_CACHES");
_Static_assert(PF_SYS_FS_INOTIFY == A20_PF_SYS_FS_INOTIFY, "pf_type mirror drifted: PF_SYS_FS_INOTIFY");
_Static_assert(PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS == A20_PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS, "pf_type mirror drifted: PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS");
_Static_assert(PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES == A20_PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES, "pf_type mirror drifted: PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES");
_Static_assert(PF_SYS_NET == A20_PF_SYS_NET, "pf_type mirror drifted: PF_SYS_NET");
_Static_assert(PF_INTERRUPTS == A20_PF_INTERRUPTS, "pf_type mirror drifted: PF_INTERRUPTS");
_Static_assert(PF_A20 == A20_PF_A20, "pf_type mirror drifted: PF_A20");
_Static_assert(PF_A20_BCACHE == A20_PF_A20_BCACHE, "pf_type mirror drifted: PF_A20_BCACHE");
_Static_assert(PF_A20_PAGE_CACHE == A20_PF_A20_PAGE_CACHE, "pf_type mirror drifted: PF_A20_PAGE_CACHE");
_Static_assert(PF_A20_OOM == A20_PF_A20_OOM, "pf_type mirror drifted: PF_A20_OOM");
_Static_assert(PF_A20_TASK_LIFETIME == A20_PF_A20_TASK_LIFETIME, "pf_type mirror drifted: PF_A20_TASK_LIFETIME");
_Static_assert(PF_A20_PERF == A20_PF_A20_PERF, "pf_type mirror drifted: PF_A20_PERF");
_Static_assert(PF_A20_ANONPROV == A20_PF_A20_ANONPROV, "pf_type mirror drifted: PF_A20_ANONPROV");
_Static_assert(PF_A20_HYP_SELFTEST == A20_PF_A20_HYP_SELFTEST, "pf_type mirror drifted: PF_A20_HYP_SELFTEST");
_Static_assert(PF_A20_LOCK_CONTENTION == A20_PF_A20_LOCK_CONTENTION, "pf_type mirror drifted: PF_A20_LOCK_CONTENTION");
_Static_assert(PF_A20_DRIVER_LIFECYCLE == A20_PF_A20_DRIVER_LIFECYCLE, "pf_type mirror drifted: PF_A20_DRIVER_LIFECYCLE");
_Static_assert(PF_A20_SCHED_BASE_SLICE == A20_PF_A20_SCHED_BASE_SLICE, "pf_type mirror drifted: PF_A20_SCHED_BASE_SLICE");
_Static_assert(PF_A20_XLATOR == A20_PF_A20_XLATOR, "pf_type mirror drifted: PF_A20_XLATOR");
_Static_assert(PF_A20_OBJECTS == A20_PF_A20_OBJECTS, "pf_type mirror drifted: PF_A20_OBJECTS");
_Static_assert(PF_A20_IOMMU == A20_PF_A20_IOMMU, "pf_type mirror drifted: PF_A20_IOMMU");
_Static_assert(PF_A20_NETFILTER == A20_PF_A20_NETFILTER, "pf_type mirror drifted: PF_A20_NETFILTER");
_Static_assert(PF_A20_NETMEM == A20_PF_A20_NETMEM, "pf_type mirror drifted: PF_A20_NETMEM");
_Static_assert(PF_A20_JOURNAL == A20_PF_A20_JOURNAL, "pf_type mirror drifted: PF_A20_JOURNAL");
_Static_assert(PF_A20_IRQ_AFFINITY == A20_PF_A20_IRQ_AFFINITY, "pf_type mirror drifted: PF_A20_IRQ_AFFINITY");
_Static_assert(PF_CGROUPS == A20_PF_CGROUPS, "pf_type mirror drifted: PF_CGROUPS");
_Static_assert(PF_SELF == A20_PF_SELF, "pf_type mirror drifted: PF_SELF");
_Static_assert(PF_FSTYPE == A20_PF_FSTYPE, "pf_type mirror drifted: PF_FSTYPE");
_Static_assert(PF_SWAPS == A20_PF_SWAPS, "pf_type mirror drifted: PF_SWAPS");
_Static_assert(PF_SYSRQ_TRIGGER == A20_PF_SYSRQ_TRIGGER, "pf_type mirror drifted: PF_SYSRQ_TRIGGER");
_Static_assert(A20_PF_TYPE_COUNT == PF_SYSRQ_TRIGGER + 1, "pf_type count drifted");
_Static_assert(S_IFLNK == 0120000 && S_IFDIR == 0040000 && S_IFREG == 0100000,
               "mode-bit mirror drifted");

#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
#include "drivers/core/driver_lifecycle_test.h"
#endif

extern size_t  frame_free_count(void);
extern int     vfs_mount_count(void);
extern struct mount *vfs_mount_at(int index);


typedef struct pf_entry {
    char name[32];
    pf_type_t type;
    int pid;                // process ID (only meaningful for process-related files)
    struct pf_entry *next;
} pf_entry_t;

int g_procfs_pipe_max_size = 1048576;
int g_sched_base_slice_ms = 10;
int g_procfs_lease_break_time = 45;


static pf_entry_t *new_entry(const char *name, pf_type_t type, int pid) {
    pf_entry_t *e = (pf_entry_t *)kmalloc(sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->type = type;
    e->pid = pid;
    return e;
}

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


static pf_type_t name_to_type(const char *name, int *out_pid) {
    /* The ordered /proc name table is owned by procfs_node_state.rs. */
    return (pf_type_t)a20_procfs_name_type(name, out_pid,
#ifdef CONFIG_XLATOR
                                           1
#else
                                           0
#endif
    );
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
    /* setns(2) pinning: an open /proc/<pid>/ns/mnt file holds one
     * mnt_namespace reference so the fd stays a valid setns target after
     * the target process leaves or exits the namespace. */
    void *ns_ref;
    int   ns_owner_uid;
    /* The owner's user namespace at open time, pinned.  setns(2) authority
     * over a mount or pid namespace is authority over the user namespace that
     * owns it, and the owner may exit before setns runs -- so the namespace,
     * not just the uid, has to outlive the open.  Holds the initial user
     * namespace statically for an owner outside any user namespace; userns_get
     * and userns_put both treat that one as needing no reference. */
    struct user_namespace *owner_userns;
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
    /* The length/capacity policy is owned by procfs_node_state.rs. */
    {
        procfs_content_plan_t plan;
        a20_procfs_content_plan((int)type, CONFIG_NR_CPUS, PAGE_SIZE, &plan);
        if (plan.generator == A20_PROCFS_GEN_PAGEMAP_LEN) {
            m->content_len = (size_t)plan.fixed_len;
        } else if (plan.meta_len_zero) {
            m->content_len = 0;
        } else {
            char tmp[4096];
            m->content_len = (size_t)generate_content(type, real_pid, tmp, sizeof(tmp));
        }
    }
    return m;
}

static int resource_kind(pf_type_t type) {
    return a20_procfs_resource_kind((int)type);
}

/* perf and lock formatters saturate at capacity-1. A successful open must
 * contain a whole snapshot, not a plausible-looking truncated tail. Grow only
 * these diagnostic files, keep allocation off the kernel stack, and report a
 * bounded failure rather than return partial data at the safety limit. */
#define PROCFS_DIAG_MAX_CONTENT (1024u * 1024u)
static int procfs_diag_content_alloc(pf_type_t type, int pid, size_t capacity,
                                     char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (capacity < 2 || capacity > PROCFS_DIAG_MAX_CONTENT)
        return -EINVAL;
    for (;;) {
        char *buf = kmalloc(capacity);
        if (!buf)
            return -ENOMEM;
        int len = generate_content(type, pid, buf, capacity);
        if (len < 0) {
            kfree(buf);
            return len;
        }
        if ((size_t)len < capacity - 1) {
            *out = buf;
            *out_len = (size_t)len;
            return 0;
        }
        kfree(buf);
        if (capacity == PROCFS_DIAG_MAX_CONTENT)
            return -EOVERFLOW;
        capacity = capacity > PROCFS_DIAG_MAX_CONTENT / 2
                       ? PROCFS_DIAG_MAX_CONTENT : capacity * 2;
    }
}

static procfs_priv_t *procfs_priv_create(pf_type_t type, int pid, int fd, int flags) {
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
    int write_only_diag = (flags & O_ACCMODE) == O_WRONLY &&
                          (type == PF_A20_PERF || type == PF_A20_LOCK_CONTENTION);
    if (!write_only_diag) {
        procfs_content_plan_t plan;
        a20_procfs_content_plan((int)type, CONFIG_NR_CPUS, PAGE_SIZE, &plan);
        if (plan.generator == A20_PROCFS_GEN_PAGEMAP_LEN) {
            p->content_len = (size_t)plan.fixed_len;
        } else if (plan.generator == A20_PROCFS_GEN_MAPS_ALLOC) {
            int ret = generate_pid_maps_alloc(real_pid, type == PF_PID_SMAPS,
                                              &p->content, &p->content_len);
            if (ret < 0) {
                task_t *cur = proc_current();
                if (cur)
                    cur->vfs_open_errno = ret;
                kfree(p);
                return NULL;
            }
        } else if (plan.generator == A20_PROCFS_GEN_GROW_CONTENT) {
            int ret = procfs_diag_content_alloc(type, real_pid, plan.capacity,
                                                &p->content, &p->content_len);
            if (ret < 0) {
                task_t *cur = proc_current();
                if (cur)
                    cur->vfs_open_errno = ret;
                kfree(p);
                return NULL;
            }
        } else {
            size_t capacity = plan.capacity;
            p->content = kmalloc(capacity);
            if (!p->content) {
                kfree(p);
                return NULL;
            }
            int len = plan.generator == A20_PROCFS_GEN_FDINFO ?
                generate_pid_fdinfo(real_pid, fd, p->content, capacity) :
                generate_content(type, real_pid, p->content, capacity);
            if (len < 0) {
                kfree(p->content);
                kfree(p);
                return NULL;
            }
            p->content_len = (size_t)len;
        }
    }
    if (type == PF_PID_NS_MNT || type == PF_PID_NS_PID ||
        type == PF_PID_NS_PID_FOR_CHILDREN || type == PF_PID_NS_USER || resource_kind(type)) {
        /* Pin the target's namespace for setns(2); released from
         * procfs_fclose().  Done last so the failure paths above have no
         * reference to drop.  ns_ref is void* so one field serves all three
         * namespace kinds -- procfs_ns_file_*_get() is what tells them
         * apart, and it checks the pf_type stored alongside. */
        task_t *target = proc_find_get(real_pid);
        if (target) {
            if (resource_kind(type))
                p->ns_ref = resource_ns_task_get(target, resource_kind(type));
            else if (type == PF_PID_NS_MNT)
                p->ns_ref = mntns_task_get(target);
            else if (type == PF_PID_NS_PID)
                p->ns_ref = pidns_task_get(target);
            else if (type == PF_PID_NS_USER)
                p->ns_ref = userns_task_get(target);
            else {
                pid_namespace_t *fc = (pid_namespace_t *)__atomic_load_n(
                    &target->pid_ns_for_children, __ATOMIC_ACQUIRE);
                p->ns_ref = pidns_get(fc);
            }
            p->ns_owner_uid = target->cred.uid;
            p->owner_userns = userns_task_get(target);
            proc_put(target);
        }
    }
    return p;
}

/* Answer the node table's task/fdtable questions.  The table itself, and the
 * order in which these are asked, live in procfs_node_state.rs; this is the
 * mechanical lookup half. */
static int procfs_node_probe(void *ctx, uint32_t which, int64_t arg, int64_t *out)
{
    vnode_t *dir = (vnode_t *)ctx;
    procfs_meta_t *dp = (procfs_meta_t *)dir->fs_data;
    if (!dp)
        return -ENOENT;
    *out = 0;
    if (which == A20_PROCFS_PROBE_FD_EXISTS) {
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
        vfile_t *target = task ? fdtable_get_file_ref(task, (int)arg, NULL) : NULL;
        proc_put(task);
        if (!target)
            return 0;
        vfs_put_file(target);
        *out = 1;
        return 0;
    }
    if (which == A20_PROCFS_PROBE_PID_GLOBAL) {
        task_t *task = proc_find_get_user((int)arg);
        if (!task)
            return 0;
        *out = task->pid;
        proc_put(task);
        return 0;
    }
    if (which == A20_PROCFS_PROBE_THREAD_OF_OWNER) {
        int owner_pid = dp->pid == -1 ? proc_current()->pid : dp->pid;
        task_t *owner = proc_find_get(owner_pid);
        task_t *thread = proc_find_get_user((int)arg);
        if (!owner || !thread || owner->tgid != thread->tgid) {
            proc_put(owner);
            proc_put(thread);
            return 0;
        }
        *out = thread->pid;
        proc_put(owner);
        proc_put(thread);
        return 0;
    }
    return 0;
}

static int procfs_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    if (!name || !*name) return -ENOENT;

    int pid = 0;
    pf_type_t type = name_to_type(name, &pid);
    procfs_meta_t *dp = (procfs_meta_t *)dir->fs_data;

    pf_entry_t *child = NULL;
    int fd_entry = -1;
    int fd_symlink = 0;
    pf_type_t class_type = type;
    {
        procfs_child_node_t node;
        int pr = a20_procfs_child_node(dp ? (int)dp->type : -1,
                                       dp ? dp->pid : 0, name,
                                       (int)type, pid,
#ifdef CONFIG_XLATOR
                                       1,
#else
                                       0,
#endif
                                       dir, procfs_node_probe, &node);
        if (pr < 0)
            return pr;
        type = (pf_type_t)node.ty;
        class_type = (pf_type_t)node.class_type;
        pid = node.pid;
        fd_symlink = (int)node.is_symlink;
        if (dp && (dp->type == PF_PID_FD || dp->type == PF_PID_FDINFO) &&
            parse_fd_name(name, &fd_entry) < 0)
            fd_entry = -1;
        child = new_entry(name, type, pid);
    }
    if (!child) return -ENOMEM;

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) { kfree(child); return -ENOMEM; }
    memset(vn, 0, sizeof(*vn));
    vn->ino = (uint64_t)(uintptr_t)child;
    /* The file-type, magic-link and mode decisions are owned by
     * procfs_node_state.rs. */
    {
        int vtype = 0;
        int vmode = 0;
        uint32_t link_flags = a20_procfs_node_class((int)class_type, is_pid_str(name),
                                                    fd_symlink, &vtype, &vmode);
        vn->type = vtype;
        vn->link_flags |= link_flags;
        vn->mode = vmode;
    }
    vn->mode = vn->type == VFS_FT_SYMLINK ? (S_IFLNK | 0777) :
               (vn->type == VFS_FT_DIR ? (S_IFDIR | 0555) : (S_IFREG | 0444));
    if (type == PF_A20_DRIVER_LIFECYCLE || type == PF_SYSRQ_TRIGGER)
        vn->mode = S_IFREG | 0200;
    else if (type == PF_PID_OOM_SCORE_ADJ ||
        type == PF_A20_SCHED_BASE_SLICE || type == PF_SYS_FS_PIPE_MAX_SIZE ||
        type == PF_SYS_FS_LEASE_BREAK_TIME ||
        /* Same authority argument as the other a20 tunables: moving a
         * device interrupt between CPUs the scheduler already uses grants no
         * capability a process did not have. */
        type == PF_A20_IRQ_AFFINITY ||
#ifdef CONFIG_XLATOR
        /* 0644 like the other a20 tunables: enabling the channel grants no
         * new authority, since a process able to exec the configured
         * translator path could already exec it directly.  What governs
         * access is which paths the administrator named at boot. */
        type == PF_A20_XLATOR ||
#endif
        type == PF_SYS_KERNEL_OVERFLOW_UID ||
        type == PF_SYS_KERNEL_OVERFLOW_GID ||
        type == PF_SYS_KERNEL_SCHED_AUTOGROUP ||
        type == PF_SYS_KERNEL_CORE_PATTERN ||
        type == PF_SYS_VM_DROP_CACHES ||
        type == PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS ||
        type == PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES ||
        type == PF_UID_MAP || type == PF_GID_MAP || type == PF_SETGROUPS)
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
    if (!meta || !entry)
        return -EINVAL;

    /* Linux magic symlinks: musl readlinks /proc/self/exe to expand $ORIGIN
     * in RPATH (Alpine's openjdk relies on it). */
    if (meta->type == PF_PID_EXE || meta->type == PF_PID_CWD) {
        int exe_pid = meta->pid;
        if (exe_pid == -1) {
            task_t *cur = proc_current();
            exe_pid = cur ? cur->pid : 0;
        }
        task_t *exe_task = proc_find_get(exe_pid);
        if (!exe_task)
            return -ENOENT;
        if (!proc_task_may_access(proc_current(), exe_task)) {
            proc_put(exe_task);
            return -EACCES;
        }
        const char *target = meta->type == PF_PID_EXE ?
            (exe_task->exec_path[0] ? exe_task->exec_path : "/sbin/init") :
            exe_task->fs.cwd;
        /* The vnode walker follows this callback directly, bypassing the
         * readlinkat syscall's procfs fast path. Both must return the same
         * root-visible executable pathname or chrooted self-exec fails. */
        char visible_exe[MAX_PATH_LEN];
        if (meta->type == PF_PID_EXE) {
            target = vfs_chroot_visible_path(proc_current(), target, visible_exe,
                                             sizeof(visible_exe));
            if (!target) {
                proc_put(exe_task);
                return -ENAMETOOLONG;
            }
        }
        size_t tlen = strlen(target);
        if (tlen > sz)
            tlen = sz;
        memcpy(buf, target, tlen);
        proc_put(exe_task);
        return (int)tlen;
    }

    if (meta->type != PF_PID_FD)
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
    vfile_t *target = task ?
        fdtable_get_file_ref(task, fd, NULL) : NULL;
    if (!target) {
        proc_put(task);
        return -ENOENT;
    }
    if (!target->path[0]) {
        vfs_put_file(target);
        proc_put(task);
        return -ENOENT;
    }
    size_t len = strlen(target->path);
    if (len > sz)
        len = sz;
    memcpy(buf, target->path, len);
    vfs_put_file(target);
    proc_put(task);
    return (int)len;
}

static void procfs_release(vnode_t *vn) {
    if (vn->fs_data) kfree(vn->fs_data);
    if (vn->ino) kfree((void *)(uintptr_t)vn->ino);
    vnode_put(vn->parent);
    kfree(vn);
}

static vfile_t *procfs_open_vnode(vnode_t *vn, int flags);

static vnode_ops_t g_procfs_vnode_ops = {
    .lookup  = procfs_lookup,
    .readlink = procfs_readlink,
    .stat    = procfs_stat,
    .open    = procfs_open_vnode,
    .release = procfs_release,
};

static int procfs_fread(vfile_t *vf, char *buf, size_t count) {
    if (!vf || !vf->priv) return -EBADF;
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    if (p->type == PF_PID_PAGEMAP) {
        task_t *t = proc_find_get(p->pid);
        if (!t) return -ESRCH;
        /* Entries are page frame numbers, so an ungated pagemap exposes any
         * process's physical map, defeating ASLR.  This node is not covered
         * by the fd/maps gates elsewhere. */
        if (!proc_task_may_access(proc_current(), t)) {
            proc_put(t);
            return -EACCES;
        }
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
    /* The offset advance and short-read/EOF decision are owned by
     * procfs_state.rs; the copy stays here. */
    uint64_t new_off = vf->offset;
    int64_t n = a20_procfs_read_plan(p->content_len, vf->offset, count, &new_off);
    if (n > 0)
        memcpy(buf, p->content + vf->offset, (size_t)n);
    vf->offset = (size_t)new_off;
    return (int)n;
}

/* Parse the body of a uid_map/gid_map write into @ext as a run of
 * (inside, outside, count) triples.  Returns the extent count, or -EINVAL.
 *
 * The whole write is parsed before any of it is applied: a map is a security
 * object, and installing the extents that happened to parse before a typo
 * would leave the process in a half-mapped namespace it did not ask for. */
static int procfs_parse_map(const char *buf, size_t count, uint32_t *ext)
{
    const char *s = buf;
    size_t left = count;
    int next = 0;
    while (left > 0) {
        /* Blank space separates lines. */
        while (left > 0 && (*s == ' ' || *s == '\t' ||
                            *s == '\n' || *s == '\r')) {
            s++; left--;
        }
        if (left == 0)
            break;
        uint32_t field[3];
        for (int i = 0; i < 3; i++) {
            /* Whitespace separates FIELDS as well as lines: "0 0 1" is the
             * canonical spelling of an extent and has to parse, so the
             * separator is consumed before each field rather than only
             * after each line. */
            while (left > 0 && (*s == ' ' || *s == '\t')) {
                s++; left--;
            }
            uint32_t v = 0;
            int digits = 0;
            while (left > 0 && *s >= '0' && *s <= '9') {
                /* Reject rather than wrap: a wrapped lower bound would map
                 * ids the writer never named. */
                if (v > (0xFFFFFFFFU - (uint32_t)(*s - '0')) / 10U)
                    return -EINVAL;
                v = v * 10U + (uint32_t)(*s - '0');
                s++; left--; digits++;
            }
            if (digits == 0)
                return -EINVAL;
            field[i] = v;
        }
        if (next >= USERNS_MAP_MAX)
            return -EINVAL;
        ext[next * 3 + 0] = field[0];
        ext[next * 3 + 1] = field[1];
        ext[next * 3 + 2] = field[2];
        next++;
    }
    if (next == 0)
        return -EINVAL;
    return next;
}

/*
 * Strip the trailing newline a shell `echo` appends, plus any other trailing
 * whitespace.  Without this every keyword command reached through `echo >` fails
 * its strcmp, because the buffer is "reset\n" rather than "reset" -- which is
 * how a reset entry point can exist and still be unreachable from a shell.
 */
static void procfs_chomp(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static int procfs_fwrite(vfile_t *vf, const char *buf, size_t count) {
    if (!vf || !vf->priv) return -EBADF;
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    if (p->type == PF_UID_MAP || p->type == PF_GID_MAP ||
        p->type == PF_SETGROUPS) {
        /* Writing the id maps is how a namespace creator tells the kernel
         * which host ids this container's 0..N are, so the permission checks
         * live in userns_write_map() rather than here: they need the namespace
         * and the caller's position in the chain, not just the bytes. */
        int owned = p->pid > 0;
        task_t *target = owned ? proc_find_get(p->pid) : proc_current();
        if (!target)
            return -ESRCH;
        user_namespace_t *ns = userns_task_own(target);
        int r;
        if (p->type == PF_SETGROUPS) {
            /* Exactly one keyword, and trailing junk is EINVAL rather than
             * silently ignored -- a typo in a security-relevant one-way switch
             * must not read as success. */
            char tmp[16];
            size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
            memcpy(tmp, buf, n);
            tmp[n] = '\0';
            char *nl = strchr(tmp, '\n');
            if (nl) *nl = '\0';
            if (strcmp(tmp, "deny") == 0)
                r = userns_write_setgroups(proc_current(), ns, 1);
            else if (strcmp(tmp, "allow") == 0)
                r = userns_write_setgroups(proc_current(), ns, 0);
            else
                r = -EINVAL;
        } else {
            uint32_t ext[USERNS_MAP_MAX * 3];
            int next = procfs_parse_map(buf, count, ext);
            if (next < 0) {
                if (owned) proc_put(target);
                return -EINVAL;
            }
            r = userns_write_map(proc_current(), ns,
                                  p->type == PF_GID_MAP, ext, next);
        }
        if (owned) proc_put(target);
        return r < 0 ? r : (int)count;
    }
    if (p->type == PF_A20_PERF) {
        char tmp[16];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        procfs_chomp(tmp);
        if (strcmp(tmp, "reset") == 0) {
            a20_perf_reset();
            return (int)count;
        }
        if (count == 0 || (buf[0] != '0' && buf[0] != '1'))
            return -EINVAL;
        __atomic_store_n(&g_a20_perf_enabled, buf[0] == '1',
                         __ATOMIC_RELEASE);
        return (int)count;
    }
    if (p->type == PF_A20_JOURNAL) {
        char tmp[64];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        while (n > 0 && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r'))
            tmp[--n] = '\0';
        if (n == 0)
            return -EINVAL;
        return ext4_journal_set_crash_point(tmp) < 0 ? -EINVAL : (int)count;
    }
    if (p->type == PF_NET_CONFIG) {
        int r = a20_net_config_write(buf, count);
        if (r < 0)
            return r;
        return (int)count;
    }
    if (p->type == PF_A20_LOCK_CONTENTION) {
        char tmp[16];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        procfs_chomp(tmp);
        if (strcmp(tmp, "reset") == 0) {
            lock_counters_reset();
            return (int)count;
        }
        return -EINVAL;
    }
    if (p->type == PF_A20_NETFILTER) {
        char tmp[192];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        procfs_chomp(tmp);
        /* Conntrack transactions own their IRQ-safe lock. Procfs must not
         * depend on the active protocol backend's synchronization. */
        if (strcmp(tmp, "reset") == 0) {
            netfilter_reset();
            netfilter_nat_reset();
            return (int)count;
        }
        if (strcmp(tmp, "flush") == 0) {
            netfilter_reset();
            return (int)count;
        }
        /* A separate verb rather than an extension of "reset": "reset" clears
         * configuration, "ctflush" throws away live flow state.  Conflating them
         * would mean a test that wants a clean NAT binding has to destroy the
         * rules it is about to exercise. */
        if (strcmp(tmp, "ctflush") == 0) {
            netfilter_conntrack_flush();
            return (int)count;
        }
        if (strcmp(tmp, "cton") == 0) {
            netfilter_conntrack_set_enabled(1);
            return (int)count;
        }
        if (strcmp(tmp, "ctoff") == 0) {
            netfilter_conntrack_set_enabled(0);
            return (int)count;
        }
        /* Idle-timeout override for the capacity gate: three milliseconds, 0
         * restoring the compiled-in default for that one.  See the declaration
         * comment in netfilter.h for why this is a runtime knob and not a
         * per-build constant. */
        if (strncmp(tmp, "cttimeout ", 10) == 0) {
            unsigned ms[3] = { 0, 0, 0 };
            const char *d = tmp + 10;
            for (int i = 0; i < 3; i++) {
                while (*d == ' ')
                    d++;
                if (*d < '0' || *d > '9')
                    return -EINVAL;
                unsigned v = 0;
                while (*d >= '0' && *d <= '9') {
                    if (v > 3600000u)
                        return -ERANGE;
                    v = v * 10 + (unsigned)(*d - '0');
                    d++;
                }
                ms[i] = v;
            }
            while (*d == ' ')
                d++;
            if (*d != '\0')
                return -EINVAL;
            netfilter_conntrack_set_timeouts(ms[0], ms[1], ms[2]);
            return (int)count;
        }
        /* Synthetic flow insertion: ctinject <src> <dst> <sport> <dport>
         * <proto> <state>, all decimal, proto 6 or 17.  Goes through the same
         * insert the packet path uses -- see netfilter.h. */
        if (strncmp(tmp, "ctinject ", 9) == 0) {
            const char *d = tmp + 9;
            char *tok[6];
            unsigned ntok = 0;
            /* The first two tokens are addresses and are parsed separately, so
             * split off six space-delimited tokens in place: `tmp` is this
             * function's own stack buffer, so NUL-ing the separators in it is
             * safe and needs no second buffer. */
            while (ntok < 6) {
                while (*d == ' ')
                    d++;
                if (*d == '\0')
                    return -EINVAL;
                tok[ntok++] = (char *)d;
                while (*d && *d != ' ')
                    d++;
                if (*d)
                    *(char *)d++ = '\0';
            }
            while (*d == ' ')
                d++;
            if (*d != '\0')
                return -EINVAL;
            uint32_t addr[2];
            for (int i = 0; i < 2; i++) {
                const char *s = tok[i];
                size_t len = 0;
                while (s[len])
                    len++;
                if (!netfilter_ipv4_from_str(s, len, &addr[i]))
                    return -EINVAL;
            }
            unsigned num[4];
            for (int i = 0; i < 4; i++) {
                const char *s = tok[2 + i];
                if (*s < '0' || *s > '9')
                    return -EINVAL;
                unsigned v = 0;
                while (*s >= '0' && *s <= '9') {
                    if (v > 65535u)
                        return -ERANGE;
                    v = v * 10 + (unsigned)(*s - '0');
                    s++;
                }
                if (*s != '\0')
                    return -EINVAL;
                num[i] = v;
            }
            int r = netfilter_conntrack_inject(addr[0], addr[1],
                                               (uint16_t)num[0],
                                               (uint16_t)num[1],
                                               (uint8_t)num[2],
                                               (uint8_t)num[3]);
            return r < 0 ? r : (int)count;
        }
        if (strncmp(tmp, "add ", 4) == 0) {
            netfilter_rule_t rule;
            int r = netfilter_parse_rule(tmp + 4, strlen(tmp + 4), &rule);
            if (r < 0)
                return r;
            int idx = netfilter_add_rule(&rule);
            if (idx < 0)
                return idx;
            return (int)count;
        }
        if (strncmp(tmp, "natadd ", 7) == 0) {
            net_nat_rule_t rule;
            int r = netfilter_nat_parse_rule(tmp + 7, strlen(tmp + 7), &rule);
            if (r < 0)
                return r;
            int idx = netfilter_nat_add_rule(&rule);
            if (idx < 0)
                return idx;
            return (int)count;
        }
        if (strncmp(tmp, "natdel ", 7) == 0) {
            const char *d = tmp + 7;
            if (*d == '\0')
                return -EINVAL;
            unsigned idx = 0;
            for (; *d; d++) {
                if (*d < '0' || *d > '9')
                    return -EINVAL;
                if (idx > (NETFILTER_MAX_NAT_RULES * 2))
                    return -ERANGE;
                idx = idx * 10 + (unsigned)(*d - '0');
            }
            return netfilter_nat_del_rule(idx) < 0 ? -EINVAL : (int)count;
        }
        if (strncmp(tmp, "del ", 4) == 0) {
            const char *d = tmp + 4;
            if (*d == '\0')
                return -EINVAL;
            unsigned idx = 0;
            for (; *d; d++) {
                if (*d < '0' || *d > '9')
                    return -EINVAL;
                if (idx > (NETFILTER_MAX_RULES * 2))
                    return -ERANGE;
                idx = idx * 10 + (unsigned)(*d - '0');
            }
            return netfilter_del_rule(idx) < 0 ? -EINVAL : (int)count;
        }
        return -EINVAL;
    }
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
    if (p->type == PF_SYS_KERNEL_OVERFLOW_UID ||
        p->type == PF_SYS_KERNEL_OVERFLOW_GID) {
        if (!count || count > 32) return -EINVAL;
        size_t n = 0;
        while (n < count && (buf[n] == ' ' || buf[n] == '\t')) n++;
        if (n == count || buf[n] < '0' || buf[n] > '9') return -EINVAL;
        unsigned value = 0;
        while (n < count && buf[n] >= '0' && buf[n] <= '9') {
            value = value * 10 + (unsigned)(buf[n++] - '0');
            if (value > 65535) return -EINVAL;
        }
        while (n < count && (buf[n] == ' ' || buf[n] == '\t' ||
                             buf[n] == '\n' || buf[n] == '\r')) n++;
        if (n != count) return -EINVAL;
        int r = userns_set_overflow_id(p->type == PF_SYS_KERNEL_OVERFLOW_GID,
                                       (int)value);
        return r < 0 ? r : (int)count;
    }
    if (p->type == PF_SYS_KERNEL_HOSTNAME ||
        p->type == PF_SYS_KERNEL_DOMAINNAME) {
        extern int linux_kernel_set_hostname(const char *name, size_t len);
        extern int linux_kernel_set_domainname(const char *name, size_t len);
        /* Strip a trailing newline from the written value. */
        size_t n = count;
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            n--;
        int r = p->type == PF_SYS_KERNEL_HOSTNAME
                    ? linux_kernel_set_hostname(buf, n)
                    : linux_kernel_set_domainname(buf, n);
        return r < 0 ? r : (int)count;
    }
    if (p->type == PF_A20_ANONPROV) {
        char tmp[32];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        int value = atoi(tmp);
        if (value < 0 || value > 65536)
            return -EINVAL;
        int r = mm_pt_set_anon_prov_max((uint32_t)value);
        return r < 0 ? r : (int)count;
    }
    /* Message-signalled vector affinity.  The write is exactly one CPU id,
     * optionally followed by whitespace, and it applies to every programmed
     * MSI-X entry in the system -- there is no per-function form here, so a
     * typo cannot leave half the vectors on one core and half on another
     * while the write reports success.  pci_msix_set_all_affinity() returns
     * the first entry's refusal, so a partial move is an error, not a
     * silent partial success. */
    if (p->type == PF_A20_IRQ_AFFINITY) {
        char tmp[32];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        /* Trim trailing newline/whitespace, then require a plain decimal
         * number: atoi() reads "1x" and "" as 0, and moving every device
         * interrupt to CPU 0 because the caller typed something odd is not a
         * failure the caller should have to notice afterwards. */
        while (n > 0 && (tmp[n - 1] == ' ' || tmp[n - 1] == '\t' ||
                         tmp[n - 1] == '\n' || tmp[n - 1] == '\r'))
            tmp[--n] = '\0';
        if (n == 0)
            return -EINVAL;
        for (size_t i = 0; i < n; i++) {
            if (tmp[i] < '0' || tmp[i] > '9')
                return -EINVAL;
        }
        int cpu = 0;
        for (size_t i = 0; i < n; i++)
            cpu = cpu * 10 + (tmp[i] - '0');
        int r = pci_msix_set_all_affinity(cpu);
        return r < 0 ? r : (int)count;
    }
#ifdef CONFIG_XLATOR
    /* Runtime switch for the foreign-architecture translation channel.
     *
     * Only the switch is writable: the translator paths stay boot-time, so
     * a process still cannot steer which binary the kernel re-execs.  This
     * is the emergency-off lever -- flipping it takes effect on the next
     * execve, while translations already running are ordinary processes and
     * are deliberately left to finish. */
    if (p->type == PF_A20_XLATOR) {
        char tmp[32];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        /* Exactly "0" or "1", optionally followed by trailing whitespace.
         * atoi() would accept "1x", "2" or "" as 0, and a silently ignored
         * typo here reads as "it worked" -- the opposite of what the admin
         * who just tried to shut the channel down needs.  The kernel has no
         * strtol(), hence the hand-rolled comparison. */
        int value;
        if (tmp[0] == '0')
            value = 0;
        else if (tmp[0] == '1')
            value = 1;
        else
            return -EINVAL;
        for (size_t i = 1; i < n; i++) {
            if (tmp[i] != ' ' && tmp[i] != '\t' && tmp[i] != '\n')
                return -EINVAL;
        }
        xlator_set_enabled(value);
        return (int)count;
    }
#endif
    if (p->type == PF_A20_SCHED_BASE_SLICE) {
        char tmp[32];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        int value = atoi(tmp);
        if (value < 1 || value > 1000)
            return -EINVAL;
        g_sched_base_slice_ms = value;
        return (int)count;
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
    if (p->type == PF_SYS_VM_DROP_CACHES) {
        char tmp[32];
        size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        int value = atoi(tmp);
        if (value < 1 || value > 3)
            return -EINVAL;

        /* Match Linux's interface: bit 0 drops clean page-cache mappings;
         * bit 1 drops dentry/inode caches.  Dirty or pinned pages stay live,
         * and callers must sync first when they need a quiescent snapshot. */
        if (value & 1)
            page_cache_drop_clean();
        if (value & 2) {
            vfs_dcache_invalidate_all();
            ext4_vnode_cache_prune_all();
            uxfs_vnode_cache_prune_all();
        }
        return (int)count;
    }
    if (p->type == PF_SYS_KERNEL_CORE_PATTERN) {
        int r = coredump_set_pattern(buf, count);
        return r < 0 ? r : (int)count;
    }
    if (p->type == PF_SYS_KERNEL_IO_URING_DISABLED ||
        p->type == PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS ||
        p->type == PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES) {
        return (int)count;
    }
    if (p->type == PF_SYSRQ_TRIGGER) {
        /*
         * Linux-compatible manual sysrq entry point.  Only the crash command
         * ('c') is implemented; it is the deliberate-panic path used to
         * exercise the panic dump.  Restricted to CAP_SYS_ADMIN, matching
         * both Linux and the driver_lifecycle test hook below.
         */
        task_t *cur = proc_current();
        if (!proc_has_cap(cur, CAP_SYS_ADMIN))
            return -EPERM;
        char cmd = 0;
        for (size_t i = 0; i < count; i++) {
            if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ')
                continue;
            cmd = buf[i];
            break;
        }
        if (cmd != 'c') {
            kerr("sysrq: unsupported command '%c' (only 'c' = crash)\n",
                 cmd ? cmd : '?');
            return -EINVAL;
        }
        kerr("sysrq: crash triggered via /proc/sysrq-trigger by pid=%d\n",
             cur ? cur->pid : -1);
        panic("sysrq: deliberate crash requested by pid=%d",
              cur ? cur->pid : -1);
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

static long procfs_flseek(vfile_t *vf, long offset, int whence) {
    if (!vf || !vf->priv) return -EBADF;
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    /* The offset arithmetic is owned by procfs_state.rs. */
    int64_t new_off = a20_procfs_seek_plan(p->content_len, vf->offset, offset,
                                           whence);
    if (new_off < 0) return (long)new_off;
    vf->offset = (size_t)new_off;
    return (long)new_off;
}

/* procfs_state.rs asks the adapter for the entry at a cursor.  The fdtable walk
 * below is the only mechanical part of an fd getdents64 batch that stays in C. */
static int procfs_dir_next_fd(void *ctx, uint64_t cursor, procfs_dir_entry_t *out)
{
    task_t *task = (task_t *)ctx;
    int fd = (int)cursor - 2;
    while (fd < MAX_FILES) {
        vfile_t *target = fdtable_get_file_ref(task, fd, NULL);
        if (target) {
            vfs_put_file(target);
            break;
        }
        fd++;
    }
    if (fd >= MAX_FILES)
        return 0;
    out->key = (uint64_t)fd;
    out->shown = (int64_t)fd;
    out->name = NULL;
    out->type_hint = 0;
    return 1;
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

    /* The cursor state machine and the dirent layout are owned by
     * procfs_state.rs; the fdtable walk is the lookup it calls back into. */
    uint64_t cursor = vf->offset;
    int64_t total = a20_procfs_readdir_plan(
        p->type == PF_PID_FD ? PROCFS_DIR_FD : PROCFS_DIR_FDINFO,
        vf->offset, count, task, procfs_dir_next_fd, dirp, &cursor);
    vf->offset = (size_t)cursor;
    proc_put(task);
    return (int)total;
}

typedef struct {
    int tgid;
} procfs_task_dir_ctx_t;

/* Thread ids are rendered in the reader's pid namespace, just like the
 * process directories.  Keep the task-list lock only while selecting an id;
 * no allocation or file operations run under it. */
static int procfs_dir_next_task(void *ctx, uint64_t cursor, procfs_dir_entry_t *out)
{
    procfs_task_dir_ctx_t *c = (procfs_task_dir_ctx_t *)ctx;
    uint64_t from = cursor - 2;
    int best = 0;
    int tid = 0;
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    pid_namespace_t *ns = pidns_current();
    for (task_t *t = proc_first_task_locked(); t;
         t = proc_next_task_locked(t)) {
        if (proc_task_state_get(t) == PROC_UNUSED || t->tgid != c->tgid ||
            !pidns_visible(ns, t) || t->pid <= 0 ||
            (uint64_t)t->pid < from)
            continue;
        if (!best || t->pid < best) {
            best = t->pid;
            tid = task_pid_nr_ns(t, ns);
        }
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    if (!best)
        return 0;
    out->key = (uint64_t)best;
    out->shown = (int64_t)tid;
    out->name = NULL;
    out->type_hint = 0;
    return 1;
}

static int procfs_task_readdir(vfile_t *vf, procfs_priv_t *p,
                               void *dirp, size_t count)
{
    task_t *owner = proc_find_get(p->pid);
    if (!owner)
        return -ENOENT;
    int tgid = owner->tgid;
    proc_put(owner);
    procfs_task_dir_ctx_t ctx = { tgid };
    uint64_t cursor = vf->offset;
    int64_t total = a20_procfs_readdir_plan(PROCFS_DIR_TASK, vf->offset, count,
                                            &ctx, procfs_dir_next_task,
                                            dirp, &cursor);
    vf->offset = (size_t)cursor;
    return (int)total;
}

typedef struct {
    const char **entries;
    int is_root;
    int static_count;
    pf_type_t type;
    char pidbuf[16];
} procfs_static_dir_ctx_t;

/* Answer "what entry is at index `cursor`" for a fixed-name or /proc root
 * directory.  The root interleaves its literal names with the visible pids,
 * which is why the name for a dynamic entry is rendered into the ctx buffer. */
static int procfs_dir_next_static(void *ctx, uint64_t cursor, procfs_dir_entry_t *out)
{
    procfs_static_dir_ctx_t *c = (procfs_static_dir_ctx_t *)ctx;
    uint64_t idx = cursor;
    const char *name = NULL;
    out->key = 0;
    out->shown = 0;
    out->name = NULL;
    out->type_hint = 0;

    if (c->is_root) {
        if (idx < (uint64_t)c->static_count) {
            name = c->entries[idx];
        } else {
            uint64_t pid_idx = idx - (uint64_t)c->static_count;
            /* Only tasks visible from the reader's own pid namespace are
             * listed, and each is listed under the id that reader would use to
             * address it. */
            uint64_t flags = spin_lock_irqsave(&tasklist_lock);
            pid_namespace_t *ns = pidns_current();
            uint64_t cur_idx = 0;
            task_t *t = NULL;
            for (task_t *it = proc_first_task_locked(); it;
                 it = proc_next_task_locked(it)) {
                if (proc_task_state_get(it) == PROC_UNUSED || it->pid <= 0)
                    continue;
                if (!pidns_visible(ns, it))
                    continue;
                if (cur_idx == pid_idx) { t = it; break; }
                cur_idx++;
            }
            if (t) {
                snprintf(c->pidbuf, sizeof(c->pidbuf), "%d",
                         task_pid_nr_ns(t, ns));
                name = c->pidbuf;
            }
            spin_unlock_irqrestore(&tasklist_lock, flags);
        }
    } else {
        name = c->entries[idx];
    }
    if (!name)
        return 0;

    int is_dir = (idx <= 1);
    if (c->is_root && is_pid_str(name))
        is_dir = 1;
    if (!c->is_root && (strcmp(name, "fd") == 0 || strcmp(name, "ns") == 0 ||
                        strcmp(name, "fdinfo") == 0 || strcmp(name, "task") == 0))
        is_dir = 1;
    if (c->type == PF_SYS && (strcmp(name, "fs") == 0 ||
                              strcmp(name, "kernel") == 0 ||
                              strcmp(name, "vm") == 0 ||
                              strcmp(name, "net") == 0))
        is_dir = 1;
    if (c->type == PF_SYS_FS && strcmp(name, "inotify") == 0)
        is_dir = 1;
    out->name = name;
    out->type_hint = is_dir ? 4 : 8; /* DT_DIR=4, DT_REG=8 per Linux getdents64 */
    return 1;
}

static int procfs_freaddir(vfile_t *vf, void *dirp, size_t count) {
    static const char *root_entries[] = {
        ".", "..", "meminfo", "stat", "version", "uptime", "cmdline",
        "cpuinfo", "mounts", "self", "thread-self", "loadavg", "net", "config.gz",
        "filesystems", "cgroups", "swaps", "interrupts", "pidmap", "sys", "a20",
        "boot_id", "cap_last_cap", "nr_open", "pressure", "uid_map", "gid_map",
        "setgroups", "sysvipc", "devices", "partitions", "diskstats", "modules",
        "misc", "iomem", "ioports", "softirqs", "route", "arp", "tty", "ldiscs",
        "drivers", "sysrq-trigger", NULL
    };
    static const char *pid_entries[] = {
        ".", "..", "stat", "status", "statm", "maps", "smaps",
        "oom_score", "oom_score_adj", "cgroup", "cmdline", "comm", "exe", "cwd",
        "fd", "environ", "auxv", "io", "loginuid", "sessionid", "task", "ns", "fdinfo",
        "mountinfo", "mounts", "pagemap", "limits", "wchan", "stack",
        /* The same three names as under /proc/, but bound to THIS task's
         * user namespace.  Writing them is how a parent installs the maps of
         * a namespace its child just created; without them a user namespace
         * could be entered but never given ids. */
        "uid_map", "gid_map", "setgroups", NULL
    };
    static const char *sys_entries[] = {
        ".", "..", "fs", "kernel", "vm", "net", NULL
    };
    static const char *sys_fs_entries[] = {
        ".", "..", "pipe-max-size", "lease-break-time", "inotify", NULL
    };
    static const char *sys_kernel_entries[] = {
        ".", "..", "osrelease", "pid_max", "pidmap", "tainted", "sched_autogroup_enabled",
        "core_pattern", "io_uring_disabled", "hostname", "domainname",
        "overflowuid", "overflowgid", NULL
    };
    static const char *sys_net_entries[] = {
        ".", "..", NULL
    };
    static const char *net_entries[] = {
        ".", "..", "status", "config", "route", "arp", "dev", "tcp", "tcp6",
        "udp", "udp6", "unix", NULL
    };
    static const char *sys_vm_entries[] = {
        ".", "..", "drop_caches", NULL
    };
    static const char *sys_fs_inotify_entries[] = {
        ".", "..", "max_queued_events", "max_user_instances", NULL
    };
    static const char *a20_entries[] = {
        ".", "..", "bcache", "page_cache", "oom", "task_lifetime", "perf",
        "anonprov", "driver_lifecycle", "objects", "iommu", "netfilter",
        "netmem", "journal", "hyp_selftest", NULL
    };
    static const char *ns_entries[] = {
        ".", "..", "pid", "pid_for_children", "user", "mnt", "uts", "ipc", "net", "cgroup", NULL
    };
    static const char *pressure_entries[] = {
        ".", "..", "cpu", "memory", "io", NULL
    };
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    if (p && p->type == PF_PID_TASK)
        return procfs_task_readdir(vf, p, dirp, count);
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
    else if (p && p->type == PF_PRESSURE)
        entries = pressure_entries;
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

    /* The index cursor, the byte-offset d_off and the dirent layout are owned
     * by procfs_state.rs; this only answers what sits at an index. */
    (void)idx;
    (void)total;
    (void)out;
    {
        int static_count = 0;
        for (int i = 0; entries[i]; i++) static_count = i + 1;
        procfs_static_dir_ctx_t ctx = {
            entries, is_root, static_count,
            p ? p->type : (pf_type_t)0, {0}
        };
        uint64_t cursor = vf->offset;
        int64_t n = a20_procfs_readdir_plan(PROCFS_DIR_STATIC, vf->offset, count,
                                            &ctx, procfs_dir_next_static,
                                            dirp, &cursor);
        vf->offset = (size_t)cursor;
        return (int)n;
    }
}

static int procfs_fclose(vfile_t *vf) {
    if (vf && vf->priv) {
        procfs_priv_t *p = (procfs_priv_t *)vf->priv;
        /* ns_ref holds a mnt_namespace, a pid_namespace OR a user_namespace, so the
         * release has to match the kind.  Dispatching on the kind is not
         * optional: each module has its own statically pinned "initial"
         * namespace, and its put() treats that pointer as "nothing to free".
         * Handing a mount namespace to pidns_put() would fail that test and
         * free a static object. */
        if (p->ns_ref) {
            if (resource_kind(p->type))
                resource_ns_put(p->ns_ref);
            else if (p->type == PF_PID_NS_MNT)
                mntns_put((mnt_namespace_t *)p->ns_ref);
            else if (p->type == PF_PID_NS_USER)
                userns_put((user_namespace_t *)p->ns_ref);
            else
                pidns_put((pid_namespace_t *)p->ns_ref);
        }
        /* The owner's user namespace pin, taken for the same reason and
         * released here whatever kind of namespace file this was. */
        if (p->owner_userns)
            userns_put(p->owner_userns);
        kfree(p->content);
        kfree(p);
        vf->priv = NULL;
    }
    return 0;
}

static vfile_ops_t g_procfs_fops = {
    .read    = procfs_fread,
    .write   = procfs_fwrite,
    .lseek   = procfs_flseek,
    .readdir = procfs_freaddir,
    .close   = procfs_fclose,
};

/* Report whether a vfile belongs to procfs.  Used by the Linux ABI read/write
 * path: procfs files must not take vf->offset_lock, because rendering a file
 * (e.g. /proc/<pid>/fdinfo) can lock another task's vfile offset_lock and a
 * procfs read under the shared-offset mutex could recurse on the same lock. */
int vfs_is_procfs_vfile(const vfile_t *vf)
{
    return vf && vf->ops == &g_procfs_fops;
}

/* setns(2) support: classify an open vfile as a /proc/<pid>/ns/<type> file.
 * Returns one of the PROCNS_* kind constants, or -1 when the file is not a
 * procfs namespace file. */
int procfs_ns_file_kind(const vfile_t *vf)
{
    if (!vfs_is_procfs_vfile(vf) || !vf->priv)
        return -1;
    switch (((const procfs_priv_t *)vf->priv)->type) {
    case PF_PID_NS_MNT:    return PROCNS_MNT;
    case PF_PID_NS_PID:
    case PF_PID_NS_PID_FOR_CHILDREN: return PROCNS_PID;
    case PF_PID_NS_UTS:    return PROCNS_UTS;
    case PF_PID_NS_USER:   return PROCNS_USER;
    case PF_PID_NS_IPC:    return PROCNS_IPC;
    case PF_PID_NS_NET:    return PROCNS_NET;
    case PF_PID_NS_CGROUP: return PROCNS_CGROUP;
    default:               return -1;
    }
}

/* setns(2) support: for an open /proc/<pid>/ns/mnt file, return the pinned
 * mount namespace with an extra reference (caller mntns_put()s it) and the
 * namespace owner's uid recorded at open time.  Returns NULL when the
 * namespace could not be pinned (target already gone at open). */
pid_namespace_t *procfs_ns_file_pidns_get(const vfile_t *vf, int *out_owner_uid,
                                         user_namespace_t **out_owner_userns)
{
    if (!vfs_is_procfs_vfile(vf) || !vf->priv)
        return NULL;
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    if ((p->type != PF_PID_NS_PID && p->type != PF_PID_NS_PID_FOR_CHILDREN) ||
        !p->ns_ref)
        return NULL;
    pid_namespace_t *ns = (pid_namespace_t *)p->ns_ref;
    if (ns != pidns_init_ns())
        refcount_inc(&ns->refs);
    if (out_owner_uid)
        *out_owner_uid = p->ns_owner_uid;
    if (out_owner_userns)
        *out_owner_userns = p->owner_userns ? userns_get(p->owner_userns) : NULL;
    return ns;
}

mnt_namespace_t *procfs_ns_file_mntns_get(const vfile_t *vf, int *out_owner_uid,
                                         user_namespace_t **out_owner_userns)
{
    if (!vfs_is_procfs_vfile(vf) || !vf->priv)
        return NULL;
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    if (p->type != PF_PID_NS_MNT || !p->ns_ref)
        return NULL;
    mnt_namespace_t *ns = (mnt_namespace_t *)p->ns_ref;
    refcount_inc(&ns->refs);
    if (out_owner_uid)
        *out_owner_uid = p->ns_owner_uid;
    if (out_owner_userns)
        *out_owner_userns = p->owner_userns ? userns_get(p->owner_userns) : NULL;
    return ns;
}

user_namespace_t *procfs_ns_file_userns_get(const vfile_t *vf,
                                            int *out_owner_uid)
{
    if (!vfs_is_procfs_vfile(vf) || !vf->priv)
        return NULL;
    procfs_priv_t *p = (procfs_priv_t *)vf->priv;
    if (p->type != PF_PID_NS_USER || !p->ns_ref)
        return NULL;
    user_namespace_t *ns = (user_namespace_t *)p->ns_ref;
    /* The initial namespace is statically pinned, exactly as for pidns. */
    if (ns != userns_init_ns())
        refcount_inc(&ns->refs);
    if (out_owner_uid)
        *out_owner_uid = p->ns_owner_uid;
    return ns;
}

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
                                              meta ? meta->fd : -1, flags);
    if (!priv) { vfile_free(vf); return NULL; }
    vnode_get(vn);
    vf->priv = priv;
    return vf;
}

struct resource_namespace *procfs_ns_file_resource_get(const vfile_t *vf)
{
    if (!vfs_is_procfs_vfile(vf) || !vf->priv) return NULL;
    procfs_priv_t *p = vf->priv;
    return resource_kind(p->type) ? resource_ns_get(p->ns_ref) : NULL;
}
