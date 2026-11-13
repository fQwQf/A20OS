/*
 * Host differential harness for the procfs node-table and content-plan engine
 * (PROCFS_NODE_IMPL).
 *
 * Includes the real fs/procfs_internal.h so every mirrored pf_type constant is
 * checked against the actual enum, then drives a C transcription of the
 * pre-migration tables (name_to_type, procfs_root_file_type, resource_kind,
 * procfs_lookup's node chain and classification, the content-length policy) and
 * the Rust engine (kernel/fs/procfs/procfs_node_state.rs, linked as a host
 * staticlib) over the same deterministic inputs, comparing every result.
 *
 * Exit 0 on success; on mismatch prints the input and both results, exits 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/procfs_internal.h"
#include "fs/procfs_node_state.h"
#include "net/socket.h"

/* ---- every mirrored constant must equal the real enum member ---- */
#define CHK(nm) _Static_assert(A20_##nm == nm, "mirror drifted: " #nm)
CHK(PF_ROOT); CHK(PF_MEMINFO); CHK(PF_STAT); CHK(PF_VERSION); CHK(PF_UPTIME);
CHK(PF_CMDLINE); CHK(PF_CPUINFO); CHK(PF_MOUNTS); CHK(PF_LOADAVG); CHK(PF_NET);
CHK(PF_NET_STATUS); CHK(PF_NET_CONFIG); CHK(PF_NET_DEV); CHK(PF_NET_TCP);
CHK(PF_NET_UDP); CHK(PF_NET_TCP6); CHK(PF_NET_UDP6); CHK(PF_NET_UNIX);
CHK(PF_CONFIG_GZ); CHK(PF_PID_STAT); CHK(PF_PID_STATUS); CHK(PF_PID_STATM);
CHK(PF_PID_MAPS); CHK(PF_PID_SMAPS); CHK(PF_PID_OOM_SCORE_ADJ); CHK(PF_PID_OOM_SCORE);
CHK(PF_PID_CGROUP); CHK(PF_PID_CMDLINE); CHK(PF_PID_COMM); CHK(PF_PID_EXE);
CHK(PF_PID_CWD); CHK(PF_PID_FD); CHK(PF_PID_ENVIRON); CHK(PF_PID_AUXV);
CHK(PF_PID_IO); CHK(PF_PID_LOGINUID); CHK(PF_PID_SESSIONID); CHK(PF_PID_TASK);
CHK(PF_PID_NS); CHK(PF_PID_NS_PID); CHK(PF_PID_NS_PID_FOR_CHILDREN);
CHK(PF_PID_NS_UTS); CHK(PF_PID_NS_USER); CHK(PF_PID_NS_IPC); CHK(PF_PID_NS_MNT);
CHK(PF_PID_NS_NET); CHK(PF_PID_NS_CGROUP); CHK(PF_PID_FDINFO);
CHK(PF_PID_FDINFO_ENTRY); CHK(PF_PID_MOUNTINFO); CHK(PF_PID_PAGEMAP); CHK(PF_SYS);
CHK(PF_SYS_FS); CHK(PF_SYS_FS_PIPE_MAX_SIZE); CHK(PF_SYS_FS_LEASE_BREAK_TIME);
CHK(PF_SYS_KERNEL); CHK(PF_SYS_KERNEL_OSRELEASE); CHK(PF_SYS_KERNEL_PID_MAX);
CHK(PF_SYS_KERNEL_PIDMAP); CHK(PF_SYS_KERNEL_TAINTED);
CHK(PF_SYS_KERNEL_SCHED_AUTOGROUP); CHK(PF_SYS_KERNEL_CORE_PATTERN);
CHK(PF_SYS_KERNEL_IO_URING_DISABLED); CHK(PF_SYS_KERNEL_HOSTNAME);
CHK(PF_SYS_KERNEL_DOMAINNAME); CHK(PF_SYS_KERNEL_OVERFLOW_UID);
CHK(PF_SYS_KERNEL_OVERFLOW_GID); CHK(PF_BOOT_ID); CHK(PF_CAP_LAST_CAP);
CHK(PF_NR_OPEN); CHK(PF_PRESSURE); CHK(PF_PRESSURE_CPU); CHK(PF_PRESSURE_MEM);
CHK(PF_PRESSURE_IO); CHK(PF_UID_MAP); CHK(PF_GID_MAP); CHK(PF_SETGROUPS);
CHK(PF_SYSVIPC); CHK(PF_DEVICES); CHK(PF_PARTITIONS); CHK(PF_DISKSTATS);
CHK(PF_MODULES); CHK(PF_MISC); CHK(PF_IOMEM); CHK(PF_IOPORTS); CHK(PF_SOFTIRQS);
CHK(PF_ROUTE); CHK(PF_ARP); CHK(PF_TTY); CHK(PF_LDISCS); CHK(PF_DRIVERS);
CHK(PF_PID_LIMITS); CHK(PF_PID_WCHAN); CHK(PF_PID_STACK); CHK(PF_THREAD_SELF);
CHK(PF_SYS_VM); CHK(PF_SYS_VM_DROP_CACHES); CHK(PF_SYS_FS_INOTIFY);
CHK(PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS); CHK(PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES);
CHK(PF_SYS_NET); CHK(PF_INTERRUPTS); CHK(PF_A20); CHK(PF_A20_BCACHE);
CHK(PF_A20_PAGE_CACHE); CHK(PF_A20_OOM); CHK(PF_A20_TASK_LIFETIME);
CHK(PF_A20_PERF); CHK(PF_A20_ANONPROV); CHK(PF_A20_HYP_SELFTEST);
CHK(PF_A20_LOCK_CONTENTION); CHK(PF_A20_DRIVER_LIFECYCLE);
CHK(PF_A20_SCHED_BASE_SLICE); CHK(PF_A20_XLATOR); CHK(PF_A20_OBJECTS);
CHK(PF_A20_IOMMU); CHK(PF_A20_NETFILTER); CHK(PF_A20_NETMEM); CHK(PF_A20_JOURNAL);
CHK(PF_A20_IRQ_AFFINITY); CHK(PF_CGROUPS); CHK(PF_SELF); CHK(PF_FSTYPE);
CHK(PF_SWAPS); CHK(PF_SYSRQ_TRIGGER);
_Static_assert(A20_PF_TYPE_COUNT == PF_SYSRQ_TRIGGER + 1, "count drifted");

/* ---- helpers shared by both sides ---- */
static int is_pid_str(const char *s)
{
    if (!s || !*s)
        return 0;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

static int parse_fd_name(const char *name, int *fd)
{
    if (!is_pid_str(name))
        return -1;
    *fd = atoi(name);
    return 0;
}

/* ---- C reference tables (pre-migration) ---- */

static int ref_root_file_type(int type)
{
    switch (type) {
    case PF_MEMINFO: case PF_STAT: case PF_VERSION: case PF_UPTIME: case PF_CMDLINE:
    case PF_CPUINFO: case PF_MOUNTS: case PF_LOADAVG: case PF_NET: case PF_CONFIG_GZ:
    case PF_FSTYPE: case PF_CGROUPS: case PF_SWAPS: case PF_INTERRUPTS:
    case PF_SYS_KERNEL_PIDMAP: case PF_BOOT_ID: case PF_CAP_LAST_CAP: case PF_NR_OPEN:
    case PF_PRESSURE: case PF_UID_MAP: case PF_GID_MAP: case PF_SETGROUPS:
    case PF_SYSVIPC: case PF_DEVICES: case PF_PARTITIONS: case PF_DISKSTATS:
    case PF_MODULES: case PF_MISC: case PF_IOMEM: case PF_IOPORTS: case PF_SOFTIRQS:
    case PF_ROUTE: case PF_ARP: case PF_TTY: case PF_LDISCS: case PF_DRIVERS:
    case PF_SYSRQ_TRIGGER:
        return 1;
    default:
        return 0;
    }
}

static int ref_resource_kind(int type)
{
    switch (type) {
    case PF_PID_NS_UTS: return 1;
    case PF_PID_NS_IPC: return 2;
    case PF_PID_NS_NET: return 3;
    case PF_PID_NS_CGROUP: return 4;
    default: return 0;
    }
}

static int ref_name_type(const char *name, int *out_pid, int xlator)
{
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
    if (strcmp(name, "sysrq-trigger") == 0) return PF_SYSRQ_TRIGGER;
    if (strcmp(name, "interrupts") == 0) return PF_INTERRUPTS;
    if (strcmp(name, "pidmap") == 0) return PF_SYS_KERNEL_PIDMAP;
    if (strcmp(name, "a20") == 0) return PF_A20;
    if (strcmp(name, "bcache") == 0) return PF_A20_BCACHE;
    if (strcmp(name, "anonprov") == 0) return PF_A20_ANONPROV;
    if (strcmp(name, "hyp_selftest") == 0) return PF_A20_HYP_SELFTEST;
    if (strcmp(name, "sched_base_slice") == 0) return PF_A20_SCHED_BASE_SLICE;
    if (xlator && strcmp(name, "xlator") == 0) return PF_A20_XLATOR;
    if (strcmp(name, "page_cache") == 0) return PF_A20_PAGE_CACHE;
    if (strcmp(name, "oom") == 0) return PF_A20_OOM;
    if (strcmp(name, "task_lifetime") == 0) return PF_A20_TASK_LIFETIME;
    if (strcmp(name, "perf") == 0) return PF_A20_PERF;
    if (strcmp(name, "lock_contention") == 0) return PF_A20_LOCK_CONTENTION;
    if (strcmp(name, "driver_lifecycle") == 0) return PF_A20_DRIVER_LIFECYCLE;
    if (strcmp(name, "objects") == 0) return PF_A20_OBJECTS;
    if (strcmp(name, "iommu") == 0) return PF_A20_IOMMU;
    if (strcmp(name, "irq_affinity") == 0) return PF_A20_IRQ_AFFINITY;
    if (strcmp(name, "cmdline") == 0) return PF_CMDLINE;
    if (is_pid_str(name)) { *out_pid = atoi(name); return PF_ROOT; }
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
    if (strcmp(name, "auxv") == 0) return PF_PID_AUXV;
    if (strcmp(name, "io") == 0) return PF_PID_IO;
    if (strcmp(name, "loginuid") == 0) return PF_PID_LOGINUID;
    if (strcmp(name, "sessionid") == 0) return PF_PID_SESSIONID;
    if (strcmp(name, "task") == 0) return PF_PID_TASK;
    if (strcmp(name, "ns") == 0) return PF_PID_NS;
    if (strcmp(name, "pid") == 0) return PF_PID_NS_PID;
    if (strcmp(name, "pid_for_children") == 0) return PF_PID_NS_PID_FOR_CHILDREN;
    if (strcmp(name, "uts") == 0) return PF_PID_NS_UTS;
    if (strcmp(name, "user") == 0) return PF_PID_NS_USER;
    if (strcmp(name, "ipc") == 0) return PF_PID_NS_IPC;
    if (strcmp(name, "mnt") == 0) return PF_PID_NS_MNT;
    if (strcmp(name, "net") == 0) return PF_PID_NS_NET;
    if (strcmp(name, "cgroup") == 0) return PF_PID_NS_CGROUP;
    if (strcmp(name, "fdinfo") == 0) return PF_PID_FDINFO;
    if (strcmp(name, "mountinfo") == 0) return PF_PID_MOUNTINFO;
    if (strcmp(name, "pagemap") == 0) return PF_PID_PAGEMAP;
    if (strcmp(name, "boot_id") == 0) return PF_BOOT_ID;
    if (strcmp(name, "cap_last_cap") == 0) return PF_CAP_LAST_CAP;
    if (strcmp(name, "nr_open") == 0) return PF_NR_OPEN;
    if (strcmp(name, "pressure") == 0) return PF_PRESSURE;
    if (strcmp(name, "uid_map") == 0) return PF_UID_MAP;
    if (strcmp(name, "gid_map") == 0) return PF_GID_MAP;
    if (strcmp(name, "setgroups") == 0) return PF_SETGROUPS;
    if (strcmp(name, "sysvipc") == 0) return PF_SYSVIPC;
    if (strcmp(name, "devices") == 0) return PF_DEVICES;
    if (strcmp(name, "partitions") == 0) return PF_PARTITIONS;
    if (strcmp(name, "diskstats") == 0) return PF_DISKSTATS;
    if (strcmp(name, "modules") == 0) return PF_MODULES;
    if (strcmp(name, "misc") == 0) return PF_MISC;
    if (strcmp(name, "iomem") == 0) return PF_IOMEM;
    if (strcmp(name, "ioports") == 0) return PF_IOPORTS;
    if (strcmp(name, "softirqs") == 0) return PF_SOFTIRQS;
    if (strcmp(name, "route") == 0) return PF_ROUTE;
    if (strcmp(name, "arp") == 0) return PF_ARP;
    if (strcmp(name, "tty") == 0) return PF_TTY;
    if (strcmp(name, "ldiscs") == 0) return PF_LDISCS;
    if (strcmp(name, "drivers") == 0) return PF_DRIVERS;
    if (strcmp(name, "thread-self") == 0) return PF_THREAD_SELF;
    if (strcmp(name, "limits") == 0) return PF_PID_LIMITS;
    if (strcmp(name, "wchan") == 0) return PF_PID_WCHAN;
    if (strcmp(name, "stack") == 0) return PF_PID_STACK;
    return PF_ROOT;
}

/* The probe answer table, shared so both sides see the same lookups. */
static int g_fd_ok;        /* the fd the probe reports as open */
static int g_pid_shown;    /* the shown id the probe knows */
static int g_pid_global;
static int g_thread_shown;
static int g_thread_global;
static int g_probe_deny;

static int probe(void *ctx, uint32_t which, int64_t arg, int64_t *out)
{
    (void)ctx;
    /* Only the fd probe can be refused by the real adapter (a task the caller
     * may not inspect); the pid/thread probes answer 0 instead. */
    if (g_probe_deny && which == A20_PROCFS_PROBE_FD_EXISTS)
        return -13;
    *out = 0;
    if (which == A20_PROCFS_PROBE_FD_EXISTS) {
        *out = (arg == g_fd_ok);
        return 0;
    }
    if (which == A20_PROCFS_PROBE_PID_GLOBAL) {
        if (arg == g_pid_shown)
            *out = g_pid_global;
        return 0;
    }
    if (which == A20_PROCFS_PROBE_THREAD_OF_OWNER) {
        if (arg == g_thread_shown)
            *out = g_thread_global;
        return 0;
    }
    return 0;
}

static int ref_child(int parent_type, int parent_pid, const char *name, int xlator,
                     procfs_child_node_t *out)
{
    int pid = 0;
    int type = ref_name_type(name, &pid, xlator);
    out->ty = type;
    out->pid = pid;
    out->is_symlink = 0;
    int fd_entry = -1;
    int fd_symlink = 0;
    int have = 0;
    int err = 0;
    /* The chain mirrors procfs_lookup; `have` records that a branch produced a
     * child, and `err` the errno of the branch that returned early. */
#define BR(cond, ty_, pid_, sym_)                                                  \
    do {                                                                           \
        if (!have && !err && (cond)) {                                             \
            out->ty = (ty_); out->pid = (pid_); out->is_symlink = (sym_);           \
            out->class_type = (ty_); have = 1;                                     \
        }                                                                          \
    } while (0)
#define ERR(cond, e) do { if (!have && !err && (cond)) err = (e); } while (0)
    int is_root = parent_type == PF_ROOT;
    int root_global = is_root && parent_pid == 0;
    int root_task = is_root && (parent_pid > 0 || parent_pid == -1);

    BR(root_global && !strcmp(name, "stat"), PF_STAT, 0, 0);
    BR(root_global && !strcmp(name, "sys"), PF_SYS, 0, 0);
    BR(parent_type == PF_PRESSURE && !strcmp(name, "cpu"), PF_PRESSURE_CPU, 0, 0);
    BR(parent_type == PF_PRESSURE && !strcmp(name, "memory"), PF_PRESSURE_MEM, 0, 0);
    BR(parent_type == PF_PRESSURE && !strcmp(name, "io"), PF_PRESSURE_IO, 0, 0);
    BR(parent_type == PF_SYS && !strcmp(name, "fs"), PF_SYS_FS, 0, 0);
    BR(parent_type == PF_SYS_FS && !strcmp(name, "pipe-max-size"), PF_SYS_FS_PIPE_MAX_SIZE, 0, 0);
    BR(parent_type == PF_SYS_FS && !strcmp(name, "lease-break-time"), PF_SYS_FS_LEASE_BREAK_TIME, 0, 0);
    BR(parent_type == PF_SYS && !strcmp(name, "kernel"), PF_SYS_KERNEL, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "osrelease"), PF_SYS_KERNEL_OSRELEASE, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "pid_max"), PF_SYS_KERNEL_PID_MAX, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "pidmap"), PF_SYS_KERNEL_PIDMAP, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "tainted"), PF_SYS_KERNEL_TAINTED, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "sched_autogroup_enabled"), PF_SYS_KERNEL_SCHED_AUTOGROUP, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "core_pattern"), PF_SYS_KERNEL_CORE_PATTERN, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "hostname"), PF_SYS_KERNEL_HOSTNAME, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "domainname"), PF_SYS_KERNEL_DOMAINNAME, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "overflowuid"), PF_SYS_KERNEL_OVERFLOW_UID, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "overflowgid"), PF_SYS_KERNEL_OVERFLOW_GID, 0, 0);
    BR(parent_type == PF_SYS_KERNEL && !strcmp(name, "io_uring_disabled"), PF_SYS_KERNEL_IO_URING_DISABLED, 0, 0);
    BR(parent_type == PF_SYS && !strcmp(name, "vm"), PF_SYS_VM, 0, 0);
    BR(parent_type == PF_SYS_VM && !strcmp(name, "drop_caches"), PF_SYS_VM_DROP_CACHES, 0, 0);
    BR(parent_type == PF_SYS_FS && !strcmp(name, "inotify"), PF_SYS_FS_INOTIFY, 0, 0);
    BR(parent_type == PF_SYS_FS_INOTIFY && !strcmp(name, "max_queued_events"), PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS, 0, 0);
    BR(parent_type == PF_SYS_FS_INOTIFY && !strcmp(name, "max_user_instances"), PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES, 0, 0);
    BR(parent_type == PF_SYS && !strcmp(name, "net"), PF_SYS_NET, 0, 0);
    BR(parent_type == PF_NET && !strcmp(name, "status"), PF_NET_STATUS, 0, 0);
    BR(parent_type == PF_NET && !strcmp(name, "config"), PF_NET_CONFIG, 0, 0);
    BR(parent_type == PF_NET && !strcmp(name, "route"), PF_ROUTE, 0, 0);
    BR(parent_type == PF_NET && !strcmp(name, "arp"), PF_ARP, 0, 0);
    BR(parent_type == PF_NET && !strcmp(name, "dev"), PF_NET_DEV, 0, 0);
    BR(parent_type == PF_NET && !strcmp(name, "tcp"), PF_NET_TCP, 0, 0);
    BR(parent_type == PF_NET && !strcmp(name, "udp"), PF_NET_UDP, 0, 0);
    BR(parent_type == PF_NET && !strcmp(name, "tcp6"), PF_NET_TCP6, 0, 0);
    BR(parent_type == PF_NET && !strcmp(name, "udp6"), PF_NET_UDP6, 0, 0);
    BR(parent_type == PF_NET && !strcmp(name, "unix"), PF_NET_UNIX, 0, 0);
    BR(root_global && !strcmp(name, "a20"), PF_A20, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "bcache"), PF_A20_BCACHE, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "page_cache"), PF_A20_PAGE_CACHE, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "oom"), PF_A20_OOM, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "task_lifetime"), PF_A20_TASK_LIFETIME, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "perf"), PF_A20_PERF, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "lock_contention"), PF_A20_LOCK_CONTENTION, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "driver_lifecycle"), PF_A20_DRIVER_LIFECYCLE, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "objects"), PF_A20_OBJECTS, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "iommu"), PF_A20_IOMMU, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "irq_affinity"), PF_A20_IRQ_AFFINITY, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "netfilter"), PF_A20_NETFILTER, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "netmem"), PF_A20_NETMEM, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "journal"), PF_A20_JOURNAL, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "anonprov"), PF_A20_ANONPROV, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "hyp_selftest"), PF_A20_HYP_SELFTEST, 0, 0);
    BR(parent_type == PF_A20 && !strcmp(name, "sched_base_slice"), PF_A20_SCHED_BASE_SLICE, 0, 0);
    BR(xlator && parent_type == PF_A20 && !strcmp(name, "xlator"), PF_A20_XLATOR, 0, 0);
    BR(root_global && !strcmp(name, "interrupts"), PF_INTERRUPTS, 0, 0);
    if (!have && !err && parent_type == PF_PID_FD) {
        int fd = -1;
        if (parse_fd_name(name, &fd) < 0) err = -2;
        else {
            int64_t v = 0;
            int r = probe(NULL, A20_PROCFS_PROBE_FD_EXISTS, fd, &v);
            if (r < 0) err = r;
            else if (!v) err = -2;
            else { fd_entry = fd; fd_symlink = 1; out->ty = PF_PID_FD; out->class_type = PF_PID_FD; out->pid = parent_pid; out->is_symlink = 1; have = 1; }
        }
    }
    if (!have && !err && parent_type == PF_PID_FDINFO) {
        int fd = -1;
        if (parse_fd_name(name, &fd) < 0) err = -2;
        else {
            int64_t v = 0;
            int r = probe(NULL, A20_PROCFS_PROBE_FD_EXISTS, fd, &v);
            if (r < 0) err = r;
            else if (!v) err = -2;
            else { fd_entry = fd; out->ty = PF_PID_FDINFO_ENTRY; out->class_type = PF_PID_FDINFO_ENTRY; out->pid = parent_pid; have = 1; }
        }
    }
    if (!have && !err && parent_type == PF_PID_TASK) {
        if (!is_pid_str(name)) err = -2;
        else {
            int64_t v = 0;
            probe(NULL, A20_PROCFS_PROBE_THREAD_OF_OWNER, atoi(name), &v);
            if (!v) err = -2;
            else { out->ty = PF_ROOT; out->class_type = PF_ROOT; out->pid = (int)v; have = 1; }
        }
    }
    if (!have && !err && parent_type == PF_PID_NS) {
        int t = -1;
        if (!strcmp(name, "pid")) t = PF_PID_NS_PID;
        else if (!strcmp(name, "pid_for_children")) t = PF_PID_NS_PID_FOR_CHILDREN;
        else if (!strcmp(name, "user")) t = PF_PID_NS_USER;
        else if (!strcmp(name, "mnt")) t = PF_PID_NS_MNT;
        else if (!strcmp(name, "uts")) t = PF_PID_NS_UTS;
        else if (!strcmp(name, "ipc")) t = PF_PID_NS_IPC;
        else if (!strcmp(name, "net")) t = PF_PID_NS_NET;
        else if (!strcmp(name, "cgroup")) t = PF_PID_NS_CGROUP;
        if (t < 0) err = -2;
        else { out->ty = t; out->class_type = t; out->pid = parent_pid; have = 1; }
    }
    /* /proc/self and /proc/thread-self store a PF_ROOT entry but keep their
     * own type for the classification. */
    if (!have && !err && root_global && (type == PF_SELF || type == PF_THREAD_SELF)) {
        out->ty = PF_ROOT;
        out->pid = -1;
        out->is_symlink = 0;
        out->class_type = type;
        have = 1;
    }
    if (!have && !err && root_global && is_pid_str(name)) {
        int64_t v = 0;
        probe(NULL, A20_PROCFS_PROBE_PID_GLOBAL, atoi(name), &v);
        if (!v) err = -2;
        else { out->ty = PF_ROOT; out->class_type = PF_ROOT; out->pid = (int)v; have = 1; }
    }
    BR(root_task && !strcmp(name, "cmdline"), PF_PID_CMDLINE, parent_pid, 0);
    BR(root_task && !strcmp(name, "mounts"), PF_MOUNTS, 0, 0);
    BR(root_task && !strcmp(name, "cgroup"), PF_PID_CGROUP, parent_pid, 0);
    if (!have && !err && root_task) {
        switch (type) {
        case PF_PID_STAT: case PF_PID_STATUS: case PF_PID_STATM: case PF_PID_MAPS:
        case PF_PID_SMAPS: case PF_PID_OOM_SCORE_ADJ: case PF_PID_OOM_SCORE:
        case PF_PID_CGROUP: case PF_PID_COMM: case PF_PID_EXE: case PF_PID_CWD:
        case PF_PID_FD: case PF_PID_ENVIRON: case PF_PID_AUXV: case PF_PID_IO:
        case PF_PID_LOGINUID: case PF_PID_SESSIONID: case PF_PID_TASK: case PF_PID_NS:
        case PF_PID_FDINFO: case PF_PID_MOUNTINFO: case PF_PID_PAGEMAP:
        case PF_PID_LIMITS: case PF_PID_WCHAN: case PF_PID_STACK:
        case PF_UID_MAP: case PF_GID_MAP: case PF_SETGROUPS:
            out->ty = type; out->class_type = type; out->pid = parent_pid; have = 1; break;
        default: err = -2; break;
        }
    }
    if (!have && !err) {
        if (!is_root || parent_pid != 0) err = -2;
        else if (!ref_root_file_type(type)) err = -2;
        else { out->ty = type; out->class_type = type; out->pid = 0; have = 1; }
    }
#undef BR
#undef ERR
    (void)fd_entry;
    (void)fd_symlink;
    return have ? 0 : err;
}

static void ref_node_class(int type, int name_is_pid, int fd_symlink, int *out_type, int *out_mode)
{
    int magic = fd_symlink || type == PF_PID_EXE || type == PF_PID_CWD;
    int dir = (type == PF_ROOT && name_is_pid) || type == PF_SELF || type == PF_THREAD_SELF ||
              type == PF_SYS || type == PF_SYS_FS || type == PF_SYS_KERNEL ||
              type == PF_SYS_NET || type == PF_SYS_VM || type == PF_SYS_FS_INOTIFY ||
              type == PF_NET || type == PF_A20 || type == PF_PID_FD || type == PF_PID_TASK ||
              type == PF_PID_NS || type == PF_PID_FDINFO || type == PF_PRESSURE;
    int t = magic ? 3 : (dir ? 2 : 1);
    int m = t == 3 ? (0120000 | 0777) : (t == 2 ? (0040000 | 0555) : (0100000 | 0444));
    if (type == PF_A20_DRIVER_LIFECYCLE || type == PF_SYSRQ_TRIGGER) {
        m = 0100000 | 0200;
    } else if (type == PF_PID_OOM_SCORE_ADJ || type == PF_A20_SCHED_BASE_SLICE ||
               type == PF_SYS_FS_PIPE_MAX_SIZE || type == PF_SYS_FS_LEASE_BREAK_TIME ||
               type == PF_A20_IRQ_AFFINITY || type == PF_A20_XLATOR ||
               type == PF_SYS_KERNEL_OVERFLOW_UID || type == PF_SYS_KERNEL_OVERFLOW_GID ||
               type == PF_SYS_KERNEL_SCHED_AUTOGROUP || type == PF_SYS_KERNEL_CORE_PATTERN ||
               type == PF_SYS_VM_DROP_CACHES ||
               type == PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS ||
               type == PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES ||
               type == PF_UID_MAP || type == PF_GID_MAP || type == PF_SETGROUPS) {
        m = 0100000 | 0644;
    }
    *out_type = t;
    *out_mode = m;
}

static void ref_content_plan(int type, uint32_t nr_cpus, uint64_t page_size,
                             procfs_content_plan_t *p)
{
    memset(p, 0, sizeof(*p));
    if (type == PF_PID_PAGEMAP) {
        p->generator = A20_PROCFS_GEN_PAGEMAP_LEN;
        p->fixed_len = (256ULL * 1024 * 1024 * 1024 / page_size) * 8;
    } else if (type == PF_PID_MAPS || type == PF_PID_SMAPS) {
        p->generator = A20_PROCFS_GEN_MAPS_ALLOC;
        p->meta_len_zero = 1;
    } else if (type == PF_PID_FDINFO_ENTRY) {
        p->generator = A20_PROCFS_GEN_FDINFO;
        p->capacity = 4096;
        p->meta_len_zero = 1;
    } else if (type == PF_A20_PERF || type == PF_A20_LOCK_CONTENTION || type == PF_PID_ENVIRON) {
        p->generator = A20_PROCFS_GEN_GROW_CONTENT;
        p->capacity = 4096;
        p->meta_len_zero = 1;
    } else if (type == PF_CPUINFO) {
        p->generator = A20_PROCFS_GEN_CONTENT;
        p->capacity = 512 * nr_cpus + 1;
        p->meta_len_zero = 1;
    } else {
        p->generator = A20_PROCFS_GEN_CONTENT;
        p->capacity = 4096;
    }
}

/* ---- op stream ---- */
static uint64_t rng_state;
static uint64_t rnd(void)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return rng_state >> 17;
}

static const char *g_names[] = {
    "meminfo", "stat", "sys", "kernel", "hostname", "osrelease", "pid_max", "pidmap",
    "tainted", "sched_autogroup_enabled", "core_pattern", "io_uring_disabled",
    "domainname", "overflowuid", "overflowgid", "vm", "drop_caches", "fs",
    "pipe-max-size", "lease-break-time", "inotify", "max_queued_events",
    "max_user_instances", "net", "status", "config", "route", "arp", "dev", "tcp",
    "udp", "tcp6", "udp6", "unix", "a20", "bcache", "page_cache", "oom",
    "task_lifetime", "perf", "lock_contention", "driver_lifecycle", "objects",
    "iommu", "irq_affinity", "netfilter", "netmem", "journal", "anonprov",
    "hyp_selftest", "sched_base_slice", "xlator", "interrupts", "pressure", "cpu",
    "memory", "io", "self", "thread-self", "cmdline", "mounts", "cgroup", "fd",
    "fdinfo", "task", "ns", "pid", "pid_for_children", "uts", "user", "ipc", "mnt",
    "exe", "cwd", "environ", "auxv", "loginuid", "sessionid", "mountinfo",
    "pagemap", "limits", "wchan", "stack", "uid_map", "gid_map", "setgroups",
    "status", "statm", "maps", "smaps", "oom_score", "oom_score_adj", "comm",
    "boot_id", "cap_last_cap", "nr_open", "sysvipc", "devices", "partitions",
    "diskstats", "modules", "misc", "iomem", "ioports", "softirqs", "tty",
    "ldiscs", "drivers", "version", "uptime", "loadavg", "cpuinfo", "config.gz",
    "filesystems", "cgroups", "swaps", "sysrq-trigger", "7", "42", "999",
    "no-such", "", "0",
};

static const int g_parents[] = {
    PF_ROOT, PF_SYS, PF_SYS_FS, PF_SYS_KERNEL, PF_SYS_VM, PF_SYS_NET, PF_NET,
    PF_A20, PF_PRESSURE, PF_PID_FD, PF_PID_FDINFO, PF_PID_TASK, PF_PID_NS,
};

/* C reference formatting for the production Rust maps/fdinfo renderers. */
static int render_content_checks(void)
{
    struct a20_procfs_map_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.start = 0x1000;
    rec.end = 0x3000;
    rec.vm_flags = (1ull << 0) | (1ull << 1) | (1ull << 4);
    rec.file_offset = 0x40;
    rec.ino = 7;
    rec.thp_eligible = 1;
    strcpy(rec.name, "[heap]");
    rec.stats.rss_pages = 1;
    rec.stats.private_clean_pages = 1;
    rec.stats.anonymous_pages = 1;

    char got[2048], expected[2048];
    int n = a20_procfs_render_maps(&rec, 1, 0, got, sizeof(got));
    int want = snprintf(expected, sizeof(expected),
        "%08lx-%08lx rw-p %08lx 00:00 %lu %s\n",
        (unsigned long)rec.start, (unsigned long)rec.end,
        (unsigned long)rec.file_offset, rec.ino, rec.name);
    if (n != want || strcmp(got, expected) != 0) {
        fprintf(stderr, "maps mismatch n=%d want=%d got='%s' expected='%s'\n",
                n, want, got, expected);
        return 1;
    }

    n = a20_procfs_render_maps(&rec, 1, 1, got, sizeof(got));
    want = snprintf(expected, sizeof(expected),
        "%08lx-%08lx rw-p %08lx 00:00 %lu %s\n"
        "Size:           %8lu kB\nKernelPageSize: %8lu kB\n"
        "MMUPageSize:    %8lu kB\nRss:            %8lu kB\n"
        "Pss:            %8lu kB\nShared_Clean:   %8lu kB\n"
        "Shared_Dirty:   %8lu kB\nPrivate_Clean:  %8lu kB\n"
        "Private_Dirty:  %8lu kB\nReferenced:     %8lu kB\n"
        "Anonymous:      %8lu kB\nAnonHugePages:  %8lu kB\n"
        "ShmemPmdMapped: %8lu kB\nFilePmdMapped:  %8lu kB\n"
        "THPeligible:    %8d\nVmFlags: rd wr mr mw me ac\n",
        (unsigned long)rec.start, (unsigned long)rec.end,
        (unsigned long)rec.file_offset, rec.ino, rec.name,
        (unsigned long)8, (unsigned long)4, (unsigned long)4,
        (unsigned long)4, (unsigned long)4, (unsigned long)0,
        (unsigned long)0, (unsigned long)4, (unsigned long)0,
        (unsigned long)4, (unsigned long)4, (unsigned long)0,
        (unsigned long)0, (unsigned long)0, rec.thp_eligible);
    if (n != want || strcmp(got, expected) != 0) {
        fprintf(stderr, "smaps mismatch n=%d want=%d\ngot:\n%s\nexpected:\n%s\n",
                n, want, got, expected);
        return 1;
    }

    memset(got, 0xa5, sizeof(got));
    n = a20_procfs_render_maps(&rec, 1, 1, got, 8);
    if (n != 7 || got[7] != '\0' || memcmp(got, "0000100", 7) != 0) {
        fprintf(stderr, "maps truncation contract mismatch n=%d\n", n);
        return 1;
    }
    if (a20_procfs_render_maps(NULL, 1, 0, got, sizeof(got)) != -22 ||
        a20_procfs_render_maps(&rec, 1, 0, got, 0) != -22) {
        fprintf(stderr, "maps invalid-buffer contract mismatch\n");
        return 1;
    }

    n = a20_procfs_render_fdinfo(123, 0100002, 99, 0, got, sizeof(got));
    want = snprintf(expected, sizeof(expected), "pos:\t%lu\nflags:\t0%o\nino:\t%lu\n",
                    123ul, 0100002u, 99ul);
    if (n != want || strcmp(got, expected) != 0) {
        fprintf(stderr, "fdinfo mismatch n=%d want=%d\n", n, want);
        return 1;
    }
    /* A pidfd adds the lines dbus reads to resolve its own credentials. */
    memset(got, 0, sizeof(got));
    n = a20_procfs_render_fdinfo(0, 02000002, 99, 456, got, sizeof(got));
    want = snprintf(expected, sizeof(expected),
                    "pos:\t%lu\nflags:\t0%o\nino:\t%lu\n"
                    "Pid:\t%d\nNSpid:\t%d\n",
                    0ul, 02000002u, 99ul, 456, 456);
    if (n != want || strcmp(got, expected) != 0) {
        fprintf(stderr, "pidfd fdinfo mismatch n=%d want=%d got='%s'\n",
                n, want, got);
        return 1;
    }
    memset(got, 0xa5, sizeof(got));
    n = a20_procfs_render_fdinfo(123, 0100002, 99, 0, got, 5);
    if (n != 4 || got[4] != '\0' || memcmp(got, "pos:", 4) != 0) {
        fprintf(stderr, "fdinfo truncation contract mismatch n=%d\n", n);
        return 1;
    }

    struct a20_procfs_net_row row;
    memset(&row, 0, sizeof(row));
    row.reg_idx = 7;
    row.domain = AF_INET;
    row.type = SOCK_STREAM;
    row.tcp_flags = 1 << 2; /* connected */
    row.local_len = 16;
    row.rx_queued = 5;
    row.owner_uid = 1000;
    row.recv_timeout_ticks = 3;
    row.local[2] = 0x1f;
    row.local[3] = 0x90;
    row.local[4] = 127;
    row.local[5] = 0;
    row.local[6] = 0;
    row.local[7] = 1;
    /* The kernel C formatter's historic output is the byte-for-byte oracle. */
    snprintf(expected, sizeof(expected),
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        "%4d: %08X:%04X %08X:%04X %02X %08X:%08X %02X:%08X %08X %d %ld %d\n",
        row.reg_idx, 0x0100007f, 8080, 0, 0, 1, 0, 5, 0, 0, 0,
        row.owner_uid, (long)row.recv_timeout_ticks, 0);
    /* A short-lived host helper exposes the production Rust row renderer via
     * a synthetic one-row content generator in the test build. */
    if (a20_procfs_render_net_snapshot(PF_NET_TCP, &row, 1, got, sizeof(got)) !=
            (int)strlen(expected) ||
        strcmp(got, expected) != 0) {
        fprintf(stderr, "net row mismatch\ngot: %s\nexpected: %s\n", got, expected);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 200000;
    rng_state = 0xbe5466cf34e90c6cull;
    g_fd_ok = 7;
    g_pid_shown = 42;
    g_pid_global = 4242;
    g_thread_shown = 42;
    g_thread_global = 4343;

    if (render_content_checks() != 0)
        return 1;

    /* Exhaustive passes over every type first. */
    for (int t = 0; t < PF_SYSRQ_TRIGGER + 1; t++) {
        if (a20_procfs_root_file_type(t) != ref_root_file_type(t)) {
            printf("root_file_type mismatch type=%d\n", t);
            return 1;
        }
        if (a20_procfs_resource_kind(t) != ref_resource_kind(t)) {
            printf("resource_kind mismatch type=%d\n", t);
            return 1;
        }
        for (int pidn = 0; pidn < 2; pidn++) {
            for (int sym = 0; sym < 2; sym++) {
                int rt = 0, rm = 0, wt = 0, wm = 0;
                ref_node_class(t, pidn, sym, &rt, &rm);
                a20_procfs_node_class(t, pidn, sym, &wt, &wm);
                if (rt != wt || rm != wm) {
                    printf("node_class mismatch type=%d pidname=%d sym=%d: %d/%d vs %d/%d\n",
                           t, pidn, sym, rt, rm, wt, wm);
                    return 1;
                }
            }
        }
        for (uint32_t nc = 1; nc <= 8; nc++) {
            for (uint64_t ps = 4096; ps <= 65536; ps *= 16) {
                procfs_content_plan_t a, b;
                ref_content_plan(t, nc, ps, &a);
                a20_procfs_content_plan(t, nc, ps, &b);
                if (memcmp(&a, &b, sizeof(a)) != 0) {
                    printf("content_plan mismatch type=%d ncpus=%u page=%llu\n", t, nc,
                           (unsigned long long)ps);
                    return 1;
                }
            }
        }
    }

    /* Every name against every parent, then a randomised stream. */
    for (unsigned i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++) {
        for (unsigned p = 0; p < sizeof(g_parents) / sizeof(g_parents[0]); p++) {
            int rp = 0, wp = 0;
            int ra = ref_name_type(g_names[i], &rp, 1);
            int wa = a20_procfs_name_type(g_names[i], &wp, 1);
            if (ra != wa || rp != wp) {
                printf("name_type mismatch '%s': %d/%d vs %d/%d\n", g_names[i], ra, rp, wa, wp);
                return 1;
            }
        }
    }

    for (unsigned step = 0; step < rounds; step++) {
        const char *name = g_names[rnd() % (sizeof(g_names) / sizeof(g_names[0]))];
        int parent = g_parents[rnd() % (sizeof(g_parents) / sizeof(g_parents[0]))];
        int parent_pid = (int)(rnd() % 5) - 1; /* -1, 0, 1, 2, 3 */
        int xlator = (int)(rnd() % 2);
        g_probe_deny = (rnd() % 32 == 0);

        int rp = 0, wp = 0;
        int rt = ref_name_type(name, &rp, xlator);
        int wt = a20_procfs_name_type(name, &wp, xlator);
        if (rt != wt || rp != wp) {
            printf("name_type mismatch step %u '%s' xlator=%d\n", step, name, xlator);
            return 1;
        }

        procfs_child_node_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        int ra = ref_child(parent, parent_pid, name, xlator, &a);
        int rb = a20_procfs_child_node(parent, parent_pid, name, rt, rp, xlator, NULL,
                                       probe, &b);
        if (ra != rb || (ra == 0 && (a.ty != b.ty || a.pid != b.pid ||
                                     a.is_symlink != b.is_symlink ||
                                     a.class_type != b.class_type))) {
            printf("child mismatch step %u parent=%d ppid=%d name='%s' xlator=%d deny=%d\n"
                   "  C:    rc=%d ty=%d pid=%d sym=%u class=%d\n"
                   "  RUST: rc=%d ty=%d pid=%d sym=%u class=%d\n",
                   step, parent, parent_pid, name, xlator, g_probe_deny,
                   ra, a.ty, a.pid, a.is_symlink, a.class_type,
                   rb, b.ty, b.pid, b.is_symlink, b.class_type);
            return 1;
        }
    }

    printf("procfs node state C/Rust differential PASS (%u steps)\n", rounds);
    return 0;
}
