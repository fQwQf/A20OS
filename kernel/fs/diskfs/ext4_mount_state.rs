//! Shared ext4 superblock mount-state policy.
//!
//! `s_state` is an on-disk ext4 field, not a private journal format.  The C
//! adapter performs the superblock IO and checksum; this module owns the
//! admission and state-transition decisions for kernel ext4 and UFSD.
#![cfg_attr(not(test), no_std)]

const EXT4_VALID_FS: u16 = 0x0001;
const EXT4_ERROR_FS: u16 = 0x0002;
const EINVAL: i32 = -22;
const EIO: i32 = -5;

pub type ReadStateOp = unsafe extern "C" fn(*mut core::ffi::c_void, *mut u16) -> i32;
pub type WriteStateOp = unsafe extern "C" fn(*mut core::ffi::c_void, u16) -> i32;
pub type FlushStateOp = unsafe extern "C" fn(*mut core::ffi::c_void) -> i32;

#[repr(C)]
pub struct MountStateOps {
    pub context: *mut core::ffi::c_void,
    pub read_state: Option<ReadStateOp>,
    pub write_state: Option<WriteStateOp>,
    pub flush: Option<FlushStateOp>,
}

/// Fail closed if the last writable no-journal mount did not finish cleanly.
/// This driver has no no-journal replay mechanism, so it cannot safely make a
/// dirty image writable again.  Read-only admission is intentionally not
/// granted here because this implementation does not have a complete RO
/// enforcement path for every metadata helper.
#[no_mangle]
pub extern "C" fn a20_ext4_mount_state_admit(state: u16) -> i32 {
    if state & EXT4_ERROR_FS != 0 || state & EXT4_VALID_FS == 0 {
        0
    } else {
        1
    }
}

/// Whether a dirty no-journal image may be made writable again after crash
/// recovery has run.
///
/// The two reasons `admit` says no are not equivalent.  `ERROR_FS` says this
/// driver caught metadata it could not trust during a previous session; no
/// mount-time recovery repairs that, so the image stays refused.  A missing
/// `VALID_FS` on its own only says the previous session did not finish, and a
/// no-journal ext4 records exactly that work in its legacy orphan chain --
/// which the caller finishes before the mount is published.
#[no_mangle]
pub extern "C" fn a20_ext4_mount_state_recoverable(state: u16) -> i32 {
    if state & EXT4_ERROR_FS != 0 || state & EXT4_VALID_FS != 0 {
        0
    } else {
        1
    }
}

/// Persisted before exposing a no-journal mount to writers.  A crash leaves
/// VALID_FS clear; a reported metadata inconsistency additionally sets
/// ERROR_FS through `a20_ext4_mount_state_error`.
#[no_mangle]
pub extern "C" fn a20_ext4_mount_state_begin(state: u16) -> u16 {
    state & !EXT4_VALID_FS
}

/// Only used after all filesystem data/metadata writes have been flushed and
/// no sticky in-memory inconsistency was observed.
#[no_mangle]
pub extern "C" fn a20_ext4_mount_state_clean(state: u16) -> u16 {
    if state & EXT4_ERROR_FS != 0 {
        state & !EXT4_VALID_FS
    } else {
        state | EXT4_VALID_FS
    }
}

/// Persist a standard ext4 error state after an unrecoverable metadata
/// compensation failure.  The caller must still keep the mounted instance
/// quarantined if this write itself fails.
#[no_mangle]
pub extern "C" fn a20_ext4_mount_state_error(state: u16) -> u16 {
    (state & !EXT4_VALID_FS) | EXT4_ERROR_FS
}

/// Own the durable transition sequence. The adapter callbacks are restricted
/// to reading/writing the standard superblock field and issuing the device
/// flush; no filesystem mutation occurs until BEGIN has completed here.
#[no_mangle]
pub unsafe extern "C" fn a20_ext4_mount_state_transition(
    ops: *const MountStateOps,
    transition: i32,
) -> i32 {
    if ops.is_null() { return EINVAL; }
    let ops = &*ops;
    let (Some(read_state), Some(write_state), Some(flush)) =
        (ops.read_state, ops.write_state, ops.flush) else { return EINVAL; };
    let mut state = 0u16;
    let ret = read_state(ops.context, &mut state);
    if ret < 0 { return ret; }
    let next = match transition {
        1 => {
            if a20_ext4_mount_state_admit(state) == 0 { return -117; }
            a20_ext4_mount_state_begin(state)
        }
        2 => {
            if state & EXT4_ERROR_FS != 0 { return -117; }
            a20_ext4_mount_state_clean(state)
        }
        3 => a20_ext4_mount_state_error(state),
        _ => return EINVAL,
    };
    // A no-op BEGIN cannot be admitted from a clean state: BEGIN always
    // clears VALID_FS. An idempotent ERROR may skip IO because it is already
    // fail-closed. CLEAN must still issue a barrier when the bytes already
    // match: a previous CLEAN write may have reached cache but its flush
    // failed, and retry must not mistake cached state for durable state.
    if next == state {
        return if transition == 2 { flush(ops.context) } else { 0 };
    }
    let ret = write_state(ops.context, next);
    if ret < 0 { return ret; }
    let ret = flush(ops.context);
    if ret < 0 { return ret; }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct FakeDisk { state: u16, fail_write: bool, fail_flush: bool, calls: [u8; 8], len: usize }
    unsafe extern "C" fn read(ctx: *mut core::ffi::c_void, out: *mut u16) -> i32 {
        let disk = &mut *(ctx as *mut FakeDisk);
        disk.calls[disk.len] = 1; disk.len += 1;
        out.write(disk.state); 0
    }
    unsafe extern "C" fn write(ctx: *mut core::ffi::c_void, state: u16) -> i32 {
        let disk = &mut *(ctx as *mut FakeDisk);
        disk.calls[disk.len] = 2; disk.len += 1;
        if disk.fail_write { return -5; }
        disk.state = state; 0
    }
    unsafe extern "C" fn flush(ctx: *mut core::ffi::c_void) -> i32 {
        let disk = &mut *(ctx as *mut FakeDisk);
        disk.calls[disk.len] = 3; disk.len += 1;
        if disk.fail_flush { -5 } else { 0 }
    }

    #[test]
    fn clean_mount_admitted_and_dirty_or_error_rejected() {
        assert_eq!(a20_ext4_mount_state_admit(EXT4_VALID_FS), 1);
        assert_eq!(a20_ext4_mount_state_admit(0), 0);
        assert_eq!(a20_ext4_mount_state_admit(EXT4_ERROR_FS), 0);
        assert_eq!(a20_ext4_mount_state_admit(EXT4_VALID_FS | EXT4_ERROR_FS), 0);
    }

    #[test]
    fn only_benign_dirty_state_is_recoverable() {
        // Crash residue: no ERROR_FS, so orphan recovery can finish the session.
        assert_eq!(a20_ext4_mount_state_recoverable(0), 1);
        assert_eq!(a20_ext4_mount_state_recoverable(0x10), 1);
        // Already clean, or a state the driver itself flagged: not "recover".
        assert_eq!(a20_ext4_mount_state_recoverable(EXT4_VALID_FS), 0);
        assert_eq!(a20_ext4_mount_state_recoverable(EXT4_ERROR_FS), 0);
        assert_eq!(a20_ext4_mount_state_recoverable(EXT4_VALID_FS | EXT4_ERROR_FS), 0);
    }

    #[test]
    fn mount_marker_survives_crash_and_clean_is_explicit() {
        let dirty = a20_ext4_mount_state_begin(EXT4_VALID_FS | 0x10);
        assert_eq!(dirty, 0x10);
        assert_eq!(a20_ext4_mount_state_admit(dirty), 0);
        let error = a20_ext4_mount_state_error(dirty);
        assert_eq!(error, 0x12);
        assert_eq!(a20_ext4_mount_state_admit(error), 0);
        assert_eq!(a20_ext4_mount_state_clean(error), error);
    }

    #[test]
    fn begin_is_write_ahead_and_io_failures_propagate() {
        let mut disk = FakeDisk { state: EXT4_VALID_FS | 0x10, ..FakeDisk::default() };
        let ops = MountStateOps { context: (&mut disk as *mut FakeDisk).cast(),
            read_state: Some(read), write_state: Some(write), flush: Some(flush) };
        assert_eq!(unsafe { a20_ext4_mount_state_transition(&ops, 1) }, 0);
        assert_eq!(disk.state, 0x10);
        assert_eq!(&disk.calls[..disk.len], &[1, 2, 3]);

        let mut disk = FakeDisk { state: EXT4_VALID_FS, fail_write: true, ..FakeDisk::default() };
        let ops = MountStateOps { context: (&mut disk as *mut FakeDisk).cast(),
            read_state: Some(read), write_state: Some(write), flush: Some(flush) };
        assert_eq!(unsafe { a20_ext4_mount_state_transition(&ops, 1) }, EIO);
        assert_eq!(disk.state, EXT4_VALID_FS);
        assert_eq!(&disk.calls[..disk.len], &[1, 2]);
    }

    #[test]
    fn begin_flush_failure_propagates_and_does_not_admit_mount() {
        let mut disk = FakeDisk { state: EXT4_VALID_FS, fail_flush: true, ..FakeDisk::default() };
        let ops = MountStateOps { context: (&mut disk as *mut FakeDisk).cast(),
            read_state: Some(read), write_state: Some(write), flush: Some(flush) };
        assert_eq!(unsafe { a20_ext4_mount_state_transition(&ops, 1) }, EIO);
        assert_eq!(disk.state, 0);
        assert_eq!(a20_ext4_mount_state_admit(disk.state), 0);
        assert_eq!(&disk.calls[..disk.len], &[1, 2, 3]);
    }

    #[test]
    fn error_state_cannot_be_cleaned_by_ordinary_unmount() {
        let mut disk = FakeDisk { state: EXT4_ERROR_FS, ..FakeDisk::default() };
        let ops = MountStateOps { context: (&mut disk as *mut FakeDisk).cast(),
            read_state: Some(read), write_state: Some(write), flush: Some(flush) };
        assert_eq!(unsafe { a20_ext4_mount_state_transition(&ops, 2) }, -117);
        assert_eq!(disk.state, EXT4_ERROR_FS);
        assert_eq!(disk.len, 1);
    }

    #[test]
    fn clean_retry_flushes_even_when_state_bytes_already_match() {
        let mut disk = FakeDisk { state: EXT4_VALID_FS, fail_flush: true,
                                  ..FakeDisk::default() };
        let ops = MountStateOps { context: (&mut disk as *mut FakeDisk).cast(),
            read_state: Some(read), write_state: Some(write), flush: Some(flush) };
        assert_eq!(unsafe { a20_ext4_mount_state_transition(&ops, 2) }, EIO);
        assert_eq!(disk.state, EXT4_VALID_FS);
        disk.fail_flush = false;
        assert_eq!(unsafe { a20_ext4_mount_state_transition(&ops, 2) }, 0);
        assert_eq!(&disk.calls[..disk.len], &[1, 3, 1, 3]);
    }
}
