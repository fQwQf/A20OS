#![no_std]

//! User-space W^X policy: the cmdline contract and the per-call decision.
//!
//! No user VMA may be both writable and executable, and every entry point
//! (`mmap`/`mmap_file`/`mmap_vmo`, `mprotect`, ELF `PT_LOAD` loading) goes
//! through `mm_wx_filter_prot()`:
//!   - `off` (default): no intervention, which is what a stock Linux kernel
//!     does -- both `mmap` and `mprotect` honour `PROT_WRITE|PROT_EXEC`, and
//!     W^X is a hardening *option* rather than a mandate;
//!   - `strip`: warn and strip the writable bit, downgrading the mapping to
//!     read-only + executable;
//!   - `deny`: refuse a W|X combination with `-EACCES`, following the Linux
//!     security-module semantics.
//!
//! The policy is selected on the kernel cmdline: `a20.wx=off|strip|deny`.
//!
//! The default used to be `deny`, justified here by the claim that nothing in
//! this tree's user space needs RWX -- "there is no dlopen and no JIT".  nodejs
//! falsified that in one line: V8 mprotects its code pages W|X and then
//! *writes* to them, so `strip` does not rescue it either.  A kernel that stops
//! V8 at startup does not have a stricter policy, it cannot run a JIT, and the
//! premise behind the default was wrong rather than the policy.
//!
//! The foreign-architecture translator needs the opposite of that reasoning and
//! the two coexist: a task re-execed through a translator must JIT guest code
//! whatever the policy is.  That exemption is keyed on the task, not on the
//! policy, so it does not weaken W^X for anything else on the machine, and it
//! exists only in a CONFIG_XLATOR build -- which the C bridge answers for,
//! since the Rust decision only asks whether the current task is such a host.
//!
//! What stays C is the task field the exemption is keyed on, the current pid,
//! and the four log lines; the cmdline text comes from the existing
//! `bootargs_get()`.

use core::ffi::c_char;
use core::sync::atomic::{AtomicI32, Ordering};

const PROT_WRITE: i32 = 2;
const PROT_EXEC: i32 = 4;
const EACCES: i32 = -13;

const MM_WX_OFF: i32 = 0;
const MM_WX_STRIP: i32 = 1;
const MM_WX_DENY: i32 = 2;

/// Vanilla-Linux semantics by default; an image that wants the guarantee opts
/// into `deny` or `strip` on the cmdline.
static POLICY: AtomicI32 = AtomicI32::new(MM_WX_OFF);

unsafe extern "C" {
    fn bootargs_get() -> *const c_char;
    fn a20_wx_task_is_xlator_host() -> i32;
    fn a20_wx_current_pid() -> i32;
    fn a20_wx_log_selected(policy: i32);
    fn a20_wx_log_unknown(value: *const c_char);
    fn a20_wx_log_exempt(ctx: *const c_char, pid: i32);
    fn a20_wx_log_refused(policy: i32, ctx: *const c_char, pid: i32);
}

const KEY: &[u8] = b"a20.wx=";

/// Scan the cmdline for `a20.wx=off|strip|deny`, in the same token style as
/// `net/control/net_config.c`: whitespace-separated tokens, each examined once.
///
/// An unrecognised value keeps the previous setting and is reported, rather
/// than being silently ignored -- a typo in a hardening option must not look
/// like the option took effect.
#[no_mangle]
pub unsafe extern "C" fn mm_wx_policy_init() {
    let cmdline = unsafe { bootargs_get() };
    let mut policy = MM_WX_OFF;

    if !cmdline.is_null() {
        let mut p = cmdline as *const u8;
        loop {
            while unsafe { *p } == b' ' || unsafe { *p } == b'\t' {
                p = unsafe { p.add(1) };
            }
            if unsafe { *p } == 0 {
                break;
            }
            let token = p;
            while unsafe { *p } != 0 && unsafe { *p } != b' ' && unsafe { *p } != b'\t' {
                p = unsafe { p.add(1) };
            }
            let len = p as usize - token as usize;
            if len >= KEY.len()
                && unsafe { core::slice::from_raw_parts(token, KEY.len()) } == KEY
            {
                let vlen = core::cmp::min(len - KEY.len(), 15);
                let mut buf = [0u8; 16];
                unsafe {
                    core::ptr::copy_nonoverlapping(token.add(KEY.len()), buf.as_mut_ptr(), vlen);
                }
                let value = &buf[..vlen];
                if value == b"off" {
                    policy = MM_WX_OFF;
                } else if value == b"strip" {
                    policy = MM_WX_STRIP;
                } else if value == b"deny" {
                    policy = MM_WX_DENY;
                } else {
                    unsafe { a20_wx_log_unknown(buf.as_ptr() as *const c_char) };
                }
            }
        }
    }

    POLICY.store(policy, Ordering::Relaxed);
    unsafe { a20_wx_log_selected(policy) };
}

#[no_mangle]
pub unsafe extern "C" fn mm_wx_policy() -> i32 {
    POLICY.load(Ordering::Relaxed)
}

#[no_mangle]
pub unsafe extern "C" fn mm_wx_filter_prot(prot: i32, ctx: *const c_char) -> i32 {
    if prot & (PROT_WRITE | PROT_EXEC) != (PROT_WRITE | PROT_EXEC) {
        return prot;
    }
    let pid = unsafe { a20_wx_current_pid() };

    if unsafe { a20_wx_task_is_xlator_host() } != 0 {
        unsafe { a20_wx_log_exempt(ctx, pid) };
        return prot;
    }

    match POLICY.load(Ordering::Relaxed) {
        MM_WX_OFF => prot,
        MM_WX_STRIP => {
            unsafe { a20_wx_log_refused(MM_WX_STRIP, ctx, pid) };
            prot & !PROT_WRITE
        }
        _ => {
            unsafe { a20_wx_log_refused(MM_WX_DENY, ctx, pid) };
            EACCES
        }
    }
}
