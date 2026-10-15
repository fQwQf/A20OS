#![no_std]

//! Rust-owned page-at-a-time usercopy loop.
//!
//! The C boundary pins one address-space reference for this call. On MMU
//! profiles, a successful page callback returns with that exact mm locked, or
//! returns an unlocked retry status so fault resolution can sleep outside the
//! lock. NOMMU callbacks instead return an identity-mapped page span without a
//! page-table lock or resolver. `PageAccess` pairs every successful callback
//! with its release operation. Kernel panic is aborting; Drop is not a
//! panic-safety mechanism.

use core::ffi::c_void;
use core::marker::PhantomData;
use core::ptr;

mod usercopy_strings;

const EFAULT: isize = -14;
const NEED_FAULT: i32 = 1;

unsafe extern "C" {
    fn a20_usercopy_page_access(
        task: *mut c_void,
        mm: *mut c_void,
        address: usize,
        write: i32,
        kernel_address: *mut *mut u8,
        chunk: *mut usize,
        irq_flags: *mut u64,
        preempt_before: *mut u32,
        shared_write_guard: *mut u32,
    ) -> i32;
    fn a20_usercopy_resolve_fault(
        task: *mut c_void,
        mm: *mut c_void,
        address: usize,
        write: i32,
        irq_before: u64,
        preempt_before: u32,
    ) -> i32;
    fn a20_usercopy_page_unlock(
        mm: *mut c_void,
        irq_flags: u64,
        preempt_before: u32,
        shared_write_guard: u32,
    );
}

struct CopyContext {
    task: *mut c_void,
    mm: *mut c_void,
}

/// Owns the release obligation for one successful page callback. Its lifetime
/// cannot outlive the context that holds the pinned mm reference.
struct PageAccess<'a> {
    mm: *mut c_void,
    irq_flags: u64,
    kernel_address: *mut u8,
    chunk: usize,
    preempt_before: u32,
    shared_write_guard: u32,
    _context: PhantomData<&'a CopyContext>,
}

impl Drop for PageAccess<'_> {
    fn drop(&mut self) {
        // SAFETY: successful page_access transfers its release obligation to
        // this guard (MM unlock on MMU, no-op release on NOMMU); the context
        // borrow keeps its C-pinned mm reference alive.
        unsafe {
            a20_usercopy_page_unlock(
                self.mm,
                self.irq_flags,
                self.preempt_before,
                self.shared_write_guard,
            )
        }
    }
}

struct AccessError {
    status: i32,
    irq_before: u64,
    preempt_before: u32,
}

impl CopyContext {
    fn access(&self, address: usize, write: bool) -> Result<PageAccess<'_>, AccessError> {
        let mut kernel_address = ptr::null_mut();
        let mut chunk = 0usize;
        let mut irq_flags = 0u64;
        let mut preempt_before = 0u32;
        let mut shared_write_guard = 0u32;
        // SAFETY: task/mm are pinned/borrowed by the C wrapper for this entire
        // call. Output pointers are local and pairwise disjoint. Status 0
        // transfers a release obligation to PageAccess.
        let status = unsafe {
            a20_usercopy_page_access(
                self.task,
                self.mm,
                address,
                i32::from(write),
                &mut kernel_address,
                &mut chunk,
                &mut irq_flags,
                &mut preempt_before,
                &mut shared_write_guard,
            )
        };
        if status != 0 {
            return Err(AccessError { status, irq_before: irq_flags, preempt_before });
        }
        if kernel_address.is_null() || chunk == 0 {
            // A successful C result must provide a nonempty page span. Still
            // discharge its release obligation if the ABI contract is broken.
            unsafe {
                a20_usercopy_page_unlock(
                    self.mm,
                    irq_flags,
                    preempt_before,
                    shared_write_guard,
                )
            };
            return Err(AccessError { status: -1, irq_before: irq_flags, preempt_before });
        }
        Ok(PageAccess {
            mm: self.mm,
            irq_flags,
            kernel_address,
            chunk,
            preempt_before,
            shared_write_guard,
            _context: PhantomData,
        })
    }

    fn resolve(&self, address: usize, write: bool, error: &AccessError) -> i32 {
        // Called only after access() returned without a PageAccess guard, so
        // the MM spinlock and disabled-interrupt section have ended.
        unsafe {
            a20_usercopy_resolve_fault(
                self.task,
                self.mm,
                address,
                i32::from(write),
                error.irq_before,
                error.preempt_before,
            )
        }
    }
}

unsafe fn copy_loop(
    context: &CopyContext,
    kernel: *mut u8,
    user: usize,
    length: usize,
    to_user: bool,
) -> isize {
    let mut copied = 0usize;
    while copied < length {
        let address = user.wrapping_add(copied);
        let access = match context.access(address, to_user) {
            Ok(access) => access,
            Err(error) if error.status == NEED_FAULT => {
                if context.resolve(address, to_user, &error) < 0 {
                    return EFAULT;
                }
                continue;
            }
            Err(_) => return EFAULT,
        };
        let count = access.chunk.min(length - copied);
        // SAFETY: C establishes that the user side is a readable/writable
        // direct-map alias for count bytes; the adapter keeps the span valid
        // until access drops (NOMMU identity mappings need no lock). The
        // kernel buffer is the caller's valid `length`-byte buffer. As with
        // the C memcpy contract these
        // ranges must not overlap.
        unsafe {
            if to_user {
                ptr::copy_nonoverlapping(kernel.add(copied), access.kernel_address, count);
            } else {
                ptr::copy_nonoverlapping(access.kernel_address, kernel.add(copied), count);
            }
        }
        drop(access);
        copied += count;
    }
    copied as isize
}

#[no_mangle]
/// # Safety
/// The C caller holds a reference to `mm`, guarantees `task` is the current
/// task bound to that address space for this syscall, has validated the user
/// range, and supplies a kernel buffer of `length` bytes. Buffers do not
/// overlap, matching the existing C `memcpy` precondition.
pub unsafe extern "C" fn a20_usercopy_copy_from_rust(
    task: *mut c_void,
    mm: *mut c_void,
    dst: *mut c_void,
    src: *const c_void,
    length: usize,
) -> isize {
    if task.is_null() || mm.is_null() {
        return EFAULT;
    }
    let context = CopyContext { task, mm };
    // SAFETY: caller contract above guarantees the kernel output buffer and
    // validated user source span; copy_loop rechecks each page under mm lock.
    unsafe { copy_loop(&context, dst.cast(), src as usize, length, false) }
}

#[no_mangle]
/// # Safety
/// Same lifetime, range, and non-overlap requirements as
/// `a20_usercopy_copy_from_rust`; `dst` is the validated user span.
pub unsafe extern "C" fn a20_usercopy_copy_to_rust(
    task: *mut c_void,
    mm: *mut c_void,
    dst: *mut c_void,
    src: *const c_void,
    length: usize,
) -> isize {
    if task.is_null() || mm.is_null() {
        return EFAULT;
    }
    let context = CopyContext { task, mm };
    // SAFETY: caller contract above guarantees the kernel input buffer and
    // validated user destination span; copy_loop rechecks each page under lock.
    unsafe { copy_loop(&context, src as *mut u8, dst as usize, length, true) }
}
