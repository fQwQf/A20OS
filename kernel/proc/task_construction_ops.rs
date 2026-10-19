// Shared C/Rust ABI for the task-construction operation table.
//
// Included by kernel/proc/task_construction.rs (the runner) and
// kernel/proc/clone_txn.rs (the caller that builds the table), so the field
// order of the table can never diverge between the two Rust objects.

pub type StageFn = unsafe extern "C" fn(*mut core::ffi::c_void) -> i32;
pub type WaitFn = unsafe extern "C" fn(*mut core::ffi::c_void);
pub type AbortFn = unsafe extern "C" fn(*mut core::ffi::c_void, u8);

#[repr(C)]
pub struct TaskConstructionOps {
    pub context: *mut core::ffi::c_void,
    pub validate: StageFn,
    pub allocate: StageFn,
    pub base: StageFn,
    pub seccomp: StageFn,
    pub fs_cgroup: StageFn,
    pub files: StageFn,
    pub signals: StageFn,
    pub namespaces: StageFn,
    pub capabilities: StageFn,
    pub mm: StageFn,
    pub arch: StageFn,
    pub publish: StageFn,
    pub post_publish: StageFn,
    pub ready: StageFn,
    pub vfork_wait: WaitFn,
    pub abort: AbortFn,
    pub deferred: u8,
    pub vfork: u8,
}
