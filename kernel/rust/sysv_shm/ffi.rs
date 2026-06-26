#![allow(non_camel_case_types)]

use core::ffi::c_void;

pub use a20rust_support::lock::spinlock_t;

pub const IPC_CREAT: i32 = 0o1000;
pub const IPC_EXCL: i32 = 0o2000;
pub const IPC_64_BIT: i32 = 0x100;
pub const IPC_RMID: i32 = 0;
pub const IPC_STAT: i32 = 2;
pub const SHM_STAT_ANY: i32 = 15;
pub const SHM_INFO: i32 = 14;

pub const SYSV_SHM_MAX: usize = 32;
pub const SHM_MAX_PAGES: usize = 1024;

pub const EINVAL: i32 = 22;
pub const EEXIST: i32 = 17;
pub const ENOENT: i32 = 2;
pub const ENOSPC: i32 = 28;
pub const ENOMEM: i32 = 12;
pub const EFAULT: i32 = 14;

pub const PFN_NONE: u32 = u32::MAX;

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct mm_struct_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct sysv_shm_t {
    pub used: i32,
    pub marked_delete: i32,
    pub key: i32,
    pub size: usize,
    pub pages: *mut u32,
    pub npages: usize,
    pub nattach: i32,
}

#[repr(C)]
pub struct shm_info_ds {
    pub perm_k: i32,
    pub perm_u: u32,
    pub perm_g: u32,
    pub perm_cu: u32,
    pub perm_cg: u32,
    pub perm_m: u32,
    pub perm_s: i32,
    pub perm_p1: u64,
    pub perm_p2: u64,
    pub segsz: usize,
    pub at: u64,
    pub dt: u64,
    pub ct: u64,
    pub cpid: i32,
    pub lpid: i32,
    pub nattch: u64,
    pub pad1: u64,
    pub pad2: u64,
}

unsafe extern "C" {
    pub fn sysv_shm_kcalloc(nmemb: usize, size: usize) -> *mut c_void;
    pub fn sysv_shm_kfree(ptr: *mut c_void);
    pub fn sysv_shm_zero_pfn(pfn: u32);

    pub fn sysv_shm_pfa_alloc_page() -> u32;
    pub fn sysv_shm_pfa_free_page(pfn: u32);
    pub fn sysv_shm_frame_put(pfn: u32);

    pub fn sysv_shm_copy_to_user(dst: *mut c_void, src: *const c_void, n: usize) -> isize;

    pub fn sysv_shm_proc_current() -> *mut task_t;
    pub fn sysv_shm_task_mm(task: *mut task_t) -> *mut mm_struct_t;
    pub fn sysv_shm_task_pid(task: *mut task_t) -> i32;

    pub fn sysv_shm_page_size() -> usize;

    pub fn sysv_shm_do_attach(
        mm: *mut mm_struct_t,
        shmaddr: u64,
        pages: *const u32,
        npages: usize,
        size: usize,
        shmid: i32,
    ) -> u64;

    pub fn sysv_shm_mm_find_vma(mm: *mut mm_struct_t, addr: u64) -> *mut c_void;
    pub fn sysv_shm_vma_matches(vma: *mut c_void, addr: u64, shmid_out: *mut i32) -> i32;
    pub fn sysv_shm_vma_len(vma: *mut c_void) -> usize;
    pub fn sysv_shm_mm_munmap(mm: *mut mm_struct_t, addr: u64, len: usize) -> i32;
}

#[cfg(not(any(
    target_arch = "riscv64",
    target_arch = "loongarch64",
    target_arch = "aarch64",
    target_arch = "x86_64"
)))]
compile_error!("sysv_shm shmid_ds layout assumes 64-bit size_t/long");
