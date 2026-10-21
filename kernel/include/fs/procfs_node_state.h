#ifndef _FS_PROCFS_NODE_STATE_H
#define _FS_PROCFS_NODE_STATE_H

#include "core/types.h"

/*
 * procfs node-table and content-plan state.
 *
 * The global name table (`name_to_type`), the parent-scoped node table
 * (`procfs_lookup`'s chain, including the entries that need a task or fdtable
 * probe), the vnode classification and mode policy, `procfs_root_file_type`,
 * `resource_kind` and the content plan are owned by
 * kernel/fs/procfs/procfs_node_state.rs.  procfs.c keeps the allocation, the
 * vnode initialisation, the content generators and the task/fdtable lookups it
 * answers through the probe callback.
 *
 * The `pf_type` numbering below mirrors the enum in fs/procfs_internal.h.  That
 * enum is flat -- no member is behind a conditional -- so the numbers are
 * profile-independent, and procfs.c asserts every mirrored value against the
 * real enum at compile time.  Keep them in sync.
 */

/* ---- mirrored pf_type values (asserted in procfs.c) ---- */
#define A20_PF_ROOT 0
#define A20_PF_MEMINFO 1
#define A20_PF_STAT 2
#define A20_PF_VERSION 3
#define A20_PF_UPTIME 4
#define A20_PF_CMDLINE 5
#define A20_PF_CPUINFO 6
#define A20_PF_MOUNTS 7
#define A20_PF_LOADAVG 8
#define A20_PF_NET 9
#define A20_PF_NET_STATUS 10
#define A20_PF_NET_CONFIG 11
#define A20_PF_NET_DEV 12
#define A20_PF_NET_TCP 13
#define A20_PF_NET_UDP 14
#define A20_PF_NET_TCP6 15
#define A20_PF_NET_UDP6 16
#define A20_PF_NET_UNIX 17
#define A20_PF_CONFIG_GZ 18
#define A20_PF_PID_STAT 19
#define A20_PF_PID_STATUS 20
#define A20_PF_PID_STATM 21
#define A20_PF_PID_MAPS 22
#define A20_PF_PID_SMAPS 23
#define A20_PF_PID_OOM_SCORE_ADJ 24
#define A20_PF_PID_OOM_SCORE 25
#define A20_PF_PID_CGROUP 26
#define A20_PF_PID_CMDLINE 27
#define A20_PF_PID_COMM 28
#define A20_PF_PID_EXE 29
#define A20_PF_PID_CWD 30
#define A20_PF_PID_FD 31
#define A20_PF_PID_ENVIRON 32
#define A20_PF_PID_AUXV 33
#define A20_PF_PID_IO 34
#define A20_PF_PID_LOGINUID 35
#define A20_PF_PID_SESSIONID 36
#define A20_PF_PID_TASK 37
#define A20_PF_PID_NS 38
#define A20_PF_PID_NS_PID 39
#define A20_PF_PID_NS_PID_FOR_CHILDREN 40
#define A20_PF_PID_NS_UTS 41
#define A20_PF_PID_NS_USER 42
#define A20_PF_PID_NS_IPC 43
#define A20_PF_PID_NS_MNT 44
#define A20_PF_PID_NS_NET 45
#define A20_PF_PID_NS_CGROUP 46
#define A20_PF_PID_FDINFO 47
#define A20_PF_PID_FDINFO_ENTRY 48
#define A20_PF_PID_MOUNTINFO 49
#define A20_PF_PID_PAGEMAP 50
#define A20_PF_SYS 51
#define A20_PF_SYS_FS 52
#define A20_PF_SYS_FS_PIPE_MAX_SIZE 53
#define A20_PF_SYS_FS_LEASE_BREAK_TIME 54
#define A20_PF_SYS_KERNEL 55
#define A20_PF_SYS_KERNEL_OSRELEASE 56
#define A20_PF_SYS_KERNEL_PID_MAX 57
#define A20_PF_SYS_KERNEL_PIDMAP 58
#define A20_PF_SYS_KERNEL_TAINTED 59
#define A20_PF_SYS_KERNEL_SCHED_AUTOGROUP 60
#define A20_PF_SYS_KERNEL_CORE_PATTERN 61
#define A20_PF_SYS_KERNEL_IO_URING_DISABLED 62
#define A20_PF_SYS_KERNEL_HOSTNAME 63
#define A20_PF_SYS_KERNEL_DOMAINNAME 64
#define A20_PF_SYS_KERNEL_OVERFLOW_UID 65
#define A20_PF_SYS_KERNEL_OVERFLOW_GID 66
#define A20_PF_BOOT_ID 67
#define A20_PF_CAP_LAST_CAP 68
#define A20_PF_NR_OPEN 69
#define A20_PF_PRESSURE 70
#define A20_PF_PRESSURE_CPU 71
#define A20_PF_PRESSURE_MEM 72
#define A20_PF_PRESSURE_IO 73
#define A20_PF_UID_MAP 74
#define A20_PF_GID_MAP 75
#define A20_PF_SETGROUPS 76
#define A20_PF_SYSVIPC 77
#define A20_PF_DEVICES 78
#define A20_PF_PARTITIONS 79
#define A20_PF_DISKSTATS 80
#define A20_PF_MODULES 81
#define A20_PF_MISC 82
#define A20_PF_IOMEM 83
#define A20_PF_IOPORTS 84
#define A20_PF_SOFTIRQS 85
#define A20_PF_ROUTE 86
#define A20_PF_ARP 87
#define A20_PF_TTY 88
#define A20_PF_LDISCS 89
#define A20_PF_DRIVERS 90
#define A20_PF_PID_LIMITS 91
#define A20_PF_PID_WCHAN 92
#define A20_PF_PID_STACK 93
#define A20_PF_THREAD_SELF 94
#define A20_PF_SYS_VM 95
#define A20_PF_SYS_VM_DROP_CACHES 96
#define A20_PF_SYS_FS_INOTIFY 97
#define A20_PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS 98
#define A20_PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES 99
#define A20_PF_SYS_NET 100
#define A20_PF_INTERRUPTS 101
#define A20_PF_A20 102
#define A20_PF_A20_BCACHE 103
#define A20_PF_A20_PAGE_CACHE 104
#define A20_PF_A20_OOM 105
#define A20_PF_A20_TASK_LIFETIME 106
#define A20_PF_A20_PERF 107
#define A20_PF_A20_ANONPROV 108
#define A20_PF_A20_HYP_SELFTEST 109
#define A20_PF_A20_LOCK_CONTENTION 110
#define A20_PF_A20_DRIVER_LIFECYCLE 111
#define A20_PF_A20_SCHED_BASE_SLICE 112
#define A20_PF_A20_XLATOR 113
#define A20_PF_A20_OBJECTS 114
#define A20_PF_A20_IOMMU 115
#define A20_PF_A20_NETFILTER 116
#define A20_PF_A20_NETMEM 117
#define A20_PF_A20_JOURNAL 118
#define A20_PF_A20_IRQ_AFFINITY 119
#define A20_PF_CGROUPS 120
#define A20_PF_SELF 121
#define A20_PF_FSTYPE 122
#define A20_PF_SWAPS 123
#define A20_PF_SYSRQ_TRIGGER 124
#define A20_PF_TYPE_COUNT 125

/* Probe kinds for a20_procfs_child_node(). */
#define A20_PROCFS_PROBE_FD_EXISTS 0u
#define A20_PROCFS_PROBE_PID_GLOBAL 1u
#define A20_PROCFS_PROBE_THREAD_OF_OWNER 2u

/* Content generators. */
#define A20_PROCFS_GEN_CONTENT 0u
#define A20_PROCFS_GEN_FDINFO 1u
#define A20_PROCFS_GEN_MAPS_ALLOC 2u
#define A20_PROCFS_GEN_PAGEMAP_LEN 3u

typedef struct procfs_child_node {
    int ty;
    int pid;
    uint32_t is_symlink;
    /* The type used for the vnode classification; it differs from `ty` only
     * for /proc/self and /proc/thread-self. */
    int class_type;
} procfs_child_node_t;

typedef struct procfs_content_plan {
    uint32_t generator;
    size_t capacity;
    uint64_t fixed_len;
    uint32_t meta_len_zero;
} procfs_content_plan_t;

/* `int (*)(void *ctx, uint32_t which, int64_t arg, int64_t *out)`. */
typedef int (*procfs_probe_fn)(void *ctx, uint32_t which, int64_t arg, int64_t *out);

int a20_procfs_name_type(const char *name, int *out_pid, int xlator_enabled);
int a20_procfs_root_file_type(int ty);
int a20_procfs_resource_kind(int ty);
uint32_t a20_procfs_node_class(int ty, int name_is_pid_name, int fd_symlink,
                                 int *out_type, int *out_mode);
int a20_procfs_child_node(int parent_type, int parent_pid, const char *name,
                          int global_type, int global_pid, int xlator_enabled,
                          void *ctx, procfs_probe_fn probe, procfs_child_node_t *out);
void a20_procfs_content_plan(int ty, uint32_t nr_cpus, uint64_t page_size,
                             procfs_content_plan_t *out);

/*
 * Content generation engine.  The migrated procfs node kinds are formatted in
 * Rust; anything not yet migrated is delegated to the adapter's retained C
 * dispatcher; the C generator is gone.
 */
struct a20_meminfo_raw {
    uint64_t total_frames;
    uint64_t free_frames;
    uint64_t slab_total_bytes;
    uint64_t slab_reclaim_bytes;
    uint64_t bcache_pool_bytes;
    uint64_t bcache_valid_pages;
    uint64_t bcache_dirty_blocks;
    uint64_t bcache_dirty_pages;
    uint64_t pcache_valid;
    uint64_t pcache_dirty;
    uint64_t vm_anon_huge;
    uint64_t vm_shmem_huge;
    uint64_t vm_file_huge;
    uint64_t huge_total;
    uint64_t huge_free;
    uint64_t swap_total_pages;
    uint64_t swap_free_pages;
    uint64_t page_size;
    uint64_t pcache_page_size;
    uint64_t bcache_block_size;
};

struct a20_mount_ent {
    char dev[32];
    char path[256];
    char fstype[32];
    char opts[32];
    uint32_t mnt_id;
    uint32_t parent_id;
    uint8_t detached;
};

struct a20_swap_ent {
    char name[64];
    uint64_t pages;
    uint64_t inuse_pages;
};

struct a20_bcache_raw {
    uint64_t caches;
    uint64_t block_pool_bytes;
    uint64_t page_pool_bytes;
    uint64_t valid_blocks;
    uint64_t dirty_blocks;
    uint64_t valid_pages;
    uint64_t dirty_pages;
};

struct a20_pagecache_raw {
    uint64_t capacity;
    uint64_t allocated;
    uint64_t bytes;
    uint64_t valid;
    uint64_t dirty;
    uint64_t pinned;
};

struct a20_oom_raw {
    uint64_t kills;
    uint64_t last_kill_tick;
    int32_t last_victim_pid;
    int32_t last_victim_score;
    uint64_t free_pages_at_kill;
    uint64_t free_pages_now;
    int32_t in_progress;
    uint64_t kswapd_passes;
    uint64_t kswapd_pages_freed;
    uint64_t kswapd_last_pass_tick;
};

struct a20_objstats_raw {
    uint64_t handles;
    uint64_t channel_eps;
    uint64_t eventqs;
    uint64_t vmos;
    uint64_t vmo_pages;
    uint64_t irq_bindings;
    uint64_t vfiles;
    uint64_t vmo_dirty_frames;
};

struct a20_sysvipc_raw {
    int32_t msg;
    int32_t sem;
    int32_t shm;
};

struct a20_pid_raw {
    int32_t pid;
    int32_t ppid;
    int32_t pgid;
    int32_t sid;
    char name[64];
    char state_char;
    char state_text[32];
    uint64_t utime_ticks;
    uint64_t stime_ticks;
    int64_t child_utime;
    int64_t child_stime;
    int32_t priority;
    uint64_t start_jiffies;
    uint64_t vsize;
    int64_t rss_pages;
    uint32_t processor;
    int32_t uid;
    int32_t euid;
    int32_t suid;
    int32_t fsuid;
    int32_t gid;
    int32_t egid;
    int32_t sgid;
    int32_t fsgid;
    uint64_t cap_inh;
    uint64_t cap_prm;
    uint64_t cap_eff;
    uint64_t cap_bnd;
    uint32_t ngroups;
    int32_t groups[64];
    uint64_t rss_kb;
    uint64_t vmlck_kb;
    uint64_t vmdata_kb;
};

struct a20_map_ent {
    uint32_t lower;
    uint32_t parent_lower;
    uint32_t count;
};

struct a20_map_raw {
    uint32_t n;
    struct a20_map_ent ent[16];
};

struct a20_msix_ent {
    char name[32];
    uint32_t index;
    uint32_t vector;
    int32_t cpu;
};

struct a20_msix_raw {
    uint32_t total;
    uint32_t n;
    struct a20_msix_ent ent[64];
};

struct a20_limits_raw {
    uint64_t nofile;
    uint64_t memlock;
};

struct a20_pid_io_raw {
    uint64_t rchar;
    uint64_t wchar;
    uint64_t syscr;
    uint64_t syscw;
    uint64_t read_bytes;
    uint64_t write_bytes;
};

struct a20_iommu_raw {
    int32_t enabled;
    uint64_t domains_claimed;
    uint64_t domains_released;
    uint64_t maps;
    uint64_t map_failures;
    uint64_t unmaps;
    uint64_t mapped_pages;
    uint64_t fault_records;
    uint64_t faults;
    uint64_t blocked_events;
    uint32_t last_fault_devid;
    uint64_t last_fault_cause;
    uint64_t last_fault_iova;
    int32_t last_fault_owner;
};

#define A20_PROCFS_NET_ADDR_MAX 128
struct a20_procfs_net_row {
    int32_t reg_idx, domain, type, tcp_flags;
    int32_t local_len, peer_len;
    uint32_t rx_queued;
    int32_t owner_uid;
    int64_t recv_timeout_ticks;
    uint8_t local[A20_PROCFS_NET_ADDR_MAX];
    uint8_t peer[A20_PROCFS_NET_ADDR_MAX];
};

struct a20_procfs_gen {
    const char *(*sysname)(void);
    const char *(*release)(void);
    const char *(*arch_name)(void);
    const char *(*version_full)(void);
    uint64_t (*ticks)(void);
    uint64_t (*ticks_per_sec)(void);
    const char *(*bootargs)(void);
    /* unsigned, not uint32_t: the pointees are proc_loadavg_snapshot()'s
     * `unsigned *` parameters, and on ilp32d uint32_t is `unsigned long` while
     * unsigned is `unsigned int` -- same width, different type. */
    void (*loadavg)(uint64_t *a1, uint64_t *a5, uint64_t *a15,
                    unsigned *running, unsigned *total, int *max_pid);
    uint32_t (*anonprov_max)(void);
    int (*sched_base_slice)(void);
    int (*pid_max)(void);
    int (*current_pid)(void);
    int (*pipe_max_size)(void);
    int (*lease_break_time)(void);
    int (*max_files)(void);
    void (*meminfo_raw)(struct a20_meminfo_raw *out);
    int (*mount_count)(void);
    int (*mount_at)(int index, struct a20_mount_ent *out);
    int (*nr_cpus)(void);
    int (*cpu_times)(int cpu, uint64_t *user, uint64_t *system, uint64_t *idle);
    void (*realtime)(uint64_t *out);
    void (*monotonic)(uint64_t *out);
    uint64_t (*tasks_created)(void);
    uint64_t (*runq_load_sum)(void);
    int (*swap_get)(int index, struct a20_swap_ent *out);
    uint32_t (*online_cpu_mask)(void);
    int (*cpuinfo_fields)(char *buf, size_t size);
    void (*bcache_raw)(struct a20_bcache_raw *out);
    void (*pagecache_raw)(struct a20_pagecache_raw *out);
    void (*oom_raw)(struct a20_oom_raw *out);
    void (*objstats_raw)(struct a20_objstats_raw *out);
    void (*iommu_raw)(struct a20_iommu_raw *out);
    void (*sysvipc_raw)(struct a20_sysvipc_raw *out);
    void (*config_gz)(const uint8_t **data, size_t *len);
    int (*userns_overflow_id)(int gid);
    const char *(*hostname)(void);
    const char *(*domainname)(void);
    int (*psi_render)(int kind, char *buf, size_t size);
    int (*pidmap)(char *buf, size_t size);
    int (*pid_raw)(int pid, struct a20_pid_raw *out);
    int (*pid_str)(int pid, int which, char *out, size_t size);
    int (*pid_io)(int pid, struct a20_pid_io_raw *out);
    int (*pid_sessionid)(int pid);
    uint64_t (*pid_ns_ino)(int pid, int kind);
    int (*format_helper)(int which, char *buf, size_t size);
    int (*hyp_selftest)(void);
    int (*pid_oom)(int pid, int *adj_out);
    int (*pid_cgroup_path)(int pid, char *out, size_t size);
    int (*userns_map)(int pid, int is_gid, struct a20_map_raw *out);
    int (*userns_setgroups_allowed)(int pid, int *allowed);
    int (*pid_exists)(int pid);
    int (*pid_limits)(int pid, struct a20_limits_raw *out);
    int (*pid_wchan)(int pid, char *out, size_t size);
    int (*pid_auxv)(int pid, unsigned char *buf, uint32_t cap);
    int (*msix_cpu_count)(void);
    uint32_t (*msix_snapshot)(struct a20_msix_raw *out);
    int (*net_snapshot)(int kind, struct a20_procfs_net_row **out,
                        size_t *count);
    void (*net_snapshot_free)(struct a20_procfs_net_row *records);
};

/* Raw VMA snapshot consumed by the Rust maps/smaps renderer.  Keep this a
 * value-only ABI: C pins/collects VM state and owns allocations, Rust owns all
 * text layout and truncation. */
#ifdef CONFIG_MCU
#ifdef CONFIG_STM32_QEMU
#define A20_PROCFS_MAP_NAME_MAX 32
#else
#define A20_PROCFS_MAP_NAME_MAX 64
#endif
#else
#define A20_PROCFS_MAP_NAME_MAX 512
#endif
struct a20_procfs_map_stats {
    size_t rss_pages, shared_clean_pages, shared_dirty_pages;
    size_t private_clean_pages, private_dirty_pages, anonymous_pages;
    size_t anon_huge_pages, shmem_pmd_pages, file_pmd_pages;
};
struct a20_procfs_map_record {
    uint64_t start, end, vm_flags, file_offset;
    unsigned long ino;
    int32_t thp_eligible;
    char name[A20_PROCFS_MAP_NAME_MAX];
    struct a20_procfs_map_stats stats;
};


int a20_procfs_render_maps(const struct a20_procfs_map_record *records,
                           size_t count, int smaps, char *buf, size_t bufsz);
int a20_procfs_render_fdinfo(size_t pos, unsigned int flags, unsigned long ino,
                             char *buf, size_t bufsz);
int a20_procfs_render_net_snapshot(int kind,
                                   const struct a20_procfs_net_row *rows,
                                   size_t count, char *buf, size_t bufsz);

int a20_procfs_generate(const struct a20_procfs_gen *gen, int ty, int pid,
                        char *buf, size_t bufsz);

#endif /* _FS_PROCFS_NODE_STATE_H */
