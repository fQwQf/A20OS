#![no_std]

//! Rust-owned receive-side IPC transfer transaction.
//!
//! The channel endpoint, handle table, and message allocation remain C-owned
//! subsystems. This state machine owns every reservation and transferred
//! object reference between `recv_begin`, dequeue, handle publication, and
//! user copyout. Its stack lifetime is bounded by one syscall; Drop performs
//! non-allocating rollback on every early return.

use core::ffi::c_void;
use core::mem::{align_of, offset_of, size_of};
use core::ptr;

const A20_ERR_NO_MEMORY: i64 = 6;
const A20_ERR_FAULT: i64 = 8;
const A20_ERR_NO_SPACE: i64 = 13;
const A20_ERR_PROTOCOL: i64 = 21;
const MAX_HANDLES: usize = 8;

type Handle = u32;
type Rights = u64;

#[repr(C)]
#[derive(Clone, Copy)]
struct HandleInfo {
    object: *mut c_void,
    object_type: u16,
    pad: u16,
    transfer_rights: Rights,
    expiry_tick: u64,
    remaining_ops: u32,
    temporal_flags: u32,
    security_label: u8,
    pad2: [u8; 7],
}

impl HandleInfo {
    const EMPTY: Self = Self {
        object: ptr::null_mut(),
        object_type: 0,
        pad: 0,
        transfer_rights: 0,
        expiry_tick: 0,
        remaining_ops: 0,
        temporal_flags: 0,
        security_label: 0,
        pad2: [0; 7],
    };
}

const _: () = {
    assert!(size_of::<HandleInfo>() == if usize::BITS == 64 { 48 } else { 40 });
    // The 32-bit Arm AAPCS aligns uint64_t fields to eight bytes, while the
    // 32-bit Rust ABI used by this slice does the same. Do not infer aggregate
    // alignment from pointer width alone: other 32-bit targets may use four.
    #[cfg(target_arch = "arm")]
    assert!(align_of::<HandleInfo>() == 8);
    #[cfg(not(target_arch = "arm"))]
    assert!(align_of::<HandleInfo>() == if usize::BITS == 64 { 8 } else { 4 });
    assert!(offset_of!(HandleInfo, object) == 0);
    assert!(offset_of!(HandleInfo, object_type) == size_of::<usize>());
    assert!(offset_of!(HandleInfo, transfer_rights) == if usize::BITS == 64 { 16 } else { 8 });
    assert!(offset_of!(HandleInfo, expiry_tick) == if usize::BITS == 64 { 24 } else { 16 });
};

type Publish = unsafe extern "C" fn(*mut c_void, u32, u32) -> i32;

unsafe extern "C" {
    fn a20_channel_recv_begin(
        ep: *mut c_void,
        flags: u32,
        data_len: *mut u32,
        handle_count: *mut u32,
    ) -> i64;
    fn a20_channel_recv_begin_donate(
        ep: *mut c_void,
        flags: u32,
        data_len: *mut u32,
        handle_count: *mut u32,
    ) -> i64;
    fn a20_channel_recv_abort(ep: *mut c_void);
    fn a20_channel_recv_finish(
        ep: *mut c_void,
        data: *mut c_void,
        data_len: *mut u32,
        handles: *mut HandleInfo,
        handle_count: *mut u32,
    ) -> i64;
    fn a20_handle_reserve_many(ht: *mut c_void, handles: *mut Handle, count: u32) -> i64;
    fn a20_handle_abort_reserved(ht: *mut c_void, handles: *mut Handle, count: u32);
    fn a20_handle_commit_reserved_temporal(
        ht: *mut c_void,
        handle: Handle,
        object: *mut c_void,
        object_type: u16,
        rights: Rights,
        expiry_tick: u64,
        remaining_ops: u32,
        temporal_flags: u32,
        security_label: u8,
    ) -> i64;
    fn a20_handle_remove(ht: *mut c_void, handle: Handle) -> i64;
    fn a20_object_release(object: *mut c_void, object_type: u16);
    fn kmalloc(size: usize) -> *mut c_void;
    fn kfree(object: *mut c_void);
    fn copy_to_user(dst: *mut c_void, src: *const c_void, size: usize) -> isize;
}

struct ReceiveTxn {
    ep: *mut c_void,
    ht: *mut c_void,
    began: bool,
    reservations: [Handle; MAX_HANDLES],
    reservation_count: usize,
    infos: [HandleInfo; MAX_HANDLES],
    info_count: usize,
    rights: [Rights; MAX_HANDLES],
    installed: [Handle; MAX_HANDLES],
    installed_count: usize,
    data: *mut c_void,
}

impl ReceiveTxn {
    fn new(ep: *mut c_void, ht: *mut c_void) -> Self {
        Self {
            ep,
            ht,
            began: false,
            reservations: [0; MAX_HANDLES],
            reservation_count: 0,
            infos: [HandleInfo::EMPTY; MAX_HANDLES],
            info_count: 0,
            rights: [0; MAX_HANDLES],
            installed: [0; MAX_HANDLES],
            installed_count: 0,
            data: ptr::null_mut(),
        }
    }

    fn clear_after_publish(&mut self) {
        self.installed_count = 0;
        self.reservation_count = 0;
        self.info_count = 0;
    }
}

impl Drop for ReceiveTxn {
    fn drop(&mut self) {
        // The C receive path drops reserved slots before aborting a begun
        // receive when allocation fails. This helper is spinlock-only and
        // non-sleeping, so it is valid under the endpoint lock. Match that
        // context exactly, then release the endpoint lock/reference before
        // freeing or invoking any other cleanup.
        unsafe {
            if self.began {
                if self.reservation_count > 0 {
                    a20_handle_abort_reserved(
                        self.ht,
                        self.reservations.as_mut_ptr(),
                        self.reservation_count as u32,
                    );
                    self.reservation_count = 0;
                }
                a20_channel_recv_abort(self.ep);
                self.began = false;
            }
            for i in 0..self.info_count {
                let info = &mut *self.infos.as_mut_ptr().add(i);
                if !info.object.is_null() {
                    a20_object_release(info.object, info.object_type);
                    info.object = ptr::null_mut();
                }
            }
            for i in 0..self.installed_count {
                a20_handle_remove(self.ht, *self.installed.as_ptr().add(i));
            }
            if self.reservation_count > self.installed_count {
                a20_handle_abort_reserved(
                    self.ht,
                    self.reservations.as_mut_ptr().add(self.installed_count),
                    (self.reservation_count - self.installed_count) as u32,
                );
            }
            if !self.data.is_null() {
                kfree(self.data);
                self.data = ptr::null_mut();
            }
        }
    }
}

/// Receive one channel message, transferring its handles only after all
/// user-visible output copies succeed. The function owns all temporary
/// state until it returns; there is no separate finalize API or reusable
/// token that C could finalize twice.
///
/// # Safety
/// `ep` and `ht` must point to live kernel objects for this whole syscall.
/// The supplied user pointers are untrusted addresses and are only passed to
/// `copy_to_user`. `publish` must be the kernel callback paired with a live
/// syscall argument copy; it must not unwind and returns zero only when the
/// final output structure was copied successfully. The endpoint must be
/// entered without holding locks that `recv_begin` may sleep while releasing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rust_channel_recv_txn(
    ep: *mut c_void,
    ht: *mut c_void,
    flags: u32,
    donate: i32,
    user_data: *mut c_void,
    data_capacity: u32,
    user_handles: *mut c_void,
    handle_capacity: u32,
    user_rights: *mut c_void,
    publish_context: *mut c_void,
    publish: Option<Publish>,
) -> i64 {
    let mut txn = ReceiveTxn::new(ep, ht);
    let mut msg_data_len = 0u32;
    let mut msg_handles = 0u32;
    let begin = unsafe {
        if donate != 0 {
            a20_channel_recv_begin_donate(ep, flags, &mut msg_data_len, &mut msg_handles)
        } else {
            a20_channel_recv_begin(ep, flags, &mut msg_data_len, &mut msg_handles)
        }
    };
    if begin < 0 {
        return begin;
    }
    txn.began = true;

    if (msg_data_len > 0 && (user_data.is_null() || data_capacity < msg_data_len))
        || (msg_handles > 0
            && (user_handles.is_null() || handle_capacity < msg_handles))
    {
        return -A20_ERR_NO_SPACE;
    }
    if msg_handles as usize > MAX_HANDLES {
        return -A20_ERR_PROTOCOL;
    }

    if msg_handles > 0 {
        let reserve = unsafe {
            a20_handle_reserve_many(
                ht,
                txn.reservations.as_mut_ptr(),
                msg_handles,
            )
        };
        if reserve < 0 {
            return reserve;
        }
        txn.reservation_count = msg_handles as usize;
    }

    if msg_data_len > 0 {
        txn.data = unsafe { kmalloc(msg_data_len as usize) };
        if txn.data.is_null() {
            return -A20_ERR_NO_MEMORY;
        }
    }

    let mut out_len = msg_data_len;
    let mut out_count = msg_handles;
    // C's finish helper unlocks and drops the begin-held endpoint reference
    // on both success and failure, so transfer that obligation before call.
    txn.began = false;
    let finish = unsafe {
        a20_channel_recv_finish(
            ep,
            txn.data,
            &mut out_len,
            txn.infos.as_mut_ptr(),
            &mut out_count,
        )
    };
    if finish < 0 {
        return finish;
    }
    txn.info_count = (out_count as usize).min(MAX_HANDLES);
    if out_count > msg_handles || out_count as usize > MAX_HANDLES {
        // The C helper caps writes by the supplied input capacity; a larger
        // count is therefore a broken internal contract, not user input.
        return -A20_ERR_PROTOCOL;
    }
    if txn.reservation_count > txn.info_count {
        unsafe {
            a20_handle_abort_reserved(
                ht,
                txn.reservations.as_mut_ptr().add(txn.info_count),
                (txn.reservation_count - txn.info_count) as u32,
            );
        }
        txn.reservation_count = txn.info_count;
    }

    for i in 0..txn.info_count {
        let info = unsafe { *txn.infos.as_ptr().add(i) };
        unsafe { *txn.rights.as_mut_ptr().add(i) = info.transfer_rights };
        let commit = unsafe {
            a20_handle_commit_reserved_temporal(
                ht,
                *txn.reservations.as_ptr().add(i),
                info.object,
                info.object_type,
                info.transfer_rights,
                info.expiry_tick,
                info.remaining_ops,
                info.temporal_flags,
                info.security_label,
            )
        };
        if commit < 0 {
            return commit;
        }
        unsafe {
            (*txn.infos.as_mut_ptr().add(i)).object = ptr::null_mut();
            *txn.installed.as_mut_ptr().add(txn.installed_count) =
                *txn.reservations.as_ptr().add(i);
        }
        txn.installed_count += 1;
    }

    if out_len > 0
        && unsafe { copy_to_user(user_data, txn.data, out_len as usize) } < 0
    {
        return -A20_ERR_FAULT;
    }
    if txn.installed_count > 0 {
        if unsafe {
            copy_to_user(
                user_handles,
                txn.installed.as_ptr().cast(),
                txn.installed_count * size_of::<Handle>(),
            )
        } < 0
        {
            return -A20_ERR_FAULT;
        }
        if !user_rights.is_null()
            && unsafe {
                copy_to_user(
                    user_rights,
                    txn.rights.as_ptr().cast(),
                    txn.installed_count * size_of::<Rights>(),
                )
            } < 0
        {
            return -A20_ERR_FAULT;
        }
    }

    let publish = match publish {
        Some(callback) => callback,
        None => return -A20_ERR_FAULT,
    };
    if unsafe { publish(publish_context, out_len, txn.installed_count as u32) } != 0 {
        return -A20_ERR_FAULT;
    }

    let result = out_len as i64;
    txn.clear_after_publish();
    result
}
