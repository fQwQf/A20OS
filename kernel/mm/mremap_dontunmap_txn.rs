#![no_std]

//! Rust-owned DONTUNMAP destination transaction.
//!
//! The C MM callbacks perform bounded VMA/page-table operations. This module
//! owns their order and the destination's rollback lifetime: source PTEs are
//! not retired until destination preparation, population, reprotection, and
//! source preflight have all succeeded.
//!
//! `rollback_destination(..., 1)` is called by Drop only after
//! `prepare_destination` has established an exact destination VMA. In that
//! state the C callback removes one whole VMA using TLB holds reserved by the
//! caller; it must not allocate or sleep. Before exact boundaries exist,
//! rollback is explicit and fallible, outside Drop.

use core::ffi::c_void;
use core::mem::{align_of, size_of};
use core::ptr;

#[repr(C)]
pub struct Ops {
    pub prepare_destination: Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    pub copy_backing: Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    pub move_pages: Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    pub reprotect: Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    pub preflight_source: Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    pub clear_source: Option<unsafe extern "C" fn(*mut c_void)>,
    pub release_source_lock: Option<unsafe extern "C" fn(*mut c_void)>,
    pub rollback_destination: Option<unsafe extern "C" fn(*mut c_void, i32) -> i32>,
    pub rollback_failed: Option<unsafe extern "C" fn(*mut c_void, i32, i32)>,
}

const _: () = {
    assert!(size_of::<Ops>() == 9 * size_of::<usize>());
    assert!(align_of::<Ops>() == align_of::<usize>());
};

type Step = unsafe extern "C" fn(*mut c_void) -> i32;
type Commit = unsafe extern "C" fn(*mut c_void);
type Rollback = unsafe extern "C" fn(*mut c_void, i32) -> i32;
type RollbackFailed = unsafe extern "C" fn(*mut c_void, i32, i32);

struct ValidOps {
    prepare_destination: Step,
    copy_backing: Step,
    move_pages: Step,
    reprotect: Step,
    preflight_source: Step,
    clear_source: Commit,
    release_source_lock: Commit,
    rollback_destination: Rollback,
    rollback_failed: RollbackFailed,
}

impl ValidOps {
    unsafe fn load(raw: *const Ops) -> Option<Self> {
        if raw.is_null() {
            return None;
        }
        let raw = unsafe { &*raw };
        Some(Self {
            prepare_destination: raw.prepare_destination?,
            copy_backing: raw.copy_backing?,
            move_pages: raw.move_pages?,
            reprotect: raw.reprotect?,
            preflight_source: raw.preflight_source?,
            clear_source: raw.clear_source?,
            release_source_lock: raw.release_source_lock?,
            rollback_destination: raw.rollback_destination?,
            rollback_failed: raw.rollback_failed?,
        })
    }
}

struct DestinationMapping<'ops, 'cleanup> {
    ops: &'ops ValidOps,
    ctx: *mut c_void,
    exact_vma: bool,
    armed: bool,
    cleanup_error: &'cleanup mut i32,
    primary_error: i32,
}

impl DestinationMapping<'_, '_> {
    fn mark_exact(&mut self) {
        self.exact_vma = true;
    }

    fn commit(&mut self) {
        self.armed = false;
    }
}

impl Drop for DestinationMapping<'_, '_> {
    fn drop(&mut self) {
        if !self.armed {
            return;
        }
        if !self.exact_vma {
            /* Preparation errors use the explicit fallible cleanup path.
             * Reaching Drop without exact boundaries means the caller broke
             * that contract; never attempt a potentially allocating partial
             * unmap from Drop while mm->lock is held. */
            unsafe { (self.ops.rollback_failed)(self.ctx, -1, -1) };
            return;
        }
        let cleanup_error = unsafe { (self.ops.rollback_destination)(self.ctx, 1) };
        if cleanup_error < 0 {
            /* Kernel panic=abort does not unwind. This handles ordinary early
             * returns: record both errors and let the outer API return the
             * cleanup errno after this destructor finishes. */
            unsafe { (self.ops.rollback_failed)(self.ctx, self.primary_error, cleanup_error) };
        }
        *self.cleanup_error = cleanup_error;
    }
}

unsafe fn run_transaction(
    ops: &ValidOps,
    ctx: *mut c_void,
    destination: usize,
    out_destination: *mut usize,
    cleanup_error: &mut i32,
) -> i32 {
    let mut mapping = DestinationMapping {
        ops,
        ctx,
        exact_vma: false,
        armed: true,
        cleanup_error,
        primary_error: 0,
    };

    let r = unsafe { (ops.prepare_destination)(ctx) };
    if r < 0 {
        /* Splitting may have failed while the target still shares a VMA with
         * neighbors. This rollback can allocate, so run it explicitly rather
         * than from Drop. */
        let cleanup = unsafe { (ops.rollback_destination)(ctx, 0) };
        mapping.armed = false;
        if cleanup < 0 {
            unsafe { (ops.rollback_failed)(ctx, r, cleanup) };
            *mapping.cleanup_error = cleanup;
        }
        return r;
    }
    mapping.mark_exact();

    /* Returning on an ordinary stage error drops `mapping`; Drop removes the
     * exact destination without allocating, while leaving the source intact. */
    let r = unsafe { (ops.copy_backing)(ctx) };
    if r < 0 {
        mapping.primary_error = r;
        return r;
    }
    let r = unsafe { (ops.move_pages)(ctx) };
    if r < 0 {
        mapping.primary_error = r;
        return r;
    }
    let r = unsafe { (ops.reprotect)(ctx) };
    if r < 0 {
        mapping.primary_error = r;
        return r;
    }
    let r = unsafe { (ops.preflight_source)(ctx) };
    if r < 0 {
        mapping.primary_error = r;
        return r;
    }

    /* Source retirement is the transaction's commit point. The adapter treats
     * this and lock-accounting release as infallible after preflight. */
    unsafe {
        (ops.clear_source)(ctx);
        (ops.release_source_lock)(ctx);
        ptr::write(out_destination, destination);
    }
    mapping.commit();
    0
}

/// Run the production DONTUNMAP destination transaction.
///
/// # Safety
/// `ops` and `ctx` must point to live kernel objects; every callback must obey
/// the MM locking and TLB reservation contract documented by its C adapter.
/// `out_destination` must be writable. In particular, rollback callback mode
/// 1 must be allocation-free and non-sleeping because Drop may call it while
/// `mm->lock` is held. The caller transfers ownership of an already allocated
/// destination to this function after supplying a complete ops table. The
/// output must not alias `ops`, `ctx`, or callback-owned state; callbacks must
/// not unwind across the C ABI. On rollback failure the cleanup errno is
/// returned and both primary and cleanup errors are logged by the adapter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mm_mremap_dontunmap_txn_rust(
    ops: *const Ops,
    ctx: *mut c_void,
    destination: usize,
    length: usize,
    out_destination: *mut usize,
) -> i32 {
    if ctx.is_null() || length == 0 || out_destination.is_null() {
        return -22; // -EINVAL
    }
    let ops_valid = match unsafe { ValidOps::load(ops) } {
        Some(ops) => ops,
        None => return -22,
    };
    let mut cleanup_error = 0;
    let primary = unsafe {
        run_transaction(
            &ops_valid,
            ctx,
            destination,
            out_destination,
            &mut cleanup_error,
        )
    };
    if cleanup_error < 0 {
        cleanup_error
    } else {
        primary
    }
}
