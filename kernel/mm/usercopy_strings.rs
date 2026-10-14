//! Bounded string helpers that share the parent module's MM-lock guard.
//!
//! These functions deliberately request only the next mapped page span. The
//! C page-access adapter validates each span against USER_VA_LIMIT and clamps
//! it at that limit; checked address arithmetic prevents a later iteration
//! from wrapping back to low user memory. A NUL at the last byte of a page
//! therefore completes without probing the following page.

use super::{CopyContext, EFAULT, NEED_FAULT};
use core::ffi::c_void;
use core::ptr;

const EINVAL: isize = -22;

unsafe fn scan_copy(context: &CopyContext, dst: *mut u8, src: usize, max: usize) -> isize {
    if max == 0 {
        return EINVAL;
    }

    let mut copied = 0usize;
    while copied < max - 1 {
        let address = match src.checked_add(copied) {
            Some(address) => address,
            None => return EFAULT,
        };
        let access = match context.access(address, false) {
            Ok(access) => access,
            Err(error) if error.status == NEED_FAULT => {
                if context.resolve(address, false, &error) < 0 {
                    return EFAULT;
                }
                continue;
            }
            Err(_) => return EFAULT,
        };

        let count = access.chunk.min(max - 1 - copied);
        for offset in 0..count {
            // checked_add above and the C adapter's range-clamped chunk make
            // every byte in this page span a valid, non-wrapping user VA.
            let byte = unsafe { ptr::read(access.kernel_address.add(offset)) };
            unsafe { ptr::write(dst.add(copied + offset), byte) };
            if byte == 0 {
                return (copied + offset) as isize;
            }
        }
        drop(access);
        copied += count;
    }

    // Match the C helper: truncation writes NUL and returns max - 1.
    unsafe { ptr::write(dst.add(copied), 0) };
    copied as isize
}

unsafe fn scan_len(context: &CopyContext, src: usize, max: usize) -> isize {
    let mut scanned = 0usize;
    while scanned < max {
        let address = match src.checked_add(scanned) {
            Some(address) => address,
            None => return EFAULT,
        };
        let access = match context.access(address, false) {
            Ok(access) => access,
            Err(error) if error.status == NEED_FAULT => {
                if context.resolve(address, false, &error) < 0 {
                    return EFAULT;
                }
                continue;
            }
            Err(_) => return EFAULT,
        };

        let count = access.chunk.min(max - scanned);
        for offset in 0..count {
            if unsafe { ptr::read(access.kernel_address.add(offset)) } == 0 {
                return (scanned + offset) as isize;
            }
        }
        drop(access);
        scanned += count;
    }
    max as isize
}

#[no_mangle]
/// # Safety
/// The C wrapper holds a reference to the current task's mm and supplies a
/// kernel destination with `max` writable bytes. Source mappings are accessed
/// through the shared `CopyContext` page guard and fault resolver.
pub(super) unsafe extern "C" fn a20_user_strncpy_rust(
    task: *mut c_void,
    mm: *mut c_void,
    dst: *mut c_void,
    src: *const c_void,
    max: usize,
) -> isize {
    if task.is_null() || mm.is_null() {
        return EFAULT;
    }
    let context = CopyContext { task, mm };
    unsafe { scan_copy(&context, dst.cast(), src as usize, max) }
}

#[no_mangle]
/// # Safety
/// The C wrapper holds a reference to the current task's mm. Source mappings
/// are accessed through the shared `CopyContext` guard only when `max != 0`.
pub(super) unsafe extern "C" fn a20_user_strnlen_rust(
    task: *mut c_void,
    mm: *mut c_void,
    src: *const c_void,
    max: usize,
) -> isize {
    if task.is_null() || mm.is_null() {
        return EFAULT;
    }
    if max == 0 {
        return 0;
    }
    let context = CopyContext { task, mm };
    unsafe { scan_len(&context, src as usize, max) }
}
