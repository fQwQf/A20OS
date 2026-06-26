#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_uint, c_void};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct refcount_t {
    pub value: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct spinlock_t {
    pub locked: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct cg_mem_state_t {
    pub lock: spinlock_t,
    pub limit: usize,
    pub swap_limit: usize,
    pub rss: usize,
    pub cache: usize,
    pub swap_usage: usize,
    pub total_vm: usize,
    pub failcnt: u64,
    pub oom_kill_disable: c_int,
    pub oom_kill_count: c_int,
    pub hierarchy: c_int,
    pub swappiness: c_uint,
    pub min_val: usize,
    pub low_val: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct cg_cpu_state_t {
    pub lock: spinlock_t,
    pub quota: u64,
    pub period: u64,
    pub runtime: u64,
    pub period_start: u64,
    pub throttled: c_int,
    pub nr_throttled: u32,
    pub throttled_time: u64,
    pub total_runtime: u64,
    pub shares: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct cg_cpuset_state_t {
    pub lock: spinlock_t,
    pub cpus_allowed: u32,
    pub mems_allowed: u32,
    pub effective_cpus: u32,
    pub memory_migrate: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct cg_resource_t {
    pub mem: cg_mem_state_t,
    pub cpu: cg_cpu_state_t,
    pub cpuset: cg_cpuset_state_t,
}

pub type cg_ver_t = c_int;
pub const CG_V1: cg_ver_t = 0;
pub const CG_V2: cg_ver_t = 1;

pub type cg_file_t = c_int;
pub const CF_TASKS: cg_file_t = 0;
pub const CF_CGROUP_PROCS: cg_file_t = 1;
pub const CF_NOTIFY_ON_RELEASE: cg_file_t = 2;
pub const CF_RELEASE_AGENT: cg_file_t = 3;
pub const CF_CLONE_CHILDREN: cg_file_t = 4;
pub const CF_EVENT_CONTROL: cg_file_t = 5;
pub const CF_CGROUP_CONTROLLERS: cg_file_t = 6;
pub const CF_CGROUP_SUBTREE_CONTROL: cg_file_t = 7;
pub const CF_CGROUP_KILL: cg_file_t = 8;
pub const CF_CGROUP_TYPE: cg_file_t = 9;
pub const CF_MEMORY_USAGE: cg_file_t = 10;
pub const CF_MEMORY_LIMIT: cg_file_t = 11;
pub const CF_MEMORY_MAX_USAGE: cg_file_t = 12;
pub const CF_MEMORY_STAT: cg_file_t = 13;
pub const CF_MEMORY_SWAPPINESS: cg_file_t = 14;
pub const CF_MEMORY_USE_HIERARCHY: cg_file_t = 15;
pub const CF_MEMORY_MEMSW_USAGE: cg_file_t = 16;
pub const CF_MEMORY_MEMSW_LIMIT: cg_file_t = 17;
pub const CF_MEMORY_KMEM_USAGE: cg_file_t = 18;
pub const CF_MEMORY_KMEM_LIMIT: cg_file_t = 19;
pub const CF_MEMORY_CURRENT: cg_file_t = 20;
pub const CF_MEMORY_MAX: cg_file_t = 21;
pub const CF_MEMORY_MIN: cg_file_t = 22;
pub const CF_MEMORY_LOW: cg_file_t = 23;
pub const CF_MEMORY_EVENTS: cg_file_t = 24;
pub const CF_MEMORY_SWAP_CURRENT: cg_file_t = 25;
pub const CF_MEMORY_SWAP_MAX: cg_file_t = 26;
pub const CF_CPUSET_CPUS: cg_file_t = 27;
pub const CF_CPUSET_MEMS: cg_file_t = 28;
pub const CF_CPUSET_MEMORY_MIGRATE: cg_file_t = 29;
pub const CF_CPU_CFS_QUOTA: cg_file_t = 30;
pub const CF_CPU_CFS_PERIOD: cg_file_t = 31;
pub const CF_CPU_SHARES: cg_file_t = 32;
pub const CF_CPU_STAT: cg_file_t = 33;
pub const CF_CPU_MAX: cg_file_t = 34;
pub const CF_FILE_MAX: cg_file_t = 35;

pub const CG_MAX_CHILDREN: usize = 128;
pub const CG_MAX_FILES: usize = 64;
pub const CG_MAX_PIDS: usize = 64;

pub const CTRL_MEMORY: u32 = 1 << 0;
pub const CTRL_CPU: u32 = 1 << 1;
pub const CTRL_CPUSET: u32 = 1 << 2;
pub const CTRL_CPUACCT: u32 = 1 << 3;

#[repr(C)]
pub struct cg_node_t {
    pub name: [c_char; 64],
    pub parent: *mut cg_node_t,
    pub children: [*mut cg_node_t; CG_MAX_CHILDREN],
    pub child_count: c_int,
    pub files: [cg_file_t; CG_MAX_FILES],
    pub file_count: c_int,
    pub is_root: c_int,
    pub clone_children: c_int,
    pub pids: [c_int; CG_MAX_PIDS],
    pub pid_count: c_int,
    pub uid: u32,
    pub gid: u32,
    pub lock: spinlock_t,
    pub res: cg_resource_t,
}

#[repr(C)]
pub struct cg_sb_t {
    pub ver: cg_ver_t,
    pub controllers: u32,
    pub root: *mut cg_node_t,
}

#[repr(C)]
pub struct kstat_t {
    pub st_dev: u64,
    pub st_ino: u64,
    pub st_mode: u32,
    pub st_nlink: u32,
    pub st_uid: u32,
    pub st_gid: u32,
    pub st_rdev: u64,
    pub st_size: u64,
    pub st_blksize: u64,
    pub st_blocks: u64,
    pub st_atime: u64,
    pub st_atime_nsec: u64,
    pub st_mtime: u64,
    pub st_mtime_nsec: u64,
    pub st_ctime: u64,
    pub st_ctime_nsec: u64,
}

#[repr(C)]
pub struct vnode_t {
    pub ino: u64,
    pub type_: c_int,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub size: usize,
    pub ref_count: refcount_t,
    pub parent: *mut vnode_t,
    pub mnt: *mut mount_t,
    pub fs_data: *mut c_void,
    pub ops: *mut vnode_ops_t,
}

#[repr(C)]
pub struct mount_t {
    pub type_: c_int,
    pub flags: c_int,
    pub path: [c_char; 512],
    pub dev: [c_char; 64],
    pub fstype: [c_char; 32],
    pub opts: [c_char; 256],
    pub root: *mut vnode_t,
    pub fs_data: *mut c_void,
}

pub type vnode_lookup_fn = extern "C" fn(*mut vnode_t, *const c_char, *mut *mut vnode_t) -> c_int;
pub type vnode_create_fn = extern "C" fn(*mut vnode_t, *const c_char, c_int, *mut *mut vnode_t) -> c_int;
pub type vnode_mkdir_fn = extern "C" fn(*mut vnode_t, *const c_char, c_int) -> c_int;
pub type vnode_unlink_fn = extern "C" fn(*mut vnode_t, *const c_char) -> c_int;
pub type vnode_rmdir_fn = extern "C" fn(*mut vnode_t, *const c_char) -> c_int;
pub type vnode_rename_fn = extern "C" fn(*mut vnode_t, *const c_char, *mut vnode_t, *const c_char, c_uint) -> c_int;
pub type vnode_link_fn = extern "C" fn(*mut vnode_t, *const c_char, *mut vnode_t) -> c_int;
pub type vnode_symlink_fn = extern "C" fn(*mut vnode_t, *const c_char, *const c_char) -> c_int;
pub type vnode_readlink_fn = extern "C" fn(*mut vnode_t, *mut c_char, usize) -> c_int;
pub type vnode_stat_fn = extern "C" fn(*mut vnode_t, *mut kstat_t) -> c_int;
pub type vnode_truncate_fn = extern "C" fn(*mut vnode_t, usize) -> c_int;
pub type vnode_writepage_fn = extern "C" fn(*mut vnode_t, u64, *const c_void, usize) -> c_int;
pub type vnode_chmod_fn = extern "C" fn(*mut vnode_t, c_int) -> c_int;
pub type vnode_chown_fn = extern "C" fn(*mut vnode_t, c_int, c_int) -> c_int;
pub type vnode_open_fn = extern "C" fn(*mut vnode_t, c_int) -> *mut vfile_t;
pub type vnode_release_fn = extern "C" fn(*mut vnode_t);

#[repr(C)]
pub struct vnode_ops_t {
    pub lookup: Option<vnode_lookup_fn>,
    pub create: Option<vnode_create_fn>,
    pub mkdir: Option<vnode_mkdir_fn>,
    pub unlink: Option<vnode_unlink_fn>,
    pub rmdir: Option<vnode_rmdir_fn>,
    pub rename: Option<vnode_rename_fn>,
    pub link: Option<vnode_link_fn>,
    pub symlink: Option<vnode_symlink_fn>,
    pub readlink: Option<vnode_readlink_fn>,
    pub stat: Option<vnode_stat_fn>,
    pub truncate: Option<vnode_truncate_fn>,
    pub writepage: Option<vnode_writepage_fn>,
    pub chmod: Option<vnode_chmod_fn>,
    pub chown: Option<vnode_chown_fn>,
    pub open: Option<vnode_open_fn>,
    pub release: Option<vnode_release_fn>,
}

pub type vfile_t = a20rust_support::vfs::vfile_t;

pub const VFS_FT_REGULAR: c_int = 1;
pub const VFS_FT_DIR: c_int = 2;

pub const S_IFREG: u32 = 0o100000;
pub const S_IFDIR: u32 = 0o040000;

pub const ENOENT: c_int = 2;
pub const ENOMEM: c_int = 12;
pub const EEXIST: c_int = 17;
pub const ENOSPC: c_int = 28;
pub const EROFS: c_int = 30;

unsafe extern "C" {
    pub fn kmalloc(size: usize) -> *mut c_void;
    pub fn kfree(ptr: *mut c_void);
    pub fn memset(dst: *mut c_void, value: c_int, size: usize) -> *mut c_void;
    pub fn vnode_ref_init(vn: *mut vnode_t, refs: c_int);
    pub fn vnode_get(vn: *mut vnode_t);
    pub fn vnode_put(vn: *mut vnode_t);
    pub fn cg_mem_init(mem: *mut cg_mem_state_t);
    pub fn cg_cpu_init(cpu: *mut cg_cpu_state_t);
    pub fn cg_cpuset_init(cs: *mut cg_cpuset_state_t, nr_cpus: c_uint);

    pub fn a20_cgroupfs_file_name(file: cg_file_t, ver: cg_ver_t) -> *const c_char;
    pub fn a20_cgroupfs_file_writable(file: cg_file_t) -> c_int;
    pub fn a20_cgroupfs_file_size(file: cg_file_t, sb: *mut cg_sb_t, node: *mut cg_node_t) -> c_int;
    pub fn a20_cgroupfs_open_vnode(vn: *mut vnode_t, flags: c_int) -> *mut vfile_t;
    pub fn a20_cgroupfs_spin_init(lock: *mut spinlock_t);
    pub fn a20_cgroupfs_spin_lock_irqsave(lock: *mut spinlock_t) -> u64;
    pub fn a20_cgroupfs_spin_unlock_irqrestore(lock: *mut spinlock_t, flags: u64);
}
