#ifndef _PROCFS_INTERNAL_H
#define _PROCFS_INTERNAL_H

#include "core/types.h"

struct task_t;
struct mm_struct;

extern int g_procfs_pipe_max_size;
extern int g_procfs_lease_break_time;
extern int g_sched_base_slice_ms;

/*
 * Private procfs glue shared between fs/procfs/procfs.c (vnode/file operations and
 * directory lookup) and fs/procfs_render.c (on-demand content generation for
 * the synthetic /proc files).  These types and helpers are not part of the
 * public VFS API and must not be referenced outside kernel/fs/.
 */

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
    PF_NET_STATUS,
    PF_NET_CONFIG,
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
    PF_PID_NS_PID,
    PF_PID_NS_UTS,
    PF_PID_NS_USER,
    PF_PID_NS_IPC,
    PF_PID_NS_MNT,
    PF_PID_NS_NET,
    PF_PID_NS_CGROUP,
    PF_PID_FDINFO,
    PF_PID_FDINFO_ENTRY,
    PF_PID_MOUNTINFO,
    PF_PID_PAGEMAP,
    PF_SYS,
    PF_SYS_FS,
    PF_SYS_FS_PIPE_MAX_SIZE,
    PF_SYS_FS_LEASE_BREAK_TIME,
    PF_SYS_KERNEL,
    PF_SYS_KERNEL_OSRELEASE,
    PF_SYS_KERNEL_PID_MAX,
    PF_SYS_KERNEL_PIDMAP,
    PF_SYS_KERNEL_TAINTED,
    PF_SYS_KERNEL_SCHED_AUTOGROUP,
    PF_SYS_KERNEL_CORE_PATTERN,
    PF_SYS_KERNEL_IO_URING_DISABLED,
    PF_SYS_VM,
    PF_SYS_VM_DROP_CACHES,
    PF_SYS_FS_INOTIFY,
    PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS,
    PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES,
    PF_SYS_NET,
    PF_INTERRUPTS,
    PF_A20,
    PF_A20_BCACHE,
    PF_A20_PAGE_CACHE,
    PF_A20_OOM,
    PF_A20_TASK_LIFETIME,
    PF_A20_DRIVER_LIFECYCLE,
    PF_A20_SCHED_BASE_SLICE,
    PF_CGROUPS,
    PF_SELF,
    PF_FSTYPE,
    PF_SWAPS,
} pf_type_t;

int generate_pid_maps_alloc(int pid, int smaps, char **buf_out,
                            size_t *len_out);
int generate_content(pf_type_t type, int pid, char *buf, size_t bufsz);
int generate_pid_fdinfo(int pid, int fd, char *buf, size_t bufsz);

#endif /* _PROCFS_INTERNAL_H */