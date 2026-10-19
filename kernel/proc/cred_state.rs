#![no_std]

//! POSIX credential transition policy for the process subsystem.
//!
//! Rust owns every saved-id, filesystem-id and capability transition: the
//! privilege tests, the id writes, the uid-transition capability
//! recalculation and the `capset(2)` permission matrix all live here.
//!
//! The C adapter hands over a borrowed view of the live credential — pointers
//! to the individual fields inside `task_t::cred` — so the reads and writes
//! happen in this module, under whatever lock the caller already holds, with
//! no change to the task layout. Clearing `mm->dumpable` is the only side
//! effect outside the credential: Rust decides *whether* it must be cleared and
//! reports that through `out_flags`; the adapter performs the mechanical MM
//! call.
//!
//! Ids are host (kernel) ids. The Linux ABI layer maps wire ids through the
//! user namespace before calling these entry points.

#[cfg(test)]
extern crate std;

/// Set in `out_flags` when the caller must clear `mm->dumpable`.
pub const CRED_FLAG_CLEAR_DUMPABLE: u32 = 1;

const EPERM: i32 = 1;
const EINVAL: i32 = 22;

/// `capset(2)` may only grow the inheritable set with `CAP_SETPCAP`.
const CAP_SETPCAP: u64 = 8;

/// Borrowed view of the live credential fields a transition can touch.
///
/// Every pointer addresses a field of `task_t::cred`. The adapter fills all of
/// them from one task before entering Rust; `cap_bounding` is only read by the
/// `capset(2)` matrix.
#[repr(C)]
pub struct CredView {
    pub uid: *mut i32,
    pub euid: *mut i32,
    pub suid: *mut i32,
    pub fsuid: *mut i32,
    pub gid: *mut i32,
    pub egid: *mut i32,
    pub sgid: *mut i32,
    pub fsgid: *mut i32,
    pub cap_effective: *mut u64,
    pub cap_permitted: *mut u64,
    pub cap_inheritable: *mut u64,
    pub cap_bounding: *mut u64,
}

impl CredView {
    fn is_complete(&self) -> bool {
        !self.uid.is_null()
            && !self.euid.is_null()
            && !self.suid.is_null()
            && !self.fsuid.is_null()
            && !self.gid.is_null()
            && !self.egid.is_null()
            && !self.sgid.is_null()
            && !self.fsgid.is_null()
            && !self.cap_effective.is_null()
            && !self.cap_permitted.is_null()
            && !self.cap_inheritable.is_null()
            && !self.cap_bounding.is_null()
    }
}

#[inline]
fn ld(p: *const i32) -> i32 {
    unsafe { core::ptr::read(p) }
}

#[inline]
fn st(p: *mut i32, value: i32) {
    unsafe { core::ptr::write(p, value) }
}

#[inline]
fn ldu(p: *const u64) -> u64 {
    unsafe { core::ptr::read(p) }
}

#[inline]
fn stu(p: *mut u64, value: u64) {
    unsafe { core::ptr::write(p, value) }
}

fn uid_is_current(v: &CredView, uid: i32) -> bool {
    uid == ld(v.uid) || uid == ld(v.euid) || uid == ld(v.suid)
}

fn gid_is_current(v: &CredView, gid: i32) -> bool {
    gid == ld(v.gid) || gid == ld(v.egid) || gid == ld(v.sgid)
}

/// Recalculate the effective/permitted sets after a real/saved uid change.
///
/// `old_*` are the ids as they were before the transition; the current ids are
/// read back from the view.
fn update_uid_caps(v: &CredView, old_uid: i32, old_euid: i32, old_suid: i32) {
    let old_had_root = old_uid == 0 || old_euid == 0 || old_suid == 0;
    let new_has_root = ld(v.uid) == 0 || ld(v.euid) == 0 || ld(v.suid) == 0;

    if old_had_root && !new_has_root {
        stu(v.cap_effective, 0);
        stu(v.cap_permitted, 0);
        return;
    }
    if old_euid == 0 && ld(v.euid) != 0 {
        stu(v.cap_effective, 0);
        return;
    }
    if old_euid != 0 && ld(v.euid) == 0 {
        stu(v.cap_effective, ldu(v.cap_permitted));
    }
}

fn mark_dumpable(out_flags: *mut u32) {
    if !out_flags.is_null() {
        let current = unsafe { core::ptr::read(out_flags) };
        unsafe { core::ptr::write(out_flags, current | CRED_FLAG_CLEAR_DUMPABLE) };
    }
}

/// `setuid(2)`. `has_cap_setuid` is `proc_has_cap(task, CAP_SETUID)`.
///
/// # Safety
/// `v` must point to a `CredView` whose pointers address a live credential, and
/// `out_flags` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_cred_setuid(
    v: *mut CredView,
    uid: i32,
    has_cap_setuid: i32,
    out_flags: *mut u32,
) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { &*v };
    if !v.is_complete() {
        return -EINVAL;
    }
    if has_cap_setuid != 0 {
        let (old_uid, old_euid, old_suid) = (ld(v.uid), ld(v.euid), ld(v.suid));
        if uid != ld(v.euid) || uid != ld(v.fsuid) {
            mark_dumpable(out_flags);
        }
        st(v.uid, uid);
        st(v.euid, uid);
        st(v.suid, uid);
        st(v.fsuid, uid);
        update_uid_caps(v, old_uid, old_euid, old_suid);
        return 0;
    }
    if !uid_is_current(v, uid) {
        return -EPERM;
    }
    let (old_uid, old_euid, old_suid) = (ld(v.uid), ld(v.euid), ld(v.suid));
    if uid != ld(v.euid) || uid != ld(v.fsuid) {
        mark_dumpable(out_flags);
    }
    st(v.euid, uid);
    st(v.fsuid, uid);
    update_uid_caps(v, old_uid, old_euid, old_suid);
    0
}

/// `setgid(2)`. `has_cap_setgid` is `proc_has_cap(task, CAP_SETGID)`.
///
/// # Safety
/// See [`a20_cred_setuid`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_cred_setgid(
    v: *mut CredView,
    gid: i32,
    has_cap_setgid: i32,
    out_flags: *mut u32,
) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { &*v };
    if !v.is_complete() {
        return -EINVAL;
    }
    if has_cap_setgid != 0 {
        if gid != ld(v.egid) || gid != ld(v.fsgid) {
            mark_dumpable(out_flags);
        }
        st(v.gid, gid);
        st(v.egid, gid);
        st(v.sgid, gid);
        st(v.fsgid, gid);
        return 0;
    }
    if !gid_is_current(v, gid) {
        return -EPERM;
    }
    if gid != ld(v.egid) || gid != ld(v.fsgid) {
        mark_dumpable(out_flags);
    }
    st(v.egid, gid);
    st(v.fsgid, gid);
    0
}

/// `setreuid(2)`. `-1` leaves an id unchanged.
///
/// # Safety
/// See [`a20_cred_setuid`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_cred_setreuid(
    v: *mut CredView,
    ruid: i32,
    euid: i32,
    has_cap_setuid: i32,
    out_flags: *mut u32,
) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { &*v };
    if !v.is_complete() {
        return -EINVAL;
    }
    if has_cap_setuid == 0 {
        if ruid != -1 && !uid_is_current(v, ruid) {
            return -EPERM;
        }
        if euid != -1 && !uid_is_current(v, euid) {
            return -EPERM;
        }
    }
    let (old_uid, old_euid, old_suid) = (ld(v.uid), ld(v.euid), ld(v.suid));
    if ruid != -1 {
        st(v.uid, ruid);
    }
    if euid != -1 {
        if euid != ld(v.euid) || euid != ld(v.fsuid) {
            mark_dumpable(out_flags);
        }
        st(v.euid, euid);
        st(v.fsuid, euid);
    }
    if ruid != -1 || euid != -1 {
        st(v.suid, ld(v.euid));
    }
    update_uid_caps(v, old_uid, old_euid, old_suid);
    0
}

/// `setregid(2)`. `-1` leaves an id unchanged.
///
/// # Safety
/// See [`a20_cred_setuid`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_cred_setregid(
    v: *mut CredView,
    rgid: i32,
    egid: i32,
    has_cap_setgid: i32,
    out_flags: *mut u32,
) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { &*v };
    if !v.is_complete() {
        return -EINVAL;
    }
    if has_cap_setgid == 0 {
        if rgid != -1 && !gid_is_current(v, rgid) {
            return -EPERM;
        }
        if egid != -1 && !gid_is_current(v, egid) {
            return -EPERM;
        }
    }
    if rgid != -1 {
        st(v.gid, rgid);
    }
    if egid != -1 {
        if egid != ld(v.egid) || egid != ld(v.fsgid) {
            mark_dumpable(out_flags);
        }
        st(v.egid, egid);
        st(v.fsgid, egid);
    }
    if rgid != -1 || egid != -1 {
        st(v.sgid, ld(v.egid));
    }
    0
}

/// `setresuid(2)`. `-1` leaves an id unchanged.
///
/// # Safety
/// See [`a20_cred_setuid`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_cred_setresuid(
    v: *mut CredView,
    ruid: i32,
    euid: i32,
    suid: i32,
    has_cap_setuid: i32,
    out_flags: *mut u32,
) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { &*v };
    if !v.is_complete() {
        return -EINVAL;
    }
    if has_cap_setuid == 0 {
        if ruid != -1 && !uid_is_current(v, ruid) {
            return -EPERM;
        }
        if euid != -1 && !uid_is_current(v, euid) {
            return -EPERM;
        }
        if suid != -1 && !uid_is_current(v, suid) {
            return -EPERM;
        }
    }
    let (old_uid, old_euid, old_suid) = (ld(v.uid), ld(v.euid), ld(v.suid));
    if ruid != -1 {
        st(v.uid, ruid);
    }
    if euid != -1 {
        if euid != ld(v.euid) || euid != ld(v.fsuid) {
            mark_dumpable(out_flags);
        }
        st(v.euid, euid);
        st(v.fsuid, euid);
    }
    if suid != -1 {
        st(v.suid, suid);
    }
    update_uid_caps(v, old_uid, old_euid, old_suid);
    0
}

/// `setresgid(2)`. `-1` leaves an id unchanged.
///
/// # Safety
/// See [`a20_cred_setuid`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_cred_setresgid(
    v: *mut CredView,
    rgid: i32,
    egid: i32,
    sgid: i32,
    has_cap_setgid: i32,
    out_flags: *mut u32,
) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { &*v };
    if !v.is_complete() {
        return -EINVAL;
    }
    if has_cap_setgid == 0 {
        if rgid != -1 && !gid_is_current(v, rgid) {
            return -EPERM;
        }
        if egid != -1 && !gid_is_current(v, egid) {
            return -EPERM;
        }
        if sgid != -1 && !gid_is_current(v, sgid) {
            return -EPERM;
        }
    }
    if rgid != -1 {
        st(v.gid, rgid);
    }
    if egid != -1 {
        if egid != ld(v.egid) || egid != ld(v.fsgid) {
            mark_dumpable(out_flags);
        }
        st(v.egid, egid);
        st(v.fsgid, egid);
    }
    if sgid != -1 {
        st(v.sgid, sgid);
    }
    0
}

/// `setfsuid(2)`. Returns the *previous* fsuid, as Linux does.
///
/// # Safety
/// See [`a20_cred_setuid`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_cred_setfsuid(
    v: *mut CredView,
    uid: i32,
    has_cap_setuid: i32,
    out_flags: *mut u32,
) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { &*v };
    if !v.is_complete() {
        return -EINVAL;
    }
    let old = ld(v.fsuid);
    if uid >= 0 && (has_cap_setuid != 0 || uid_is_current(v, uid)) {
        if uid != ld(v.fsuid) {
            mark_dumpable(out_flags);
        }
        st(v.fsuid, uid);
    }
    old
}

/// `setfsgid(2)`. Returns the *previous* fsgid, as Linux does.
///
/// # Safety
/// See [`a20_cred_setuid`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_cred_setfsgid(
    v: *mut CredView,
    gid: i32,
    has_cap_setgid: i32,
    out_flags: *mut u32,
) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { &*v };
    if !v.is_complete() {
        return -EINVAL;
    }
    let old = ld(v.fsgid);
    if gid >= 0 && (has_cap_setgid != 0 || gid_is_current(v, gid)) {
        if gid != ld(v.fsgid) {
            mark_dumpable(out_flags);
        }
        st(v.fsgid, gid);
    }
    old
}

/// `capset(2)` permission matrix. Applies nothing unless every check passes.
///
/// # Safety
/// See [`a20_cred_setuid`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_cred_capset_apply(
    v: *mut CredView,
    new_effective: u64,
    new_permitted: u64,
    new_inheritable: u64,
) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { &*v };
    if !v.is_complete() {
        return -EINVAL;
    }
    let old_effective = ldu(v.cap_effective);
    let old_permitted = ldu(v.cap_permitted);
    let old_inheritable = ldu(v.cap_inheritable);

    if (new_effective & !new_permitted) != 0 {
        return -EPERM;
    }
    if (new_permitted & !old_permitted) != 0 {
        return -EPERM;
    }
    let mut inheritable_allowed = old_inheritable | old_permitted;
    if old_effective & (1u64 << CAP_SETPCAP) != 0 {
        inheritable_allowed |= ldu(v.cap_bounding);
    }
    if (new_inheritable & !inheritable_allowed) != 0 {
        return -EPERM;
    }

    stu(v.cap_effective, new_effective);
    stu(v.cap_permitted, new_permitted);
    stu(v.cap_inheritable, new_inheritable);
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Test fixture: a plain credential the view points into.
    #[derive(Clone, Copy)]
    struct Fake {
        uid: i32,
        euid: i32,
        suid: i32,
        fsuid: i32,
        gid: i32,
        egid: i32,
        sgid: i32,
        fsgid: i32,
        cap_effective: u64,
        cap_permitted: u64,
        cap_inheritable: u64,
        cap_bounding: u64,
    }

    impl Fake {
        fn root() -> Self {
            Fake {
                uid: 0, euid: 0, suid: 0, fsuid: 0,
                gid: 0, egid: 0, sgid: 0, fsgid: 0,
                cap_effective: u64::MAX,
                cap_permitted: u64::MAX,
                cap_inheritable: 0,
                cap_bounding: u64::MAX,
            }
        }

        fn user(uid: i32, euid: i32, suid: i32) -> Self {
            Fake {
                uid, euid, suid, fsuid: euid,
                gid: 100, egid: 100, sgid: 100, fsgid: 100,
                cap_effective: 0, cap_permitted: 0,
                cap_inheritable: 0, cap_bounding: u64::MAX,
            }
        }

        fn view(&mut self) -> CredView {
            CredView {
                uid: &mut self.uid, euid: &mut self.euid,
                suid: &mut self.suid, fsuid: &mut self.fsuid,
                gid: &mut self.gid, egid: &mut self.egid,
                sgid: &mut self.sgid, fsgid: &mut self.fsgid,
                cap_effective: &mut self.cap_effective,
                cap_permitted: &mut self.cap_permitted,
                cap_inheritable: &mut self.cap_inheritable,
                cap_bounding: &mut self.cap_bounding,
            }
        }
    }

    fn call<F: FnOnce(&mut CredView, &mut u32) -> i32>(f: &mut Fake, body: F) -> (i32, u32) {
        let mut flags = 0u32;
        let mut view = f.view();
        let rc = body(&mut view, &mut flags);
        (rc, flags)
    }

    #[test]
    fn setuid_from_root_drops_all_capability_and_clears_dumpable() {
        let mut f = Fake::root();
        let (rc, flags) = call(&mut f, |v, o| unsafe { a20_cred_setuid(v, 1000, 1, o) });
        assert_eq!(rc, 0);
        assert_eq!((f.uid, f.euid, f.suid, f.fsuid), (1000, 1000, 1000, 1000));
        assert_eq!(f.cap_effective, 0);
        assert_eq!(f.cap_permitted, 0);
        assert_eq!(flags, CRED_FLAG_CLEAR_DUMPABLE);
    }

    #[test]
    fn setuid_back_to_root_restores_effective_from_permitted() {
        let mut f = Fake::root();
        call(&mut f, |v, o| unsafe { a20_cred_setuid(v, 1000, 1, o) });
        f.cap_permitted = 0x42;
        let (rc, flags) = call(&mut f, |v, o| unsafe { a20_cred_setuid(v, 0, 1, o) });
        assert_eq!(rc, 0);
        assert_eq!(f.euid, 0);
        assert_eq!(f.cap_effective, 0x42);
        assert_eq!(flags, CRED_FLAG_CLEAR_DUMPABLE);
    }

    #[test]
    fn unprivileged_setuid_only_accepts_a_current_id() {
        let mut f = Fake::user(1000, 1001, 1002);
        let (rc, flags) = call(&mut f, |v, o| unsafe { a20_cred_setuid(v, 2000, 0, o) });
        assert_eq!(rc, -EPERM);
        assert_eq!((f.uid, f.euid, f.suid, f.fsuid), (1000, 1001, 1002, 1001));
        assert_eq!(flags, 0);

        let (rc, flags) = call(&mut f, |v, o| unsafe { a20_cred_setuid(v, 1002, 0, o) });
        assert_eq!(rc, 0);
        assert_eq!((f.uid, f.euid, f.suid, f.fsuid), (1000, 1002, 1002, 1002));
        assert_eq!(flags, CRED_FLAG_CLEAR_DUMPABLE);
    }

    #[test]
    fn setuid_to_the_effective_id_does_not_clear_dumpable() {
        let mut f = Fake::user(1000, 1001, 1002);
        let (rc, flags) = call(&mut f, |v, o| unsafe { a20_cred_setuid(v, 1001, 0, o) });
        assert_eq!(rc, 0);
        assert_eq!(flags, 0);
    }

    #[test]
    fn setreuid_updates_saved_id_and_keeps_one_error_check_per_id() {
        let mut f = Fake::user(1000, 1001, 1002);
        let (rc, _) = call(&mut f, |v, o| unsafe { a20_cred_setreuid(v, 2000, -1, 0, o) });
        assert_eq!(rc, -EPERM);
        assert_eq!(f.uid, 1000);

        let (rc, flags) = call(&mut f, |v, o| unsafe { a20_cred_setreuid(v, 1002, 1000, 0, o) });
        assert_eq!(rc, 0);
        assert_eq!(f.uid, 1002);
        assert_eq!(f.euid, 1000);
        assert_eq!(f.fsuid, 1000);
        assert_eq!(f.suid, 1000, "saved id follows the new effective id");
        assert_eq!(flags, CRED_FLAG_CLEAR_DUMPABLE);
    }

    #[test]
    fn setreuid_leaves_everything_alone_when_both_arguments_are_unchanged() {
        let mut f = Fake::user(1000, 1001, 1002);
        let (rc, flags) = call(&mut f, |v, o| unsafe { a20_cred_setreuid(v, -1, -1, 0, o) });
        assert_eq!(rc, 0);
        assert_eq!((f.uid, f.euid, f.suid), (1000, 1001, 1002));
        assert_eq!(flags, 0);
    }

    #[test]
    fn setresuid_keeps_saved_id_independent() {
        let mut f = Fake::root();
        let (rc, _) = call(&mut f, |v, o| unsafe { a20_cred_setresuid(v, 10, 11, 12, 1, o) });
        assert_eq!(rc, 0);
        assert_eq!((f.uid, f.euid, f.suid, f.fsuid), (10, 11, 12, 11));
        assert_eq!(f.cap_effective, 0);
        assert_eq!(f.cap_permitted, 0);
    }

    #[test]
    fn setresuid_non_root_requires_every_id_to_be_current() {
        let mut f = Fake::user(1000, 1001, 1002);
        let (rc, _) = call(&mut f, |v, o| unsafe { a20_cred_setresuid(v, 1000, 1001, 2000, 0, o) });
        assert_eq!(rc, -EPERM);
        assert_eq!((f.uid, f.euid, f.suid), (1000, 1001, 1002));
    }

    #[test]
    fn setgid_family_uses_the_group_capability_and_group_ids() {
        let mut f = Fake::root();
        let (rc, flags) = call(&mut f, |v, o| unsafe { a20_cred_setgid(v, 500, 1, o) });
        assert_eq!(rc, 0);
        assert_eq!((f.gid, f.egid, f.sgid, f.fsgid), (500, 500, 500, 500));
        assert_eq!(flags, CRED_FLAG_CLEAR_DUMPABLE);

        let mut g = Fake::user(1000, 1001, 1002);
        g.gid = 100;
        g.egid = 100;
        g.sgid = 100;
        g.fsgid = 100;
        let (rc, _) = call(&mut g, |v, o| unsafe { a20_cred_setgid(v, 101, 0, o) });
        assert_eq!(rc, -EPERM);
        let (rc, flags) = call(&mut g, |v, o| unsafe { a20_cred_setregid(v, 100, 100, 0, o) });
        assert_eq!(rc, 0);
        assert_eq!((g.gid, g.egid, g.sgid, g.fsgid), (100, 100, 100, 100));
        assert_eq!(flags, 0);
    }

    #[test]
    fn setresgid_non_root_rejects_foreign_ids_without_mutating() {
        let mut f = Fake::user(1000, 1001, 1002);
        let (rc, _) = call(&mut f, |v, o| unsafe { a20_cred_setresgid(v, 100, 100, 2000, 0, o) });
        assert_eq!(rc, -EPERM);
        assert_eq!((f.gid, f.egid, f.sgid, f.fsgid), (100, 100, 100, 100));
    }

    #[test]
    fn setfsuid_returns_the_previous_value_and_ignores_negative_input() {
        let mut f = Fake::user(1000, 1001, 1002);
        let (old, flags) = call(&mut f, |v, o| unsafe { a20_cred_setfsuid(v, 1000, 0, o) });
        assert_eq!(old, 1001);
        assert_eq!(f.fsuid, 1000);
        assert_eq!(flags, CRED_FLAG_CLEAR_DUMPABLE);

        let (old, flags) = call(&mut f, |v, o| unsafe { a20_cred_setfsuid(v, -1, 0, o) });
        assert_eq!(old, 1000);
        assert_eq!(f.fsuid, 1000);
        assert_eq!(flags, 0);

        let (old, flags) = call(&mut f, |v, o| unsafe { a20_cred_setfsuid(v, 4321, 0, o) });
        assert_eq!(old, 1000, "unprivileged foreign id leaves the fsuid alone");
        assert_eq!(f.fsuid, 1000);
        assert_eq!(flags, 0);
    }

    #[test]
    fn setfsuid_privileged_accepts_any_non_negative_id() {
        let mut f = Fake::root();
        let (old, _) = call(&mut f, |v, o| unsafe { a20_cred_setfsuid(v, 4321, 1, o) });
        assert_eq!(old, 0);
        assert_eq!(f.fsuid, 4321);
    }

    #[test]
    fn setfsgid_returns_the_previous_value() {
        let mut f = Fake::user(1000, 1001, 1002);
        let (old, flags) = call(&mut f, |v, o| unsafe { a20_cred_setfsgid(v, 100, 0, o) });
        assert_eq!(old, 100);
        assert_eq!(f.fsgid, 100);
        assert_eq!(flags, 0);
    }

    #[test]
    fn capset_rejects_effective_bits_outside_permitted() {
        let mut f = Fake::root();
        let (rc, _) = call(&mut f, |v, _| unsafe { a20_cred_capset_apply(v, 0b11, 0b01, 0) });
        assert_eq!(rc, -EPERM);
        assert_eq!(f.cap_effective, u64::MAX, "no partial application on failure");
    }

    #[test]
    fn capset_rejects_growing_permitted() {
        let mut f = Fake::root();
        f.cap_permitted = 0b01;
        f.cap_effective = 0b01;
        let (rc, _) = call(&mut f, |v, _| unsafe { a20_cred_capset_apply(v, 0b10, 0b11, 0) });
        assert_eq!(rc, -EPERM);
        assert_eq!(f.cap_permitted, 0b01);
    }

    #[test]
    fn capset_inheritable_needs_setpcap_or_prior_permission() {
        let mut f = Fake::root();
        f.cap_permitted = 0b01;
        f.cap_effective = 0b01;
        f.cap_inheritable = 0;
        f.cap_bounding = 0b100;
        let (rc, _) = call(&mut f, |v, _| unsafe { a20_cred_capset_apply(v, 0b01, 0b01, 0b100) });
        assert_eq!(rc, -EPERM, "no CAP_SETPCAP in the effective set");

        f.cap_effective = 0b01 | (1 << CAP_SETPCAP);
        let (rc, _) = call(&mut f, |v, _| unsafe { a20_cred_capset_apply(v, 0b01, 0b01, 0b100) });
        assert_eq!(rc, 0);
        assert_eq!(f.cap_inheritable, 0b100);
    }

    #[test]
    fn capset_inheritable_allowed_from_permitted_without_setpcap() {
        let mut f = Fake::root();
        f.cap_permitted = 0b1010;
        f.cap_effective = 0b1010;
        f.cap_inheritable = 0;
        let (rc, _) = call(&mut f, |v, _| unsafe { a20_cred_capset_apply(v, 0b1010, 0b1010, 0b1000) });
        assert_eq!(rc, 0);
        assert_eq!(f.cap_inheritable, 0b1000);
    }

    #[test]
    fn an_incomplete_view_is_rejected_before_any_write() {
        let mut f = Fake::root();
        let mut view = f.view();
        view.fsuid = core::ptr::null_mut();
        let rc = unsafe { a20_cred_setuid(&mut view, 1000, 1, core::ptr::null_mut()) };
        assert_eq!(rc, -EINVAL);
        assert_eq!(f.uid, 0);
        assert_eq!(unsafe { a20_cred_setuid(core::ptr::null_mut(), 1000, 1, core::ptr::null_mut()) },
                   -EINVAL);
    }

    #[test]
    fn a_null_out_flags_pointer_is_tolerated() {
        let mut f = Fake::root();
        let mut view = f.view();
        let rc = unsafe { a20_cred_setuid(&mut view, 1000, 1, core::ptr::null_mut()) };
        assert_eq!(rc, 0);
        assert_eq!(f.euid, 1000);
    }
}
