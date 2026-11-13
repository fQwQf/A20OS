//! procfs node-table and content-plan state engine (Rust-owned split for
//! `fs/procfs.c`).
//!
//! `procfs.c` built every `/proc` vnode by consulting two ordered tables and
//! then classifying the result.  This module owns both tables and the
//! classification:
//!
//! * the **global name table** (`name_to_type`): which `/proc/<name>` exists
//!   at the root and what node it is, including the pid-shaped names;
//! * the **parent-scoped node table** (`procfs_lookup`'s chain): which child a
//!   given parent node exposes under a given name, in the original order --
//!   including the four entries whose resolution needs a task or fdtable probe,
//!   which the adapter answers through `ProcfsProbeFn`;
//! * the **node classification**: whether the child is a magic symlink, a
//!   directory or a regular file, and its permission mode (the `0555`/`0444`
//!   defaults plus the `0200` and `0644` writable-node overrides);
//! * `procfs_root_file_type` and `resource_kind`;
//! * the **content plan**: which generator renders a node, the buffer capacity
//!   it needs (`512 * NR_CPUS + 1` for `cpuinfo`, `4096` otherwise), the fixed
//!   `pagemap` length, and whether the vnode-size path reports length 0.
//!
//! The adapter keeps the mechanical half: allocation, the vnode
//! initialisation, `generate_content`/`generate_pid_fdinfo`/
//! `generate_pid_maps_alloc`, `proc_find_get`/`fdtable_get_file_ref` behind the
//! probe callback, and the namespace pinning.
//!
//! The `pf_type` numbering is mirrored from `fs/procfs_internal.h`; that enum is
//! flat (no conditional members), and the host harness asserts every mirrored
//! value against the real enum so drift cannot go unnoticed.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_int, c_void};

// ---------------------------------------------------------------------------
// pf_type values (mirrored from fs/procfs_internal.h)
// ---------------------------------------------------------------------------

pub const PF_ROOT: i32 = 0;
pub const PF_MEMINFO: i32 = 1;
pub const PF_STAT: i32 = 2;
pub const PF_VERSION: i32 = 3;
pub const PF_UPTIME: i32 = 4;
pub const PF_CMDLINE: i32 = 5;
pub const PF_CPUINFO: i32 = 6;
pub const PF_MOUNTS: i32 = 7;
pub const PF_LOADAVG: i32 = 8;
pub const PF_NET: i32 = 9;
pub const PF_NET_STATUS: i32 = 10;
pub const PF_NET_CONFIG: i32 = 11;
pub const PF_NET_DEV: i32 = 12;
pub const PF_NET_TCP: i32 = 13;
pub const PF_NET_UDP: i32 = 14;
pub const PF_NET_TCP6: i32 = 15;
pub const PF_NET_UDP6: i32 = 16;
pub const PF_NET_UNIX: i32 = 17;
pub const PF_CONFIG_GZ: i32 = 18;
pub const PF_PID_STAT: i32 = 19;
pub const PF_PID_STATUS: i32 = 20;
pub const PF_PID_STATM: i32 = 21;
pub const PF_PID_MAPS: i32 = 22;
pub const PF_PID_SMAPS: i32 = 23;
pub const PF_PID_OOM_SCORE_ADJ: i32 = 24;
pub const PF_PID_OOM_SCORE: i32 = 25;
pub const PF_PID_CGROUP: i32 = 26;
pub const PF_PID_CMDLINE: i32 = 27;
pub const PF_PID_COMM: i32 = 28;
pub const PF_PID_EXE: i32 = 29;
pub const PF_PID_CWD: i32 = 30;
pub const PF_PID_FD: i32 = 31;
pub const PF_PID_ENVIRON: i32 = 32;
pub const PF_PID_AUXV: i32 = 33;
pub const PF_PID_IO: i32 = 34;
pub const PF_PID_LOGINUID: i32 = 35;
pub const PF_PID_SESSIONID: i32 = 36;
pub const PF_PID_TASK: i32 = 37;
pub const PF_PID_NS: i32 = 38;
pub const PF_PID_NS_PID: i32 = 39;
pub const PF_PID_NS_PID_FOR_CHILDREN: i32 = 40;
pub const PF_PID_NS_UTS: i32 = 41;
pub const PF_PID_NS_USER: i32 = 42;
pub const PF_PID_NS_IPC: i32 = 43;
pub const PF_PID_NS_MNT: i32 = 44;
pub const PF_PID_NS_NET: i32 = 45;
pub const PF_PID_NS_CGROUP: i32 = 46;
pub const PF_PID_FDINFO: i32 = 47;
pub const PF_PID_FDINFO_ENTRY: i32 = 48;
pub const PF_PID_MOUNTINFO: i32 = 49;
pub const PF_PID_PAGEMAP: i32 = 50;
pub const PF_SYS: i32 = 51;
pub const PF_SYS_FS: i32 = 52;
pub const PF_SYS_FS_PIPE_MAX_SIZE: i32 = 53;
pub const PF_SYS_FS_LEASE_BREAK_TIME: i32 = 54;
pub const PF_SYS_KERNEL: i32 = 55;
pub const PF_SYS_KERNEL_OSRELEASE: i32 = 56;
pub const PF_SYS_KERNEL_PID_MAX: i32 = 57;
pub const PF_SYS_KERNEL_PIDMAP: i32 = 58;
pub const PF_SYS_KERNEL_TAINTED: i32 = 59;
pub const PF_SYS_KERNEL_SCHED_AUTOGROUP: i32 = 60;
pub const PF_SYS_KERNEL_CORE_PATTERN: i32 = 61;
pub const PF_SYS_KERNEL_IO_URING_DISABLED: i32 = 62;
pub const PF_SYS_KERNEL_HOSTNAME: i32 = 63;
pub const PF_SYS_KERNEL_DOMAINNAME: i32 = 64;
pub const PF_SYS_KERNEL_OVERFLOW_UID: i32 = 65;
pub const PF_SYS_KERNEL_OVERFLOW_GID: i32 = 66;
pub const PF_BOOT_ID: i32 = 67;
pub const PF_CAP_LAST_CAP: i32 = 68;
pub const PF_NR_OPEN: i32 = 69;
pub const PF_PRESSURE: i32 = 70;
pub const PF_PRESSURE_CPU: i32 = 71;
pub const PF_PRESSURE_MEM: i32 = 72;
pub const PF_PRESSURE_IO: i32 = 73;
pub const PF_UID_MAP: i32 = 74;
pub const PF_GID_MAP: i32 = 75;
pub const PF_SETGROUPS: i32 = 76;
pub const PF_SYSVIPC: i32 = 77;
pub const PF_DEVICES: i32 = 78;
pub const PF_PARTITIONS: i32 = 79;
pub const PF_DISKSTATS: i32 = 80;
pub const PF_MODULES: i32 = 81;
pub const PF_MISC: i32 = 82;
pub const PF_IOMEM: i32 = 83;
pub const PF_IOPORTS: i32 = 84;
pub const PF_SOFTIRQS: i32 = 85;
pub const PF_ROUTE: i32 = 86;
pub const PF_ARP: i32 = 87;
pub const PF_TTY: i32 = 88;
pub const PF_LDISCS: i32 = 89;
pub const PF_DRIVERS: i32 = 90;
pub const PF_PID_LIMITS: i32 = 91;
pub const PF_PID_WCHAN: i32 = 92;
pub const PF_PID_STACK: i32 = 93;
pub const PF_THREAD_SELF: i32 = 94;
pub const PF_SYS_VM: i32 = 95;
pub const PF_SYS_VM_DROP_CACHES: i32 = 96;
pub const PF_SYS_FS_INOTIFY: i32 = 97;
pub const PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS: i32 = 98;
pub const PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES: i32 = 99;
pub const PF_SYS_NET: i32 = 100;
pub const PF_INTERRUPTS: i32 = 101;
pub const PF_A20: i32 = 102;
pub const PF_A20_BCACHE: i32 = 103;
pub const PF_A20_PAGE_CACHE: i32 = 104;
pub const PF_A20_OOM: i32 = 105;
pub const PF_A20_TASK_LIFETIME: i32 = 106;
pub const PF_A20_PERF: i32 = 107;
pub const PF_A20_ANONPROV: i32 = 108;
pub const PF_A20_HYP_SELFTEST: i32 = 109;
pub const PF_A20_LOCK_CONTENTION: i32 = 110;
pub const PF_A20_DRIVER_LIFECYCLE: i32 = 111;
pub const PF_A20_SCHED_BASE_SLICE: i32 = 112;
pub const PF_A20_XLATOR: i32 = 113;
pub const PF_A20_OBJECTS: i32 = 114;
pub const PF_A20_IOMMU: i32 = 115;
pub const PF_A20_NETFILTER: i32 = 116;
pub const PF_A20_NETMEM: i32 = 117;
pub const PF_A20_JOURNAL: i32 = 118;
pub const PF_A20_IRQ_AFFINITY: i32 = 119;
pub const PF_CGROUPS: i32 = 120;
pub const PF_SELF: i32 = 121;
pub const PF_FSTYPE: i32 = 122;
pub const PF_SWAPS: i32 = 123;
pub const PF_SYSRQ_TRIGGER: i32 = 124;
/// Number of members, used by the harness assertion.
pub const PF_TYPE_COUNT: i32 = 125;
pub const MAX_SWAPFILES: c_int = 16;
pub const PAGE_SIZE: u64 = 4096;

// VFS file types and modes.
pub const VFS_FT_REGULAR: i32 = 1;
pub const VFS_FT_DIR: i32 = 2;
pub const VFS_FT_SYMLINK: i32 = 3;
pub const VNODE_MAGICLINK: u32 = 0x1;

const S_IFREG: i32 = 0o100000;
const S_IFDIR: i32 = 0o040000;
const S_IFLNK: i32 = 0o120000;

pub const ENOENT: i32 = -2;
pub const ESRCH: i32 = -3;
pub const EACCES: i32 = -13;
pub const ENOMEM: i32 = -12;

/// procns kinds for `resource_kind`.
pub const PROCNS_UTS: i32 = 1;
pub const PROCNS_IPC: i32 = 2;
pub const PROCNS_NET: i32 = 3;
pub const PROCNS_CGROUP: i32 = 4;

// ---------------------------------------------------------------------------
// name helpers
// ---------------------------------------------------------------------------

#[inline]
unsafe fn slen(p: *const u8) -> usize {
    let mut n = 0usize;
    // SAFETY: NUL-terminated by contract.
    while unsafe { *p.add(n) } != 0 {
        n += 1;
    }
    n
}

unsafe fn name_is(p: *const u8, lit: &[u8]) -> bool {
    // SAFETY: p is NUL-terminated.
    let n = unsafe { slen(p) };
    if n != lit.len() {
        return false;
    }
    for (i, &c) in lit.iter().enumerate() {
        // SAFETY: i < n.
        if unsafe { *p.add(i) } != c {
            return false;
        }
    }
    true
}

/// `is_pid_str`: a non-empty run of ASCII digits.
unsafe fn is_pid_str(p: *const u8) -> bool {
    // SAFETY: p is NUL-terminated.
    let n = unsafe { slen(p) };
    if n == 0 {
        return false;
    }
    for i in 0..n {
        // SAFETY: i < n.
        if !unsafe { *p.add(i) }.is_ascii_digit() {
            return false;
        }
    }
    true
}

/// `atoi` over a pid-shaped name (the caller has checked `is_pid_str`).
unsafe fn atoi_pid(p: *const u8) -> i32 {
    let mut v: i64 = 0;
    let mut i = 0usize;
    // SAFETY: p is NUL-terminated.
    while unsafe { *p.add(i) } != 0 {
        // SAFETY: i is within the string.
        let d = unsafe { *p.add(i) } as i64 - b'0' as i64;
        v = v * 10 + d;
        if v > i32::MAX as i64 {
            v = i32::MAX as i64;
        }
        i += 1;
    }
    v as i32
}

/// `parse_fd_name`: all digits, non-empty.  Returns the fd, or -1.
unsafe fn parse_fd_name(p: *const u8) -> i32 {
    if !unsafe { is_pid_str(p) } {
        return -1;
    }
    unsafe { atoi_pid(p) }
}

// ---------------------------------------------------------------------------
// the global name table
// ---------------------------------------------------------------------------

/// `name_to_type`: the ordered `/proc/<name>` table.  Writes the pid carried by
/// a pid-shaped name to `out_pid`.  Names not in the table fall back to
/// `PF_ROOT`, exactly as the C table did.
///
/// # Safety
/// `name` is NUL-terminated; `out_pid` is writable.
#[no_mangle]
pub unsafe extern "C" fn a20_procfs_name_type(
    name: *const u8,
    out_pid: *mut i32,
    xlator_enabled: c_int,
) -> i32 {
    if name.is_null() {
        return PF_ROOT;
    }
    if !out_pid.is_null() {
        // SAFETY: out_pid is writable by contract.
        unsafe { *out_pid = 0 };
    }
    macro_rules! n {
        ($lit:expr, $ty:expr) => {
            // SAFETY: name is NUL-terminated.
            if unsafe { name_is(name, $lit) } {
                return $ty;
            }
        };
    }
    n!(b"meminfo", PF_MEMINFO);
    n!(b"version", PF_VERSION);
    n!(b"uptime", PF_UPTIME);
    n!(b"cpuinfo", PF_CPUINFO);
    n!(b"mounts", PF_MOUNTS);
    n!(b"self", PF_SELF);
    n!(b"loadavg", PF_LOADAVG);
    n!(b"net", PF_NET);
    n!(b"config.gz", PF_CONFIG_GZ);
    n!(b"filesystems", PF_FSTYPE);
    n!(b"cgroups", PF_CGROUPS);
    n!(b"swaps", PF_SWAPS);
    n!(b"sysrq-trigger", PF_SYSRQ_TRIGGER);
    n!(b"interrupts", PF_INTERRUPTS);
    n!(b"pidmap", PF_SYS_KERNEL_PIDMAP);
    n!(b"a20", PF_A20);
    n!(b"bcache", PF_A20_BCACHE);
    n!(b"anonprov", PF_A20_ANONPROV);
    n!(b"hyp_selftest", PF_A20_HYP_SELFTEST);
    n!(b"sched_base_slice", PF_A20_SCHED_BASE_SLICE);
    if xlator_enabled != 0 {
        n!(b"xlator", PF_A20_XLATOR);
    }
    n!(b"page_cache", PF_A20_PAGE_CACHE);
    n!(b"oom", PF_A20_OOM);
    n!(b"task_lifetime", PF_A20_TASK_LIFETIME);
    n!(b"perf", PF_A20_PERF);
    n!(b"lock_contention", PF_A20_LOCK_CONTENTION);
    n!(b"driver_lifecycle", PF_A20_DRIVER_LIFECYCLE);
    n!(b"objects", PF_A20_OBJECTS);
    n!(b"iommu", PF_A20_IOMMU);
    n!(b"irq_affinity", PF_A20_IRQ_AFFINITY);
    n!(b"cmdline", PF_CMDLINE);
    // SAFETY: name is NUL-terminated.
    if unsafe { is_pid_str(name) } {
        if !out_pid.is_null() {
            // SAFETY: out_pid is writable by contract.
            unsafe { *out_pid = atoi_pid(name) };
        }
        return PF_ROOT;
    }
    n!(b"stat", PF_PID_STAT);
    n!(b"status", PF_PID_STATUS);
    n!(b"statm", PF_PID_STATM);
    n!(b"maps", PF_PID_MAPS);
    n!(b"smaps", PF_PID_SMAPS);
    n!(b"oom_score_adj", PF_PID_OOM_SCORE_ADJ);
    n!(b"oom_score", PF_PID_OOM_SCORE);
    n!(b"cgroup", PF_PID_CGROUP);
    n!(b"cmdline", PF_PID_CMDLINE);
    n!(b"comm", PF_PID_COMM);
    n!(b"exe", PF_PID_EXE);
    n!(b"cwd", PF_PID_CWD);
    n!(b"fd", PF_PID_FD);
    n!(b"environ", PF_PID_ENVIRON);
    n!(b"auxv", PF_PID_AUXV);
    n!(b"io", PF_PID_IO);
    n!(b"loginuid", PF_PID_LOGINUID);
    n!(b"sessionid", PF_PID_SESSIONID);
    n!(b"task", PF_PID_TASK);
    n!(b"ns", PF_PID_NS);
    n!(b"pid", PF_PID_NS_PID);
    n!(b"pid_for_children", PF_PID_NS_PID_FOR_CHILDREN);
    n!(b"uts", PF_PID_NS_UTS);
    n!(b"user", PF_PID_NS_USER);
    n!(b"ipc", PF_PID_NS_IPC);
    n!(b"mnt", PF_PID_NS_MNT);
    n!(b"net", PF_PID_NS_NET);
    n!(b"cgroup", PF_PID_NS_CGROUP);
    n!(b"fdinfo", PF_PID_FDINFO);
    n!(b"mountinfo", PF_PID_MOUNTINFO);
    n!(b"pagemap", PF_PID_PAGEMAP);
    n!(b"boot_id", PF_BOOT_ID);
    n!(b"cap_last_cap", PF_CAP_LAST_CAP);
    n!(b"nr_open", PF_NR_OPEN);
    n!(b"pressure", PF_PRESSURE);
    n!(b"uid_map", PF_UID_MAP);
    n!(b"gid_map", PF_GID_MAP);
    n!(b"setgroups", PF_SETGROUPS);
    n!(b"sysvipc", PF_SYSVIPC);
    n!(b"devices", PF_DEVICES);
    n!(b"partitions", PF_PARTITIONS);
    n!(b"diskstats", PF_DISKSTATS);
    n!(b"modules", PF_MODULES);
    n!(b"misc", PF_MISC);
    n!(b"iomem", PF_IOMEM);
    n!(b"ioports", PF_IOPORTS);
    n!(b"softirqs", PF_SOFTIRQS);
    n!(b"route", PF_ROUTE);
    n!(b"arp", PF_ARP);
    n!(b"tty", PF_TTY);
    n!(b"ldiscs", PF_LDISCS);
    n!(b"drivers", PF_DRIVERS);
    n!(b"thread-self", PF_THREAD_SELF);
    n!(b"limits", PF_PID_LIMITS);
    n!(b"wchan", PF_PID_WCHAN);
    n!(b"stack", PF_PID_STACK);
    PF_ROOT
}

/// `procfs_root_file_type`: 1 when a `/proc` root entry is a file.
#[no_mangle]
pub extern "C" fn a20_procfs_root_file_type(ty: i32) -> c_int {
    match ty {
        PF_MEMINFO | PF_STAT | PF_VERSION | PF_UPTIME | PF_CMDLINE | PF_CPUINFO
        | PF_MOUNTS | PF_LOADAVG | PF_NET | PF_CONFIG_GZ | PF_FSTYPE | PF_CGROUPS
        | PF_SWAPS | PF_INTERRUPTS | PF_SYS_KERNEL_PIDMAP | PF_BOOT_ID
        | PF_CAP_LAST_CAP | PF_NR_OPEN | PF_PRESSURE | PF_UID_MAP | PF_GID_MAP
        | PF_SETGROUPS | PF_SYSVIPC | PF_DEVICES | PF_PARTITIONS | PF_DISKSTATS
        | PF_MODULES | PF_MISC | PF_IOMEM | PF_IOPORTS | PF_SOFTIRQS | PF_ROUTE
        | PF_ARP | PF_TTY | PF_LDISCS | PF_DRIVERS | PF_SYSRQ_TRIGGER => 1,
        _ => 0,
    }
}

/// `resource_kind`: the resource namespace a node targets, or 0.
#[no_mangle]
pub extern "C" fn a20_procfs_resource_kind(ty: i32) -> c_int {
    match ty {
        PF_PID_NS_UTS => PROCNS_UTS,
        PF_PID_NS_IPC => PROCNS_IPC,
        PF_PID_NS_NET => PROCNS_NET,
        PF_PID_NS_CGROUP => PROCNS_CGROUP,
        _ => 0,
    }
}

// ---------------------------------------------------------------------------
// node classification
// ---------------------------------------------------------------------------

/// `procfs_lookup`'s vnode classification: file type, magic-link flag and mode.
///
/// # Safety
/// `out_type` and `out_mode` are writable.
#[no_mangle]
pub unsafe extern "C" fn a20_procfs_node_class(
    ty: i32,
    name_is_pid_name: c_int,
    fd_symlink: c_int,
    out_type: *mut i32,
    out_mode: *mut i32,
) -> u32 {
    let magic_symlink = fd_symlink != 0 || ty == PF_PID_EXE || ty == PF_PID_CWD;
    let dir = (ty == PF_ROOT && name_is_pid_name != 0)
        || ty == PF_SELF
        || ty == PF_THREAD_SELF
        || ty == PF_SYS
        || ty == PF_SYS_FS
        || ty == PF_SYS_KERNEL
        || ty == PF_SYS_NET
        || ty == PF_SYS_VM
        || ty == PF_SYS_FS_INOTIFY
        || ty == PF_NET
        || ty == PF_A20
        || ty == PF_PID_FD
        || ty == PF_PID_TASK
        || ty == PF_PID_NS
        || ty == PF_PID_FDINFO
        || ty == PF_PRESSURE;
    let vtype = if magic_symlink {
        VFS_FT_SYMLINK
    } else if dir {
        VFS_FT_DIR
    } else {
        VFS_FT_REGULAR
    };
    let mut mode = if vtype == VFS_FT_SYMLINK {
        S_IFLNK | 0o777
    } else if vtype == VFS_FT_DIR {
        S_IFDIR | 0o555
    } else {
        S_IFREG | 0o444
    };
    if ty == PF_A20_DRIVER_LIFECYCLE || ty == PF_SYSRQ_TRIGGER {
        mode = S_IFREG | 0o200;
    } else if matches!(
        ty,
        PF_PID_OOM_SCORE_ADJ
            | PF_A20_SCHED_BASE_SLICE
            | PF_SYS_FS_PIPE_MAX_SIZE
            | PF_SYS_FS_LEASE_BREAK_TIME
            | PF_A20_IRQ_AFFINITY
            | PF_A20_XLATOR
            | PF_SYS_KERNEL_OVERFLOW_UID
            | PF_SYS_KERNEL_OVERFLOW_GID
            | PF_SYS_KERNEL_SCHED_AUTOGROUP
            | PF_SYS_KERNEL_CORE_PATTERN
            | PF_SYS_VM_DROP_CACHES
            | PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS
            | PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES
            | PF_UID_MAP
            | PF_GID_MAP
            | PF_SETGROUPS
    ) {
        mode = S_IFREG | 0o644;
    }
    if !out_type.is_null() {
        // SAFETY: writable by contract.
        unsafe { *out_type = vtype };
    }
    if !out_mode.is_null() {
        // SAFETY: writable by contract.
        unsafe { *out_mode = mode };
    }
    if magic_symlink {
        VNODE_MAGICLINK
    } else {
        0
    }
}

// ---------------------------------------------------------------------------
// the parent-scoped node table
// ---------------------------------------------------------------------------

/// Probe kinds for entries whose resolution needs a task or fdtable lookup.
pub const PROBE_FD_EXISTS: u32 = 0;
pub const PROBE_PID_GLOBAL: u32 = 1;
pub const PROBE_THREAD_OF_OWNER: u32 = 2;

/// `int (*)(void *ctx, uint32_t which, int64_t arg, int64_t *out)`.
///
/// * `PROBE_FD_EXISTS`: `arg` is the fd; `*out` is 1 when the fd is open.
/// * `PROBE_PID_GLOBAL`: `arg` is the id the reader named; `*out` is the
///   resolved global pid, or 0 when it is not visible.
/// * `PROBE_THREAD_OF_OWNER`: `arg` is the named id; `*out` is the thread's
///   global pid when it shares the owner's tgid, else 0.
///
/// A negative return is an errno the caller should report (e.g. `-EACCES`).
pub type ProcfsProbeFn =
    unsafe extern "C" fn(ctx: *mut c_void, which: u32, arg: i64, out: *mut i64) -> c_int;

/// Rust mirror of `procfs_child_node_t`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcfsChildNode {
    pub ty: i32,
    pub pid: i32,
    pub is_symlink: u32,
    /// The type the vnode classification uses.  It differs from `ty` only for
    /// `/proc/self` and `/proc/thread-self`: those store a `PF_ROOT` entry so
    /// that the directory resolves against the caller, but they are classified
    /// as the `PF_SELF`/`PF_THREAD_SELF` directory they name.
    pub class_type: i32,
}

/// The `procfs_lookup` node table: resolve a name under a parent node.
///
/// Returns 0 with `out` filled, or a negative errno.  `global_type` and
/// `global_pid` are the results of `a20_procfs_name_type` for this name.
///
/// # Safety
/// `name` is NUL-terminated; `out` is writable; `probe` is the adapter's
/// lookup callback and `ctx` is whatever it expects.
#[no_mangle]
pub unsafe extern "C" fn a20_procfs_child_node(
    parent_type: i32,
    parent_pid: i32,
    name: *const u8,
    global_type: i32,
    _global_pid: i32,
    xlator_enabled: c_int,
    ctx: *mut c_void,
    probe: Option<ProcfsProbeFn>,
    out: *mut ProcfsChildNode,
) -> c_int {
    if name.is_null() || out.is_null() {
        return ENOENT;
    }
    let is_root = parent_type == PF_ROOT;
    let root_global = is_root && parent_pid == 0;
    let root_task = is_root && (parent_pid > 0 || parent_pid == -1);

    macro_rules! n {
        ($lit:expr) => {
            // SAFETY: name is NUL-terminated.
            unsafe { name_is(name, $lit) }
        };
    }
    macro_rules! put {
        ($ty:expr, $pid:expr, $sym:expr) => {{
            // SAFETY: out is writable by contract.
            unsafe {
                (*out).ty = $ty;
                (*out).pid = $pid;
                (*out).is_symlink = $sym;
                (*out).class_type = $ty;
            }
            return 0;
        }};
    }
    macro_rules! put_class {
        ($ty:expr, $pid:expr, $sym:expr, $class:expr) => {{
            // SAFETY: out is writable by contract.
            unsafe {
                (*out).ty = $ty;
                (*out).pid = $pid;
                (*out).is_symlink = $sym;
                (*out).class_type = $class;
            }
            return 0;
        }};
    }
    macro_rules! probe {
        ($which:expr, $arg:expr, $err:expr) => {{
            let f = match probe {
                Some(f) => f,
                None => return $err,
            };
            let mut v: i64 = 0;
            // SAFETY: f is the adapter's probe and ctx is its context.
            let r = unsafe { f(ctx, $which, $arg, &mut v) };
            if r < 0 {
                return r;
            }
            v
        }};
    }

    if root_global && n!(b"stat") {
        put!(PF_STAT, 0, 0);
    }
    if root_global && n!(b"sys") {
        put!(PF_SYS, 0, 0);
    }
    if parent_type == PF_PRESSURE && n!(b"cpu") {
        put!(PF_PRESSURE_CPU, 0, 0);
    }
    if parent_type == PF_PRESSURE && n!(b"memory") {
        put!(PF_PRESSURE_MEM, 0, 0);
    }
    if parent_type == PF_PRESSURE && n!(b"io") {
        put!(PF_PRESSURE_IO, 0, 0);
    }
    if parent_type == PF_SYS && n!(b"fs") {
        put!(PF_SYS_FS, 0, 0);
    }
    if parent_type == PF_SYS_FS && n!(b"pipe-max-size") {
        put!(PF_SYS_FS_PIPE_MAX_SIZE, 0, 0);
    }
    if parent_type == PF_SYS_FS && n!(b"lease-break-time") {
        put!(PF_SYS_FS_LEASE_BREAK_TIME, 0, 0);
    }
    if parent_type == PF_SYS && n!(b"kernel") {
        put!(PF_SYS_KERNEL, 0, 0);
    }
    if parent_type == PF_SYS_KERNEL {
        if n!(b"osrelease") {
            put!(PF_SYS_KERNEL_OSRELEASE, 0, 0);
        }
        if n!(b"pid_max") {
            put!(PF_SYS_KERNEL_PID_MAX, 0, 0);
        }
        if n!(b"pidmap") {
            put!(PF_SYS_KERNEL_PIDMAP, 0, 0);
        }
        if n!(b"tainted") {
            put!(PF_SYS_KERNEL_TAINTED, 0, 0);
        }
        if n!(b"sched_autogroup_enabled") {
            put!(PF_SYS_KERNEL_SCHED_AUTOGROUP, 0, 0);
        }
        if n!(b"core_pattern") {
            put!(PF_SYS_KERNEL_CORE_PATTERN, 0, 0);
        }
        if n!(b"hostname") {
            put!(PF_SYS_KERNEL_HOSTNAME, 0, 0);
        }
        if n!(b"domainname") {
            put!(PF_SYS_KERNEL_DOMAINNAME, 0, 0);
        }
        if n!(b"overflowuid") {
            put!(PF_SYS_KERNEL_OVERFLOW_UID, 0, 0);
        }
        if n!(b"overflowgid") {
            put!(PF_SYS_KERNEL_OVERFLOW_GID, 0, 0);
        }
        if n!(b"io_uring_disabled") {
            put!(PF_SYS_KERNEL_IO_URING_DISABLED, 0, 0);
        }
    }
    if parent_type == PF_SYS && n!(b"vm") {
        put!(PF_SYS_VM, 0, 0);
    }
    if parent_type == PF_SYS_VM && n!(b"drop_caches") {
        put!(PF_SYS_VM_DROP_CACHES, 0, 0);
    }
    if parent_type == PF_SYS_FS && n!(b"inotify") {
        put!(PF_SYS_FS_INOTIFY, 0, 0);
    }
    if parent_type == PF_SYS_FS_INOTIFY {
        if n!(b"max_queued_events") {
            put!(PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS, 0, 0);
        }
        if n!(b"max_user_instances") {
            put!(PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES, 0, 0);
        }
    }
    if parent_type == PF_SYS && n!(b"net") {
        put!(PF_SYS_NET, 0, 0);
    }
    if parent_type == PF_NET {
        if n!(b"status") {
            put!(PF_NET_STATUS, 0, 0);
        }
        if n!(b"config") {
            put!(PF_NET_CONFIG, 0, 0);
        }
        if n!(b"route") {
            put!(PF_ROUTE, 0, 0);
        }
        if n!(b"arp") {
            put!(PF_ARP, 0, 0);
        }
        if n!(b"dev") {
            put!(PF_NET_DEV, 0, 0);
        }
        if n!(b"tcp") {
            put!(PF_NET_TCP, 0, 0);
        }
        if n!(b"udp") {
            put!(PF_NET_UDP, 0, 0);
        }
        if n!(b"tcp6") {
            put!(PF_NET_TCP6, 0, 0);
        }
        if n!(b"udp6") {
            put!(PF_NET_UDP6, 0, 0);
        }
        if n!(b"unix") {
            put!(PF_NET_UNIX, 0, 0);
        }
    }
    if root_global && n!(b"a20") {
        put!(PF_A20, 0, 0);
    }
    if parent_type == PF_A20 {
        let a20 = [
            (&b"bcache"[..], PF_A20_BCACHE),
            (&b"page_cache"[..], PF_A20_PAGE_CACHE),
            (&b"oom"[..], PF_A20_OOM),
            (&b"task_lifetime"[..], PF_A20_TASK_LIFETIME),
            (&b"perf"[..], PF_A20_PERF),
            (&b"lock_contention"[..], PF_A20_LOCK_CONTENTION),
            (&b"driver_lifecycle"[..], PF_A20_DRIVER_LIFECYCLE),
            (&b"objects"[..], PF_A20_OBJECTS),
            (&b"iommu"[..], PF_A20_IOMMU),
            (&b"irq_affinity"[..], PF_A20_IRQ_AFFINITY),
            (&b"netfilter"[..], PF_A20_NETFILTER),
            (&b"netmem"[..], PF_A20_NETMEM),
            (&b"journal"[..], PF_A20_JOURNAL),
            (&b"anonprov"[..], PF_A20_ANONPROV),
            (&b"hyp_selftest"[..], PF_A20_HYP_SELFTEST),
            (&b"sched_base_slice"[..], PF_A20_SCHED_BASE_SLICE),
        ];
        for (nm, ty) in a20.iter() {
            // SAFETY: name is NUL-terminated.
            if unsafe { name_is(name, nm) } {
                put!(*ty, 0, 0);
            }
        }
        if xlator_enabled != 0 && n!(b"xlator") {
            put!(PF_A20_XLATOR, 0, 0);
        }
    }
    if root_global && n!(b"interrupts") {
        put!(PF_INTERRUPTS, 0, 0);
    }
    if parent_type == PF_PID_FD {
        let fd = unsafe { parse_fd_name(name) };
        if fd < 0 {
            return ENOENT;
        }
        // SAFETY: probe is the adapter's lookup.
        let ok = probe!(PROBE_FD_EXISTS, fd as i64, ENOENT);
        if ok == 0 {
            return ENOENT;
        }
        put!(PF_PID_FD, parent_pid, 1);
    }
    if parent_type == PF_PID_FDINFO {
        let fd = unsafe { parse_fd_name(name) };
        if fd < 0 {
            return ENOENT;
        }
        // SAFETY: probe is the adapter's lookup.
        let ok = probe!(PROBE_FD_EXISTS, fd as i64, ENOENT);
        if ok == 0 {
            return ENOENT;
        }
        put!(PF_PID_FDINFO_ENTRY, parent_pid, 0);
    }
    if parent_type == PF_PID_TASK {
        // SAFETY: name is NUL-terminated.
        if !unsafe { is_pid_str(name) } {
            return ENOENT;
        }
        // SAFETY: name is a digit run.
        let shown = unsafe { atoi_pid(name) } as i64;
        // SAFETY: probe is the adapter's lookup.
        let global = probe!(PROBE_THREAD_OF_OWNER, shown, ENOENT);
        if global == 0 {
            return ENOENT;
        }
        put!(PF_ROOT, global as i32, 0);
    }
    if parent_type == PF_PID_NS {
        let ns = [
            (&b"pid"[..], PF_PID_NS_PID),
            (&b"pid_for_children"[..], PF_PID_NS_PID_FOR_CHILDREN),
            (&b"user"[..], PF_PID_NS_USER),
            (&b"mnt"[..], PF_PID_NS_MNT),
            (&b"uts"[..], PF_PID_NS_UTS),
            (&b"ipc"[..], PF_PID_NS_IPC),
            (&b"net"[..], PF_PID_NS_NET),
            (&b"cgroup"[..], PF_PID_NS_CGROUP),
        ];
        for (nm, ty) in ns.iter() {
            // SAFETY: name is NUL-terminated.
            if unsafe { name_is(name, nm) } {
                put!(*ty, parent_pid, 0);
            }
        }
        return ENOENT;
    }
    if root_global && (global_type == PF_SELF || global_type == PF_THREAD_SELF) {
        put_class!(PF_ROOT, -1, 0, global_type);
    }
    if root_global && unsafe { is_pid_str(name) } {
        // SAFETY: name is a digit run.
        let shown = unsafe { atoi_pid(name) } as i64;
        // SAFETY: probe is the adapter's lookup.
        let global = probe!(PROBE_PID_GLOBAL, shown, ENOENT);
        if global == 0 {
            return ENOENT;
        }
        put!(PF_ROOT, global as i32, 0);
    }
    if root_task && n!(b"cmdline") {
        put!(PF_PID_CMDLINE, parent_pid, 0);
    }
    if root_task && n!(b"mounts") {
        put!(PF_MOUNTS, 0, 0);
    }
    if root_task && n!(b"cgroup") {
        put!(PF_PID_CGROUP, parent_pid, 0);
    }
    if root_task {
        let pid_files = [
            PF_PID_STAT,
            PF_PID_STATUS,
            PF_PID_STATM,
            PF_PID_MAPS,
            PF_PID_SMAPS,
            PF_PID_OOM_SCORE_ADJ,
            PF_PID_OOM_SCORE,
            PF_PID_CGROUP,
            PF_PID_COMM,
            PF_PID_EXE,
            PF_PID_CWD,
            PF_PID_FD,
            PF_PID_ENVIRON,
            PF_PID_AUXV,
            PF_PID_IO,
            PF_PID_LOGINUID,
            PF_PID_SESSIONID,
            PF_PID_TASK,
            PF_PID_NS,
            PF_PID_FDINFO,
            PF_PID_MOUNTINFO,
            PF_PID_PAGEMAP,
            PF_PID_LIMITS,
            PF_PID_WCHAN,
            PF_PID_STACK,
            PF_UID_MAP,
            PF_GID_MAP,
            PF_SETGROUPS,
        ];
        if pid_files.contains(&global_type) {
            put!(global_type, parent_pid, 0);
        }
        return ENOENT;
    }
    if !root_global {
        return ENOENT;
    }
    if a20_procfs_root_file_type(global_type) == 0 {
        return ENOENT;
    }
    put!(global_type, 0, 0);
}

// ---------------------------------------------------------------------------
// content plan
// ---------------------------------------------------------------------------

/// Which generator renders a node.
pub const GEN_CONTENT: u32 = 0;
/// `generate_pid_fdinfo(pid, fd, buf, cap)`.
pub const GEN_FDINFO: u32 = 1;
/// `generate_pid_maps_alloc(pid, smaps, &buf, &len)`.
pub const GEN_MAPS_ALLOC: u32 = 2;
/// No buffer: the node's length is the fixed `pagemap` size.
pub const GEN_PAGEMAP_LEN: u32 = 3;
/// Diagnostic snapshot: retry with a larger buffer on formatter saturation.
pub const GEN_GROW_CONTENT: u32 = 4;

/// The `(256 GiB / PAGE_SIZE) * 8` pagemap length from the pre-migration code.
/// `page_size` is a parameter so the engine follows the profile's page size.
#[inline]
pub const fn pagemap_len(page_size: u64) -> u64 {
    (256u64 * 1024 * 1024 * 1024 / page_size) * 8
}

/// Rust mirror of `procfs_content_plan_t`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcfsContentPlan {
    pub generator: u32,
    /// Buffer capacity for `GEN_CONTENT`/`GEN_FDINFO`.
    pub capacity: usize,
    /// Node length when the generator needs no buffer.
    pub fixed_len: u64,
    /// 1 when the vnode-size path reports length 0 for this node.
    pub meta_len_zero: u32,
}

/// Decide how a procfs node's content is produced: the generator, its buffer
/// capacity and the length the vnode advertises before the first read.
///
/// # Safety
/// `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn a20_procfs_content_plan(
    ty: i32,
    nr_cpus: u32,
    page_size: u64,
    out: *mut ProcfsContentPlan,
) {
    if out.is_null() {
        return;
    }
    let plan = if ty == PF_PID_PAGEMAP {
        ProcfsContentPlan {
            generator: GEN_PAGEMAP_LEN,
            capacity: 0,
            fixed_len: pagemap_len(page_size),
            meta_len_zero: 0,
        }
    } else if ty == PF_PID_MAPS || ty == PF_PID_SMAPS {
        ProcfsContentPlan {
            generator: GEN_MAPS_ALLOC,
            capacity: 0,
            fixed_len: 0,
            meta_len_zero: 1,
        }
    } else if ty == PF_PID_FDINFO_ENTRY {
        ProcfsContentPlan {
            generator: GEN_FDINFO,
            capacity: 4096,
            fixed_len: 0,
            meta_len_zero: 1,
        }
    } else if ty == PF_A20_PERF || ty == PF_A20_LOCK_CONTENTION || ty == PF_PID_ENVIRON {
        // stat/lookup must neither arm collection nor take a partial snapshot.
        // Open starts small; the C adapter grows only if the formatter fills it.
        ProcfsContentPlan {
            generator: GEN_GROW_CONTENT,
            capacity: 4096,
            fixed_len: 0,
            meta_len_zero: 1,
        }
    } else if ty == PF_CPUINFO {
        // CPU records scale with the configured SMP capacity.
        ProcfsContentPlan {
            generator: GEN_CONTENT,
            capacity: 512 * nr_cpus as usize + 1,
            fixed_len: 0,
            meta_len_zero: 1,
        }
    } else {
        ProcfsContentPlan {
            generator: GEN_CONTENT,
            capacity: 4096,
            fixed_len: 0,
            meta_len_zero: 0,
        }
    };
    // SAFETY: out is writable by contract.
    unsafe { *out = plan };
}

// ---------------------------------------------------------------------------
// content generation
// ---------------------------------------------------------------------------

use core::ffi::c_char;
use core::fmt::{self, Write};

pub type GenStrFn = extern "C" fn() -> *const c_char;
pub type GenU64Fn = extern "C" fn() -> u64;
pub type GenU32Fn = extern "C" fn() -> u32;
pub type GenI32Fn = extern "C" fn() -> c_int;
pub type GenLoadavgFn =
    extern "C" fn(*mut u64, *mut u64, *mut u64, *mut u32, *mut u32, *mut c_int);
pub type MeminfoFn = extern "C" fn(*mut A20MeminfoRaw);
pub type MountCountFn = extern "C" fn() -> c_int;
pub type MountAtFn = extern "C" fn(c_int, *mut A20MountEnt) -> c_int;
pub type CpuTimesFn = extern "C" fn(c_int, *mut u64, *mut u64, *mut u64) -> c_int;
pub type U64OutFn = extern "C" fn(*mut u64);
pub type SwapGetFn = extern "C" fn(c_int, *mut A20SwapEnt) -> c_int;
pub type CpuinfoFieldsFn = extern "C" fn(*mut c_char, usize) -> c_int;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20SwapEnt {
    pub name: [u8; 64],
    pub pages: u64,
    pub inuse_pages: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20BcacheRaw {
    pub caches: u64,
    pub block_pool_bytes: u64,
    pub page_pool_bytes: u64,
    pub valid_blocks: u64,
    pub dirty_blocks: u64,
    pub valid_pages: u64,
    pub dirty_pages: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20PagecacheRaw {
    pub capacity: u64,
    pub allocated: u64,
    pub bytes: u64,
    pub valid: u64,
    pub dirty: u64,
    pub pinned: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20OomRaw {
    pub kills: u64,
    pub last_kill_tick: u64,
    pub last_victim_pid: i32,
    pub last_victim_score: i32,
    pub free_pages_at_kill: u64,
    pub free_pages_now: u64,
    pub in_progress: i32,
    pub kswapd_passes: u64,
    pub kswapd_pages_freed: u64,
    pub kswapd_last_pass_tick: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20ObjstatsRaw {
    pub handles: u64,
    pub channel_eps: u64,
    pub eventqs: u64,
    pub vmos: u64,
    pub vmo_pages: u64,
    pub irq_bindings: u64,
    pub vfiles: u64,
    pub vmo_dirty_frames: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20PidRaw {
    pub pid: i32,
    pub ppid: i32,
    pub pgid: i32,
    pub sid: i32,
    pub name: [u8; 64],
    pub state_char: u8,
    pub state_text: [u8; 32],
    pub utime_ticks: u64,
    pub stime_ticks: u64,
    pub child_utime: i64,
    pub child_stime: i64,
    pub priority: i32,
    pub start_jiffies: u64,
    pub vsize: u64,
    pub rss_pages: i64,
    pub processor: u32,
    pub uid: i32,
    pub euid: i32,
    pub suid: i32,
    pub fsuid: i32,
    pub gid: i32,
    pub egid: i32,
    pub sgid: i32,
    pub fsgid: i32,
    pub cap_inh: u64,
    pub cap_prm: u64,
    pub cap_eff: u64,
    pub cap_bnd: u64,
    pub ngroups: u32,
    pub groups: [i32; 64],
    pub rss_kb: u64,
    pub vmlck_kb: u64,
    pub vmdata_kb: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20MapEnt {
    pub lower: u32,
    pub parent_lower: u32,
    pub count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20MapRaw {
    pub n: u32,
    pub ent: [A20MapEnt; 16],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20MsixEnt {
    pub name: [u8; 32],
    pub index: u32,
    pub vector: u32,
    pub cpu: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20MsixRaw {
    pub total: u32,
    pub n: u32,
    pub ent: [A20MsixEnt; 64],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20LimitsRaw {
    pub nofile: u64,
    pub memlock: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20PidIoRaw {
    pub rchar: u64,
    pub wchar: u64,
    pub syscr: u64,
    pub syscw: u64,
    pub read_bytes: u64,
    pub write_bytes: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20SysvipcRaw {
    pub msg: i32,
    pub sem: i32,
    pub shm: i32,
}

pub type SysvipcFn = extern "C" fn(*mut A20SysvipcRaw);
pub type ConfigGzFn = extern "C" fn(*mut *const u8, *mut usize);
pub type OverflowIdFn = extern "C" fn(c_int) -> c_int;
pub type PsiRenderFn = extern "C" fn(c_int, *mut c_char, usize) -> c_int;
pub type PidRawFn = extern "C" fn(c_int, *mut A20PidRaw) -> c_int;
pub type PidStrFn = extern "C" fn(c_int, c_int, *mut c_char, usize) -> c_int;
pub type PidIoFn = extern "C" fn(c_int, *mut A20PidIoRaw) -> c_int;
pub type SessionIdFn = extern "C" fn(c_int) -> c_int;
pub type PidNsInoFn = extern "C" fn(c_int, c_int) -> u64;
pub type FormatHelperFn = extern "C" fn(c_int, *mut c_char, usize) -> c_int;
pub type PidOomFn = extern "C" fn(c_int, *mut c_int) -> c_int;
pub type UsernsMapFn = extern "C" fn(c_int, c_int, *mut A20MapRaw) -> c_int;
pub type UsernsSetgroupsFn = extern "C" fn(c_int, *mut c_int) -> c_int;
pub type CgroupPathFn = extern "C" fn(c_int, *mut c_char, usize) -> c_int;
pub type PidLimitsFn = extern "C" fn(c_int, *mut A20LimitsRaw) -> c_int;
pub type PidAuxvFn = extern "C" fn(c_int, *mut u8, u32) -> c_int;
pub type PidExistsFn = extern "C" fn(c_int) -> c_int;
pub type MsixSnapshotFn = extern "C" fn(*mut A20MsixRaw) -> u32;
pub type NetSnapshotFn = extern "C" fn(c_int, *mut *mut A20ProcfsNetRow, *mut usize) -> c_int;
pub type NetSnapshotFreeFn = extern "C" fn(*mut A20ProcfsNetRow);
pub type BufFillFn = extern "C" fn(*mut c_char, usize) -> c_int;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20IommuRaw {
    pub enabled: i32,
    pub domains_claimed: u64,
    pub domains_released: u64,
    pub maps: u64,
    pub map_failures: u64,
    pub unmaps: u64,
    pub mapped_pages: u64,
    pub fault_records: u64,
    pub faults: u64,
    pub blocked_events: u64,
    pub last_fault_devid: u32,
    pub last_fault_cause: u64,
    pub last_fault_iova: u64,
    pub last_fault_owner: i32,
}

pub type BcacheFn = extern "C" fn(*mut A20BcacheRaw);
pub type PagecacheFn = extern "C" fn(*mut A20PagecacheRaw);
pub type OomFn = extern "C" fn(*mut A20OomRaw);
pub type ObjstatsFn = extern "C" fn(*mut A20ObjstatsRaw);
pub type IommuFn = extern "C" fn(*mut A20IommuRaw);

/// Raw meminfo counters (bytes/frames/pages); Rust does every kB conversion.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20MeminfoRaw {
    pub total_frames: u64,
    pub free_frames: u64,
    pub slab_total_bytes: u64,
    pub slab_reclaim_bytes: u64,
    pub bcache_pool_bytes: u64,
    pub bcache_valid_pages: u64,
    pub bcache_dirty_blocks: u64,
    pub bcache_dirty_pages: u64,
    pub pcache_valid: u64,
    pub pcache_dirty: u64,
    pub vm_anon_huge: u64,
    pub vm_shmem_huge: u64,
    pub vm_file_huge: u64,
    pub huge_total: u64,
    pub huge_free: u64,
    pub swap_total_pages: u64,
    pub swap_free_pages: u64,
    pub page_size: u64,
    pub pcache_page_size: u64,
    pub bcache_block_size: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20MountEnt {
    pub dev: [u8; 32],
    pub path: [u8; 256],
    pub fstype: [u8; 32],
    pub opts: [u8; 32],
    pub mnt_id: u32,
    pub parent_id: u32,
    pub detached: u8,
}

/// Narrow mechanical bridge for the procfs content generators.  Field order
/// mirrors `struct a20_procfs_gen` in `fs/procfs_node_state.h`.
#[repr(C)]
pub struct A20ProcfsGen {
    pub sysname: Option<GenStrFn>,
    pub release: Option<GenStrFn>,
    pub arch_name: Option<GenStrFn>,
    pub version_full: Option<GenStrFn>,
    pub ticks: Option<GenU64Fn>,
    pub ticks_per_sec: Option<GenU64Fn>,
    pub bootargs: Option<GenStrFn>,
    pub loadavg: Option<GenLoadavgFn>,
    pub anonprov_max: Option<GenU32Fn>,
    pub sched_base_slice: Option<GenI32Fn>,
    pub pid_max: Option<GenI32Fn>,
    pub current_pid: Option<GenI32Fn>,
    pub pipe_max_size: Option<GenI32Fn>,
    pub lease_break_time: Option<GenI32Fn>,
    pub max_files: Option<GenI32Fn>,
    pub meminfo_raw: Option<MeminfoFn>,
    pub mount_count: Option<MountCountFn>,
    pub mount_at: Option<MountAtFn>,
    pub nr_cpus: Option<GenI32Fn>,
    pub cpu_times: Option<CpuTimesFn>,
    pub realtime: Option<U64OutFn>,
    pub monotonic: Option<U64OutFn>,
    pub tasks_created: Option<GenU64Fn>,
    pub runq_load_sum: Option<GenU64Fn>,
    pub swap_get: Option<SwapGetFn>,
    pub online_cpu_mask: Option<GenU32Fn>,
    pub cpuinfo_fields: Option<CpuinfoFieldsFn>,
    pub bcache_raw: Option<BcacheFn>,
    pub pagecache_raw: Option<PagecacheFn>,
    pub oom_raw: Option<OomFn>,
    pub objstats_raw: Option<ObjstatsFn>,
    pub iommu_raw: Option<IommuFn>,
    pub sysvipc_raw: Option<SysvipcFn>,
    pub config_gz: Option<ConfigGzFn>,
    pub userns_overflow_id: Option<OverflowIdFn>,
    pub hostname: Option<GenStrFn>,
    pub domainname: Option<GenStrFn>,
    pub psi_render: Option<PsiRenderFn>,
    pub pidmap: Option<BufFillFn>,
    pub pid_raw: Option<PidRawFn>,
    pub pid_str: Option<PidStrFn>,
    pub pid_io: Option<PidIoFn>,
    pub pid_sessionid: Option<SessionIdFn>,
    pub pid_ns_ino: Option<PidNsInoFn>,
    pub format_helper: Option<FormatHelperFn>,
    pub hyp_selftest: Option<GenI32Fn>,
    pub pid_oom: Option<PidOomFn>,
    pub pid_cgroup_path: Option<CgroupPathFn>,
    pub userns_map: Option<UsernsMapFn>,
    pub userns_setgroups_allowed: Option<UsernsSetgroupsFn>,
    pub pid_exists: Option<PidExistsFn>,
    pub pid_limits: Option<PidLimitsFn>,
    pub pid_wchan: Option<CgroupPathFn>,
    pub pid_auxv: Option<PidAuxvFn>,
    pub msix_cpu_count: Option<GenI32Fn>,
    pub msix_snapshot: Option<MsixSnapshotFn>,
    pub net_snapshot: Option<NetSnapshotFn>,
    pub net_snapshot_free: Option<NetSnapshotFreeFn>,
}

/// Value-only VMA/smaps snapshot.  Kernel locks and page-table inspection are
/// performed by the C adapter; this type contains no pointers except the
/// fixed name storage, so Rust can render after those locks have been dropped.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct A20ProcfsMapStats {
    pub rss_pages: usize,
    pub shared_clean_pages: usize,
    pub shared_dirty_pages: usize,
    pub private_clean_pages: usize,
    pub private_dirty_pages: usize,
    pub anonymous_pages: usize,
    pub anon_huge_pages: usize,
    pub shmem_pmd_pages: usize,
    pub file_pmd_pages: usize,
}

const PROCFS_MAP_NAME_MAX: usize = 512;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20ProcfsMapRecord {
    pub start: u64,
    pub end: u64,
    pub vm_flags: u64,
    pub file_offset: u64,
    pub ino: core::ffi::c_ulong,
    pub thp_eligible: i32,
    pub name: [u8; PROCFS_MAP_NAME_MAX],
    pub stats: A20ProcfsMapStats,
}

pub const PROCFS_NET_ADDR_MAX: usize = 128;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20ProcfsNetRow {
    pub reg_idx: i32,
    pub domain: i32,
    pub ty: i32,
    pub tcp_flags: i32,
    pub local_len: i32,
    pub peer_len: i32,
    pub rx_queued: u32,
    pub owner_uid: i32,
    pub recv_timeout_ticks: i64,
    pub local: [u8; PROCFS_NET_ADDR_MAX],
    pub peer: [u8; PROCFS_NET_ADDR_MAX],
}

const VM_READ: u64 = 1 << 0;
const VM_WRITE: u64 = 1 << 1;
const VM_EXEC: u64 = 1 << 2;
const VM_SHARED: u64 = 1 << 3;
const VM_ANON: u64 = 1 << 4;
const VM_STACK: u64 = 1 << 5;
const VM_HUGEPAGE: u64 = 1 << 11;
const VM_NOHUGEPAGE: u64 = 1 << 12;

/// Render maps/smaps from a stable raw snapshot. The caller retains allocation
/// ownership; this function only writes into the provided buffer.
///
/// # Safety
/// `records` points to `count` readable records and `buf` to `bufsz` writable
/// bytes. `bufsz` must be nonzero.
#[no_mangle]
pub unsafe extern "C" fn a20_procfs_render_maps(
    records: *const A20ProcfsMapRecord,
    count: usize,
    smaps: c_int,
    buf: *mut c_char,
    bufsz: usize,
) -> c_int {
    if buf.is_null() || bufsz == 0 || (count != 0 && records.is_null()) {
        return -22;
    }
    // SAFETY: the caller provides writable output storage.
    unsafe { *buf = 0; }
    let cap = bufsz - 1;
    let mut w = W { p: buf as *mut u8, cap, len: 0, over: false };
    for i in 0..count {
        // SAFETY: records points to count initialized records by contract.
        let r = unsafe { &*records.add(i) };
        let perms = [
            if r.vm_flags & VM_READ != 0 { 'r' } else { '-' },
            if r.vm_flags & VM_WRITE != 0 { 'w' } else { '-' },
            if r.vm_flags & VM_EXEC != 0 { 'x' } else { '-' },
            if r.vm_flags & VM_SHARED != 0 { 's' } else { 'p' },
        ];
        let name = zero_terminated(&r.name);
        if name.is_empty() {
            let _ = write!(w, "{:08x}-{:08x} {}{}{}{} {:08x} 00:00 {}\n",
                r.start as usize, r.end as usize, perms[0], perms[1], perms[2], perms[3],
                r.file_offset as usize, r.ino as usize);
        } else {
            let _ = write!(w, "{:08x}-{:08x} {}{}{}{} {:08x} 00:00 {} ",
                r.start as usize, r.end as usize, perms[0], perms[1], perms[2], perms[3],
                r.file_offset as usize, r.ino as usize);
            let _ = w.bytes(name);
            let _ = w.bytes(b"\n");
        }
        if smaps == 0 || w.over { continue; }
        let kb = (r.end.saturating_sub(r.start) / 1024) as usize;
        let rss = r.stats.rss_pages.saturating_mul(PAGE_SIZE as usize) / 1024;
        let page_kb = PAGE_SIZE as usize / 1024;
        let st = &r.stats;
        let _ = write!(w,
            "Size:           {:8} kB\nKernelPageSize: {:8} kB\nMMUPageSize:    {:8} kB\nRss:            {:8} kB\nPss:            {:8} kB\nShared_Clean:   {:8} kB\nShared_Dirty:   {:8} kB\nPrivate_Clean:  {:8} kB\nPrivate_Dirty:  {:8} kB\nReferenced:     {:8} kB\nAnonymous:      {:8} kB\nAnonHugePages:  {:8} kB\nShmemPmdMapped: {:8} kB\nFilePmdMapped:  {:8} kB\nTHPeligible:    {:8}\n",
            kb, page_kb, page_kb, rss, rss,
            st.shared_clean_pages.saturating_mul(PAGE_SIZE as usize) / 1024,
            st.shared_dirty_pages.saturating_mul(PAGE_SIZE as usize) / 1024,
            st.private_clean_pages.saturating_mul(PAGE_SIZE as usize) / 1024,
            st.private_dirty_pages.saturating_mul(PAGE_SIZE as usize) / 1024,
            rss, st.anonymous_pages.saturating_mul(PAGE_SIZE as usize) / 1024,
            st.anon_huge_pages.saturating_mul(PAGE_SIZE as usize) / 1024,
            st.shmem_pmd_pages.saturating_mul(PAGE_SIZE as usize) / 1024,
            st.file_pmd_pages.saturating_mul(PAGE_SIZE as usize) / 1024,
            r.thp_eligible);
        let _ = w.bytes(b"VmFlags:");
        for (bit, text) in [(VM_READ,&b" rd"[..]), (VM_WRITE,&b" wr"[..]),
            (VM_EXEC,&b" ex"[..]), (VM_STACK,&b" gd"[..]), (VM_HUGEPAGE,&b" hg"[..]),
            (VM_NOHUGEPAGE,&b" nh"[..])] {
            if r.vm_flags & bit != 0 { let _ = w.bytes(text); }
        }
        let _ = w.bytes(b" mr mw me ac\n");
        if w.over { break; }
    }
    // SAFETY: len is bounded by bufsz - 1.
    unsafe { *buf.add(w.len) = 0; }
    w.len as c_int
}

/// Render `/proc/<pid>/fdinfo/<fd>` after the adapter has pinned the file and
/// sampled its offset, flags and inode.  `pid` is the pid a pidfd refers to
/// (Linux reports it as `Pid:`/`NSpid:`); it is <= 0 for every other file.
#[no_mangle]
pub unsafe extern "C" fn a20_procfs_render_fdinfo(
    pos: usize,
    flags: u32,
    ino: core::ffi::c_ulong,
    pid: c_int,
    buf: *mut c_char,
    bufsz: usize,
) -> c_int {
    if buf.is_null() || bufsz == 0 { return -22; }
    unsafe { *buf = 0; }
    let mut w = W { p: buf as *mut u8, cap: bufsz - 1, len: 0, over: false };
    let _ = write!(w, "pos:\t{}\nflags:\t0{:o}\nino:\t{}\n", pos, flags, ino);
    if pid > 0 {
        let _ = write!(w, "Pid:\t{}\nNSpid:\t{}\n", pid, pid);
    }
    unsafe { *buf.add(w.len) = 0; }
    w.len as c_int
}

struct Gen<'a> {
    sysname: &'a GenStrFn,
    release: &'a GenStrFn,
    arch_name: &'a GenStrFn,
    version_full: &'a GenStrFn,
    ticks: &'a GenU64Fn,
    ticks_per_sec: &'a GenU64Fn,
    bootargs: &'a GenStrFn,
    loadavg: &'a GenLoadavgFn,
    anonprov_max: &'a GenU32Fn,
    sched_base_slice: &'a GenI32Fn,
    pid_max: &'a GenI32Fn,
    current_pid: &'a GenI32Fn,
    pipe_max_size: &'a GenI32Fn,
    lease_break_time: &'a GenI32Fn,
    max_files: &'a GenI32Fn,
    meminfo_raw: &'a MeminfoFn,
    mount_count: &'a MountCountFn,
    mount_at: &'a MountAtFn,
    nr_cpus: &'a GenI32Fn,
    cpu_times: &'a CpuTimesFn,
    realtime: &'a U64OutFn,
    monotonic: &'a U64OutFn,
    tasks_created: &'a GenU64Fn,
    runq_load_sum: &'a GenU64Fn,
    swap_get: &'a SwapGetFn,
    online_cpu_mask: &'a GenU32Fn,
    cpuinfo_fields: &'a CpuinfoFieldsFn,
    bcache_raw: &'a BcacheFn,
    pagecache_raw: &'a PagecacheFn,
    oom_raw: &'a OomFn,
    objstats_raw: &'a ObjstatsFn,
    iommu_raw: &'a IommuFn,
    sysvipc_raw: &'a SysvipcFn,
    config_gz: &'a ConfigGzFn,
    userns_overflow_id: &'a OverflowIdFn,
    hostname: &'a GenStrFn,
    domainname: &'a GenStrFn,
    psi_render: &'a PsiRenderFn,
    pidmap: &'a BufFillFn,
    pid_raw: &'a PidRawFn,
    pid_str: &'a PidStrFn,
    pid_io: &'a PidIoFn,
    pid_sessionid: &'a SessionIdFn,
    pid_ns_ino: &'a PidNsInoFn,
    format_helper: &'a FormatHelperFn,
    hyp_selftest: &'a GenI32Fn,
    pid_oom: &'a PidOomFn,
    pid_cgroup_path: &'a CgroupPathFn,
    userns_map: &'a UsernsMapFn,
    userns_setgroups_allowed: &'a UsernsSetgroupsFn,
    pid_exists: &'a PidExistsFn,
    pid_limits: &'a PidLimitsFn,
    pid_wchan: &'a CgroupPathFn,
    pid_auxv: &'a PidAuxvFn,
    msix_cpu_count: &'a GenI32Fn,
    msix_snapshot: &'a MsixSnapshotFn,
    net_snapshot: &'a NetSnapshotFn,
    net_snapshot_free: &'a NetSnapshotFreeFn,
}

impl<'a> Gen<'a> {
    unsafe fn load(t: *const A20ProcfsGen) -> Option<Gen<'a>> {
        if t.is_null() {
            return None;
        }
        // SAFETY: caller passes a live table for the duration of the call.
        let t = unsafe { &*t };
        Some(Gen {
            sysname: t.sysname.as_ref()?,
            release: t.release.as_ref()?,
            arch_name: t.arch_name.as_ref()?,
            version_full: t.version_full.as_ref()?,
            ticks: t.ticks.as_ref()?,
            ticks_per_sec: t.ticks_per_sec.as_ref()?,
            bootargs: t.bootargs.as_ref()?,
            loadavg: t.loadavg.as_ref()?,
            anonprov_max: t.anonprov_max.as_ref()?,
            sched_base_slice: t.sched_base_slice.as_ref()?,
            pid_max: t.pid_max.as_ref()?,
            current_pid: t.current_pid.as_ref()?,
            pipe_max_size: t.pipe_max_size.as_ref()?,
            lease_break_time: t.lease_break_time.as_ref()?,
            max_files: t.max_files.as_ref()?,
            meminfo_raw: t.meminfo_raw.as_ref()?,
            mount_count: t.mount_count.as_ref()?,
            mount_at: t.mount_at.as_ref()?,
            nr_cpus: t.nr_cpus.as_ref()?,
            cpu_times: t.cpu_times.as_ref()?,
            realtime: t.realtime.as_ref()?,
            monotonic: t.monotonic.as_ref()?,
            tasks_created: t.tasks_created.as_ref()?,
            runq_load_sum: t.runq_load_sum.as_ref()?,
            swap_get: t.swap_get.as_ref()?,
            online_cpu_mask: t.online_cpu_mask.as_ref()?,
            cpuinfo_fields: t.cpuinfo_fields.as_ref()?,
            bcache_raw: t.bcache_raw.as_ref()?,
            pagecache_raw: t.pagecache_raw.as_ref()?,
            oom_raw: t.oom_raw.as_ref()?,
            objstats_raw: t.objstats_raw.as_ref()?,
            iommu_raw: t.iommu_raw.as_ref()?,
            sysvipc_raw: t.sysvipc_raw.as_ref()?,
            config_gz: t.config_gz.as_ref()?,
            userns_overflow_id: t.userns_overflow_id.as_ref()?,
            hostname: t.hostname.as_ref()?,
            domainname: t.domainname.as_ref()?,
            psi_render: t.psi_render.as_ref()?,
            pidmap: t.pidmap.as_ref()?,
            pid_raw: t.pid_raw.as_ref()?,
            pid_str: t.pid_str.as_ref()?,
            pid_io: t.pid_io.as_ref()?,
            pid_sessionid: t.pid_sessionid.as_ref()?,
            pid_ns_ino: t.pid_ns_ino.as_ref()?,
            format_helper: t.format_helper.as_ref()?,
            hyp_selftest: t.hyp_selftest.as_ref()?,
            pid_oom: t.pid_oom.as_ref()?,
            pid_cgroup_path: t.pid_cgroup_path.as_ref()?,
            userns_map: t.userns_map.as_ref()?,
            userns_setgroups_allowed: t.userns_setgroups_allowed.as_ref()?,
            pid_exists: t.pid_exists.as_ref()?,
            pid_limits: t.pid_limits.as_ref()?,
            pid_wchan: t.pid_wchan.as_ref()?,
            pid_auxv: t.pid_auxv.as_ref()?,
            msix_cpu_count: t.msix_cpu_count.as_ref()?,
            msix_snapshot: t.msix_snapshot.as_ref()?,
            net_snapshot: t.net_snapshot.as_ref()?,
            net_snapshot_free: t.net_snapshot_free.as_ref()?,
        })
    }
}

/// Bounded writer over the caller's content buffer.  Overflow latches `over`
/// and stops emitting, matching the truncating `snprintf`/`appendf` contract.
struct W {
    p: *mut u8,
    cap: usize,
    len: usize,
    over: bool,
}

impl W {
    fn bytes(&mut self, b: &[u8]) -> fmt::Result {
        let avail = self.cap - self.len;
        if b.len() > avail {
            /* Truncate like snprintf: keep the fitting prefix, then stop. */
            if avail > 0 {
                // SAFETY: `p` has `cap` writable bytes and len + avail == cap.
                unsafe { core::ptr::copy_nonoverlapping(b.as_ptr(), self.p.add(self.len), avail) };
            }
            self.len += avail;
            self.over = true;
            return Err(fmt::Error);
        }
        if !b.is_empty() {
            // SAFETY: `p` has `cap` writable bytes and len + b.len() <= cap.
            unsafe { core::ptr::copy_nonoverlapping(b.as_ptr(), self.p.add(self.len), b.len()) };
        }
        self.len += b.len();
        Ok(())
    }

    fn cstr(&mut self, p: *const c_char) {
        if p.is_null() {
            return;
        }
        // SAFETY: NUL-terminated by contract.
        let b = unsafe { core::ffi::CStr::from_ptr(p) }.to_bytes();
        let _ = self.bytes(b);
    }
}

impl Write for W {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.bytes(s.as_bytes())
    }
}

const AF_UNIX_PROC: i32 = 1;
const AF_INET_PROC: i32 = 2;
const AF_INET6_PROC: i32 = 10;
const SOCK_STREAM_PROC: i32 = 1;
const TCP_LISTEN_PROC: u8 = 1 << 0;
const TCP_CONNECTING_PROC: u8 = 1 << 1;
const TCP_CONNECTED_PROC: u8 = 1 << 2;
const TCP_PEER_CLOSED_PROC: u8 = 1 << 3;

fn net_u16_be(p: &[u8]) -> u16 {
    if p.len() < 4 { 0 } else { u16::from_be_bytes([p[2], p[3]]) }
}

fn net_le32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}

fn net_state(row: &A20ProcfsNetRow, tcp: bool) -> u8 {
    if !tcp { return 0x07; }
    let f = row.tcp_flags as u8;
    if f & TCP_LISTEN_PROC != 0 { 0x0a }
    else if f & TCP_CONNECTING_PROC != 0 { 0x02 }
    else if f & TCP_CONNECTED_PROC != 0 {
        if f & TCP_PEER_CLOSED_PROC != 0 { 0x08 } else { 0x01 }
    } else { 0x07 }
}

fn render_net_addr(w: &mut W, data: &[u8], domain: i32) {
    let len = data.len();
    let port = net_u16_be(data);
    if domain == AF_INET6_PROC {
        let addr = if len >= 24 { &data[8..24] } else { &[0u8; 16][..] };
        for i in 0..4 {
            if i != 0 { let _ = w.bytes(b""); }
            let _ = write!(w, "{:08X}", net_le32(&addr[i * 4..i * 4 + 4]));
        }
        let _ = write!(w, ":{:04X}", port);
    } else {
        let v4 = if len >= 8 && domain == AF_INET_PROC { net_le32(&data[4..8]) } else { 0 };
        let _ = write!(w, "{:08X}:{:04X}", v4, port);
    }
}

fn render_net_rows(kind: c_int, rows: &[A20ProcfsNetRow], buf: *mut c_char,
                   bufsz: usize) -> c_int {
    if buf.is_null() || bufsz == 0 { return -22; }
    unsafe { *buf = 0; }
    let mut w = W { p: buf as *mut u8, cap: bufsz - 1, len: 0, over: false };
    if kind == PF_NET_UNIX {
        let _ = w.bytes(b"Num       RefCount Protocol Flags    Type St Inode Path\n");
        for r in rows {
            let _ = write!(w, "0000000000000000: 00000000 00000000 00000000 {:04X} {:02X} 0 ",
                r.ty as u16, net_state(r, true));
            let len = (r.local_len.max(0) as usize).min(r.local.len());
            if len > 2 && r.local[2] != 0 {
                let path = &r.local[2..len];
                let n = path.iter().position(|&b| b == 0).unwrap_or(path.len());
                let _ = w.bytes(&path[..n]);
            }
            let _ = w.bytes(b"\n");
            if w.over { break; }
        }
    } else {
        let _ = w.bytes(b"  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
        let is6 = kind == PF_NET_TCP6 || kind == PF_NET_UDP6;
        let tcp = kind == PF_NET_TCP || kind == PF_NET_TCP6;
        for r in rows {
            let ll = (r.local_len.max(0) as usize).min(r.local.len());
            let pl = (r.peer_len.max(0) as usize).min(r.peer.len());
            let _ = write!(w, "{:4}: ", r.reg_idx);
            render_net_addr(&mut w, &r.local[..ll], if is6 { AF_INET6_PROC } else { AF_INET_PROC });
            let _ = w.bytes(b" ");
            render_net_addr(&mut w, &r.peer[..pl], if is6 { AF_INET6_PROC } else { AF_INET_PROC });
            let _ = write!(w, " {:02X} 00000000:{:08X} 00:00000000 00000000 {} {} 0\n",
                net_state(r, tcp), r.rx_queued, r.owner_uid, r.recv_timeout_ticks);
            if w.over { break; }
        }
    }
    unsafe { *buf.add(w.len) = 0; }
    w.len as c_int
}

#[no_mangle]
pub unsafe extern "C" fn a20_procfs_render_net_snapshot(
    kind: c_int, rows: *const A20ProcfsNetRow, count: usize,
    buf: *mut c_char, bufsz: usize,
) -> c_int {
    if count != 0 && rows.is_null() { return -22; }
    let rows = if count == 0 { &[] } else {
        // SAFETY: the C snapshot provider gives us `count` initialized rows.
        unsafe { core::slice::from_raw_parts(rows, count) }
    };
    render_net_rows(kind, rows, buf, bufsz)
}

fn render_net_snapshot(g: &Gen<'_>, kind: c_int, buf: *mut c_char,
                       bufsz: usize) -> c_int {
    let mut records = core::ptr::null_mut();
    let mut count = 0usize;
    let rc = (g.net_snapshot)(kind, &mut records, &mut count);
    if rc < 0 { return rc; }
    if count != 0 && records.is_null() { return -12; }
    // SAFETY: the provider returns a live allocation containing count rows;
    // the matching release callback runs after Rust has finished formatting.
    let n = unsafe { a20_procfs_render_net_snapshot(kind, records, count, buf, bufsz) };
    (g.net_snapshot_free)(records);
    n
}

/// Call a helper-backed formatter and normalise its return (negative -> 0).
fn helper_ret(g: &Gen<'_>, which: c_int, buf: *mut c_char, bufsz: usize) -> c_int {
    let n = (g.format_helper)(which, buf, bufsz);
    if n < 0 {
        0
    } else {
        n
    }
}

/// The bytes up to (not including) the first NUL in a fixed C array.
fn zero_terminated(buf: &[u8]) -> &[u8] {
    match buf.iter().position(|&c| c == 0) {
        Some(n) => &buf[..n],
        None => buf,
    }
}

fn zero_pid() -> A20PidRaw {
    A20PidRaw {
        pid: 0,
        ppid: 0,
        pgid: 0,
        sid: 0,
        name: [0; 64],
        state_char: 0,
        state_text: [0; 32],
        utime_ticks: 0,
        stime_ticks: 0,
        child_utime: 0,
        child_stime: 0,
        priority: 0,
        start_jiffies: 0,
        vsize: 0,
        rss_pages: 0,
        processor: 0,
        uid: 0,
        euid: 0,
        suid: 0,
        fsuid: 0,
        gid: 0,
        egid: 0,
        sgid: 0,
        fsgid: 0,
        cap_inh: 0,
        cap_prm: 0,
        cap_eff: 0,
        cap_bnd: 0,
        ngroups: 0,
        groups: [0; 64],
        rss_kb: 0,
        vmlck_kb: 0,
        vmdata_kb: 0,
    }
}

/// Write the decimal text of an i32 into a small stack buffer; returns the
/// length written.
fn fmt_i32(buf: &mut [u8; 16], v: i32) -> usize {
    use core::fmt::Write as _;
    let mut w = SmallBuf { b: buf, n: 0 };
    let _ = write!(w, "{}", v);
    w.n
}

struct SmallBuf<'a> {
    b: &'a mut [u8; 16],
    n: usize,
}

impl core::fmt::Write for SmallBuf<'_> {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for &c in s.as_bytes() {
            if self.n >= self.b.len() {
                return Err(fmt::Error);
            }
            self.b[self.n] = c;
            self.n += 1;
        }
        Ok(())
    }
}

/// Generate the content of a procfs node.  The migrated node kinds are
/// formatted here; anything not yet moved is delegated to the adapter's
/// retained C generator, so the C fallback shrinks as kinds migrate.
///
/// # Safety
/// `buf` is `bufsz` writable bytes; `gen` is a live bridge table.
#[no_mangle]
pub unsafe extern "C" fn a20_procfs_generate(
    gen: *const A20ProcfsGen,
    ty: c_int,
    pid: c_int,
    buf: *mut c_char,
    bufsz: usize,
) -> c_int {
    let g = match unsafe { Gen::load(gen) } {
        Some(g) => g,
        None => return 0,
    };
    if buf.is_null() || bufsz == 0 {
        return 0;
    }
    // SAFETY: buf[0] is writable.
    unsafe { *buf = 0; }

    if ty == PF_PID_PAGEMAP {
        return 0; /* no content; the size path reports length 0 */
    }
    if ty == PF_CONFIG_GZ {
        /* Raw blob copy: no NUL guarantee and the exact copied length is
         * returned, matching the C memcpy path. */
        let mut p: *const u8 = core::ptr::null();
        let mut len: usize = 0;
        (g.config_gz)(&mut p, &mut len);
        let n = if len < bufsz { len } else { bufsz };
        if !p.is_null() && n > 0 {
            // SAFETY: p has len bytes; buf has bufsz >= n.
            unsafe { core::ptr::copy_nonoverlapping(p, buf as *mut u8, n) };
        }
        return n as c_int;
    }

    let cap = bufsz - 1;
    let mut w = W {
        p: buf as *mut u8,
        cap,
        len: 0,
        over: false,
    };
    let handled = match ty {
        PF_VERSION => {
            w.cstr((g.sysname)());
            let _ = w.bytes(b" version ");
            w.cstr((g.release)());
            let _ = w.bytes(b" (");
            w.cstr((g.arch_name)());
            let _ = w.bytes(b") (");
            w.cstr((g.version_full)());
            let _ = w.bytes(b")\n");
            true
        }
        PF_UPTIME => {
            let tps = (g.ticks_per_sec)();
            let ticks = (g.ticks)();
            if tps == 0 {
                let _ = w.bytes(b"0.00\n");
            } else {
                let _ = write!(w, "{}.{:02}\n", ticks / tps, (ticks % tps) * 100 / tps);
            }
            true
        }
        PF_CMDLINE => {
            w.cstr((g.bootargs)());
            let _ = w.bytes(b"\n");
            true
        }
        PF_LOADAVG => {
            let (mut a1, mut a5, mut a15) = (0u64, 0u64, 0u64);
            let (mut run, mut tot) = (0u32, 0u32);
            let mut max_pid: c_int = 0;
            (g.loadavg)(&mut a1, &mut a5, &mut a15, &mut run, &mut tot, &mut max_pid);
            let l1 = ((a1 * 100) >> 16) as u32;
            let l5 = ((a5 * 100) >> 16) as u32;
            let l15 = ((a15 * 100) >> 16) as u32;
            let _ = write!(
                w,
                "{}.{:02} {}.{:02} {}.{:02} {}/{} {}\n",
                l1 / 100,
                l1 % 100,
                l5 / 100,
                l5 % 100,
                l15 / 100,
                l15 % 100,
                run,
                tot,
                max_pid
            );
            true
        }
        PF_BOOT_ID => {
            let t = (g.ticks)();
            let _ = write!(
                w,
                "{:08x}-{:04x}-{:04x}-{:04x}-{:08x}{:08x}\n",
                (t & 0xffff_ffff) as u32,
                ((t >> 32) & 0xffff) as u32,
                ((t >> 48) & 0xffff) as u32,
                0x4000 | ((t >> 16) & 0x0fff) as u32,
                (t >> 32) as u32,
                (t & 0xffff_ffff) as u32
            );
            true
        }
        PF_SELF => {
            let _ = write!(w, "{}\n", (g.current_pid)());
            true
        }
        PF_NR_OPEN => {
            let _ = write!(w, "{}\n", (g.max_files)());
            true
        }
        PF_CAP_LAST_CAP => {
            let _ = w.bytes(b"63\n");
            true
        }
        PF_SYS_KERNEL_OSRELEASE => {
            w.cstr((g.release)());
            let _ = w.bytes(b"\n");
            true
        }
        PF_SYS_KERNEL_PID_MAX => {
            let _ = write!(w, "{}\n", (g.pid_max)());
            true
        }
        PF_SYS_KERNEL_TAINTED
        | PF_SYS_KERNEL_SCHED_AUTOGROUP
        | PF_SYS_KERNEL_IO_URING_DISABLED
        | PF_SYS_VM_DROP_CACHES => {
            let _ = w.bytes(b"0\n");
            true
        }
        PF_SYS_FS_PIPE_MAX_SIZE => {
            let _ = write!(w, "{}\n", (g.pipe_max_size)());
            true
        }
        PF_SYS_FS_LEASE_BREAK_TIME => {
            let _ = write!(w, "{}\n", (g.lease_break_time)());
            true
        }
        PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS => {
            let _ = w.bytes(b"16384\n");
            true
        }
        PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES => {
            let _ = w.bytes(b"128\n");
            true
        }
        PF_A20_ANONPROV => {
            let _ = write!(w, "{}\n", (g.anonprov_max)());
            true
        }
        PF_A20_SCHED_BASE_SLICE => {
            let _ = write!(w, "{}\n", (g.sched_base_slice)());
            true
        }
        PF_SYSRQ_TRIGGER => {
            let _ = w.bytes(b"sysrq commands: c (crash \xe2\x80\x94 deliberate kernel panic)\n");
            true
        }
        PF_MEMINFO => {
            let mut m = A20MeminfoRaw {
                total_frames: 0,
                free_frames: 0,
                slab_total_bytes: 0,
                slab_reclaim_bytes: 0,
                bcache_pool_bytes: 0,
                bcache_valid_pages: 0,
                bcache_dirty_blocks: 0,
                bcache_dirty_pages: 0,
                pcache_valid: 0,
                pcache_dirty: 0,
                vm_anon_huge: 0,
                vm_shmem_huge: 0,
                vm_file_huge: 0,
                huge_total: 0,
                huge_free: 0,
                swap_total_pages: 0,
                swap_free_pages: 0,
                page_size: 0,
                pcache_page_size: 0,
                bcache_block_size: 0,
            };
            (g.meminfo_raw)(&mut m);
            let kb = |bytes: u64| bytes / 1024;
            let total_kb = kb(m.total_frames * m.page_size);
            let free_kb = kb(m.free_frames * m.page_size);
            let buffers_kb = kb(m.bcache_pool_bytes);
            let cached_kb = kb(m.bcache_valid_pages * m.pcache_page_size + m.pcache_valid * m.page_size);
            let dirty_kb = kb(m.bcache_dirty_blocks * m.bcache_block_size
                + m.bcache_dirty_pages * m.pcache_page_size
                + m.pcache_dirty * m.page_size);
            let slab_kb = kb(m.slab_total_bytes);
            let sreclaim_kb = kb(m.slab_reclaim_bytes);
            let sunreclaim_kb = slab_kb.saturating_sub(sreclaim_kb);
            let mut available_kb = free_kb + cached_kb + sreclaim_kb;
            if available_kb > total_kb {
                available_kb = total_kb;
            }
            let swap_total_kb = kb(m.swap_total_pages * m.page_size);
            let swap_free_kb = kb(m.swap_free_pages * m.page_size);
            let anon_huge_kb = kb(m.vm_anon_huge * m.page_size);
            let shmem_huge_kb = kb(m.vm_shmem_huge * m.page_size);
            let file_huge_kb = kb(m.vm_file_huge * m.page_size);
            let _ = write!(
                w,
                "MemTotal:       {} kB\n\
                 MemFree:        {} kB\n\
                 MemAvailable:   {} kB\n\
                 Buffers:        {} kB\n\
                 Cached:         {} kB\n\
                 SwapTotal:      {} kB\n\
                 SwapFree:       {} kB\n\
                 Shmem:          0 kB\n\
                 Dirty:          {} kB\n\
                 Slab:           {} kB\n\
                 SReclaimable:   {} kB\n\
                 SUnreclaim:     {} kB\n\
                 AnonHugePages:  {} kB\n\
                 ShmemHugePages: {} kB\n\
                 FileHugePages:  {} kB\n\
                 HugePages_Total: {}\n\
                 HugePages_Free:  {}\n\
                 Hugepagesize:   2048 kB\n",
                total_kb,
                free_kb,
                available_kb,
                buffers_kb,
                cached_kb,
                swap_total_kb,
                swap_free_kb,
                dirty_kb,
                slab_kb,
                sreclaim_kb,
                sunreclaim_kb,
                anon_huge_kb,
                shmem_huge_kb,
                file_huge_kb,
                m.huge_total,
                m.huge_free
            );
            true
        }
        PF_MOUNTS => {
            let count = (g.mount_count)();
            'mounts: for i in 0..count {
                let mut e = A20MountEnt {
                    dev: [0; 32],
                    path: [0; 256],
                    fstype: [0; 32],
                    opts: [0; 32],
                    mnt_id: 0,
                    parent_id: 0,
                    detached: 0,
                };
                if (g.mount_at)(i, &mut e) == 0 {
                    continue;
                }
                let segs: [&[u8]; 4] = [
                    zero_terminated(&e.dev),
                    zero_terminated(&e.path),
                    zero_terminated(&e.fstype),
                    zero_terminated(&e.opts),
                ];
                let line_len = segs.iter().map(|s| s.len()).sum::<usize>() + 3 + b" 0 0\n".len();
                if w.len + line_len > w.cap {
                    break 'mounts; /* C breaks before adding an overflowing line */
                }
                for (n, seg) in segs.iter().enumerate() {
                    if n > 0 {
                        let _ = w.bytes(b" ");
                    }
                    let _ = w.bytes(seg);
                }
                let _ = w.bytes(b" 0 0\n");
            }
            true
        }
        PF_CGROUPS => {
            let _ = w.bytes(
                b"#subsys_name\thierarchy\tnum_cgroups\tenabled\n\
                  cpuset\t1\t1\t1\ncpu\t1\t1\t1\ncpuacct\t1\t1\t1\nmemory\t1\t1\t1\n",
            );
            true
        }
        PF_FSTYPE => {
            let _ = w.bytes(
                b"nodev\tproc\nnodev\tcgroup\nnodev\tcgroup2\n\text4\n\tvfat\n\tramfs\n\ttmpfs\n",
            );
            true
        }
        PF_STAT => {
            let nr = (g.nr_cpus)();
            let mut tot_user = 0u64;
            let mut tot_sys = 0u64;
            let mut tot_idle = 0u64;
            for cpu in 0..nr {
                let (mut u, mut s, mut idl) = (0u64, 0u64, 0u64);
                (g.cpu_times)(cpu, &mut u, &mut s, &mut idl);
                tot_user += u;
                tot_sys += s;
                tot_idle += idl;
            }
            let _ = write!(
                w,
                "cpu  {} 0 {} {} 0 0 0 0 0 0\n",
                tot_user, tot_sys, tot_idle
            );
            for cpu in 0..nr {
                let (mut u, mut s, mut idl) = (0u64, 0u64, 0u64);
                (g.cpu_times)(cpu, &mut u, &mut s, &mut idl);
                let _ = write!(w, "cpu{} {} 0 {} {} 0 0 0 0 0 0\n", cpu, u, s, idl);
            }
            let mut rt = 0u64;
            let mut mono = 0u64;
            (g.realtime)(&mut rt);
            (g.monotonic)(&mut mono);
            let btime = if rt > mono { rt - mono } else { 0 };
            let _ = write!(
                w,
                "intr 0\nctxt 0\nbtime {}\nprocesses {}\nprocs_running {}\nprocs_blocked 0\n",
                btime,
                (g.tasks_created)(),
                (g.runq_load_sum)()
            );
            true
        }
        PF_SWAPS => {
            let _ = w.bytes(b"Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n");
            for i in 0..MAX_SWAPFILES {
                let mut e = A20SwapEnt {
                    name: [0; 64],
                    pages: 0,
                    inuse_pages: 0,
                };
                if (g.swap_get)(i, &mut e) == 0 {
                    continue;
                }
                let size_kb = e.pages * (PAGE_SIZE / 1024);
                let used_kb = e.inuse_pages * (PAGE_SIZE / 1024);
                let name = zero_terminated(&e.name);
                let _ = w.bytes(name);
                let _ = write!(w, "\tpartition\t{}\t{}\t-2\n", size_kb, used_kb);
            }
            true
        }
        PF_CPUINFO => {
            let online = (g.online_cpu_mask)();
            let nr = (g.nr_cpus)();
            for cpu in 0..nr {
                if online & (1u32 << cpu) == 0 {
                    continue;
                }
                let _ = write!(w, "processor\t: {}\n", cpu);
                let mut fields = [0u8; 256];
                let _ = (g.cpuinfo_fields)(fields.as_mut_ptr() as *mut c_char, fields.len());
                let _ = w.bytes(zero_terminated(&fields));
                let _ = w.bytes(b"\n");
            }
            true
        }
        PF_A20_BCACHE => {
            let mut b = A20BcacheRaw {
                caches: 0,
                block_pool_bytes: 0,
                page_pool_bytes: 0,
                valid_blocks: 0,
                dirty_blocks: 0,
                valid_pages: 0,
                dirty_pages: 0,
            };
            (g.bcache_raw)(&mut b);
            let _ = write!(
                w,
                "caches: {}\nblock_pool_bytes: {}\npage_pool_bytes: {}\nvalid_blocks: {}\n\
                 dirty_blocks: {}\nvalid_pages: {}\ndirty_pages: {}\n",
                b.caches,
                b.block_pool_bytes,
                b.page_pool_bytes,
                b.valid_blocks,
                b.dirty_blocks,
                b.valid_pages,
                b.dirty_pages
            );
            true
        }
        PF_A20_PAGE_CACHE => {
            let mut p = A20PagecacheRaw {
                capacity: 0,
                allocated: 0,
                bytes: 0,
                valid: 0,
                dirty: 0,
                pinned: 0,
            };
            (g.pagecache_raw)(&mut p);
            let _ = write!(
                w,
                "capacity: {}\nallocated: {}\nbytes: {}\nvalid: {}\ndirty: {}\npinned: {}\n",
                p.capacity, p.allocated, p.bytes, p.valid, p.dirty, p.pinned
            );
            true
        }
        PF_A20_OOM => {
            let mut o = A20OomRaw {
                kills: 0,
                last_kill_tick: 0,
                last_victim_pid: 0,
                last_victim_score: 0,
                free_pages_at_kill: 0,
                free_pages_now: 0,
                in_progress: 0,
                kswapd_passes: 0,
                kswapd_pages_freed: 0,
                kswapd_last_pass_tick: 0,
            };
            (g.oom_raw)(&mut o);
            let _ = write!(
                w,
                "kills: {}\nlast_kill_tick: {}\nlast_victim_pid: {}\nlast_victim_score: {}\n\
                 free_pages_at_kill: {}\nfree_pages_now: {}\nin_progress: {}\nkswapd_passes: {}\n\
                 kswapd_pages_freed: {}\nkswapd_last_pass_tick: {}\n",
                o.kills,
                o.last_kill_tick,
                o.last_victim_pid,
                o.last_victim_score,
                o.free_pages_at_kill,
                o.free_pages_now,
                o.in_progress,
                o.kswapd_passes,
                o.kswapd_pages_freed,
                o.kswapd_last_pass_tick
            );
            true
        }
        PF_A20_OBJECTS => {
            let mut o = A20ObjstatsRaw {
                handles: 0,
                channel_eps: 0,
                eventqs: 0,
                vmos: 0,
                vmo_pages: 0,
                irq_bindings: 0,
                vfiles: 0,
                vmo_dirty_frames: 0,
            };
            (g.objstats_raw)(&mut o);
            let _ = write!(
                w,
                "handles: {}\nchannel_eps: {}\neventqs: {}\nvmos: {}\nvmo_pages: {}\n\
                 irq_bindings: {}\nvfiles: {}\nvmo_dirty_frames: {}\n",
                o.handles,
                o.channel_eps,
                o.eventqs,
                o.vmos,
                o.vmo_pages,
                o.irq_bindings,
                o.vfiles,
                o.vmo_dirty_frames
            );
            true
        }
        PF_A20_IOMMU => {
            let mut s = A20IommuRaw {
                enabled: 0,
                domains_claimed: 0,
                domains_released: 0,
                maps: 0,
                map_failures: 0,
                unmaps: 0,
                mapped_pages: 0,
                fault_records: 0,
                faults: 0,
                blocked_events: 0,
                last_fault_devid: 0,
                last_fault_cause: 0,
                last_fault_iova: 0,
                last_fault_owner: 0,
            };
            (g.iommu_raw)(&mut s);
            let _ = write!(
                w,
                "enabled: {}\ndomains_claimed: {}\ndomains_released: {}\nmaps: {}\n\
                 map_failures: {}\nunmaps: {}\nmapped_pages: {}\nfault_records: {}\nfaults: {}\n\
                 blocked_events: {}\nlast_fault_devid: {}\nlast_fault_cause: {}\n\
                 last_fault_iova: 0x{:x}\nlast_fault_owner: {}\n",
                s.enabled,
                s.domains_claimed,
                s.domains_released,
                s.maps,
                s.map_failures,
                s.unmaps,
                s.mapped_pages,
                s.fault_records,
                s.faults,
                s.blocked_events,
                s.last_fault_devid,
                s.last_fault_cause,
                s.last_fault_iova,
                s.last_fault_owner
            );
            true
        }
        PF_DEVICES => {
            let _ = w.bytes(
                b"Character devices:\n  1 mem\n  4 tty\n  5 /dev/tty\n  5 /dev/console\n  10 misc\n  13 input\n  29 fb\n  254 ptmx\n\nBlock devices:\n  7 loop\n",
            );
            true
        }
        PF_PARTITIONS => {
            let _ = w.bytes(b"major minor  #blocks  name\n\n");
            true
        }
        PF_DISKSTATS => true, /* empty */
        PF_A20_DRIVER_LIFECYCLE => true, /* empty */
        PF_SYSVIPC => {
            let mut c = A20SysvipcRaw { msg: 0, sem: 0, shm: 0 };
            (g.sysvipc_raw)(&mut c);
            let _ = write!(w, "msg\t{} sem\t{} shm\t{}\n", c.msg, c.sem, c.shm);
            true
        }
        PF_SYS_KERNEL_OVERFLOW_UID => {
            let _ = write!(w, "{}\n", (g.userns_overflow_id)(0));
            true
        }
        PF_SYS_KERNEL_OVERFLOW_GID => {
            let _ = write!(w, "{}\n", (g.userns_overflow_id)(1));
            true
        }
        PF_SYS_KERNEL_HOSTNAME => {
            w.cstr((g.hostname)());
            let _ = w.bytes(b"\n");
            true
        }
        PF_SYS_KERNEL_DOMAINNAME => {
            w.cstr((g.domainname)());
            let _ = w.bytes(b"\n");
            true
        }
        PF_SYS_KERNEL_PIDMAP => {
            let n = (g.pidmap)(buf, bufsz);
            if n < 0 {
                return 0;
            }
            return n;
        }
        PF_TTY => {
            let _ = w.bytes(
                b"driver               refcnt\npty_slave               0\npty_master              0\nttyS0                   0\n/dev/console            0\n",
            );
            true
        }
        PF_LDISCS => {
            let _ = w.bytes(b"n_tty        0\nn_gsm        5\n");
            true
        }
        PF_PRESSURE | PF_PRESSURE_CPU | PF_PRESSURE_MEM | PF_PRESSURE_IO => {
            if bufsz < 64 {
                return 0;
            }
            let kind = if ty == PF_PRESSURE_MEM {
                1
            } else if ty == PF_PRESSURE_IO {
                2
            } else {
                0
            };
            let mut tmp = [0u8; 64];
            let _ = (g.psi_render)(kind, tmp.as_mut_ptr() as *mut c_char, tmp.len());
            let _ = w.bytes(zero_terminated(&tmp));
            true
        }
        PF_PID_STAT => {
            let mut p = zero_pid();
            if (g.pid_raw)(pid, &mut p) == 0 {
                let _ = write!(w, "{} (unknown) S 0 0\n", pid);
            } else {
                let _ = write!(
                    w,
                    "{} ({}) {} {} {} {} 0 0 0 0 0 0 0 {} {} {} {} {} 0 1 0 {} {} {} -1 0 0 0 0 0 0 0 0 0 0 0 0 0 {}\n",
                    p.pid,
                    core::str::from_utf8(zero_terminated(&p.name)).unwrap_or(""),
                    p.state_char as char,
                    p.ppid,
                    p.pgid,
                    p.sid,
                    p.utime_ticks,
                    p.stime_ticks,
                    p.child_utime,
                    p.child_stime,
                    p.priority,
                    p.start_jiffies,
                    p.vsize,
                    p.rss_pages,
                    p.processor
                );
            }
            true
        }
        PF_PID_STATUS => {
            let mut p = zero_pid();
            if (g.pid_raw)(pid, &mut p) == 0 {
                let _ = write!(w, "Name:\tunknown\nPid:\t{}\n", pid);
            } else {
                let mut groups = [0u8; 160];
                let mut glen = 0usize;
                let ng = core::cmp::min(p.ngroups as usize, p.groups.len());
                for i in 0..ng {
                    let sep: &[u8] = if i > 0 { b" " } else { b"" };
                    let mut tmp = [0u8; 16];
                    let tn = fmt_i32(&mut tmp, p.groups[i]);
                    if glen + sep.len() + tn >= groups.len() {
                        break;
                    }
                    groups[glen..glen + sep.len()].copy_from_slice(sep);
                    glen += sep.len();
                    groups[glen..glen + tn].copy_from_slice(&tmp[..tn]);
                    glen += tn;
                }
                let _ = write!(
                    w,
                    "Name:\t{}\nPid:\t{}\nPPid:\t{}\nPGid:\t{}\nSid:\t{}\nTgid:\t{}\nNgid:\t0\nState:\t{}\nUid:\t{}\t{}\t{}\t{}\nGid:\t{}\t{}\t{}\t{}\nGroups:\t{}\nCapInh:\t{:016x}\nCapPrm:\t{:016x}\nCapEff:\t{:016x}\nCapBnd:\t{:016x}\nThreads:\t1\nVmRSS:\t{} kB\nVmLck:\t{} kB\nVmData:\t{} kB\n",
                    core::str::from_utf8(zero_terminated(&p.name)).unwrap_or(""),
                    p.pid,
                    p.ppid,
                    p.pgid,
                    p.sid,
                    p.pid,
                    core::str::from_utf8(zero_terminated(&p.state_text)).unwrap_or(""),
                    p.uid,
                    p.euid,
                    p.suid,
                    p.fsuid,
                    p.gid,
                    p.egid,
                    p.sgid,
                    p.fsgid,
                    core::str::from_utf8(&groups[..glen]).unwrap_or(""),
                    p.cap_inh,
                    p.cap_prm,
                    p.cap_eff,
                    p.cap_bnd,
                    p.rss_kb,
                    p.vmlck_kb,
                    p.vmdata_kb
                );
            }
            true
        }
        PF_PID_STATM => {
            let mut p = zero_pid();
            if (g.pid_raw)(pid, &mut p) == 0 {
                let _ = w.bytes(b"0 0 0 0 0 0 0\n");
            } else {
                let total = p.vsize / PAGE_SIZE;
                let _ = write!(w, "{} {} 0 0 0 0 0\n", total, p.rss_pages);
            }
            true
        }
        PF_PID_COMM | PF_PID_EXE | PF_PID_CWD => {
            let which = if ty == PF_PID_EXE {
                1
            } else if ty == PF_PID_CWD {
                2
            } else {
                0
            };
            let mut s = [0u8; 256];
            (g.pid_str)(pid, which, s.as_mut_ptr() as *mut c_char, s.len());
            let _ = w.bytes(zero_terminated(&s));
            let _ = w.bytes(b"\n");
            true
        }
        PF_PID_ENVIRON => return 0,
        PF_PID_IO => {
            let mut io = A20PidIoRaw {
                rchar: 0,
                wchar: 0,
                syscr: 0,
                syscw: 0,
                read_bytes: 0,
                write_bytes: 0,
            };
            (g.pid_io)(pid, &mut io);
            let _ = write!(
                w,
                "rchar: {}\nwchar: {}\nsyscr: {}\nsyscw: {}\nread_bytes: {}\nwrite_bytes: {}\ncancelled_write_bytes: 0\n",
                io.rchar, io.wchar, io.syscr, io.syscw, io.read_bytes, io.write_bytes
            );
            true
        }
        PF_PID_LOGINUID => {
            let _ = w.bytes(b"4294967295\n");
            true
        }
        PF_PID_SESSIONID => {
            let _ = write!(w, "{}\n", (g.pid_sessionid)(pid));
            true
        }
        PF_PID_NS_PID
        | PF_PID_NS_PID_FOR_CHILDREN
        | PF_PID_NS_UTS
        | PF_PID_NS_USER
        | PF_PID_NS_IPC
        | PF_PID_NS_MNT
        | PF_PID_NS_NET
        | PF_PID_NS_CGROUP => {
            let (kind, label): (c_int, &[u8]) = match ty {
                PF_PID_NS_PID => (0, b"pid:"),
                PF_PID_NS_PID_FOR_CHILDREN => (1, b"pid:"),
                PF_PID_NS_UTS => (2, b"uts:"),
                PF_PID_NS_USER => (3, b"user:"),
                PF_PID_NS_IPC => (4, b"ipc:"),
                PF_PID_NS_MNT => (5, b"mnt:"),
                PF_PID_NS_NET => (6, b"net:"),
                _ => (7, b"cgroup:"),
            };
            let ino = (g.pid_ns_ino)(pid, kind);
            let _ = w.bytes(label);
            let _ = write!(w, "[{}]\n", ino);
            true
        }
        PF_PID_MOUNTINFO => {
            let count = (g.mount_count)();
            'mi: for i in 0..count {
                let mut e = A20MountEnt {
                    dev: [0; 32],
                    path: [0; 256],
                    fstype: [0; 32],
                    opts: [0; 32],
                    mnt_id: 0,
                    parent_id: 0,
                    detached: 0,
                };
                if (g.mount_at)(i, &mut e) == 0 {
                    continue;
                }
                let segs: [&[u8]; 4] = [
                    zero_terminated(&e.path),
                    zero_terminated(&e.opts),
                    zero_terminated(&e.fstype),
                    zero_terminated(&e.dev),
                ];
                let root: &[u8] = if e.detached != 0 { b"none" } else { b"/" };
                /* "%u %u 0:%u %s %s %s - %s %s %s\n" */
                let line_len = 24
                    + root.len()
                    + segs[0].len()
                    + segs[1].len()
                    + segs[2].len()
                    + segs[3].len()
                    + segs[1].len()
                    + 6;
                if w.len + line_len > w.cap {
                    break 'mi;
                }
                let _ = write!(
                    w,
                    "{} {} 0:{} ",
                    e.mnt_id, e.parent_id, e.mnt_id
                );
                let _ = w.bytes(root);
                let _ = w.bytes(b" ");
                let _ = w.bytes(segs[0]);
                let _ = w.bytes(b" ");
                let _ = w.bytes(segs[1]);
                let _ = w.bytes(b" - ");
                let _ = w.bytes(segs[2]);
                let _ = w.bytes(b" ");
                let _ = w.bytes(segs[3]);
                let _ = w.bytes(b" ");
                let _ = w.bytes(segs[1]);
                let _ = w.bytes(b"\n");
            }
            true
        }
        PF_NET | PF_NET_STATUS => {
            let n = (g.format_helper)(15, buf, bufsz);
            if n < 0 {
                return 0;
            }
            return n;
        }
        PF_NET_CONFIG => return helper_ret(&g, 12, buf, bufsz),
        PF_NET_DEV => return helper_ret(&g, 13, buf, bufsz),
        PF_ROUTE => return helper_ret(&g, 14, buf, bufsz),
        PF_A20_HYP_SELFTEST => {
            let _ = write!(w, "hyp_selftest={}\n", if (g.hyp_selftest)() == 0 { "PASS" } else { "FAIL" });
            true
        }
        PF_SYS_KERNEL_CORE_PATTERN => {
            // The kernel owns the pattern value behind a narrow getter. Rust
            // owns the procfs line framing and applies the caller's capacity.
            let mut pattern = [0u8; 512];
            let n = (g.format_helper)(1, pattern.as_mut_ptr() as *mut c_char, pattern.len());
            if n > 0 {
                let n = core::cmp::min(n as usize, pattern.len());
                let end = if pattern[n - 1] == b'\n' { n - 1 } else { n };
                let _ = w.bytes(&pattern[..end]);
            }
            let _ = w.bytes(b"\n");
            true
        }
        PF_A20_XLATOR => return helper_ret(&g, 2, buf, bufsz),
        PF_A20_TASK_LIFETIME => return helper_ret(&g, 3, buf, bufsz),
        PF_A20_PERF => return helper_ret(&g, 4, buf, bufsz),
        PF_A20_LOCK_CONTENTION => return helper_ret(&g, 5, buf, bufsz),
        PF_A20_NETFILTER => return helper_ret(&g, 6, buf, bufsz),
        PF_A20_NETMEM => return helper_ret(&g, 7, buf, bufsz),
        PF_A20_JOURNAL => return helper_ret(&g, 8, buf, bufsz),
        PF_MODULES => return helper_ret(&g, 9, buf, bufsz),
        PF_DRIVERS => return helper_ret(&g, 10, buf, bufsz),
        PF_INTERRUPTS => return helper_ret(&g, 11, buf, bufsz),
        PF_PID_OOM_SCORE_ADJ => {
            let mut adj: c_int = 0;
            let present = (g.pid_oom)(pid, &mut adj);
            let _ = write!(w, "{}\n", if present != 0 { adj } else { 0 });
            true
        }
        PF_PID_OOM_SCORE => {
            let mut adj: c_int = 0;
            let present = (g.pid_oom)(pid, &mut adj);
            let score = if present != 0 && adj >= 0 { adj } else { 0 };
            let _ = write!(w, "{}\n", score);
            true
        }
        PF_PID_CGROUP => {
            let mut path = [0u8; 1024];
            (g.pid_cgroup_path)(pid, path.as_mut_ptr() as *mut c_char, path.len());
            let _ = w.bytes(b"0::");
            let _ = w.bytes(zero_terminated(&path));
            let _ = w.bytes(b"\n");
            true
        }
        PF_PID_CMDLINE => {
            let mut s = [0u8; 1024];
            (g.pid_str)(pid, 3, s.as_mut_ptr() as *mut c_char, s.len());
            let raw = zero_terminated(&s);
            if raw.is_empty() {
                // No exec path: one NUL byte, length 1.
                // SAFETY: bufsz >= 1 (checked above).
                unsafe { *buf = 0; }
                return 1;
            }
            let mut len = raw.len();
            if len + 1 > bufsz {
                len = bufsz - 1;
            }
            // SAFETY: buf has bufsz >= len+1 bytes.
            unsafe {
                core::ptr::copy_nonoverlapping(raw.as_ptr(), buf as *mut u8, len);
                *buf.add(len) = 0;
            }
            return (len + 1) as c_int;
        }
        PF_UID_MAP | PF_GID_MAP => {
            let is_gid = if ty == PF_GID_MAP { 1 } else { 0 };
            let mut m = A20MapRaw {
                n: 0,
                ent: [A20MapEnt { lower: 0, parent_lower: 0, count: 0 }; 16],
            };
            let n = (g.userns_map)(pid, is_gid, &mut m);
            let mut off: usize = 0;
            let cap = bufsz - 1;
            let mut i = 0;
            while i < n && off < cap {
                if let Some(e) = m.ent.get(i as usize) {
                    let _ = write!(w, "{:>10} {:>10} {:>10}\n", e.lower, e.parent_lower, e.count);
                }
                off += 33; /* snprintf's would-be length for 10/10/10 fields */
                i += 1;
            }
            return off as c_int;
        }
        PF_SETGROUPS => {
            let mut allowed: c_int = 0;
            if (g.userns_setgroups_allowed)(pid, &mut allowed) == 0 {
                return 0;
            }
            let _ = w.bytes(if allowed != 0 { b"allow\n" } else { b"deny\n" });
            true
        }
        PF_PID_LIMITS => {
            let mut l = A20LimitsRaw { nofile: 0, memlock: 0 };
            if (g.pid_limits)(pid, &mut l) == 0 {
                return -ESRCH;
            }
            let _ = write!(
                w,
                "Max open files             {:>5}                {} files\n\
                 Max stack size                unlimited            unlimited bytes\n\
                 Max locked memory       {:>5}              {} bytes\n",
                l.nofile, l.nofile, l.memlock, l.memlock
            );
            true
        }
        PF_PID_WCHAN => {
            let mut s = [0u8; 64];
            let rc = (g.pid_wchan)(pid, s.as_mut_ptr() as *mut c_char, s.len());
            if rc < 0 {
                return -ESRCH;
            }
            let _ = w.bytes(zero_terminated(&s));
            let _ = w.bytes(b"\n");
            true
        }
        PF_PID_STACK => {
            if (g.pid_exists)(pid) == 0 {
                return -ESRCH;
            }
            let _ = w.bytes(b"[<0000000000000000>] 0\n");
            true
        }
        PF_PID_AUXV => {
            let n = (g.pid_auxv)(pid, buf as *mut u8, bufsz as u32);
            if n < 0 {
                return 0;
            }
            return n;
        }
        PF_PID_MAPS | PF_PID_SMAPS => true, /* empty in generate_content */
        PF_ARP => {
            let _ = w.bytes(b"IP address       HW type     Flags       HW address            Mask     Device\n");
            true
        }
        PF_IOMEM => {
            let _ = w.bytes(
                b"00000000-00000fff : reserved\n00001000-0fffffff : System RAM\n10000000-1fffffff : PCI Bus 0000:00\n",
            );
            true
        }
        PF_IOPORTS => {
            let _ = w.bytes(b"0000-0cf7 : PCI Bus 0000:00\n");
            true
        }
        PF_MISC => {
            let _ = w.bytes(b" 10 cpu_dma_latency\n 59 random\n");
            true
        }
        PF_SOFTIRQS => {
            let _ = w.bytes(
                b"    HI: 0\n TIMER: 0\n NET_TX: 0\n NET_RX: 0\n BLOCK: 0\n IRQ_POLL: 0\n TASKLET: 0\n SCHED: 0\n HRTIMER: 0\n RCU: 0\n",
            );
            true
        }
        PF_A20_IRQ_AFFINITY => {
            let mut m = A20MsixRaw {
                total: 0,
                n: 0,
                ent: [A20MsixEnt { name: [0; 32], index: 0, vector: 0, cpu: 0 }; 64],
            };
            let total = (g.msix_snapshot)(&mut m);
            let _ = write!(w, "cpus: {}\nentries: {}\n", (g.msix_cpu_count)(), total);
            let shown = core::cmp::min(total, m.n);
            for i in 0..shown {
                if let Some(e) = m.ent.get(i as usize) {
                    let _ = w.bytes(zero_terminated(&e.name));
                    let _ = write!(w, "\t{}\t{}\t{}\n", e.index, e.vector, e.cpu);
                }
            }
            true
        }
        PF_NET_TCP | PF_NET_UDP | PF_NET_TCP6 | PF_NET_UDP6 | PF_NET_UNIX =>
            return render_net_snapshot(&g, ty, buf, bufsz),
        _ => false,
    };

    if !handled {
        return 0; /* no generator: directory/unknown kinds carry no content */
    }
    // SAFETY: len <= cap < bufsz, so buf[len] is writable.
    unsafe { *buf.add(w.len) = 0; }
    let _ = w.over;
    w.len as c_int
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;

    fn nt(name: &str, xlator: bool) -> (i32, i32) {
        let mut b = [0u8; 64];
        b[..name.len()].copy_from_slice(name.as_bytes());
        let mut pid = 0i32;
        let t = unsafe { a20_procfs_name_type(b.as_ptr(), &mut pid, xlator as c_int) };
        (t, pid)
    }

    #[test]
    fn maps_and_smaps_render_raw_snapshot_with_c_layout() {
        let mut rec = A20ProcfsMapRecord {
            start: 0x1000, end: 0x3000, vm_flags: VM_READ | VM_WRITE | VM_ANON,
            file_offset: 0x40, ino: 7, thp_eligible: 1, name: [0; PROCFS_MAP_NAME_MAX],
            stats: A20ProcfsMapStats { rss_pages: 1, shared_clean_pages: 0,
                shared_dirty_pages: 0, private_clean_pages: 1, private_dirty_pages: 0,
                anonymous_pages: 1, anon_huge_pages: 0, shmem_pmd_pages: 0,
                file_pmd_pages: 0 },
        };
        rec.name[..5].copy_from_slice(b"[heap");
        rec.name[5] = b']';
        let mut out = [0xa5u8; 512];
        let n = unsafe { a20_procfs_render_maps(&rec, 1, 0, out.as_mut_ptr() as *mut c_char, out.len()) };
        assert_eq!(n as usize, b"00001000-00003000 rw-p 00000040 00:00 7 [heap]\n".len());
        assert_eq!(&out[..n as usize], b"00001000-00003000 rw-p 00000040 00:00 7 [heap]\n");
        assert_eq!(out[n as usize], 0);

        out.fill(0xa5);
        let n = unsafe { a20_procfs_render_maps(&rec, 1, 1, out.as_mut_ptr() as *mut c_char, out.len()) };
        let rendered = &out[..n as usize];
        assert!(rendered.starts_with(b"00001000-00003000 rw-p 00000040 00:00 7 [heap]\nSize:"), "{}", String::from_utf8_lossy(rendered));
        assert!(rendered.ends_with(b"THPeligible:           1\nVmFlags: rd wr mr mw me ac\n"));
        assert_eq!(out[n as usize], 0);
    }

    #[test]
    fn fdinfo_renders_pidfd_target_and_rejects_invalid_buffers() {
        let mut out = [0xa5u8; 128];
        let n = unsafe {
            a20_procfs_render_fdinfo(0, 0o2000002, 99, 456, out.as_mut_ptr() as *mut c_char, out.len())
        };
        assert_eq!(&out[..n as usize],
                   b"pos:\t0\nflags:\t02000002\nino:\t99\nPid:\t456\nNSpid:\t456\n");
        assert_eq!(out[n as usize], 0);

        // A non-pidfd (pid <= 0) must not claim a target pid.
        let n = unsafe {
            a20_procfs_render_fdinfo(7, 0o2, 3, -1, out.as_mut_ptr() as *mut c_char, out.len())
        };
        assert_eq!(&out[..n as usize], b"pos:\t7\nflags:\t02\nino:\t3\n");

        assert_eq!(unsafe {
            a20_procfs_render_fdinfo(0, 0, 0, 0, core::ptr::null_mut(), 8)
        }, -22);
    }

    #[test]
    fn maps_render_rejects_invalid_buffers_and_truncates_safely() {
        let rec = A20ProcfsMapRecord {
            start: 0x1000, end: 0x2000, vm_flags: VM_EXEC | VM_STACK,
            file_offset: 0, ino: 0, thp_eligible: 0, name: [0; PROCFS_MAP_NAME_MAX],
            stats: A20ProcfsMapStats::default(),
        };
        assert_eq!(unsafe { a20_procfs_render_maps(&rec, 1, 0, core::ptr::null_mut(), 4) }, -22);
        let mut one = [0x7fu8; 1];
        assert_eq!(unsafe { a20_procfs_render_maps(&rec, 1, 0, one.as_mut_ptr() as *mut c_char, 1) }, 0);
        assert_eq!(one, [0]);
        let mut small = [0x7fu8; 8];
        let n = unsafe { a20_procfs_render_maps(&rec, 1, 0, small.as_mut_ptr() as *mut c_char, small.len()) };
        assert_eq!(n, 7);
        assert_eq!(small[7], 0);
        assert_eq!(&small[..7], b"0000100");
    }

    #[test]
    fn net_snapshot_rows_render_linux_proc_layout() {
        let mut row = A20ProcfsNetRow {
            reg_idx: 7, domain: AF_INET_PROC, ty: SOCK_STREAM_PROC,
            tcp_flags: TCP_CONNECTED_PROC as i32, local_len: 16, peer_len: 0,
            rx_queued: 5, owner_uid: 1000, recv_timeout_ticks: 3,
            local: [0; PROCFS_NET_ADDR_MAX], peer: [0; PROCFS_NET_ADDR_MAX],
        };
        row.local[2] = 0x1f;
        row.local[3] = 0x90;
        row.local[4..8].copy_from_slice(&[127, 0, 0, 1]);
        let mut out = [0xa5u8; 256];
        let n = render_net_rows(PF_NET_TCP, &[row], out.as_mut_ptr() as *mut c_char, out.len());
        let expected = b"  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n   7: 0100007F:1F90 00000000:0000 01 00000000:00000005 00:00000000 00000000 1000 3 0\n";
        assert_eq!(&out[..n as usize], expected);
        assert_eq!(out[n as usize], 0);
        let mut tiny = [0xa5u8; 8];
        let tn = render_net_rows(PF_NET_TCP, &[row], tiny.as_mut_ptr() as *mut c_char, tiny.len());
        assert_eq!(tn, 7);
        assert_eq!(&tiny[..7], b"  sl  l");
        assert_eq!(tiny[7], 0);
    }

    fn child(
        parent_type: i32,
        parent_pid: i32,
        name: &str,
        xlator: bool,
    ) -> (i32, ProcfsChildNode) {
        let mut b = [0u8; 64];
        b[..name.len()].copy_from_slice(name.as_bytes());
        let (g, gpid) = nt(name, xlator);
        let mut out = ProcfsChildNode { ty: 0, pid: 0, is_symlink: 0, class_type: 0 };
        let r = unsafe {
            a20_procfs_child_node(parent_type, parent_pid, b.as_ptr(), g, gpid, xlator as c_int,
                                  core::ptr::null_mut(), Some(probe_all), &mut out)
        };
        (r, out)
    }

    /// A probe that reports fd 7 open, id 42 visible as global 4242, and a
    /// thread sharing the owner's tgid for id 42.
    unsafe extern "C" fn probe_all(_c: *mut c_void, which: u32, arg: i64, out: *mut i64) -> i32 {
        let v = match which {
            PROBE_FD_EXISTS => (arg == 7) as i64,
            PROBE_PID_GLOBAL => (arg == 42) as i64 * 4242,
            PROBE_THREAD_OF_OWNER => (arg == 42) as i64 * 4343,
            _ => 0,
        };
        unsafe { *out = v };
        0
    }

    unsafe extern "C" fn probe_deny(_c: *mut c_void, _w: u32, _a: i64, _o: *mut i64) -> i32 {
        EACCES
    }

    #[test]
    fn name_table_keeps_the_original_order() {
        assert_eq!(nt("meminfo", true).0, PF_MEMINFO);
        assert_eq!(nt("a20", true).0, PF_A20);
        // "cgroup" resolves to the pid form, not the namespace form, because
        // the pid entry comes first.
        assert_eq!(nt("cgroup", true).0, PF_PID_CGROUP);
        // "net" resolves to the root net directory first.
        assert_eq!(nt("net", true).0, PF_NET);
        // "cmdline" resolves to the root form first.
        assert_eq!(nt("cmdline", true).0, PF_CMDLINE);
        // A pid-shaped name yields PF_ROOT plus the parsed id.
        assert_eq!(nt("1234", true), (PF_ROOT, 1234));
        // xlator is gated on the build flag.
        assert_eq!(nt("xlator", true).0, PF_A20_XLATOR);
        assert_eq!(nt("xlator", false).0, PF_ROOT);
        // Unknown names fall back to PF_ROOT with pid 0.
        assert_eq!(nt("no-such-node", true), (PF_ROOT, 0));
    }

    #[test]
    fn classification_and_modes() {
        let mut t = 0;
        let mut mo = 0;
        let l = unsafe { a20_procfs_node_class(PF_PID_EXE, 0, 0, &mut t, &mut mo) };
        assert_eq!((t, mo, l), (VFS_FT_SYMLINK, S_IFLNK | 0o777, VNODE_MAGICLINK));
        unsafe { a20_procfs_node_class(PF_PID_CWD, 0, 0, &mut t, &mut mo) };
        assert_eq!(t, VFS_FT_SYMLINK);
        // A /proc/<pid>/fd/N entry is a magic symlink even though its node
        // type is PF_PID_FD (which is otherwise a directory).
        unsafe { a20_procfs_node_class(PF_PID_FD, 0, 1, &mut t, &mut mo) };
        assert_eq!((t, mo), (VFS_FT_SYMLINK, S_IFLNK | 0o777));
        unsafe { a20_procfs_node_class(PF_SYS, 0, 0, &mut t, &mut mo) };
        assert_eq!((t, mo), (VFS_FT_DIR, S_IFDIR | 0o555));
        unsafe { a20_procfs_node_class(PF_MEMINFO, 0, 0, &mut t, &mut mo) };
        assert_eq!((t, mo), (VFS_FT_REGULAR, S_IFREG | 0o444));
        // A pid-shaped root name is a directory.
        unsafe { a20_procfs_node_class(PF_ROOT, 1, 0, &mut t, &mut mo) };
        assert_eq!((t, mo), (VFS_FT_DIR, S_IFDIR | 0o555));
        // Writable nodes override the mode.
        unsafe { a20_procfs_node_class(PF_SYSRQ_TRIGGER, 0, 0, &mut t, &mut mo) };
        assert_eq!(mo, S_IFREG | 0o200);
        unsafe { a20_procfs_node_class(PF_SYS_KERNEL_HOSTNAME, 0, 0, &mut t, &mut mo) };
        assert_eq!(mo, S_IFREG | 0o444);
        unsafe { a20_procfs_node_class(PF_SYS_KERNEL_CORE_PATTERN, 0, 0, &mut t, &mut mo) };
        assert_eq!(mo, S_IFREG | 0o644);
        unsafe { a20_procfs_node_class(PF_UID_MAP, 0, 0, &mut t, &mut mo) };
        assert_eq!(mo, S_IFREG | 0o644);
    }

    #[test]
    fn root_and_subtree_nodes_resolve() {
        assert_eq!(child(PF_ROOT, 0, "stat", true).1.ty, PF_STAT);
        assert_eq!(child(PF_ROOT, 0, "sys", true).1.ty, PF_SYS);
        assert_eq!(child(PF_SYS, 0, "kernel", true).1.ty, PF_SYS_KERNEL);
        assert_eq!(
            child(PF_SYS_KERNEL, 0, "hostname", true).1.ty,
            PF_SYS_KERNEL_HOSTNAME
        );
        assert_eq!(child(PF_SYS, 0, "vm", true).1.ty, PF_SYS_VM);
        assert_eq!(child(PF_SYS_VM, 0, "drop_caches", true).1.ty, PF_SYS_VM_DROP_CACHES);
        assert_eq!(child(PF_SYS, 0, "net", true).1.ty, PF_SYS_NET);
        assert_eq!(child(PF_NET, 0, "tcp6", true).1.ty, PF_NET_TCP6);
        assert_eq!(child(PF_A20, 0, "page_cache", true).1.ty, PF_A20_PAGE_CACHE);
        assert_eq!(child(PF_PRESSURE, 0, "memory", true).1.ty, PF_PRESSURE_MEM);
        // A node that only exists under a different parent is not found.
        assert_eq!(child(PF_SYS_VM, 0, "hostname", true).0, ENOENT);
        assert_eq!(child(PF_ROOT, 0, "no-such-node", true).0, ENOENT);
    }

    #[test]
    fn pid_scoped_nodes_use_the_named_pid() {
        let (_r, selfn) = child(PF_ROOT, 0, "self", true);
        assert_eq!((selfn.ty, selfn.pid), (PF_ROOT, -1));
        assert_eq!(selfn.class_type, PF_SELF);
        let (_r2, tself) = child(PF_ROOT, 0, "thread-self", true);
        assert_eq!((tself.ty, tself.pid), (PF_ROOT, -1));
        assert_eq!(tself.class_type, PF_THREAD_SELF);
        let (r, n) = child(PF_ROOT, 0, "42", true);
        assert_eq!((r, n.ty, n.pid), (0, PF_ROOT, 4242));
        assert_eq!(child(PF_ROOT, 0, "43", true).0, ENOENT);
        let (r, n) = child(PF_ROOT, 77, "cmdline", true);
        assert_eq!((r, n.ty, n.pid), (0, PF_PID_CMDLINE, 77));
        let (r, n) = child(PF_ROOT, 77, "mounts", true);
        assert_eq!((r, n.ty, n.pid), (0, PF_MOUNTS, 0));
        assert_eq!(child(PF_ROOT, 77, "meminfo", true).0, ENOENT);
        let (r, n) = child(PF_ROOT, 77, "uid_map", true);
        assert_eq!((r, n.ty, n.pid), (0, PF_UID_MAP, 77));
    }

    #[test]
    fn dynamic_entries_go_through_the_probe() {
        assert_eq!(child(PF_PID_FD, 9, "7", true).1.is_symlink, 1);
        assert_eq!(child(PF_PID_FD, 9, "8", true).0, ENOENT);
        assert_eq!(child(PF_PID_FD, 9, "abc", true).0, ENOENT);
        assert_eq!(child(PF_PID_FDINFO, 9, "7", true).1.ty, PF_PID_FDINFO_ENTRY);
        let (r, n) = child(PF_PID_TASK, 9, "42", true);
        assert_eq!((r, n.ty, n.pid), (0, PF_ROOT, 4343));
        assert_eq!(child(PF_PID_TASK, 9, "43", true).0, ENOENT);
        // The namespace directory is a static table.
        assert_eq!(child(PF_PID_NS, 9, "mnt", true).1.ty, PF_PID_NS_MNT);
        assert_eq!(child(PF_PID_NS, 9, "mnt", true).1.pid, 9);
        assert_eq!(child(PF_PID_NS, 9, "bogus", true).0, ENOENT);
        // A probe refusal is reported as its errno.
        let mut b = [0u8; 8];
        b[..1].copy_from_slice(b"7");
        let mut out = ProcfsChildNode { ty: 0, pid: 0, is_symlink: 0, class_type: 0 };
        let r = unsafe {
            a20_procfs_child_node(PF_PID_FD, 9, b.as_ptr(), PF_PID_FD, 0, 1,
                                  core::ptr::null_mut(), Some(probe_deny), &mut out)
        };
        assert_eq!(r, EACCES);
    }

    #[test]
    fn content_plan_matches_the_original_lengths() {
        let mut p = ProcfsContentPlan { generator: 0, capacity: 0, fixed_len: 0, meta_len_zero: 0 };
        unsafe { a20_procfs_content_plan(PF_PID_PAGEMAP, 4, 4096, &mut p) };
        assert_eq!(p.generator, GEN_PAGEMAP_LEN);
        assert_eq!(p.fixed_len, pagemap_len(4096));
        let mut q = ProcfsContentPlan { generator: 0, capacity: 0, fixed_len: 0, meta_len_zero: 0 };
        unsafe { a20_procfs_content_plan(PF_PID_PAGEMAP, 4, 65536, &mut q) };
        assert_eq!(q.fixed_len, pagemap_len(65536));
        assert_eq!(p.meta_len_zero, 0);
        unsafe { a20_procfs_content_plan(PF_PID_SMAPS, 4, 4096, &mut p) };
        assert_eq!((p.generator, p.meta_len_zero), (GEN_MAPS_ALLOC, 1));
        unsafe { a20_procfs_content_plan(PF_PID_FDINFO_ENTRY, 4, 4096, &mut p) };
        assert_eq!((p.generator, p.capacity), (GEN_FDINFO, 4096));
        unsafe { a20_procfs_content_plan(PF_CPUINFO, 4, 4096, &mut p) };
        assert_eq!((p.generator, p.capacity), (GEN_CONTENT, 512 * 4 + 1));
        for ty in [PF_A20_PERF, PF_A20_LOCK_CONTENTION] {
            unsafe { a20_procfs_content_plan(ty, 8, 4096, &mut p) };
            assert_eq!((p.generator, p.capacity, p.meta_len_zero),
                       (GEN_GROW_CONTENT, 4096, 1));
        }
        unsafe { a20_procfs_content_plan(PF_MEMINFO, 8, 4096, &mut p) };
        assert_eq!((p.generator, p.capacity, p.meta_len_zero), (GEN_CONTENT, 4096, 0));
    }

    #[test]
    fn classification_and_resource_kind_helpers() {
        assert_eq!(a20_procfs_root_file_type(PF_MEMINFO), 1);
        assert_eq!(a20_procfs_root_file_type(PF_SYSRQ_TRIGGER), 1);
        assert_eq!(a20_procfs_root_file_type(PF_SYS), 0);
        assert_eq!(a20_procfs_root_file_type(PF_A20), 0);
        assert_eq!(a20_procfs_resource_kind(PF_PID_NS_UTS), PROCNS_UTS);
        assert_eq!(a20_procfs_resource_kind(PF_PID_NS_IPC), PROCNS_IPC);
        assert_eq!(a20_procfs_resource_kind(PF_PID_NS_NET), PROCNS_NET);
        assert_eq!(a20_procfs_resource_kind(PF_PID_NS_CGROUP), PROCNS_CGROUP);
        assert_eq!(a20_procfs_resource_kind(PF_PID_NS_MNT), 0);
    }

    /* ------------------------ content generation ------------------------ */

    #[derive(Default)]
    struct CWorld {
        ticks: u64,
        tps: u64,
        loadavg: [u64; 3],
        running: u32,
        total: u32,
        max_pid: c_int,
        anonprov: u32,
        sched_slice: c_int,
        pid_max: c_int,
        cur_pid: c_int,
        pipe_max: c_int,
        lease: c_int,
        max_files: c_int,
        mem_total_frames: u64,
        mem_free_frames: u64,
        mem_slab_total: u64,
        mem_slab_reclaim: u64,
        mem_bcache_pool: u64,
        mem_bcache_valid: u64,
        mem_bcache_dirty_blocks: u64,
        mem_bcache_dirty_pages: u64,
        mem_pcache_valid: u64,
        mem_pcache_dirty: u64,
        mem_huge_total: u64,
        mem_huge_free: u64,
        mem_swap_total: u64,
        mem_swap_free: u64,
        mounts: Vec<(&'static [u8], &'static [u8], &'static [u8], &'static [u8])>,
        nr_cpus: c_int,
        cpu_times: Vec<(u64, u64, u64)>,
        realtime: u64,
        monotonic: u64,
        tasks_created: u64,
        runq_load_sum: u64,
        swaps: Vec<(&'static [u8], u64, u64)>,
        online_mask: u32,
        oom_kills: u64,
        obj_vfiles: u64,
        sysv_msg: i32,
        sysv_sem: i32,
        sysv_shm: i32,
        pid_present: bool,
        pid_vsize: u64,
        oom_adj: i32,
        cgroup_path: Vec<u8>,
        setgroups_allowed: i32,
        msix_cpus: i32,
        msix_total: u32,
        msix_n: u32,
    }

    thread_local! {
        static CW: RefCell<CWorld> = RefCell::new(CWorld::default());
    }

    static SYSNAME: &[u8] = b"A20OS\0";
    static RELEASE: &[u8] = b"1.0.3\0";
    static ARCH: &[u8] = b"riscv64\0";
    static VFULL: &[u8] = b"v1.0.3-full\0";
    static BOOTARGS: &[u8] = b"root=x a20.ip=1.2.3.4\0";

    extern "C" fn g_sysname() -> *const c_char {
        SYSNAME.as_ptr() as *const c_char
    }
    extern "C" fn g_release() -> *const c_char {
        RELEASE.as_ptr() as *const c_char
    }
    extern "C" fn g_arch() -> *const c_char {
        ARCH.as_ptr() as *const c_char
    }
    extern "C" fn g_vfull() -> *const c_char {
        VFULL.as_ptr() as *const c_char
    }
    extern "C" fn g_ticks() -> u64 {
        CW.with(|w| w.borrow().ticks)
    }
    extern "C" fn g_tps() -> u64 {
        CW.with(|w| w.borrow().tps)
    }
    extern "C" fn g_bootargs() -> *const c_char {
        BOOTARGS.as_ptr() as *const c_char
    }
    extern "C" fn g_loadavg(
        a1: *mut u64,
        a5: *mut u64,
        a15: *mut u64,
        run: *mut u32,
        tot: *mut u32,
        mp: *mut c_int,
    ) {
        CW.with(|w| {
            let b = w.borrow();
            unsafe {
                *a1 = b.loadavg[0];
                *a5 = b.loadavg[1];
                *a15 = b.loadavg[2];
                *run = b.running;
                *tot = b.total;
                *mp = b.max_pid;
            }
        });
    }
    extern "C" fn g_anonprov() -> u32 {
        CW.with(|w| w.borrow().anonprov)
    }
    extern "C" fn g_sched() -> c_int {
        CW.with(|w| w.borrow().sched_slice)
    }
    extern "C" fn g_pidmax() -> c_int {
        CW.with(|w| w.borrow().pid_max)
    }
    extern "C" fn g_curpid() -> c_int {
        CW.with(|w| w.borrow().cur_pid)
    }
    extern "C" fn g_pipemax() -> c_int {
        CW.with(|w| w.borrow().pipe_max)
    }
    extern "C" fn g_lease() -> c_int {
        CW.with(|w| w.borrow().lease)
    }
    extern "C" fn g_maxfiles() -> c_int {
        CW.with(|w| w.borrow().max_files)
    }
    extern "C" fn g_meminfo(out: *mut A20MeminfoRaw) {
        CW.with(|w| {
            let b = w.borrow();
            unsafe {
                (*out).total_frames = b.mem_total_frames;
                (*out).free_frames = b.mem_free_frames;
                (*out).slab_total_bytes = b.mem_slab_total;
                (*out).slab_reclaim_bytes = b.mem_slab_reclaim;
                (*out).bcache_pool_bytes = b.mem_bcache_pool;
                (*out).bcache_valid_pages = b.mem_bcache_valid;
                (*out).bcache_dirty_blocks = b.mem_bcache_dirty_blocks;
                (*out).bcache_dirty_pages = b.mem_bcache_dirty_pages;
                (*out).pcache_valid = b.mem_pcache_valid;
                (*out).pcache_dirty = b.mem_pcache_dirty;
                (*out).vm_anon_huge = 0;
                (*out).vm_shmem_huge = 0;
                (*out).vm_file_huge = 0;
                (*out).huge_total = b.mem_huge_total;
                (*out).huge_free = b.mem_huge_free;
                (*out).swap_total_pages = b.mem_swap_total;
                (*out).swap_free_pages = b.mem_swap_free;
                (*out).page_size = 4096;
                (*out).pcache_page_size = 4096;
                (*out).bcache_block_size = 512;
            }
        });
    }

    extern "C" fn g_mount_count() -> c_int {
        CW.with(|w| w.borrow().mounts.len() as c_int)
    }

    extern "C" fn g_nr_cpus() -> c_int {
        CW.with(|w| w.borrow().nr_cpus)
    }
    extern "C" fn g_cpu_times(cpu: c_int, u: *mut u64, s: *mut u64, idl: *mut u64) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let idx = cpu as usize;
            let t = b.cpu_times.get(idx).copied().unwrap_or((0, 0, 0));
            unsafe {
                *u = t.0;
                *s = t.1;
                *idl = t.2;
            }
            0
        })
    }
    extern "C" fn g_realtime(out: *mut u64) {
        CW.with(|w| unsafe { *out = w.borrow().realtime });
    }
    extern "C" fn g_monotonic(out: *mut u64) {
        CW.with(|w| unsafe { *out = w.borrow().monotonic });
    }
    extern "C" fn g_tasks_created() -> u64 {
        CW.with(|w| w.borrow().tasks_created)
    }
    extern "C" fn g_runq_load_sum() -> u64 {
        CW.with(|w| w.borrow().runq_load_sum)
    }
    extern "C" fn g_swap_get(i: c_int, out: *mut A20SwapEnt) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let idx = i as usize;
            if idx >= b.swaps.len() {
                return 0;
            }
            let s = &b.swaps[idx];
            unsafe {
                let e = &mut *out;
                e.name = [0; 64];
                e.name[..s.0.len()].copy_from_slice(s.0);
                e.pages = s.1;
                e.inuse_pages = s.2;
            }
            1
        })
    }
    extern "C" fn g_online_mask() -> u32 {
        CW.with(|w| w.borrow().online_mask)
    }
    extern "C" fn g_cpuinfo_fields(buf: *mut c_char, sz: usize) -> c_int {
        let msg = b"isa:\trv64imafdc\nmmu:\tsv39\n";
        let n = core::cmp::min(msg.len(), sz.saturating_sub(1));
        for i in 0..n {
            unsafe { *buf.add(i) = msg[i] as c_char };
        }
        if sz > 0 {
            unsafe { *buf.add(n) = 0 };
        }
        n as c_int
    }

    extern "C" fn g_mount_at(i: c_int, out: *mut A20MountEnt) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let idx = i as usize;
            if idx >= b.mounts.len() {
                return 0;
            }
            let m = &b.mounts[idx];
            unsafe {
                let e = &mut *out;
                e.dev = [0; 32];
                e.path = [0; 256];
                e.fstype = [0; 32];
                e.opts = [0; 32];
                e.mnt_id = 100 + idx as u32;
                e.parent_id = 1;
                e.detached = 0;
                e.dev[..m.0.len()].copy_from_slice(m.0);
                e.path[..m.1.len()].copy_from_slice(m.1);
                e.fstype[..m.2.len()].copy_from_slice(m.2);
                e.opts[..m.3.len()].copy_from_slice(m.3);
            }
            1
        })
    }

    extern "C" fn g_bcache(out: *mut A20BcacheRaw) {
        CW.with(|w| {
            let b = w.borrow();
            unsafe {
                (*out).caches = 2;
                (*out).block_pool_bytes = b.mem_bcache_pool;
                (*out).page_pool_bytes = 4096;
                (*out).valid_blocks = b.mem_bcache_valid;
                (*out).dirty_blocks = b.mem_bcache_dirty_blocks;
                (*out).valid_pages = b.mem_bcache_dirty_pages;
                (*out).dirty_pages = b.mem_pcache_dirty;
            }
        });
    }
    extern "C" fn g_pagecache(out: *mut A20PagecacheRaw) {
        CW.with(|w| {
            let b = w.borrow();
            unsafe {
                (*out).capacity = 64;
                (*out).allocated = b.mem_pcache_valid;
                (*out).bytes = b.mem_pcache_valid * 4096;
                (*out).valid = b.mem_pcache_valid;
                (*out).dirty = b.mem_pcache_dirty;
                (*out).pinned = 1;
            }
        });
    }
    extern "C" fn g_oom(out: *mut A20OomRaw) {
        CW.with(|w| {
            let b = w.borrow();
            unsafe {
                (*out).kills = b.oom_kills;
                (*out).last_kill_tick = 0;
                (*out).last_victim_pid = -1;
                (*out).last_victim_score = 0;
                (*out).free_pages_at_kill = 0;
                (*out).free_pages_now = 0;
                (*out).in_progress = 0;
                (*out).kswapd_passes = 0;
                (*out).kswapd_pages_freed = 0;
                (*out).kswapd_last_pass_tick = 0;
            }
        });
    }
    extern "C" fn g_objstats(out: *mut A20ObjstatsRaw) {
        CW.with(|w| {
            let b = w.borrow();
            unsafe {
                (*out).handles = 1;
                (*out).channel_eps = 2;
                (*out).eventqs = 3;
                (*out).vmos = 4;
                (*out).vmo_pages = 5;
                (*out).irq_bindings = 6;
                (*out).vfiles = b.obj_vfiles;
                (*out).vmo_dirty_frames = 8;
            }
        });
    }
    extern "C" fn g_iommu(out: *mut A20IommuRaw) {
        CW.with(|w| {
            let _ = w;
            unsafe {
                (*out).enabled = 1;
                (*out).domains_claimed = 1;
                (*out).domains_released = 0;
                (*out).maps = 2;
                (*out).map_failures = 0;
                (*out).unmaps = 0;
                (*out).mapped_pages = 3;
                (*out).fault_records = 0;
                (*out).faults = 0;
                (*out).blocked_events = 0;
                (*out).last_fault_devid = 0x1234;
                (*out).last_fault_cause = 7;
                (*out).last_fault_iova = 0xdeadbeef;
                (*out).last_fault_owner = -1;
            }
        });
    }

    extern "C" fn g_sysvipc(out: *mut A20SysvipcRaw) {
        CW.with(|w| {
            let b = w.borrow();
            unsafe {
                (*out).msg = b.sysv_msg;
                (*out).sem = b.sysv_sem;
                (*out).shm = b.sysv_shm;
            }
        });
    }
    static GZBLOB: &[u8] = b"\x1f\x8b\x08\x00raw-gz";
    extern "C" fn g_config_gz(data: *mut *const u8, len: *mut usize) {
        unsafe {
            *data = GZBLOB.as_ptr();
            *len = GZBLOB.len();
        }
    }

    extern "C" fn g_overflow_id(gid: c_int) -> c_int {
        if gid != 0 {
            65534
        } else {
            65534
        }
    }
    static HOSTNAME: &[u8] = b"a20os\0";
    static DOMAINNAME: &[u8] = b"(none)\0";
    extern "C" fn g_hostname() -> *const c_char {
        HOSTNAME.as_ptr() as *const c_char
    }
    extern "C" fn g_domainname() -> *const c_char {
        DOMAINNAME.as_ptr() as *const c_char
    }
    extern "C" fn g_psi(kind: c_int, buf: *mut c_char, sz: usize) -> c_int {
        let msg: &[u8] = match kind {
            1 => b"some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n",
            2 => b"some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n",
            _ => b"some avg10=1.00 avg60=2.00 avg300=3.00 total=4\n",
        };
        let n = core::cmp::min(msg.len(), sz.saturating_sub(1));
        for i in 0..n {
            unsafe { *buf.add(i) = msg[i] as c_char };
        }
        if sz > 0 {
            unsafe { *buf.add(n) = 0 };
        }
        n as c_int
    }
    extern "C" fn g_pidmap(buf: *mut c_char, sz: usize) -> c_int {
        let msg = b"00000000 : 00004000\n";
        let n = core::cmp::min(msg.len(), sz.saturating_sub(1));
        for i in 0..n {
            unsafe { *buf.add(i) = msg[i] as c_char };
        }
        if sz > 0 {
            unsafe { *buf.add(n) = 0 };
        }
        n as c_int
    }

    extern "C" fn g_pid_raw(pid: c_int, out: *mut A20PidRaw) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            if !b.pid_present {
                return 0;
            }
            unsafe {
                let p = &mut *out;
                *p = zero_pid();
                p.pid = pid;
                p.ppid = 1;
                p.pgid = pid;
                p.sid = pid;
                p.name[..4].copy_from_slice(b"init");
                p.state_char = b'R';
                p.state_text[..11].copy_from_slice(b"R (running)");
                p.utime_ticks = 100;
                p.stime_ticks = 50;
                p.child_utime = 1;
                p.child_stime = 2;
                p.priority = 20;
                p.start_jiffies = 7;
                p.vsize = b.pid_vsize;
                p.rss_pages = 3;
                p.processor = 13;
                p.uid = 0;
                p.euid = 0;
                p.suid = 0;
                p.fsuid = 0;
                p.gid = 0;
                p.egid = 0;
                p.sgid = 0;
                p.fsgid = 0;
                p.cap_inh = 0;
                p.cap_prm = 0x3fffffffff;
                p.cap_eff = 0x3fffffffff;
                p.cap_bnd = 0x3fffffffff;
                p.ngroups = 2;
                p.groups[0] = 0;
                p.groups[1] = 4;
                p.rss_kb = 12;
                p.vmlck_kb = 0;
                p.vmdata_kb = 8;
            }
            1
        })
    }
    extern "C" fn g_pid_str(pid: c_int, which: c_int, out: *mut c_char, sz: usize) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let msg: &[u8] = match which {
                1 => b"/sbin/init",
                2 => b"/",
                3 => {
                    if b.pid_present {
                        b"/sbin/init"
                    } else {
                        b""
                    }
                }
                _ => {
                    if b.pid_present {
                        b"init"
                    } else {
                        b"unknown"
                    }
                }
            };
            let _ = pid;
            let n = core::cmp::min(msg.len(), sz);
            for i in 0..n {
                unsafe { *out.add(i) = msg[i] as c_char };
            }
            n as c_int
        })
    }
    extern "C" fn g_pid_io(pid: c_int, out: *mut A20PidIoRaw) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let _ = pid;
            unsafe {
                (*out).rchar = 11;
                (*out).wchar = 22;
                (*out).syscr = 3;
                (*out).syscw = 4;
                (*out).read_bytes = 55;
                (*out).write_bytes = 66;
            }
            if b.pid_present { 1 } else { 0 }
        })
    }
    extern "C" fn g_pid_sessionid(pid: c_int) -> c_int {
        CW.with(|w| if w.borrow().pid_present { pid } else { 0 })
    }

    extern "C" fn g_pid_ns_ino(pid: c_int, kind: c_int) -> u64 {
        CW.with(|w| {
            let b = w.borrow();
            let _ = pid;
            if !b.pid_present {
                return 0x4000;
            }
            match kind {
                0 => 0x4020,
                1 => 0x4021,
                2 => 0x4030,
                3 => 0x4040,
                4 => 0x4050,
                5 => 0x4060,
                6 => 0x4070,
                _ => 0x4080,
            }
        })
    }
    extern "C" fn g_format_helper(which: c_int, buf: *mut c_char, sz: usize) -> c_int {
        let msg: &[u8] = match which {
            0 => b"hyp_selftest=PASS\n",
            9 => b"module A 0\n",
            _ => b"H\n",
        };
        let n = core::cmp::min(msg.len(), sz.saturating_sub(1));
        for i in 0..n {
            unsafe { *buf.add(i) = msg[i] as c_char };
        }
        if sz > 0 {
            unsafe { *buf.add(n) = 0 };
        }
        n as c_int
    }
    extern "C" fn g_hyp_selftest() -> c_int {
        0
    }

    extern "C" fn g_pid_oom(pid: c_int, adj: *mut c_int) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let _ = pid;
            unsafe { *adj = b.oom_adj };
            if b.pid_present { 1 } else { 0 }
        })
    }
    extern "C" fn g_pid_cgroup(pid: c_int, out: *mut c_char, sz: usize) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let _ = pid;
            let msg: &[u8] = final_cgroup(&b.cgroup_path);
            let n = core::cmp::min(msg.len(), sz.saturating_sub(1));
            for i in 0..n {
                unsafe { *out.add(i) = msg[i] as c_char };
            }
            if sz > 0 {
                unsafe { *out.add(n) = 0 };
            }
            n as c_int
        })
    }
    fn final_cgroup(p: &[u8]) -> &[u8] {
        p
    }
    extern "C" fn g_userns_map(pid: c_int, is_gid: c_int, out: *mut A20MapRaw) -> c_int {
        CW.with(|_w| {
            let _ = (pid, is_gid);
            unsafe {
                (*out).n = 2;
                (*out).ent[0] = A20MapEnt { lower: 0, parent_lower: 65534, count: 1 };
                (*out).ent[1] = A20MapEnt { lower: 1, parent_lower: 100000, count: 65535 };
            }
            2
        })
    }
    extern "C" fn g_setgroups(pid: c_int, allowed: *mut c_int) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let _ = pid;
            unsafe { *allowed = b.setgroups_allowed };
            if b.pid_present { 1 } else { 0 }
        })
    }

    extern "C" fn g_pid_exists(_pid: c_int) -> c_int {
        CW.with(|w| if w.borrow().pid_present { 1 } else { 0 })
    }
    extern "C" fn g_pid_limits(pid: c_int, out: *mut A20LimitsRaw) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let _ = pid;
            if !b.pid_present {
                return 0;
            }
            unsafe {
                (*out).nofile = 1024;
                (*out).memlock = 65536;
            }
            1
        })
    }
    extern "C" fn g_pid_wchan(pid: c_int, out: *mut c_char, sz: usize) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let _ = pid;
            if !b.pid_present {
                return -3; /* -ESRCH */
            }
            let msg: &[u8] = b"do_wait";
            for i in 0..msg.len().min(sz) {
                unsafe { *out.add(i) = msg[i] as c_char };
            }
            msg.len() as c_int
        })
    }
    extern "C" fn g_pid_auxv(pid: c_int, buf: *mut u8, cap: u32) -> c_int {
        CW.with(|w| {
            let b = w.borrow();
            let _ = pid;
            if !b.pid_present {
                return 0;
            }
            let pairs: [u64; 4] = [7, 0x1000, 0, 0]; /* AT_PHDR/AT_PHENT/AT_NULL */
            let len = pairs.len() * 8;
            if len as u32 > cap {
                return 0;
            }
            for (i, v) in pairs.iter().enumerate() {
                let bytes = v.to_ne_bytes();
                for (j, byte) in bytes.iter().enumerate() {
                    unsafe { *buf.add(i * 8 + j) = *byte };
                }
            }
            len as c_int
        })
    }

    extern "C" fn g_msix_cpu_count() -> c_int {
        CW.with(|w| w.borrow().msix_cpus)
    }
    extern "C" fn g_msix_snapshot(out: *mut A20MsixRaw) -> u32 {
        CW.with(|w| {
            let b = w.borrow();
            unsafe {
                let o = &mut *out;
                o.total = b.msix_total;
                o.n = b.msix_n;
                for i in 0..b.msix_n as usize {
                    o.ent[i] = A20MsixEnt { name: [0; 32], index: i as u32, vector: 0x10 + i as u32, cpu: 1 };
                }
                o.ent[0].name[..3].copy_from_slice(b"eth");
            }
            b.msix_total
        })
    }
    extern "C" fn g_net_snapshot(_kind: c_int, out: *mut *mut A20ProcfsNetRow,
                                  count: *mut usize) -> c_int {
        unsafe { *out = core::ptr::null_mut(); *count = 0; }
        0
    }
    extern "C" fn g_net_snapshot_free(_rows: *mut A20ProcfsNetRow) {}

    fn gtable() -> A20ProcfsGen {
        A20ProcfsGen {
            sysname: Some(g_sysname),
            release: Some(g_release),
            arch_name: Some(g_arch),
            version_full: Some(g_vfull),
            ticks: Some(g_ticks),
            ticks_per_sec: Some(g_tps),
            bootargs: Some(g_bootargs),
            loadavg: Some(g_loadavg),
            anonprov_max: Some(g_anonprov),
            sched_base_slice: Some(g_sched),
            pid_max: Some(g_pidmax),
            current_pid: Some(g_curpid),
            pipe_max_size: Some(g_pipemax),
            lease_break_time: Some(g_lease),
            max_files: Some(g_maxfiles),
            meminfo_raw: Some(g_meminfo),
            mount_count: Some(g_mount_count),
            mount_at: Some(g_mount_at),
            nr_cpus: Some(g_nr_cpus),
            cpu_times: Some(g_cpu_times),
            realtime: Some(g_realtime),
            monotonic: Some(g_monotonic),
            tasks_created: Some(g_tasks_created),
            runq_load_sum: Some(g_runq_load_sum),
            swap_get: Some(g_swap_get),
            online_cpu_mask: Some(g_online_mask),
            cpuinfo_fields: Some(g_cpuinfo_fields),
            bcache_raw: Some(g_bcache),
            pagecache_raw: Some(g_pagecache),
            oom_raw: Some(g_oom),
            objstats_raw: Some(g_objstats),
            iommu_raw: Some(g_iommu),
            sysvipc_raw: Some(g_sysvipc),
            config_gz: Some(g_config_gz),
            userns_overflow_id: Some(g_overflow_id),
            hostname: Some(g_hostname),
            domainname: Some(g_domainname),
            psi_render: Some(g_psi),
            pidmap: Some(g_pidmap),
            pid_raw: Some(g_pid_raw),
            pid_str: Some(g_pid_str),
            pid_io: Some(g_pid_io),
            pid_sessionid: Some(g_pid_sessionid),
            pid_ns_ino: Some(g_pid_ns_ino),
            format_helper: Some(g_format_helper),
            hyp_selftest: Some(g_hyp_selftest),
            pid_oom: Some(g_pid_oom),
            pid_cgroup_path: Some(g_pid_cgroup),
            userns_map: Some(g_userns_map),
            userns_setgroups_allowed: Some(g_setgroups),
            pid_exists: Some(g_pid_exists),
            pid_limits: Some(g_pid_limits),
            pid_wchan: Some(g_pid_wchan),
            pid_auxv: Some(g_pid_auxv),
            msix_cpu_count: Some(g_msix_cpu_count),
            msix_snapshot: Some(g_msix_snapshot),
            net_snapshot: Some(g_net_snapshot),
            net_snapshot_free: Some(g_net_snapshot_free),
        }
    }

    fn gen_text(ty: i32, pid: c_int, cap: usize) -> (c_int, String) {
        let g = gtable();
        let mut buf = vec![0u8; cap];
        let rc = unsafe {
            a20_procfs_generate(
                &g,
                ty,
                pid,
                buf.as_mut_ptr() as *mut c_char,
                cap,
            )
        };
        let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
        (rc, String::from_utf8_lossy(&buf[..end]).into_owned())
    }

    fn cw_reset() {
        CW.with(|w| {
            let mut b = w.borrow_mut();
            *b = CWorld::default();
            b.tps = 100;
            b.max_files = 1024;
            b.nr_cpus = 2;
            b.online_mask = 0x3;
            b.pid_present = true;
            b.pid_vsize = 8192;
            b.oom_adj = 5;
            b.cgroup_path = b"/a20/init".to_vec();
            b.setgroups_allowed = 1;
            b.msix_cpus = 1;
            b.msix_total = 1;
            b.msix_n = 1;
        });
    }

    #[test]
    fn content_version_line() {
        cw_reset();
        let (_, s) = gen_text(PF_VERSION, 0, 256);
        assert_eq!(s, "A20OS version 1.0.3 (riscv64) (v1.0.3-full)\n");
    }

    #[test]
    fn content_uptime_format() {
        cw_reset();
        CW.with(|w| w.borrow_mut().ticks = 1234); /* 100 ticks/s -> 12.34 */
        let (rc, s) = gen_text(PF_UPTIME, 0, 64);
        assert_eq!(s, "12.34\n");
        assert_eq!(rc, 6);
    }

    #[test]
    fn content_cmdline_and_boot_id() {
        cw_reset();
        let (_, s) = gen_text(PF_CMDLINE, 0, 128);
        assert_eq!(s, "root=x a20.ip=1.2.3.4\n");

        CW.with(|w| w.borrow_mut().ticks = 0x0000_0001_0000_0002);
        let (_, u) = gen_text(PF_BOOT_ID, 0, 128);
        /* ticks-derived uuid: 8-4-4-4-12 hex fields + newline */
        let parts: Vec<&str> = u.trim_end().split('-').collect();
        assert_eq!(parts.len(), 5);
        assert_eq!(parts[0].len(), 8);
        assert_eq!(parts[1].len(), 4);
        assert_eq!(parts[3].len(), 4);
        /* C emits two 8-hex groups in the last field (16), not the standard 12. */
        assert_eq!(parts[4].len(), 16);
    }

    #[test]
    fn content_loadavg_scales_to_hundredths() {
        cw_reset();
        CW.with(|w| {
            let mut b = w.borrow_mut();
            b.loadavg = [65536 / 2, 65536, 65536 + 65536 / 2]; /* 0.5,1.0,1.5 */
            b.running = 2;
            b.total = 7;
            b.max_pid = 4194304;
        });
        let (_, s) = gen_text(PF_LOADAVG, 0, 128);
        assert_eq!(s, "0.50 1.00 1.50 2/7 4194304\n");
    }

    #[test]
    fn content_scalar_nodes() {
        cw_reset();
        CW.with(|w| {
            let mut b = w.borrow_mut();
            b.anonprov = 12;
            b.sched_slice = 9;
            b.pid_max = 4194304;
            b.cur_pid = 42;
            b.pipe_max = 1048576;
            b.lease = 45;
        });
        assert_eq!(gen_text(PF_A20_ANONPROV, 0, 32).1, "12\n");
        assert_eq!(gen_text(PF_A20_SCHED_BASE_SLICE, 0, 32).1, "9\n");
        assert_eq!(gen_text(PF_SYS_KERNEL_PID_MAX, 0, 32).1, "4194304\n");
        assert_eq!(gen_text(PF_SELF, 0, 32).1, "42\n");
        assert_eq!(gen_text(PF_SYS_FS_PIPE_MAX_SIZE, 0, 32).1, "1048576\n");
        assert_eq!(gen_text(PF_SYS_FS_LEASE_BREAK_TIME, 0, 32).1, "45\n");
        assert_eq!(gen_text(PF_NR_OPEN, 0, 32).1, "1024\n");
        assert_eq!(gen_text(PF_CAP_LAST_CAP, 0, 32).1, "63\n");
    }

    #[test]
    fn content_fixed_nodes_and_zero_nodes() {
        cw_reset();
        assert_eq!(gen_text(PF_SYS_KERNEL_TAINTED, 0, 32).1, "0\n");
        assert_eq!(gen_text(PF_SYS_KERNEL_SCHED_AUTOGROUP, 0, 32).1, "0\n");
        assert_eq!(gen_text(PF_SYS_KERNEL_IO_URING_DISABLED, 0, 32).1, "0\n");
        assert_eq!(gen_text(PF_SYS_VM_DROP_CACHES, 0, 32).1, "0\n");
        assert_eq!(gen_text(PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS, 0, 32).1, "16384\n");
        assert_eq!(gen_text(PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES, 0, 32).1, "128\n");
        assert!(gen_text(PF_SYSRQ_TRIGGER, 0, 256).1.starts_with("sysrq commands: c"));
        assert!(gen_text(PF_FSTYPE, 0, 256).1.contains("nodev\tproc\n"));
        assert!(gen_text(PF_CGROUPS, 0, 256).1.contains("cpu\t1\t1\t1\n"));
        assert_eq!(gen_text(PF_SYS_KERNEL_OSRELEASE, 0, 32).1, "1.0.3\n");
    }

    #[test]
    fn content_pagemap_is_empty_zero() {
        cw_reset();
        let g = gtable();
        let mut buf = [0xAAu8; 16];
        let rc = unsafe {
            a20_procfs_generate(&g, PF_PID_PAGEMAP, 1, buf.as_mut_ptr() as *mut c_char, 16)
        };
        assert_eq!(rc, 0);
        assert_eq!(buf[0], 0);
    }

    #[test]
    fn content_unknown_kind_is_empty() {
        cw_reset();
        /* a directory/unknown kind carries no content */
        let (rc, s) = gen_text(PF_PID_FD, 0, 64);
        assert_eq!((rc, s.as_str()), (0, ""));
    }

    #[test]
    fn content_truncates_at_capacity() {
        cw_reset();
        /* cap 8 -> at most 7 content bytes + NUL */
        let (rc, s) = gen_text(PF_CMDLINE, 0, 8);
        assert!(rc <= 7);
        assert_eq!(s.len(), rc as usize);
        assert!(s.len() <= 7);
    }

    #[test]
    fn content_meminfo_formulas() {
        cw_reset();
        CW.with(|w| {
            let mut b = w.borrow_mut();
            b.mem_total_frames = 1024; /* 1024*4096 = 4 MiB -> 4096 kB */
            b.mem_free_frames = 256; /* 1024 kB */
            b.mem_bcache_pool = 2048 * 1024; /* 2048 kB */
            b.mem_bcache_valid = 8; /* 8*4096 = 32 KiB -> 32 kB */
            b.mem_pcache_valid = 4; /* 16 KiB -> 16 kB; cached = 48 kB */
            b.mem_slab_total = 4096 * 1024; /* 4096 kB */
            b.mem_slab_reclaim = 1024 * 1024; /* 1024 kB */
            b.mem_huge_total = 512;
            b.mem_huge_free = 500;
            b.mem_swap_total = 128; /* 512 kB */
            b.mem_swap_free = 64; /* 256 kB */
        });
        let (_, s) = gen_text(PF_MEMINFO, 0, 1024);
        assert!(s.contains("MemTotal:       4096 kB\n"), "{}", s);
        assert!(s.contains("MemFree:        1024 kB\n"), "{}", s);
        /* MemAvailable = free 1024 + cached 48 + sreclaim 1024 = 2096 */
        assert!(s.contains("MemAvailable:   2096 kB\n"), "{}", s);
        assert!(s.contains("Buffers:        2048 kB\n"), "{}", s);
        assert!(s.contains("Cached:         48 kB\n"), "{}", s);
        assert!(s.contains("SwapTotal:      512 kB\n"), "{}", s);
        assert!(s.contains("SwapFree:       256 kB\n"), "{}", s);
        assert!(s.contains("Slab:           4096 kB\n"), "{}", s);
        assert!(s.contains("SReclaimable:   1024 kB\n"), "{}", s);
        assert!(s.contains("SUnreclaim:     3072 kB\n"), "{}", s);
        assert!(s.contains("HugePages_Total: 512\n"), "{}", s);
        assert!(s.contains("HugePages_Free:  500\n"), "{}", s);
        assert!(s.contains("Hugepagesize:   2048 kB\n"), "{}", s);
    }

    #[test]
    fn content_mounts_lists_entries_skipping_empty_paths() {
        cw_reset();
        CW.with(|w| {
            w.borrow_mut().mounts = vec![
                (b"rootfs", b"/", b"ext4", b"rw"),
                (b"none", b"/proc", b"proc", b"rw"),
                (b"none", b"/dev", b"devtmpfs", b"rw"),
            ];
        });
        let (_, s) = gen_text(PF_MOUNTS, 0, 512);
        assert_eq!(
            s,
            "rootfs / ext4 rw 0 0\nnone /proc proc rw 0 0\nnone /dev devtmpfs rw 0 0\n"
        );
    }

    #[test]
    fn content_mounts_stops_before_overflowing_line() {
        cw_reset();
        CW.with(|w| {
            w.borrow_mut().mounts = vec![
                (b"d1", b"/a", b"ext4", b"rw"),
                (b"d2", b"/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", b"ext4", b"rw"),
            ];
        });
        /* cap small: the first line (19 bytes + 6) fits, the second cannot. */
        let (rc, s) = gen_text(PF_MOUNTS, 0, 32);
        assert_eq!(s, "d1 /a ext4 rw 0 0\n");
        assert_eq!(rc, 18);
    }

    #[test]
    fn content_stat_cpu_lines_and_btime() {
        cw_reset();
        CW.with(|w| {
            let mut b = w.borrow_mut();
            b.cpu_times = vec![(10, 20, 30), (1, 2, 3)];
            b.realtime = 5000;
            b.monotonic = 2000;
            b.tasks_created = 77;
            b.runq_load_sum = 3;
        });
        let (_, s) = gen_text(PF_STAT, 0, 4096);
        assert!(s.starts_with("cpu  11 0 22 33 0 0 0 0 0 0\n"), "{}", s);
        assert!(s.contains("cpu0 10 0 20 30 0 0 0 0 0 0\n"), "{}", s);
        assert!(s.contains("cpu1 1 0 2 3 0 0 0 0 0 0\n"), "{}", s);
        assert!(s.contains("btime 3000\n"), "{}", s);
        assert!(s.contains("processes 77\n"), "{}", s);
        assert!(s.contains("procs_running 3\n"), "{}", s);
        assert!(s.ends_with("procs_blocked 0\n"), "{}", s);
    }

    #[test]
    fn content_swaps_lists_entries() {
        cw_reset();
        CW.with(|w| {
            w.borrow_mut().swaps = vec![(b"/dev/vda2", 1024, 256)];
        });
        let (_, s) = gen_text(PF_SWAPS, 0, 512);
        assert_eq!(
            s,
            "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n/dev/vda2\tpartition\t4096\t1024\t-2\n"
        );
    }

    #[test]
    fn content_swaps_header_only_when_none() {
        cw_reset();
        let (_, s) = gen_text(PF_SWAPS, 0, 512);
        assert_eq!(s, "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n");
    }

    #[test]
    fn content_cpuinfo_lists_online_cpus() {
        cw_reset();
        CW.with(|w| {
            let mut b = w.borrow_mut();
            b.nr_cpus = 4;
            b.online_mask = 0b0101; /* cpu0 and cpu2 online */
        });
        let (_, s) = gen_text(PF_CPUINFO, 0, 1024);
        assert!(s.contains("processor\t: 0\nisa:\trv64imafdc\nmmu:\tsv39\n"), "{}", s);
        assert!(s.contains("processor\t: 2\n"), "{}", s);
        assert!(!s.contains("processor\t: 1\n"), "{}", s);
        assert!(!s.contains("processor\t: 3\n"), "{}", s);
    }

    #[test]
    fn content_a20_cache_stats_labels() {
        cw_reset();
        CW.with(|w| {
            let mut b = w.borrow_mut();
            b.mem_bcache_pool = 8192;
            b.mem_bcache_valid = 5;
            b.mem_bcache_dirty_blocks = 2;
            b.mem_bcache_dirty_pages = 3;
            b.mem_pcache_valid = 7;
            b.mem_pcache_dirty = 1;
        });
        let (_, bc) = gen_text(PF_A20_BCACHE, 0, 512);
        assert_eq!(
            bc,
            "caches: 2\nblock_pool_bytes: 8192\npage_pool_bytes: 4096\nvalid_blocks: 5\n\
             dirty_blocks: 2\nvalid_pages: 3\ndirty_pages: 1\n"
        );
        let (_, pc) = gen_text(PF_A20_PAGE_CACHE, 0, 512);
        assert_eq!(
            pc,
            "capacity: 64\nallocated: 7\nbytes: 28672\nvalid: 7\ndirty: 1\npinned: 1\n"
        );
    }

    #[test]
    fn content_a20_oom_and_objects() {
        cw_reset();
        CW.with(|w| {
            let mut b = w.borrow_mut();
            b.oom_kills = 4;
            b.obj_vfiles = 42;
        });
        let (_, o) = gen_text(PF_A20_OOM, 0, 512);
        assert!(o.starts_with("kills: 4\nlast_kill_tick: 0\nlast_victim_pid: -1\n"), "{}", o);
        assert!(o.ends_with("kswapd_last_pass_tick: 0\n"), "{}", o);
        let (_, ob) = gen_text(PF_A20_OBJECTS, 0, 512);
        assert_eq!(
            ob,
            "handles: 1\nchannel_eps: 2\neventqs: 3\nvmos: 4\nvmo_pages: 5\n\
             irq_bindings: 6\nvfiles: 42\nvmo_dirty_frames: 8\n"
        );
    }

    #[test]
    fn content_a20_iommu_mixed_types_and_hex() {
        cw_reset();
        let (_, s) = gen_text(PF_A20_IOMMU, 0, 1024);
        assert!(s.starts_with("enabled: 1\ndomains_claimed: 1\ndomains_released: 0\n"), "{}", s);
        assert!(s.contains("last_fault_devid: 4660\n"), "{}", s);
        assert!(s.contains("last_fault_iova: 0xdeadbeef\n"), "{}", s);
        assert!(s.ends_with("last_fault_owner: -1\n"), "{}", s);
    }

    #[test]
    fn content_fixed_device_and_partition_tables() {
        cw_reset();
        let (_, d) = gen_text(PF_DEVICES, 0, 512);
        assert!(d.starts_with("Character devices:\n  1 mem\n"), "{}", d);
        assert!(d.contains("Block devices:\n  7 loop\n"), "{}", d);
        assert_eq!(gen_text(PF_PARTITIONS, 0, 64).1, "major minor  #blocks  name\n\n");
        assert_eq!(gen_text(PF_DISKSTATS, 0, 64).1, "");
        assert_eq!(gen_text(PF_A20_DRIVER_LIFECYCLE, 0, 64).1, "");
    }

    #[test]
    fn content_sysvipc_line() {
        cw_reset();
        CW.with(|w| {
            let mut b = w.borrow_mut();
            b.sysv_msg = 3;
            b.sysv_sem = 5;
            b.sysv_shm = 7;
        });
        assert_eq!(gen_text(PF_SYSVIPC, 0, 64).1, "msg\t3 sem\t5 shm\t7\n");
    }

    #[test]
    fn content_sys_utsname_and_overflow() {
        cw_reset();
        assert_eq!(gen_text(PF_SYS_KERNEL_HOSTNAME, 0, 64).1, "a20os\n");
        assert_eq!(gen_text(PF_SYS_KERNEL_DOMAINNAME, 0, 64).1, "(none)\n");
        assert_eq!(gen_text(PF_SYS_KERNEL_OVERFLOW_UID, 0, 32).1, "65534\n");
        assert_eq!(gen_text(PF_SYS_KERNEL_OVERFLOW_GID, 0, 32).1, "65534\n");
    }

    #[test]
    fn content_tty_ldiscs_and_pidmap() {
        cw_reset();
        let (_, t) = gen_text(PF_TTY, 0, 256);
        assert!(t.starts_with("driver               refcnt\n"), "{}", t);
        assert_eq!(gen_text(PF_LDISCS, 0, 64).1, "n_tty        0\nn_gsm        5\n");
        assert_eq!(gen_text(PF_SYS_KERNEL_PIDMAP, 0, 64).1, "00000000 : 00004000\n");
    }

    #[test]
    fn content_pressure_uses_psi_render() {
        cw_reset();
        let (_, cpu) = gen_text(PF_PRESSURE_CPU, 0, 128);
        assert!(cpu.starts_with("some avg10=1.00"), "{}", cpu);
        let (_, mem) = gen_text(PF_PRESSURE_MEM, 0, 128);
        assert!(mem.starts_with("some avg10=0.00"), "{}", mem);
        /* too-small buffer returns 0 without rendering */
        let (rc, _) = gen_text(PF_PRESSURE, 0, 32);
        assert_eq!(rc, 0);
    }

    #[test]
    fn content_pid_stat_and_statm() {
        cw_reset();
        let (_, st) = gen_text(PF_PID_STAT, 42, 512);
        assert!(st.starts_with("42 (init) R 1 42 42 "), "{}", st);
        // Check Linux ABI field positions, rather than a substring that can
        // preserve a shifted layout. Field 21 is obsolete but still present.
        let (_, tail) = st.rsplit_once(") ").expect("stat comm terminator");
        let fields: Vec<&str> = tail.split_whitespace().collect();
        let field = |number: usize| fields[number - 3];
        assert_eq!(fields.len() + 2, 39, "{}", st);
        assert_eq!(field(14), "100"); // utime
        assert_eq!(field(15), "50");  // stime
        assert_eq!(field(18), "20");  // priority
        assert_eq!(field(20), "1");   // num_threads
        assert_eq!(field(21), "0");   // itrealvalue
        assert_eq!(field(22), "7");   // starttime
        assert_eq!(field(23), "8192"); // vsize bytes
        assert_eq!(field(24), "3");   // rss pages, never the -1 rsslim
        assert_eq!(field(25), "-1");  // rsslim
        assert_eq!(field(39), "13");  // processor
        let (_, stm) = gen_text(PF_PID_STATM, 42, 64);
        assert_eq!(stm, "2 3 0 0 0 0 0\n"); /* vsize/PAGE_SIZE = 8192/4096 */

        CW.with(|w| w.borrow_mut().pid_present = false);
        let (_, unk) = gen_text(PF_PID_STAT, 42, 64);
        assert_eq!(unk, "42 (unknown) S 0 0\n");
        let (_, z) = gen_text(PF_PID_STATM, 42, 64);
        assert_eq!(z, "0 0 0 0 0 0 0\n");
    }

    #[test]
    fn content_pid_status_fields_and_groups() {
        cw_reset();
        let (_, s) = gen_text(PF_PID_STATUS, 42, 2048);
        assert!(s.starts_with("Name:\tinit\nPid:\t42\nPPid:\t1\n"), "{}", s);
        assert!(s.contains("State:\tR (running)\n"), "{}", s);
        assert!(s.contains("Groups:\t0 4\n"), "{}", s);
        assert!(s.contains("CapEff:\t0000003fffffffff\n"), "{}", s);
        assert!(s.contains("VmRSS:\t12 kB\n"), "{}", s);
        assert!(s.ends_with("VmLck:\t0 kB\nVmData:\t8 kB\n"), "{}", s);

        CW.with(|w| w.borrow_mut().pid_present = false);
        let (_, u) = gen_text(PF_PID_STATUS, 7, 128);
        assert_eq!(u, "Name:\tunknown\nPid:\t7\n");
    }

    #[test]
    fn content_pid_string_and_io_nodes() {
        cw_reset();
        assert_eq!(gen_text(PF_PID_COMM, 42, 64).1, "init\n");
        assert_eq!(gen_text(PF_PID_EXE, 42, 64).1, "/sbin/init\n");
        assert_eq!(gen_text(PF_PID_CWD, 42, 64).1, "/\n");
        assert_eq!(gen_text(PF_PID_LOGINUID, 42, 32).1, "4294967295\n");
        assert_eq!(gen_text(PF_PID_SESSIONID, 42, 32).1, "42\n");
        let (_, io) = gen_text(PF_PID_IO, 42, 256);
        assert_eq!(
            io,
            "rchar: 11\nwchar: 22\nsyscr: 3\nsyscw: 4\nread_bytes: 55\nwrite_bytes: 66\ncancelled_write_bytes: 0\n"
        );
        let (rc, s) = gen_text(PF_PID_ENVIRON, 42, 64);
        assert_eq!((rc, s.as_str()), (0, ""));
    }

    #[test]
    fn content_pid_ns_inodes() {
        cw_reset();
        assert_eq!(gen_text(PF_PID_NS_PID, 42, 64).1, "pid:[16416]\n");
        assert_eq!(gen_text(PF_PID_NS_UTS, 42, 64).1, "uts:[16432]\n");
        assert_eq!(gen_text(PF_PID_NS_USER, 42, 64).1, "user:[16448]\n");
        assert_eq!(gen_text(PF_PID_NS_MNT, 42, 64).1, "mnt:[16480]\n");
        assert_eq!(gen_text(PF_PID_NS_CGROUP, 42, 64).1, "cgroup:[16512]\n");
        /* absent task -> the initial namespace inode */
        CW.with(|w| w.borrow_mut().pid_present = false);
        assert_eq!(gen_text(PF_PID_NS_PID, 42, 64).1, "pid:[16384]\n");
    }

    #[test]
    fn content_pid_mountinfo_lines() {
        cw_reset();
        CW.with(|w| {
            w.borrow_mut().mounts = vec![(b"rootfs", b"/", b"ext4", b"rw")];
        });
        let (_, s) = gen_text(PF_PID_MOUNTINFO, 42, 512);
        assert_eq!(s, "100 1 0:100 / / rw - ext4 rootfs rw\n");
    }

    #[test]
    fn content_helper_backed_nodes() {
        cw_reset();
        assert_eq!(gen_text(PF_A20_HYP_SELFTEST, 0, 64).1, "hyp_selftest=PASS\n");
        assert_eq!(gen_text(PF_SYS_KERNEL_CORE_PATTERN, 0, 64).1, "H\n");
        let (rc, truncated) = gen_text(PF_SYS_KERNEL_CORE_PATTERN, 0, 2);
        assert_eq!((rc, truncated.as_str()), (1, "H"));
        assert_eq!(gen_text(PF_MODULES, 0, 64).1, "module A 0\n");
        assert_eq!(gen_text(PF_NET_CONFIG, 0, 64).1, "H\n");
        assert_eq!(gen_text(PF_ROUTE, 0, 64).1, "H\n");
    }

    #[test]
    fn content_pid_oom_cgroup_and_cmdline() {
        cw_reset();
        assert_eq!(gen_text(PF_PID_OOM_SCORE_ADJ, 42, 32).1, "5\n");
        assert_eq!(gen_text(PF_PID_OOM_SCORE, 42, 32).1, "5\n");
        assert_eq!(gen_text(PF_PID_CGROUP, 42, 64).1, "0::/a20/init\n");
        /* cmdline = exec path bytes + NUL, length includes the NUL */
        let (rc, s) = gen_text(PF_PID_CMDLINE, 42, 64);
        assert_eq!(s, "/sbin/init");
        assert_eq!(rc as usize, "/sbin/init".len() + 1);
        /* absent task: negative adj clamps to 0 for the score */
        CW.with(|w| {
            let mut b = w.borrow_mut();
            b.pid_present = false;
            b.oom_adj = -3;
        });
        assert_eq!(gen_text(PF_PID_OOM_SCORE_ADJ, 42, 32).1, "0\n");
        assert_eq!(gen_text(PF_PID_OOM_SCORE, 42, 32).1, "0\n");
        let (rc, s) = gen_text(PF_PID_CMDLINE, 42, 64);
        assert_eq!((rc, s.as_str()), (1, ""));
    }

    #[test]
    fn content_uid_gid_map_and_setgroups() {
        cw_reset();
        let (rc, m) = gen_text(PF_UID_MAP, 42, 256);
        assert_eq!(
            m,
            "         0      65534          1\n         1     100000      65535\n"
        );
        assert_eq!(rc as usize, 66);
        let (rc, g) = gen_text(PF_GID_MAP, 42, 256);
        assert_eq!(rc as usize, 66);
        assert_eq!(g.len(), 66);
        assert_eq!(gen_text(PF_SETGROUPS, 42, 32).1, "allow\n");
        CW.with(|w| w.borrow_mut().setgroups_allowed = 0);
        assert_eq!(gen_text(PF_SETGROUPS, 42, 32).1, "deny\n");
        /* absent task -> the setgroups write is refused */
        CW.with(|w| w.borrow_mut().pid_present = false);
        assert_eq!(gen_text(PF_SETGROUPS, 42, 32).0, 0);
    }

    #[test]
    fn content_pid_limits_wchan_stack() {
        cw_reset();
        let (_, l) = gen_text(PF_PID_LIMITS, 42, 256);
        assert!(l.starts_with("Max open files              1024                1024 files\n"), "{}", l);
        assert!(l.contains("Max stack size                unlimited            unlimited bytes\n"), "{}", l);
        assert!(l.ends_with("Max locked memory       65536              65536 bytes\n"), "{}", l);
        assert_eq!(gen_text(PF_PID_WCHAN, 42, 32).1, "do_wait\n");
        assert_eq!(gen_text(PF_PID_STACK, 42, 64).1, "[<0000000000000000>] 0\n");
        CW.with(|w| w.borrow_mut().pid_present = false);
        assert_eq!(gen_text(PF_PID_LIMITS, 42, 64).0, -ESRCH);
        assert_eq!(gen_text(PF_PID_WCHAN, 42, 64).0, -ESRCH);
        assert_eq!(gen_text(PF_PID_STACK, 42, 64).0, -ESRCH);
    }

    #[test]
    fn content_pid_auxv_binary_pairs() {
        cw_reset();
        let g = gtable();
        let mut buf = [0xAAu8; 64];
        let rc = unsafe {
            a20_procfs_generate(&g, PF_PID_AUXV, 42, buf.as_mut_ptr() as *mut c_char, 64)
        };
        assert_eq!(rc, 32);
        let v0 = u64::from_ne_bytes(buf[0..8].try_into().unwrap());
        let v1 = u64::from_ne_bytes(buf[8..16].try_into().unwrap());
        assert_eq!((v0, v1), (7, 0x1000));
        /* content is not NUL-terminated: the byte past the vector is untouched */
        assert_eq!(buf[32], 0xAA);
    }

    #[test]
    fn content_fixed_a20_and_net_tables() {
        cw_reset();
        assert!(gen_text(PF_ARP, 0, 128).1.starts_with("IP address       HW type"));
        assert!(gen_text(PF_IOMEM, 0, 256).1.contains("System RAM\n"));
        assert_eq!(gen_text(PF_IOPORTS, 0, 64).1, "0000-0cf7 : PCI Bus 0000:00\n");
        assert_eq!(gen_text(PF_MISC, 0, 64).1, " 10 cpu_dma_latency\n 59 random\n");
        assert!(gen_text(PF_SOFTIRQS, 0, 256).1.contains("TASKLET: 0\n"));
        /* pid maps are empty in generate_content */
        assert_eq!(gen_text(PF_PID_MAPS, 42, 64).1, "");
        assert_eq!(gen_text(PF_PID_SMAPS, 42, 64).1, "");
    }

    #[test]
    fn content_irq_affinity_header_and_entries() {
        cw_reset();
        let (_, s) = gen_text(PF_A20_IRQ_AFFINITY, 0, 256);
        assert_eq!(s, "cpus: 1\nentries: 1\neth\t0\t16\t1\n");
    }

    #[test]
    fn content_net_rows_use_snapshot_renderer() {
        cw_reset();
        assert!(gen_text(PF_NET_TCP, 0, 64).1.starts_with("  sl  local_address rem_address"));
        assert_eq!(gen_text(PF_NET_UNIX, 0, 64).1,
            "Num       RefCount Protocol Flags    Type St Inode Path\n");
    }

    #[test]
    fn content_config_gz_blob_copy() {
        cw_reset();
        let g = gtable();
        let mut buf = [0xAAu8; 32];
        let rc = unsafe {
            a20_procfs_generate(&g, PF_CONFIG_GZ, 0, buf.as_mut_ptr() as *mut c_char, 32)
        };
        assert_eq!(rc as usize, GZBLOB.len());
        assert_eq!(&buf[..GZBLOB.len()], GZBLOB);
        /* no NUL terminator is appended by the raw copy */
        assert_eq!(buf[GZBLOB.len()], 0xAA);
    }
}
