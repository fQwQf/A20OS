//! UXFS per-operation decision engine (Rust-owned split for `uxfs.c`).
//!
//! The RPC transport state machine (admission, deadline/retry, stale-reply
//! filtering, errno mapping) already lives in `uxfs_state.rs`; the canonical
//! identity index and the lifetime capability table live in their own modules.
//! This engine owns the layer above them: for each VFS operation it decides
//! which wire request to build (opcode, ino, arg0/arg1, name, payload), how to
//! decode the response `out0`/`out1`/`out2` into a vnode/kstat/byte count, and
//! the ordered local state transitions that follow (canonical forget-before-
//! create, iget with the decoded type/mode/size, page-cache truncate plus size
//! set on truncate, short-read zero fill, writepage tail clamping and size
//! growth).
//!
//! The adapter supplies only the mechanical half through `a20_uxfs_ops`: the
//! request-header init (which allocates the superblock's next `req_id`), the
//! RPC transport call, vnode accessors, canonical lookup-or-create / identity-
//! forget / page-cache-truncate, and the lifetime gate for LINK.  Every
//! operation reaches the outside only through that table, so the host test
//! drives the exact production decision path with a scripted backend.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

pub const UFS_REQ_MAGIC: u32 = 0x5546_5352;
pub const UFS_PROTO_VERSION: u32 = 2;
pub const UFS_ROOT_INO: u64 = 1;

/* opcodes */
pub const UFS_OP_LOOKUP: u32 = 2;
pub const UFS_OP_GETATTR: u32 = 3;
pub const UFS_OP_READDIR: u32 = 4;
pub const UFS_OP_CREATE: u32 = 5;
pub const UFS_OP_MKDIR: u32 = 6;
pub const UFS_OP_UNLINK: u32 = 7;
pub const UFS_OP_RMDIR: u32 = 8;
pub const UFS_OP_TRUNCATE: u32 = 9;
pub const UFS_OP_READ: u32 = 10;
pub const UFS_OP_WRITE: u32 = 11;
pub const UFS_OP_SYNC: u32 = 12;
pub const UFS_OP_STATFS: u32 = 13;
pub const UFS_OP_RENAME: u32 = 14;
pub const UFS_OP_LINK: u32 = 17;

/* node types in a GETATTR/LOOKUP reply (low 4 bits of VFS_FT_*) */
pub const UFS_FT_DIR: u64 = 2;
pub const UFS_FT_SYMLINK: u64 = 5;

/* VFS node types the adapter's iget consumes */
pub const VFS_FT_REGULAR: c_int = 1;
pub const VFS_FT_DIR: c_int = 2;
pub const VFS_FT_SYMLINK: c_int = 3;

/* mode bits used by CREATE's iget */
pub const S_IFREG: u32 = 0o100000;
pub const CREATE_MODE: u32 = S_IFREG | 0o755;

/* statfs constants */
pub const STATFS_BLOCK: u64 = 512;
pub const STATFS_NAMELEN: u64 = 255;
pub const STATFS_TYPE: u64 = 0x5558_4653; /* 'UXFS' */

/* page size used by readpage/writepage offsets */
pub const PAGE_SIZE: u64 = 4096;

pub const ENOMEM: c_int = 12;
pub const EXDEV: c_int = 18;
pub const ENOSYS: c_int = 38;
pub const ENOTDIR: c_int = 20;
pub const EISDIR: c_int = 21;
pub const EINVAL: c_int = 22;

/* data-plane limits / formats shared with the C adapter */
pub const UFS_MAX_PAYLOAD: usize = 48 * 1024;
/* vfs_dirent64_t is packed: ino(8) off(8) reclen(2) type(1) name[1] */
pub const DIRENT_BASE: usize = 20;
pub const DT_DIR: u8 = 4;
pub const DT_LNK: u8 = 10;
pub const DT_REG: u8 = 8;
pub const SEEK_SET: c_int = 0;
pub const SEEK_CUR: c_int = 1;
pub const SEEK_END: c_int = 2;

/// Borrowed view of the adapter's `uxfs_fctx_t` (layout asserted in uxfs.c).
/// The offset/cookie, the directional and the directory flag are the production
/// open-file state the data-plane decision engine mutates in place.
#[repr(C)]
pub struct UxfsFctx {
    pub sb: *mut c_void,
    pub ino: u64,
    pub close_token: u64,
    pub close_confirm_mode: u8,
    pub close_accepted: u8,
    pub is_dir: c_int,
    pub off: usize,
}

pub type FcSbFn = extern "C" fn(*mut c_void) -> *mut c_void;
pub type FcInoFn = extern "C" fn(*const c_void) -> u64;
pub type FcOffFn = extern "C" fn(*const c_void) -> usize;
pub type FcSetOffFn = extern "C" fn(*mut c_void, usize);
pub type FcIsDirFn = extern "C" fn(*const c_void) -> c_int;
pub type VfVnodeFn = extern "C" fn(*mut c_void) -> *mut c_void;
pub type VfSetOffsetFn = extern "C" fn(*mut c_void, usize);

/// Narrow mechanical bridge for the open-file (fctx/vfile) plane.  Field order
/// mirrors `struct a20_uxfs_fops` in `fs/uxfs_ops_state.h`.
#[repr(C)]
pub struct A20UxfsFops {
    pub req_init: Option<ReqInitFn>,
    pub rpc: Option<RpcFn>,
    pub fc_sb: Option<FcSbFn>,
    pub fc_ino: Option<FcInoFn>,
    pub fc_off: Option<FcOffFn>,
    pub fc_set_off: Option<FcSetOffFn>,
    pub fc_is_dir: Option<FcIsDirFn>,
    pub vf_vnode: Option<VfVnodeFn>,
    pub vf_set_offset: Option<VfSetOffsetFn>,
    pub vn_size: Option<VnSizeFn>,
    pub vn_set_size: Option<VnSetSizeFn>,
}

struct Fops<'a> {
    req_init: &'a ReqInitFn,
    rpc: &'a RpcFn,
    fc_sb: &'a FcSbFn,
    fc_ino: &'a FcInoFn,
    fc_off: &'a FcOffFn,
    fc_set_off: &'a FcSetOffFn,
    fc_is_dir: &'a FcIsDirFn,
    vf_vnode: &'a VfVnodeFn,
    vf_set_offset: &'a VfSetOffsetFn,
    vn_size: &'a VnSizeFn,
    vn_set_size: &'a VnSetSizeFn,
}

impl<'a> Fops<'a> {
    unsafe fn load(t: *const A20UxfsFops) -> Option<Fops<'a>> {
        if t.is_null() {
            return None;
        }
        // SAFETY: caller passes a live table for the duration of the call.
        let t = unsafe { &*t };
        Some(Fops {
            req_init: t.req_init.as_ref()?,
            rpc: t.rpc.as_ref()?,
            fc_sb: t.fc_sb.as_ref()?,
            fc_ino: t.fc_ino.as_ref()?,
            fc_off: t.fc_off.as_ref()?,
            fc_set_off: t.fc_set_off.as_ref()?,
            fc_is_dir: t.fc_is_dir.as_ref()?,
            vf_vnode: t.vf_vnode.as_ref()?,
            vf_set_offset: t.vf_set_offset.as_ref()?,
            vn_size: t.vn_size.as_ref()?,
            vn_set_size: t.vn_set_size.as_ref()?,
        })
    }
}

/// Build an `a20_uxfs_fops`-style request via the adapter's `req_init` and run
/// it through the transport.
#[allow(clippy::too_many_arguments)]
unsafe fn fop_run(
    f: &Fops<'_>,
    ctx: *mut c_void,
    opcode: u32,
    ino: u64,
    arg0: u64,
    arg1: u64,
    name: *const c_char,
    payload: *const c_void,
    payload_len: u32,
    resp: *mut UfsRespHdr,
    resp_payload: *mut c_void,
    resp_cap: u32,
    resp_payload_len: *mut u32,
) -> c_int {
    let mut req = UfsReqHdr {
        magic: 0,
        version: 0,
        opcode: 0,
        req_id: 0,
        ino: 0,
        arg0: 0,
        arg1: 0,
        name_len: 0,
        payload_len: 0,
    };
    (f.req_init)(ctx, &mut req, opcode, ino, arg0, arg1, name, payload_len);
    (f.rpc)(
        ctx,
        &req,
        name,
        payload,
        resp,
        resp_payload,
        resp_cap,
        resp_payload_len,
    )
}

/// READ: clamp the count to the wire payload limit, decode the byte count,
/// zero-fill a short reply, then advance the shared offset cookie.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_file_read(
    table: *const A20UxfsFops,
    fc: *mut c_void,
    vf: *mut c_void,
    buf: *mut c_char,
    count: usize,
) -> c_int {
    let f = match unsafe { Fops::load(table) } {
        Some(f) => f,
        None => return -EXDEV,
    };
    if (f.fc_is_dir)(fc) != 0 {
        return -EISDIR;
    }
    let count = if count > UFS_MAX_PAYLOAD {
        UFS_MAX_PAYLOAD
    } else {
        count
    };
    let ctx = (f.fc_sb)(fc);
    let ino = (f.fc_ino)(fc);
    let off = (f.fc_off)(fc) as u64;
    let mut resp = zero_resp();
    let rc = unsafe {
        fop_run(
            &f,
            ctx,
            UFS_OP_READ,
            ino,
            off,
            count as u64,
            ptr::null(),
            ptr::null(),
            0,
            &mut resp,
            buf as *mut c_void,
            count as u32,
            ptr::null_mut(),
        )
    };
    if rc != 0 {
        return rc;
    }
    let mut got = resp.out0 as usize;
    if got > count {
        got = count;
    }
    if got < count {
        // SAFETY: `buf` has `count` writable bytes; got < count <= cap.
        unsafe { ptr::write_bytes(buf.add(got) as *mut u8, 0, count - got) };
    }
    let noff = off + got as u64;
    (f.fc_set_off)(fc, noff as usize);
    (f.vf_set_offset)(vf, noff as usize);
    got as c_int
}

/// WRITE: clamp, decode the written count, advance the cookie, and grow the
/// vnode size to the new end when the vfile still observes the vnode.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_file_write(
    table: *const A20UxfsFops,
    fc: *mut c_void,
    vf: *mut c_void,
    buf: *const c_char,
    count: usize,
) -> c_int {
    let f = match unsafe { Fops::load(table) } {
        Some(f) => f,
        None => return -EXDEV,
    };
    if (f.fc_is_dir)(fc) != 0 {
        return -EISDIR;
    }
    let count = if count > UFS_MAX_PAYLOAD {
        UFS_MAX_PAYLOAD
    } else {
        count
    };
    let ctx = (f.fc_sb)(fc);
    let ino = (f.fc_ino)(fc);
    let off = (f.fc_off)(fc) as u64;
    let mut resp = zero_resp();
    let rc = unsafe {
        fop_run(
            &f,
            ctx,
            UFS_OP_WRITE,
            ino,
            off,
            count as u64,
            ptr::null(),
            buf as *const c_void,
            count as u32,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    };
    if rc != 0 {
        return rc;
    }
    let mut put = resp.out0 as usize;
    if put > count {
        put = count;
    }
    let noff = off + put as u64;
    (f.fc_set_off)(fc, noff as usize);
    (f.vf_set_offset)(vf, noff as usize);
    let vn = (f.vf_vnode)(vf);
    if !vn.is_null() && (f.vn_size)(vn) < noff {
        (f.vn_set_size)(vn, noff);
    }
    put as c_int
}

/// LSEEK: pure offset arithmetic.  A directory has no size limit; a regular
/// file is capped by the vnode size at call time.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_file_lseek(
    table: *const A20UxfsFops,
    fc: *mut c_void,
    vf: *mut c_void,
    offset: i64,
    whence: c_int,
) -> i64 {
    let f = match unsafe { Fops::load(table) } {
        Some(f) => f,
        None => return -EXDEV as i64,
    };
    let cur = (f.fc_off)(fc) as i64;
    let limit: i64 = if (f.fc_is_dir)(fc) != 0 {
        -1 /* ~0ul */
    } else {
        let vn = (f.vf_vnode)(vf);
        if vn.is_null() {
            0
        } else {
            (f.vn_size)(vn) as i64
        }
    };
    let target = match whence {
        SEEK_SET => offset,
        SEEK_CUR => cur + offset,
        SEEK_END => limit + offset,
        _ => return -EINVAL as i64,
    };
    if target < 0 {
        return -EINVAL as i64;
    }
    (f.fc_set_off)(fc, target as usize);
    (f.vf_set_offset)(vf, target as usize);
    target
}

/// READDIR: request entries past the current cookie, then translate the compact
/// on-wire sequence into the packed `vfs_dirent64_t` stream the VFS consumes.
/// Only a non-empty reply advances the cookie (buffer-full keeps the resume
/// point at the next unconsumed ordinal).
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_file_readdir(
    table: *const A20UxfsFops,
    fc: *mut c_void,
    vf: *mut c_void,
    dirp: *mut c_void,
    count: usize,
    scratch: *mut u8,
    scratch_cap: u32,
) -> c_int {
    let f = match unsafe { Fops::load(table) } {
        Some(f) => f,
        None => return -EXDEV,
    };
    if (f.fc_is_dir)(fc) == 0 {
        return -ENOTDIR;
    }
    let ctx = (f.fc_sb)(fc);
    let ino = (f.fc_ino)(fc);
    let start = (f.fc_off)(fc) as u64;
    let mut resp = zero_resp();
    let mut plen: u32 = 0;
    let rc = unsafe {
        fop_run(
            &f,
            ctx,
            UFS_OP_READDIR,
            ino,
            start,
            count as u64,
            ptr::null(),
            ptr::null(),
            0,
            &mut resp,
            scratch as *mut c_void,
            scratch_cap,
            &mut plen,
        )
    };
    if rc != 0 {
        return rc;
    }

    let out = dirp as *mut u8;
    let mut total: usize = 0;
    let mut i: u32 = 0;
    let mut ordinal = start;
    while i + 10 <= plen {
        // SAFETY: bounds checked; scratch holds `plen` payload bytes.
        let ino = unsafe { ptr::read_unaligned(scratch.add(i as usize) as *const u64) };
        i += 8;
        let ty = unsafe { *scratch.add(i as usize) };
        i += 1;
        let nlen = unsafe { *scratch.add(i as usize) } as usize;
        i += 1;
        if i as usize + nlen > plen as usize {
            break;
        }
        let rec = DIRENT_BASE + nlen + 1;
        let reclen = (rec + 7) & !7usize;
        if total + reclen > count {
            break;
        }
        let base = unsafe { out.add(total) };
        let d_type = if ty == UFS_FT_DIR as u8 {
            DT_DIR
        } else if ty == UFS_FT_SYMLINK as u8 {
            DT_LNK
        } else {
            DT_REG
        };
        // SAFETY: `reclen` bytes are available at base (bounds checked above);
        // the vfs_dirent64_t layout is packed ino/off/reclen/type/name.
        unsafe {
            ptr::write_unaligned(base as *mut u64, ino);
            ptr::write_unaligned(base.add(8) as *mut i64, (ordinal + 1) as i64);
            ptr::write_unaligned(base.add(16) as *mut u16, reclen as u16);
            *base.add(18) = d_type;
            ptr::copy_nonoverlapping(scratch.add(i as usize), base.add(19), nlen);
            *base.add(19 + nlen) = 0;
        }
        total += reclen;
        ordinal += 1;
        i += nlen as u32;
    }

    if total > 0 {
        (f.fc_set_off)(fc, ordinal as usize);
    }
    let final_off = if total > 0 { ordinal } else { start };
    (f.vf_set_offset)(vf, final_off as usize);
    total as c_int
}

/// `ufs_req_hdr_t`, asserted against the C header by the adapter.
#[repr(C)]
pub struct UfsReqHdr {
    pub magic: u32,
    pub version: u32,
    pub opcode: u32,
    pub req_id: u32,
    pub ino: u64,
    pub arg0: u64,
    pub arg1: u64,
    pub name_len: u32,
    pub payload_len: u32,
}

/// `ufs_resp_hdr_t`.
#[repr(C)]
pub struct UfsRespHdr {
    pub magic: u32,
    pub opcode: u32,
    pub req_id: u32,
    pub status: i32,
    pub out0: u64,
    pub out1: u64,
    pub out2: u64,
    pub payload_len: u32,
    pub _pad: u32,
}

/// `kstat_t`, mirrored so the engine can populate the caller's struct.
#[repr(C)]
pub struct Kstat {
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

/// `kstatfs_t`, mirrored.
#[repr(C)]
pub struct Kstatfs {
    pub f_type: u64,
    pub f_bsize: u64,
    pub f_blocks: u64,
    pub f_bfree: u64,
    pub f_bavail: u64,
    pub f_files: u64,
    pub f_ffree: u64,
    pub f_fsid: u64,
    pub f_namelen: u64,
    pub f_frsize: u64,
    pub f_flags: u64,
}

pub type ReqInitFn = extern "C" fn(
    *mut c_void,
    *mut UfsReqHdr,
    u32,
    u64,
    u64,
    u64,
    *const c_char,
    u32,
);
pub type RpcFn = extern "C" fn(
    *mut c_void,
    *const UfsReqHdr,
    *const c_char,
    *const c_void,
    *mut UfsRespHdr,
    *mut c_void,
    u32,
    *mut u32,
) -> c_int;
pub type VprivSbFn = extern "C" fn(*mut c_void) -> *mut c_void;
pub type VnInoFn = extern "C" fn(*const c_void) -> u64;
pub type VnSizeFn = extern "C" fn(*const c_void) -> u64;
pub type VnSetSizeFn = extern "C" fn(*mut c_void, u64);
pub type VnParentFn = extern "C" fn(*mut c_void) -> *mut c_void;
pub type IgetFn = extern "C" fn(
    *mut c_void,
    u64,
    c_int,
    u32,
    u64,
    *mut c_void,
) -> *mut c_void;
pub type IdentityForgetFn = extern "C" fn(*mut c_void, u64);
pub type VnodeGetFn = extern "C" fn(*mut c_void);
pub type PageCacheTruncateFn = extern "C" fn(*mut c_void, u64);
pub type NegotiateLifetimeFn = extern "C" fn(*mut c_void) -> c_int;
pub type LifetimeIsV2Fn = extern "C" fn(*mut c_void) -> c_int;

/// Narrow mechanical bridge table.  Field order mirrors
/// `struct a20_uxfs_ops` in `fs/uxfs_ops_state.h`.
#[repr(C)]
pub struct A20UxfsOps {
    pub req_init: Option<ReqInitFn>,
    pub rpc: Option<RpcFn>,
    pub vpriv_sb: Option<VprivSbFn>,
    pub vn_ino: Option<VnInoFn>,
    pub vn_size: Option<VnSizeFn>,
    pub vn_set_size: Option<VnSetSizeFn>,
    pub vn_parent: Option<VnParentFn>,
    pub iget: Option<IgetFn>,
    pub identity_forget: Option<IdentityForgetFn>,
    pub vnode_get: Option<VnodeGetFn>,
    pub page_cache_truncate: Option<PageCacheTruncateFn>,
    pub negotiate_lifetime: Option<NegotiateLifetimeFn>,
    pub lifetime_is_v2: Option<LifetimeIsV2Fn>,
}

struct Ops<'a> {
    req_init: &'a ReqInitFn,
    rpc: &'a RpcFn,
    vpriv_sb: &'a VprivSbFn,
    vn_ino: &'a VnInoFn,
    vn_size: &'a VnSizeFn,
    vn_set_size: &'a VnSetSizeFn,
    vn_parent: &'a VnParentFn,
    iget: &'a IgetFn,
    identity_forget: &'a IdentityForgetFn,
    vnode_get: &'a VnodeGetFn,
    page_cache_truncate: &'a PageCacheTruncateFn,
    negotiate_lifetime: &'a NegotiateLifetimeFn,
    lifetime_is_v2: &'a LifetimeIsV2Fn,
}

impl<'a> Ops<'a> {
    unsafe fn load(t: *const A20UxfsOps) -> Option<Ops<'a>> {
        if t.is_null() {
            return None;
        }
        // SAFETY: caller passes a live table for the duration of the call.
        let t = unsafe { &*t };
        Some(Ops {
            req_init: t.req_init.as_ref()?,
            rpc: t.rpc.as_ref()?,
            vpriv_sb: t.vpriv_sb.as_ref()?,
            vn_ino: t.vn_ino.as_ref()?,
            vn_size: t.vn_size.as_ref()?,
            vn_set_size: t.vn_set_size.as_ref()?,
            vn_parent: t.vn_parent.as_ref()?,
            iget: t.iget.as_ref()?,
            identity_forget: t.identity_forget.as_ref()?,
            vnode_get: t.vnode_get.as_ref()?,
            page_cache_truncate: t.page_cache_truncate.as_ref()?,
            negotiate_lifetime: t.negotiate_lifetime.as_ref()?,
            lifetime_is_v2: t.lifetime_is_v2.as_ref()?,
        })
    }
}

/// Compare a NUL-terminated C string against a byte literal.
#[inline]
fn c_str_is(s: *const c_char, lit: &[u8]) -> bool {
    if s.is_null() {
        return false;
    }
    let mut i = 0usize;
    loop {
        // SAFETY: `s` is a live NUL-terminated string for this call.
        let b = unsafe { *s.add(i) } as u8;
        let expect = if i < lit.len() { lit[i] } else { 0 };
        if b != expect {
            return false;
        }
        if b == 0 {
            return true;
        }
        i += 1;
    }
}

/// Map a server node type (UFS_FT_*) to the VFS node type for iget.
#[inline]
fn ufs_ft_to_vfs(t: u64) -> c_int {
    match t {
        UFS_FT_DIR => VFS_FT_DIR,
        UFS_FT_SYMLINK => VFS_FT_SYMLINK,
        _ => VFS_FT_REGULAR,
    }
}

/// Build a request header via the adapter's `req_init` (which allocates the
/// superblock's next `req_id`), then run the RPC.  `name`/`payload` are passed
/// through to both so the transport can frame them.  Returns the transport/
/// status code (0 or a negative errno) and leaves the decoded header in `resp`.
unsafe fn run_op(
    ops: &Ops<'_>,
    ctx: *mut c_void,
    opcode: u32,
    ino: u64,
    arg0: u64,
    arg1: u64,
    name: *const c_char,
    payload: *const c_void,
    payload_len: u32,
    resp: *mut UfsRespHdr,
    resp_payload: *mut c_void,
    resp_cap: u32,
    resp_payload_len: *mut u32,
) -> c_int {
    let mut req = UfsReqHdr {
        magic: 0,
        version: 0,
        opcode: 0,
        req_id: 0,
        ino: 0,
        arg0: 0,
        arg1: 0,
        name_len: 0,
        payload_len: 0,
    };
    (ops.req_init)(
        ctx,
        &mut req,
        opcode,
        ino,
        arg0,
        arg1,
        name,
        payload_len,
    );
    (ops.rpc)(
        ctx,
        &req,
        name,
        payload,
        resp,
        resp_payload,
        resp_cap,
        resp_payload_len,
    )
}

#[inline]
fn zero_resp() -> UfsRespHdr {
    UfsRespHdr {
        magic: 0,
        opcode: 0,
        req_id: 0,
        status: 0,
        out0: 0,
        out1: 0,
        out2: 0,
        payload_len: 0,
        _pad: 0,
    }
}

/// LOOKUP: `.` returns `dir`, `..` returns `dir->parent` (or `dir` at the
/// root); otherwise a wire LOOKUP whose reply supplies ino/type/mode/size for
/// canonical lookup-or-create.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_lookup(
    table: *const A20UxfsOps,
    dir: *mut c_void,
    name: *const c_char,
    out: *mut *mut c_void,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    if out.is_null() {
        return -EXDEV;
    }

    if c_str_is(name, b".") {
        unsafe {
            *out = dir;
            (ops.vnode_get)(dir);
        }
        return 0;
    }
    if c_str_is(name, b"..") {
        let parent = (ops.vn_parent)(dir);
        unsafe {
            if !parent.is_null() {
                *out = parent;
                (ops.vnode_get)(parent);
                /* the caller does vnode_put on dir */
                (ops.vnode_get)(dir);
            } else {
                *out = dir;
                (ops.vnode_get)(dir);
            }
        }
        return 0;
    }

    let ctx = (ops.vpriv_sb)(dir);
    let dir_ino = (ops.vn_ino)(dir);
    let mut resp = zero_resp();
    let rc = unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_LOOKUP,
            dir_ino,
            0,
            0,
            name,
            ptr::null(),
            0,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    };
    if rc != 0 {
        return rc;
    }
    let vfs_type = ufs_ft_to_vfs(resp.out1);
    let mode = (resp.out2 >> 32) as u32;
    let size = resp.out2 as u32 as u64;
    let vn = (ops.iget)(ctx, resp.out0, vfs_type, mode, size, dir);
    unsafe {
        *out = vn;
    }
    if vn.is_null() {
        -ENOMEM
    } else {
        0
    }
}

/// GETATTR: refresh the cached size and populate the caller's `kstat`.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_stat(
    table: *const A20UxfsOps,
    vn: *mut c_void,
    kstat_out: *mut c_void,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    if kstat_out.is_null() {
        return -EXDEV;
    }
    let ctx = (ops.vpriv_sb)(vn);
    let ino = (ops.vn_ino)(vn);
    let mut resp = zero_resp();
    let rc = unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_GETATTR,
            ino,
            0,
            0,
            ptr::null(),
            ptr::null(),
            0,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    };
    if rc != 0 {
        return rc;
    }
    let size = resp.out0;
    let mode = (resp.out1 >> 32) as u32;
    // SAFETY: the caller passes a live `kstat_t`; the mirror matches its layout.
    let st = unsafe { &mut *(kstat_out as *mut Kstat) };
    *st = Kstat {
        st_dev: 0,
        st_ino: ino,
        st_mode: mode,
        st_nlink: 1,
        st_uid: 0,
        st_gid: 0,
        st_rdev: 0,
        st_size: size,
        st_blksize: STATFS_BLOCK,
        st_blocks: (size + 511) / 512,
        st_atime: 0,
        st_atime_nsec: 0,
        st_mtime: 0,
        st_mtime_nsec: 0,
        st_ctime: 0,
        st_ctime_nsec: 0,
    };
    /* refresh the cached size so reads on the open fd match the server */
    (ops.vn_set_size)(vn, size);
    0
}

/// STATFS: decode the free/total byte counts into 512-byte blocks.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_statfs(
    table: *const A20UxfsOps,
    vn: *mut c_void,
    kstatfs_out: *mut c_void,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    if kstatfs_out.is_null() {
        return -EXDEV;
    }
    let ctx = (ops.vpriv_sb)(vn);
    let mut resp = zero_resp();
    let rc = unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_STATFS,
            0,
            0,
            0,
            ptr::null(),
            ptr::null(),
            0,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    };
    if rc != 0 {
        return rc;
    }
    let bfree = resp.out1 / STATFS_BLOCK;
    // SAFETY: the caller passes a live `kstatfs_t`; the mirror matches layout.
    let st = unsafe { &mut *(kstatfs_out as *mut Kstatfs) };
    *st = Kstatfs {
        f_type: STATFS_TYPE,
        f_bsize: STATFS_BLOCK,
        f_blocks: resp.out0 / STATFS_BLOCK,
        f_bfree: bfree,
        f_bavail: bfree,
        f_files: 0,
        f_ffree: 0,
        f_fsid: 0,
        f_namelen: STATFS_NAMELEN,
        f_frsize: STATFS_BLOCK,
        f_flags: 0,
    };
    0
}

/// CREATE: forget any stale canonical entry for a recycled inode number, then
/// lookup-or-create the regular vnode.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_create(
    table: *const A20UxfsOps,
    dir: *mut c_void,
    name: *const c_char,
    mode: u32,
    out: *mut *mut c_void,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    let ctx = (ops.vpriv_sb)(dir);
    let dir_ino = (ops.vn_ino)(dir);
    let mut resp = zero_resp();
    let rc = unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_CREATE,
            dir_ino,
            mode as u64,
            0,
            name,
            ptr::null(),
            0,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    };
    if rc != 0 {
        return rc;
    }
    if out.is_null() {
        return 0;
    }
    /* CREATE may reuse a recently freed inode number: forget any stale
     * canonical entry first, then register the created vnode. */
    if resp.out0 != 0 {
        (ops.identity_forget)(ctx, resp.out0);
    }
    let vn = (ops.iget)(ctx, resp.out0, VFS_FT_REGULAR, CREATE_MODE, 0, dir);
    unsafe {
        *out = vn;
    }
    if vn.is_null() {
        -ENOMEM
    } else {
        0
    }
}

/// MKDIR: a stale entry for a recycled inode would otherwise alias a deleted
/// directory, so forget the new inode's canonical key on success.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_mkdir(
    table: *const A20UxfsOps,
    dir: *mut c_void,
    name: *const c_char,
    mode: u32,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    let ctx = (ops.vpriv_sb)(dir);
    let dir_ino = (ops.vn_ino)(dir);
    let mut resp = zero_resp();
    let rc = unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_MKDIR,
            dir_ino,
            mode as u64,
            0,
            name,
            ptr::null(),
            0,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    };
    if rc == 0 && resp.out0 != 0 {
        (ops.identity_forget)(ctx, resp.out0);
    }
    rc
}

/// UNLINK: the canonical identity is retained (a hard-link alias may still
/// name the inode); CREATE forgets it if the backend recycles the number.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_unlink(
    table: *const A20UxfsOps,
    dir: *mut c_void,
    name: *const c_char,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    let ctx = (ops.vpriv_sb)(dir);
    let dir_ino = (ops.vn_ino)(dir);
    let mut resp = zero_resp();
    unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_UNLINK,
            dir_ino,
            0,
            0,
            name,
            ptr::null(),
            0,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    }
}

/// LINK: cross-superblock is EXDEV, the operation needs a negotiated v2
/// lifetime, then a wire LINK naming the target inode in arg0.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_link(
    table: *const A20UxfsOps,
    dir: *mut c_void,
    name: *const c_char,
    target: *mut c_void,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    if target.is_null() {
        return -EXDEV;
    }
    let ctx = (ops.vpriv_sb)(dir);
    let tctx = (ops.vpriv_sb)(target);
    if ctx.is_null() || tctx.is_null() || ctx != tctx {
        return -EXDEV;
    }
    let rc = (ops.negotiate_lifetime)(ctx);
    if rc != 0 {
        return rc;
    }
    if (ops.lifetime_is_v2)(ctx) == 0 {
        return -ENOSYS;
    }
    let dir_ino = (ops.vn_ino)(dir);
    let target_ino = (ops.vn_ino)(target);
    let mut resp = zero_resp();
    unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_LINK,
            dir_ino,
            target_ino,
            0,
            name,
            ptr::null(),
            0,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    }
}

/// RMDIR: the detached directory identity is retained until inode reuse.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_rmdir(
    table: *const A20UxfsOps,
    dir: *mut c_void,
    name: *const c_char,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    let ctx = (ops.vpriv_sb)(dir);
    let dir_ino = (ops.vn_ino)(dir);
    let mut resp = zero_resp();
    unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_RMDIR,
            dir_ino,
            0,
            0,
            name,
            ptr::null(),
            0,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    }
}

/// RENAME: the source name is the name segment and the target name is the
/// payload; arg0 is the target directory inode and arg1 the target length.  All
/// identities are kept until CREATE/MKDIR proves inode-number reuse.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_rename(
    table: *const A20UxfsOps,
    old_dir: *mut c_void,
    old_name: *const c_char,
    new_dir: *mut c_void,
    new_name: *const c_char,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    let ctx = (ops.vpriv_sb)(old_dir);
    let old_ino = (ops.vn_ino)(old_dir);
    let new_ino = (ops.vn_ino)(new_dir);
    let new_len = c_str_len(new_name);
    let mut resp = zero_resp();
    unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_RENAME,
            old_ino,
            new_ino,
            new_len as u64,
            old_name,
            new_name as *const c_void,
            new_len as u32,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    }
}

/// TRUNCATE: on success truncate the page cache and pin the exact size.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_truncate(
    table: *const A20UxfsOps,
    vn: *mut c_void,
    size: u64,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    let ctx = (ops.vpriv_sb)(vn);
    let ino = (ops.vn_ino)(vn);
    let mut resp = zero_resp();
    let rc = unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_TRUNCATE,
            ino,
            size,
            0,
            ptr::null(),
            ptr::null(),
            0,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    };
    if rc == 0 {
        (ops.page_cache_truncate)(vn, size);
        (ops.vn_set_size)(vn, size);
    }
    rc
}

/// SYNC: flush the inode through the service.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_sync_vnode(
    table: *const A20UxfsOps,
    vn: *mut c_void,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    let ctx = (ops.vpriv_sb)(vn);
    let ino = (ops.vn_ino)(vn);
    let mut resp = zero_resp();
    unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_SYNC,
            ino,
            0,
            0,
            ptr::null(),
            ptr::null(),
            0,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    }
}

/// READPAGE: a full page read at `index * PAGE_SIZE`; the last page may read
/// short, so zero-fill the remainder.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_readpage(
    table: *const A20UxfsOps,
    vn: *mut c_void,
    index: u64,
    data: *mut c_void,
    len: u64,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    let ctx = (ops.vpriv_sb)(vn);
    let ino = (ops.vn_ino)(vn);
    let off = index * PAGE_SIZE;
    let mut resp = zero_resp();
    let mut plen = 0u32;
    let rc = unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_READ,
            ino,
            off,
            len,
            ptr::null(),
            ptr::null(),
            0,
            &mut resp,
            data,
            len as u32,
            &mut plen,
        )
    };
    if rc != 0 {
        return rc;
    }
    let got = resp.out0;
    if got < len {
        unsafe {
            ptr::write_bytes((data as *mut u8).add(got as usize), 0, (len - got) as usize);
        }
    }
    0
}

/// WRITEPAGE: writeback flushes whole pages, but only bytes within the file are
/// authoritative (otherwise the service-side size would inflate to the page
/// boundary on the tail page); grow the cached size to cover the written span.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_op_writepage(
    table: *const A20UxfsOps,
    vn: *mut c_void,
    index: u64,
    data: *const c_void,
    len: u64,
) -> c_int {
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EXDEV,
    };
    let ctx = (ops.vpriv_sb)(vn);
    let ino = (ops.vn_ino)(vn);
    let off = index * PAGE_SIZE;
    let size = (ops.vn_size)(vn);
    /* only bytes within the file are authoritative */
    if off >= size {
        return 0;
    }
    let mut len = len;
    if off + len > size {
        len = size - off;
    }
    let mut resp = zero_resp();
    let rc = unsafe {
        run_op(
            &ops,
            ctx,
            UFS_OP_WRITE,
            ino,
            off,
            len,
            ptr::null(),
            data,
            len as u32,
            &mut resp,
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    };
    if rc != 0 {
        return rc;
    }
    if off + resp.out0 > size {
        (ops.vn_set_size)(vn, off + resp.out0);
    }
    0
}

/// Length of a NUL-terminated C string.
#[inline]
fn c_str_len(s: *const c_char) -> usize {
    if s.is_null() {
        return 0;
    }
    let mut i = 0usize;
    loop {
        // SAFETY: `s` is a live NUL-terminated string for this call.
        if unsafe { *s.add(i) } == 0 {
            return i;
        }
        i += 1;
    }
}

/* ==================================================================== */
/* mount/serve/unmount namespace + block-device ownership               */
/* ==================================================================== */

pub const ENOENT: c_int = 2;
pub const ESRCH: c_int = 3;
pub const EBUSY: c_int = 16;
pub const ENODEV: c_int = 19;
pub const EMFILE: c_int = 24;
pub const EOPNOTSUPP: c_int = 95;
pub const EIO: c_int = 5;
pub const FS_TYPE_UXFS: u32 = 10;
pub const VFS_MOUNT_RDONLY: u32 = 0x1;
pub const S_IFDIR: u32 = 0o040000;
pub const ROOT_DIR_MODE: u32 = S_IFDIR | 0o755;

/// Borrowed view of `uxfs_lifetime_state_t` (mirrored in lifetime_state.rs; a
/// local copy keeps this object self-contained).
#[derive(Clone, Copy)]
#[repr(C)]
pub struct UxfsLifetimeView {
    pub mode: u32,
    pub shutdown_mode: u32,
    pub close_token_mode: u32,
    pub close_confirm_mode: u32,
    pub generation: u64,
    pub server: *mut c_void,
}
const _: [(); 32] = [(); core::mem::size_of::<UxfsLifetimeView>()];
const _: [(); 16] = [(); core::mem::offset_of!(UxfsLifetimeView, generation)];

/// Borrowed view of the leading fields of the adapter's `uxfs_sb_t`.  The
/// trailing `mutex_t`s are opaque here; Rust indexes the static table with the
/// stride the adapter reports, so only this asserted prefix is modelled.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct UxfsSbView {
    pub ep: *mut c_void,
    pub server: *mut c_void,
    pub block_index: c_int,
    pub next_req_id: u32,
    pub lifetime: UxfsLifetimeView,
    pub shutdown_started: u32,
    pub slot_reserved: u32,
    pub live_vnodes: u32,
    pub server_generation: u64,
}

/* The sb view mirrors the kernel's 64-bit `uxfs_sb_t` prefix; the host smoke
 * build may target 32-bit, so these offset checks only apply to 64-bit
 * pointers (the C adapter pins the same layout for the real kernel). */
const _: () = {
    if core::mem::size_of::<*mut c_void>() == 8 {
        assert!(core::mem::size_of::<UxfsLifetimeView>() == 32);
        assert!(core::mem::offset_of!(UxfsSbView, server) == 8);
        assert!(core::mem::offset_of!(UxfsSbView, block_index) == 16);
        assert!(core::mem::offset_of!(UxfsSbView, lifetime) == 24);
        assert!(core::mem::offset_of!(UxfsSbView, shutdown_started) == 56);
        assert!(core::mem::offset_of!(UxfsSbView, slot_reserved) == 60);
        assert!(core::mem::offset_of!(UxfsSbView, live_vnodes) == 64);
        assert!(core::mem::offset_of!(UxfsSbView, server_generation) == 72);
    }
};
pub type SbTableFn = extern "C" fn() -> *mut c_void;
pub type SbStrideFn = extern "C" fn() -> u64;
pub type SbCountFn = extern "C" fn() -> c_int;
pub type LockMountsFn = extern "C" fn() -> u64;
pub type UnlockMountsFn = extern "C" fn(u64);
pub type BlockDeviceFn = extern "C" fn(c_int) -> *mut c_void;
pub type DeviceHasNativeFn = extern "C" fn(*mut c_void) -> c_int;
pub type PathIsDirFn = extern "C" fn(*const c_char) -> c_int;
pub type Iget2Fn =
    extern "C" fn(*mut c_void, u64, c_int, u32, u64, *mut c_void) -> *mut c_void;
pub type MountAllocFn = extern "C" fn() -> *mut c_void;
pub type MountInitFn =
    extern "C" fn(*mut c_void, *const c_char, u32, *mut c_void, *mut c_void);
pub type OnePtrFn = extern "C" fn(*mut c_void);
pub type ProcGetFn = extern "C" fn(*mut c_void) -> *mut c_void;
pub type PeerClosedFn = extern "C" fn(*mut c_void) -> c_int;
pub type NowNsFn = extern "C" fn() -> u64;
pub type NegotiateUntilFn = extern "C" fn(*mut c_void, u64) -> c_int;
pub type ShutdownRpcFn = extern "C" fn(*mut c_void, u64, *mut c_int) -> c_int;
pub type MountLogFn = extern "C" fn(*const c_char, c_int);

/// Narrow mechanical bridge for the UXFS namespace plane.  Field order mirrors
/// `struct a20_uxfs_ns` in `fs/uxfs_ops_state.h`.
#[repr(C)]
pub struct A20UxfsNs {
    pub sb_table: Option<SbTableFn>,
    pub sb_stride: Option<SbStrideFn>,
    pub sb_count: Option<SbCountFn>,
    pub lock_mounts: Option<LockMountsFn>,
    pub unlock_mounts: Option<UnlockMountsFn>,
    pub block_device: Option<BlockDeviceFn>,
    pub device_has_native_mount: Option<DeviceHasNativeFn>,
    pub path_is_dir: Option<PathIsDirFn>,
    pub iget: Option<Iget2Fn>,
    pub mount_alloc: Option<MountAllocFn>,
    pub mount_init: Option<MountInitFn>,
    pub mount_link_tree: Option<OnePtrFn>,
    pub dcache_invalidate_all: Option<OnePtrFn>,
    pub identity_register_peek: Option<OnePtrFn>,
    pub identity_clear_sb: Option<OnePtrFn>,
    pub vnode_get: Option<OnePtrFn>,
    pub vnode_put: Option<OnePtrFn>,
    pub vnode_fs_data: Option<ProcGetFn>,
    pub lifetime_reset: Option<OnePtrFn>,
    pub proc_get: Option<ProcGetFn>,
    pub proc_put: Option<OnePtrFn>,
    pub slot_init_req_lock: Option<OnePtrFn>,
    pub detach_server: Option<OnePtrFn>,
    pub peer_closed: Option<PeerClosedFn>,
    pub now_ns: Option<NowNsFn>,
    pub negotiate_lifetime_until: Option<NegotiateUntilFn>,
    pub shutdown_rpc: Option<ShutdownRpcFn>,
    pub mount_log: Option<MountLogFn>,
}

struct Ns<'a> {
    sb_table: &'a SbTableFn,
    sb_stride: &'a SbStrideFn,
    sb_count: &'a SbCountFn,
    lock_mounts: &'a LockMountsFn,
    unlock_mounts: &'a UnlockMountsFn,
    block_device: &'a BlockDeviceFn,
    device_has_native_mount: &'a DeviceHasNativeFn,
    path_is_dir: &'a PathIsDirFn,
    iget: &'a Iget2Fn,
    mount_alloc: &'a MountAllocFn,
    mount_init: &'a MountInitFn,
    mount_link_tree: &'a OnePtrFn,
    dcache_invalidate_all: &'a OnePtrFn,
    identity_register_peek: &'a OnePtrFn,
    identity_clear_sb: &'a OnePtrFn,
    vnode_get: &'a OnePtrFn,
    vnode_put: &'a OnePtrFn,
    vnode_fs_data: &'a ProcGetFn,
    lifetime_reset: &'a OnePtrFn,
    proc_get: &'a ProcGetFn,
    proc_put: &'a OnePtrFn,
    slot_init_req_lock: &'a OnePtrFn,
    detach_server: &'a OnePtrFn,
    peer_closed: &'a PeerClosedFn,
    now_ns: &'a NowNsFn,
    negotiate_lifetime_until: &'a NegotiateUntilFn,
    shutdown_rpc: &'a ShutdownRpcFn,
    mount_log: &'a MountLogFn,
}

impl<'a> Ns<'a> {
    unsafe fn load(t: *const A20UxfsNs) -> Option<Ns<'a>> {
        if t.is_null() {
            return None;
        }
        // SAFETY: caller passes a live table for the duration of the call.
        let t = unsafe { &*t };
        Some(Ns {
            sb_table: t.sb_table.as_ref()?,
            sb_stride: t.sb_stride.as_ref()?,
            sb_count: t.sb_count.as_ref()?,
            lock_mounts: t.lock_mounts.as_ref()?,
            unlock_mounts: t.unlock_mounts.as_ref()?,
            block_device: t.block_device.as_ref()?,
            device_has_native_mount: t.device_has_native_mount.as_ref()?,
            path_is_dir: t.path_is_dir.as_ref()?,
            iget: t.iget.as_ref()?,
            mount_alloc: t.mount_alloc.as_ref()?,
            mount_init: t.mount_init.as_ref()?,
            mount_link_tree: t.mount_link_tree.as_ref()?,
            dcache_invalidate_all: t.dcache_invalidate_all.as_ref()?,
            identity_register_peek: t.identity_register_peek.as_ref()?,
            identity_clear_sb: t.identity_clear_sb.as_ref()?,
            vnode_get: t.vnode_get.as_ref()?,
            vnode_put: t.vnode_put.as_ref()?,
            vnode_fs_data: t.vnode_fs_data.as_ref()?,
            lifetime_reset: t.lifetime_reset.as_ref()?,
            proc_get: t.proc_get.as_ref()?,
            proc_put: t.proc_put.as_ref()?,
            slot_init_req_lock: t.slot_init_req_lock.as_ref()?,
            detach_server: t.detach_server.as_ref()?,
            peer_closed: t.peer_closed.as_ref()?,
            now_ns: t.now_ns.as_ref()?,
            negotiate_lifetime_until: t.negotiate_lifetime_until.as_ref()?,
            shutdown_rpc: t.shutdown_rpc.as_ref()?,
            mount_log: t.mount_log.as_ref()?,
        })
    }

    unsafe fn slot(&self, i: c_int) -> *mut UxfsSbView {
        let base = (self.sb_table)() as *mut u8;
        unsafe { base.add(i as usize * (self.sb_stride)() as usize) as *mut UxfsSbView }
    }

    unsafe fn live(&self, p: *const UxfsSbView) -> u32 {
        use core::sync::atomic::{AtomicU32, Ordering};
        // SAFETY: `p` points at a live slot's `live_vnodes`.
        unsafe { (core::ptr::addr_of!((*p).live_vnodes) as *const AtomicU32).as_ref().unwrap().load(Ordering::Acquire) }
    }
}

/// Block-device ownership query: a native disk mount must not attach a second
/// cache to a device a live UXFS service already owns.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_ns_device_owned(ns: *const A20UxfsNs, dev: *mut c_void) -> c_int {
    let n = match unsafe { Ns::load(ns) } {
        Some(n) => n,
        None => return 0,
    };
    if dev.is_null() {
        return 0;
    }
    let count = (n.sb_count)();
    for i in 0..count {
        let s = unsafe { n.slot(i) };
        // SAFETY: slot pointer is within the asserted static table.
        let owned = unsafe { !(*s).ep.is_null() && (n.block_device)((*s).block_index) == dev };
        if owned {
            return 1;
        }
    }
    0
}

/// The caller's own registration for a disk number: several mounts may share a
/// block index, so only the slot whose server is the caller matches.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_ns_owned_block_dev(
    ns: *const A20UxfsNs,
    task: *mut c_void,
    block_index: c_int,
) -> *mut c_void {
    let n = match unsafe { Ns::load(ns) } {
        Some(n) => n,
        None => return ptr::null_mut(),
    };
    let count = (n.sb_count)();
    for i in 0..count {
        let s = unsafe { n.slot(i) };
        // SAFETY: slot pointer is within the asserted static table.
        if unsafe { !(*s).ep.is_null() && (*s).block_index == block_index && (*s).server == task } {
            return (n.block_device)(block_index);
        }
    }
    ptr::null_mut()
}

/// SERVE: attach a user filesystem service to `path`.  Owns the whole
/// transaction: path check, device/native-mount conflict, slot selection and
/// reservation under the mount-table lock, the server generation/lifetime
/// reset, root vnode creation, mount object construction and tree link, and
/// every unwind that must release exactly what was taken.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_ns_serve_mount(
    ns: *const A20UxfsNs,
    path: *const c_char,
    ep: *mut c_void,
    server: *mut c_void,
    block_index: c_int,
    serve_flags: u32,
) -> c_int {
    let n = match unsafe { Ns::load(ns) } {
        Some(n) => n,
        None => return -EINVAL,
    };
    if path.is_null() || ep.is_null() {
        return -EINVAL;
    }
    let is_dir = (n.path_is_dir)(path);
    if is_dir < 0 {
        return is_dir;
    }
    if is_dir == 0 {
        return -ENOTDIR;
    }

    let service_dev = (n.block_device)(block_index);
    if service_dev.is_null() {
        return -ENODEV;
    }
    if (n.device_has_native_mount)(service_dev) != 0 {
        return -EBUSY;
    }

    let count = (n.sb_count)();
    let flags = (n.lock_mounts)();
    for i in 0..count {
        let s = unsafe { n.slot(i) };
        if unsafe { !(*s).ep.is_null() } && (n.block_device)(unsafe { (*s).block_index }) == service_dev
        {
            (n.unlock_mounts)(flags);
            return -EBUSY;
        }
    }
    let mut slot: c_int = -1;
    for i in 0..count {
        let s = unsafe { n.slot(i) };
        if unsafe { (*s).ep.is_null() && (*s).slot_reserved == 0 && n.live(s) == 0 } {
            slot = i;
            break;
        }
    }
    if slot < 0 {
        (n.unlock_mounts)(flags);
        return -EMFILE;
    }
    let sb = unsafe { n.slot(slot) };

    unsafe {
        (*sb).slot_reserved = 1;
        (*sb).ep = ep;
        (*sb).server = (n.proc_get)(server);
    }
    if unsafe { (*sb).server.is_null() } {
        unsafe {
            (*sb).ep = ptr::null_mut();
            (*sb).slot_reserved = 0;
        }
        (n.unlock_mounts)(flags);
        return -ESRCH;
    }
    unsafe {
        (n.lifetime_reset)(core::ptr::addr_of_mut!((*sb).lifetime) as *mut c_void);
        (*sb).block_index = block_index;
        (*sb).next_req_id = 0;
        (*sb).shutdown_started = 0;
        (*sb).server_generation = (*sb).server_generation.wrapping_add(1);
        if (*sb).server_generation == 0 {
            (*sb).server_generation = 1;
        }
    }
    (n.slot_init_req_lock)(sb as *mut c_void);
    (n.unlock_mounts)(flags);

    /* No synchronous handshake: the caller of fs_serve is the service, so a
     * blocking INIT here would self-deadlock.  The eviction scan needs the
     * adapter's non-blocking reference reader before the first canonical vnode
     * is registered. */
    (n.identity_register_peek)(ptr::null_mut());

    let root = (n.iget)(sb as *mut c_void, UFS_ROOT_INO, VFS_FT_DIR, ROOT_DIR_MODE, 0, ptr::null_mut());
    if root.is_null() {
        let failed_server = unsafe { (*sb).server };
        let f2 = (n.lock_mounts)();
        unsafe {
            (*sb).ep = ptr::null_mut();
            (*sb).server = ptr::null_mut();
        }
        (n.unlock_mounts)(f2);
        (n.identity_clear_sb)(sb as *mut c_void);
        (n.proc_put)(failed_server);
        let f3 = (n.lock_mounts)();
        unsafe { (*sb).slot_reserved = 0; }
        (n.unlock_mounts)(f3);
        return -ENOMEM;
    }

    let mnt = (n.mount_alloc)();
    if mnt.is_null() {
        (n.vnode_put)(root); /* caller reference first: the entry reference keeps
                              * root alive until the index is cleared */
        (n.identity_clear_sb)(sb as *mut c_void);
        let failed_server = unsafe { (*sb).server };
        let f2 = (n.lock_mounts)();
        unsafe {
            (*sb).ep = ptr::null_mut();
            (*sb).server = ptr::null_mut();
        }
        (n.unlock_mounts)(f2);
        (n.proc_put)(failed_server);
        let f3 = (n.lock_mounts)();
        unsafe { (*sb).slot_reserved = 0; }
        (n.unlock_mounts)(f3);
        return -ENOMEM;
    }

    let mount_flags = if serve_flags & 1 != 0 { VFS_MOUNT_RDONLY } else { 0 };
    (n.mount_init)(mnt, path, mount_flags, root, sb as *mut c_void);
    (n.vnode_get)(root); /* mount holds a persistent ref, like other FS do */
    (n.mount_link_tree)(mnt);
    (n.dcache_invalidate_all)(ptr::null_mut());
    let f3 = (n.lock_mounts)();
    unsafe { (*sb).slot_reserved = 0; }
    (n.unlock_mounts)(f3);
    (n.mount_log)(path, block_index);
    0
}

/// UNMOUNT: negotiate the shutdown with the service and decide, from the
/// transport outcome and the peer state, whether to detach a dead peer, keep
/// the mount for a retry, re-open admission after a pre-teardown EBUSY, or
/// refuse a captured v1 peer whose durability cannot be proven.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_ns_unmount(ns: *const A20UxfsNs, root: *mut c_void) -> c_int {
    let n = match unsafe { Ns::load(ns) } {
        Some(n) => n,
        None => return 0,
    };
    if root.is_null() {
        return 0;
    }
    let vpriv = (n.vnode_fs_data)(root);
    if vpriv.is_null() {
        return 0;
    }
    /* uxfs_vpriv_t { uxfs_sb_t *sb; int is_dir; } -> sb is the first field. */
    let sb = unsafe { *(vpriv as *const *mut UxfsSbView) };
    if sb.is_null() || unsafe { (*sb).ep.is_null() } {
        return 0;
    }

    unsafe { (*sb).shutdown_started = 1; }
    let deadline_ns = (n.now_ns)().wrapping_add(5_000_000_000);
    /* The peer may perform backend sync/unmount and ACK while block IO
     * ownership is still valid.  A timeout/error keeps the mount and endpoint
     * attached so the caller can retry; a live service is never revoked
     * mid-flush. */
    let rc = (n.negotiate_lifetime_until)(sb as *mut c_void, deadline_ns);
    if rc < 0 {
        if (n.peer_closed)(unsafe { (*sb).ep }) != 0 {
            /* No task can issue further block I/O: drop the dead peer
             * registration, leaving on-disk dirty state for recovery. */
            (n.detach_server)(sb as *mut c_void);
            return 0;
        }
        return rc;
    }

    if unsafe { (*sb).lifetime.shutdown_mode } != 0 {
        let mut retry_safe: c_int = 0;
        let rc2 = (n.shutdown_rpc)(sb as *mut c_void, deadline_ns, &mut retry_safe);
        if rc2 < 0 {
            if (n.peer_closed)(unsafe { (*sb).ep }) != 0 {
                (n.detach_server)(sb as *mut c_void);
                return 0;
            }
            /* Only a matched, complete EBUSY reply that the service marked
             * retry-safe can reopen admission (its open-count preflight left
             * the backend untouched). */
            if rc2 == -EBUSY && retry_safe != 0 {
                unsafe { (*sb).shutdown_started = 0; }
            }
            return rc2;
        }
    } else {
        if (n.peer_closed)(unsafe { (*sb).ep }) != 0 {
            (n.detach_server)(sb as *mut c_void);
            return 0;
        }
        /* A captured v1 peer cannot prove its backend is durable before the
         * kernel revokes block IO: keep the mount registered. */
        unsafe { (*sb).shutdown_started = 0; }
        return -EOPNOTSUPP;
    }
    (n.detach_server)(sb as *mut c_void);
    0
}

/* ==================================================================== */
/* open-file lifecycle (open/close token state machine)                 */
/* ==================================================================== */

pub type ZeroArgPtrFn = extern "C" fn() -> *mut c_void;
pub type OpenTokenFn = extern "C" fn(*mut c_void, u64, u64, *mut u64) -> c_int;
pub type ConfirmModeFn = extern "C" fn(*mut c_void) -> c_int;
pub type FcInitFn = extern "C" fn(*mut c_void, *mut c_void, u64, u64, c_int, c_int, u64);
pub type VfInitFn =
    extern "C" fn(*mut c_void, *mut c_void, c_int, c_int, u64, *mut c_void);
pub type CloseOpFn =
    extern "C" fn(*mut c_void, u64, u64, *mut c_int, *mut c_int) -> c_int;
pub type SetCloseAttemptFn = extern "C" fn(*mut c_void, c_int, c_int, c_int);
pub type SetInt1Fn = extern "C" fn(*mut c_void, c_int);
pub type ConstIntFn = extern "C" fn(*const c_void) -> c_int;
pub type FcReleaseFn = extern "C" fn(*mut c_void, *mut c_void);
pub type TraceFn = extern "C" fn(*mut c_void, *mut c_void);

/// Narrow mechanical bridge for the UXFS open-file lifecycle.  Field order
/// mirrors `struct a20_uxfs_lifecycle` in `fs/uxfs_ops_state.h`.
#[repr(C)]
pub struct A20UxfsLifecycle {
    pub fc_alloc: Option<ZeroArgPtrFn>,
    pub vfile_alloc: Option<ZeroArgPtrFn>,
    pub vfile_free: Option<OnePtrFn>,
    pub kfree: Option<OnePtrFn>,
    pub vn_vpriv: Option<ProcGetFn>,
    pub vpriv_sb: Option<ProcGetFn>,
    pub vpriv_is_dir: Option<PeerClosedFn>,
    pub vn_ino: Option<VnInoFn>,
    pub vn_size: Option<VnSizeFn>,
    pub lifetime_open: Option<OpenTokenFn>,
    pub lifetime_confirm_mode: Option<ConfirmModeFn>,
    pub fc_init: Option<FcInitFn>,
    pub vf_init: Option<VfInitFn>,
    pub fc_sb: Option<ProcGetFn>,
    pub fc_ino: Option<FcInoFn>,
    pub fc_close_token: Option<FcOffFn>,
    pub fc_close_confirm_mode: Option<ConstIntFn>,
    pub fc_close_accepted: Option<ConstIntFn>,
    pub fc_set_close_accepted: Option<SetInt1Fn>,
    pub vf_vnode: Option<VfVnodeFn>,
    pub vf_priv: Option<ProcGetFn>,
    pub vf_set_close_attempt: Option<SetCloseAttemptFn>,
    pub fc_release: Option<FcReleaseFn>,
    pub pc_writeback: Option<PeerClosedFn>,
    pub lifetime_close: Option<CloseOpFn>,
    pub lifetime_close_confirm: Option<CloseOpFn>,
    pub trace: Option<TraceFn>,
}

struct Lc<'a> {
    fc_alloc: &'a ZeroArgPtrFn,
    vfile_alloc: &'a ZeroArgPtrFn,
    vfile_free: &'a OnePtrFn,
    kfree: &'a OnePtrFn,
    vn_vpriv: &'a ProcGetFn,
    vpriv_sb: &'a ProcGetFn,
    vpriv_is_dir: &'a PeerClosedFn,
    vn_ino: &'a VnInoFn,
    vn_size: &'a VnSizeFn,
    lifetime_open: &'a OpenTokenFn,
    lifetime_confirm_mode: &'a ConfirmModeFn,
    fc_init: &'a FcInitFn,
    vf_init: &'a VfInitFn,
    fc_sb: &'a ProcGetFn,
    fc_ino: &'a FcInoFn,
    fc_close_token: &'a FcOffFn,
    fc_close_confirm_mode: &'a ConstIntFn,
    fc_close_accepted: &'a ConstIntFn,
    fc_set_close_accepted: &'a SetInt1Fn,
    vf_vnode: &'a VfVnodeFn,
    vf_priv: &'a ProcGetFn,
    vf_set_close_attempt: &'a SetCloseAttemptFn,
    fc_release: &'a FcReleaseFn,
    pc_writeback: &'a PeerClosedFn,
    lifetime_close: &'a CloseOpFn,
    lifetime_close_confirm: &'a CloseOpFn,
    trace: &'a TraceFn,
}

impl<'a> Lc<'a> {
    unsafe fn load(t: *const A20UxfsLifecycle) -> Option<Lc<'a>> {
        if t.is_null() {
            return None;
        }
        // SAFETY: caller passes a live table for the duration of the call.
        let t = unsafe { &*t };
        Some(Lc {
            fc_alloc: t.fc_alloc.as_ref()?,
            vfile_alloc: t.vfile_alloc.as_ref()?,
            vfile_free: t.vfile_free.as_ref()?,
            kfree: t.kfree.as_ref()?,
            vn_vpriv: t.vn_vpriv.as_ref()?,
            vpriv_sb: t.vpriv_sb.as_ref()?,
            vpriv_is_dir: t.vpriv_is_dir.as_ref()?,
            vn_ino: t.vn_ino.as_ref()?,
            vn_size: t.vn_size.as_ref()?,
            lifetime_open: t.lifetime_open.as_ref()?,
            lifetime_confirm_mode: t.lifetime_confirm_mode.as_ref()?,
            fc_init: t.fc_init.as_ref()?,
            vf_init: t.vf_init.as_ref()?,
            fc_sb: t.fc_sb.as_ref()?,
            fc_ino: t.fc_ino.as_ref()?,
            fc_close_token: t.fc_close_token.as_ref()?,
            fc_close_confirm_mode: t.fc_close_confirm_mode.as_ref()?,
            fc_close_accepted: t.fc_close_accepted.as_ref()?,
            fc_set_close_accepted: t.fc_set_close_accepted.as_ref()?,
            vf_vnode: t.vf_vnode.as_ref()?,
            vf_priv: t.vf_priv.as_ref()?,
            vf_set_close_attempt: t.vf_set_close_attempt.as_ref()?,
            fc_release: t.fc_release.as_ref()?,
            pc_writeback: t.pc_writeback.as_ref()?,
            lifetime_close: t.lifetime_close.as_ref()?,
            lifetime_close_confirm: t.lifetime_close_confirm.as_ref()?,
            trace: t.trace.as_ref()?,
        })
    }
}

/// OPEN: reserve the local fctx and vfile before the server-side lifetime is
/// created (once the peer accepts OPEN, no allocation failure may lose the
/// only close token), then bind the decoded token/confirm mode and the
/// O_APPEND start offset into the fresh context.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_file_open(
    lc: *const A20UxfsLifecycle,
    vn: *mut c_void,
    flags: c_int,
) -> *mut c_void {
    let l = match unsafe { Lc::load(lc) } {
        Some(l) => l,
        None => return ptr::null_mut(),
    };
    let fc = (l.fc_alloc)();
    if fc.is_null() {
        return ptr::null_mut();
    }
    let vf = (l.vfile_alloc)();
    if vf.is_null() {
        (l.kfree)(fc);
        return ptr::null_mut();
    }

    let vpriv = (l.vn_vpriv)(vn);
    let sb = (l.vpriv_sb)(vpriv);
    let ino = (l.vn_ino)(vn);
    let mut token: u64 = 0;
    if (l.lifetime_open)(sb, ino, (flags & 3) as u64, &mut token) != 0 {
        (l.vfile_free)(vf);
        (l.kfree)(fc);
        return ptr::null_mut();
    }
    let confirm = token != 0 && (l.lifetime_confirm_mode)(sb) != 0;
    let is_dir = (l.vpriv_is_dir)(vpriv);
    let off = if flags & 0x400 != 0 {
        (l.vn_size)(vn)
    } else {
        0
    };
    (l.fc_init)(
        fc,
        sb,
        ino,
        token,
        confirm as c_int,
        is_dir,
        off,
    );
    (l.vf_init)(
        vf,
        vn,
        flags,
        confirm as c_int,
        off,
        fc,
    );
    vf
}

/// CLOSE: the close-token state machine.  A failed writeback retries later
/// without touching the server's OPEN count; a CLOSE/CONFIRM whose ACK is
/// ambiguous is only retryable when the request never reached the wire, and
/// only the fctx is retired after a confirmed ACK.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_file_close(
    lc: *const A20UxfsLifecycle,
    vf: *mut c_void,
) -> c_int {
    let l = match unsafe { Lc::load(lc) } {
        Some(l) => l,
        None => return 0,
    };
    if vf.is_null() {
        return 0;
    }
    let fc = (l.vf_priv)(vf);
    if fc.is_null() {
        return 0;
    }
    (l.trace)(vf, fc);
    (l.vf_set_close_attempt)(vf, 0, 0, 0);

    let vn = (l.vf_vnode)(vf);
    if !vn.is_null() {
        let flush_rc = (l.pc_writeback)(vn);
        if flush_rc < 0 {
            (l.vf_set_close_attempt)(vf, 0, 0, 1);
            return flush_rc;
        }
    }

    let sb = (l.fc_sb)(fc);
    let ino = (l.fc_ino)(fc);
    let token = (l.fc_close_token)(fc) as u64;

    let mut acked: c_int = 0;
    let mut sent: c_int = 0;
    let mut close_rc = 0;
    let final_acked;
    if (l.fc_close_accepted)(fc) == 0 {
        close_rc = (l.lifetime_close)(sb, ino, token, &mut acked, &mut sent);
        (l.vf_set_close_attempt)(vf, if sent != 0 { 1 } else { 0 }, 0, 0);
        if close_rc != 0 || acked == 0 {
            (l.vf_set_close_attempt)(vf, if sent != 0 { 1 } else { 0 }, 0, if sent == 0 { 1 } else { 0 });
            return if close_rc < 0 { close_rc } else { -EIO };
        }
        if (l.fc_close_confirm_mode)(fc) != 0 {
            (l.fc_set_close_accepted)(fc, 1);
        }
    }
    if (l.fc_close_confirm_mode)(fc) != 0 && (l.fc_close_accepted)(fc) != 0 {
        acked = 0;
        sent = 0;
        close_rc = (l.lifetime_close_confirm)(sb, ino, token, &mut acked, &mut sent);
        (l.vf_set_close_attempt)(
            vf,
            if sent != 0 { 1 } else { 0 },
            if acked != 0 && close_rc == 0 { 1 } else { 0 },
            0,
        );
        if close_rc != 0 || acked == 0 {
            (l.vf_set_close_attempt)(vf, if sent != 0 { 1 } else { 0 }, 0, if sent == 0 { 1 } else { 0 });
            return if close_rc < 0 { close_rc } else { -EIO };
        }
        final_acked = 1;
    } else {
        (l.vf_set_close_attempt)(vf, if sent != 0 { 1 } else { 0 }, 1, 0);
        final_acked = 1;
    }
    if close_rc != 0 || final_acked == 0 {
        if sent == 0 {
            (l.vf_set_close_attempt)(vf, 0, 0, 1);
        }
        return if close_rc < 0 { close_rc } else { -EIO };
    }
    (l.fc_release)(vf, fc);
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::os::raw::c_char;
    thread_local! {
        static WORLD: RefCell<World> = RefCell::new(World::new());
    }

    #[derive(Default, Clone)]
    struct Req {
        opcode: u32,
        ino: u64,
        arg0: u64,
        arg1: u64,
        name: Vec<u8>,
        payload: Vec<u8>,
    }

    #[derive(Default)]
    struct World {
        next_req_id: u32,
        /* scripted reply */
        reply_status: c_int,
        reply_out0: u64,
        reply_out1: u64,
        reply_out2: u64,
        /* observed request */
        sent: Vec<Req>,
        /* vnode model */
        ino: u64,
        size: u64,
        parent: usize, /* 0 = none */
        /* side-effect log */
        log: Vec<String>,
        /* lifetime */
        lt_negotiate_rc: c_int,
        lt_is_v2: bool,
        /* iget result: ino recorded, or 0 for allocation failure */
        iget_ino: u64,
        iget_fail: bool,
        /* data-plane (fops) model */
        reply_payload: Vec<u8>,
        vf_has_vnode: bool,
        vf_offset: u64,
    }

    impl World {
        fn new() -> Self {
            Self {
                next_req_id: 0,
                reply_status: 0,
                reply_out0: 0,
                reply_out1: 0,
                reply_out2: 0,
                sent: Vec::new(),
                ino: 7,
                size: 0,
                parent: 0,
                log: Vec::new(),
                lt_negotiate_rc: 0,
                lt_is_v2: true,
                iget_ino: 0,
                iget_fail: false,
                reply_payload: Vec::new(),
                vf_has_vnode: true,
                vf_offset: 0,
            }
        }
    }

    const CTX: *mut c_void = 0x1000 as *mut c_void;
    const DIR: *mut c_void = 0x2000 as *mut c_void;
    const VN: *mut c_void = 0x3000 as *mut c_void;
    const PARENT: *mut c_void = 0x4000 as *mut c_void;

    fn name_ptr(b: &[u8]) -> *const c_char {
        // Leak a NUL-terminated copy; fine for a host test.
        let mut v = b.to_vec();
        v.push(0);
        let p = v.as_ptr() as *const c_char;
        std::mem::forget(v);
        p
    }

    extern "C" fn req_init(
        _ctx: *mut c_void,
        req: *mut UfsReqHdr,
        opcode: u32,
        ino: u64,
        arg0: u64,
        arg1: u64,
        name: *const c_char,
        payload_len: u32,
    ) {
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            b.next_req_id += 1;
            unsafe {
                (*req).magic = UFS_REQ_MAGIC;
                (*req).version = UFS_PROTO_VERSION;
                (*req).opcode = opcode;
                (*req).req_id = b.next_req_id;
                (*req).ino = ino;
                (*req).arg0 = arg0;
                (*req).arg1 = arg1;
                (*req).name_len = c_str_len(name) as u32;
                (*req).payload_len = payload_len;
            }
        });
    }

    extern "C" fn rpc(
        _ctx: *mut c_void,
        req: *const UfsReqHdr,
        name: *const c_char,
        payload: *const c_void,
        resp: *mut UfsRespHdr,
        _resp_payload: *mut c_void,
        _resp_cap: u32,
        _resp_payload_len: *mut u32,
    ) -> c_int {
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            let nl = c_str_len(name);
            let mut nv = vec![0u8; nl];
            for i in 0..nl {
                nv[i] = unsafe { *name.add(i) } as u8;
            }
            let mut pv = Vec::new();
            let pl = unsafe { (*req).payload_len } as usize;
            if pl > 0 && !payload.is_null() {
                for i in 0..pl {
                    pv.push(unsafe { *(payload as *const u8).add(i) });
                }
            }
            b.sent.push(Req {
                opcode: unsafe { (*req).opcode },
                ino: unsafe { (*req).ino },
                arg0: unsafe { (*req).arg0 },
                arg1: unsafe { (*req).arg1 },
                name: nv,
                payload: pv,
            });
            let status = b.reply_status;
            let (o0, o1, o2) = (b.reply_out0, b.reply_out1, b.reply_out2);
            unsafe {
                (*resp).magic = 0x5546_5350;
                (*resp).opcode = (*req).opcode;
                (*resp).req_id = (*req).req_id;
                (*resp).status = status;
                (*resp).out0 = o0;
                (*resp).out1 = o1;
                (*resp).out2 = o2;
                (*resp).payload_len = 0;
                (*resp)._pad = 0;
            }
            let pl = b.reply_payload.len();
            if pl > 0 && !_resp_payload.is_null() && _resp_cap as usize >= pl {
                for i in 0..pl {
                    unsafe { *(_resp_payload as *mut u8).add(i) = b.reply_payload[i] };
                }
                if !_resp_payload_len.is_null() {
                    unsafe { *_resp_payload_len = pl as u32 };
                }
                unsafe { (*resp).payload_len = pl as u32 };
            }
            status
        })
    }

    extern "C" fn vpriv_sb(_vn: *mut c_void) -> *mut c_void {
        CTX
    }
    extern "C" fn vn_ino(_vn: *const c_void) -> u64 {
        WORLD.with(|w| w.borrow().ino)
    }
    extern "C" fn vn_size(_vn: *const c_void) -> u64 {
        WORLD.with(|w| w.borrow().size)
    }
    extern "C" fn vn_set_size(_vn: *mut c_void, sz: u64) {
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            b.size = sz;
            b.log.push(format!("set_size={}", sz));
        });
    }
    extern "C" fn vn_parent(_vn: *mut c_void) -> *mut c_void {
        WORLD.with(|w| {
            if w.borrow().parent != 0 {
                PARENT
            } else {
                ptr::null_mut()
            }
        })
    }
    extern "C" fn iget(
        _ctx: *mut c_void,
        ino: u64,
        _t: c_int,
        _m: u32,
        _s: u64,
        _p: *mut c_void,
    ) -> *mut c_void {
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            if b.iget_fail {
                return ptr::null_mut();
            }
            b.log.push(format!("iget={}", ino));
            b.iget_ino = ino;
            VN
        })
    }
    extern "C" fn identity_forget(_ctx: *mut c_void, ino: u64) {
        WORLD.with(|w| w.borrow_mut().log.push(format!("forget={}", ino)));
    }
    extern "C" fn vnode_get(_vn: *mut c_void) {
        WORLD.with(|w| w.borrow_mut().log.push("get".to_string()));
    }
    extern "C" fn page_cache_truncate(_vn: *mut c_void, sz: u64) {
        WORLD.with(|w| w.borrow_mut().log.push(format!("pc_trunc={}", sz)));
    }
    extern "C" fn negotiate_lifetime(_ctx: *mut c_void) -> c_int {
        WORLD.with(|w| w.borrow().lt_negotiate_rc)
    }
    extern "C" fn lifetime_is_v2(_ctx: *mut c_void) -> c_int {
        WORLD.with(|w| w.borrow().lt_is_v2 as c_int)
    }

    fn table() -> A20UxfsOps {
        A20UxfsOps {
            req_init: Some(req_init),
            rpc: Some(rpc),
            vpriv_sb: Some(vpriv_sb),
            vn_ino: Some(vn_ino),
            vn_size: Some(vn_size),
            vn_set_size: Some(vn_set_size),
            vn_parent: Some(vn_parent),
            iget: Some(iget),
            identity_forget: Some(identity_forget),
            vnode_get: Some(vnode_get),
            page_cache_truncate: Some(page_cache_truncate),
            negotiate_lifetime: Some(negotiate_lifetime),
            lifetime_is_v2: Some(lifetime_is_v2),
        }
    }

    thread_local! {
        static FC: RefCell<UxfsFctx> = RefCell::new(fctx_new());
    }

    fn fctx_new() -> UxfsFctx {
        UxfsFctx {
            sb: CTX,
            ino: 7,
            close_token: 0,
            close_confirm_mode: 0,
            close_accepted: 0,
            is_dir: 0,
            off: 0,
        }
    }

    fn fc_reset() {
        FC.with(|f| *f.borrow_mut() = fctx_new());
    }

    fn fc_ptr() -> *mut c_void {
        FC.with(|f| f.as_ptr() as *mut c_void)
    }

    extern "C" fn fc_sb(_fc: *mut c_void) -> *mut c_void {
        CTX
    }
    extern "C" fn fc_ino(_fc: *const c_void) -> u64 {
        FC.with(|f| f.borrow().ino)
    }
    extern "C" fn fc_off(_fc: *const c_void) -> usize {
        FC.with(|f| f.borrow().off)
    }
    extern "C" fn fc_set_off(_fc: *mut c_void, off: usize) {
        FC.with(|f| f.borrow_mut().off = off);
    }
    extern "C" fn fc_is_dir(_fc: *const c_void) -> c_int {
        FC.with(|f| f.borrow().is_dir)
    }
    extern "C" fn vf_vnode(_vf: *mut c_void) -> *mut c_void {
        WORLD.with(|w| if w.borrow().vf_has_vnode { VN } else { ptr::null_mut() })
    }
    extern "C" fn vf_set_offset(_vf: *mut c_void, off: usize) {
        WORLD.with(|w| w.borrow_mut().vf_offset = off as u64);
    }

    fn ftable() -> A20UxfsFops {
        A20UxfsFops {
            req_init: Some(req_init),
            rpc: Some(rpc),
            fc_sb: Some(fc_sb),
            fc_ino: Some(fc_ino),
            fc_off: Some(fc_off),
            fc_set_off: Some(fc_set_off),
            fc_is_dir: Some(fc_is_dir),
            vf_vnode: Some(vf_vnode),
            vf_set_offset: Some(vf_set_offset),
            vn_size: Some(vn_size),
            vn_set_size: Some(vn_set_size),
        }
    }

    fn reset() {
        WORLD.with(|w| *w.borrow_mut() = World::new());
    }

    #[test]
    fn lookup_dot_returns_dir() {
        reset();
        let t = table();
        let mut out: *mut c_void = ptr::null_mut();
        let rc = unsafe { a20_uxfs_op_lookup(&t, DIR, name_ptr(b"."), &mut out) };
        assert_eq!(rc, 0);
        assert_eq!(out, DIR);
        assert!(WORLD.with(|w| w.borrow().sent.is_empty()));
        assert!(WORLD.with(|w| w.borrow().log.iter().any(|l| l == "get")));
    }

    #[test]
    fn lookup_dotdot_uses_parent() {
        reset();
        WORLD.with(|w| w.borrow_mut().parent = 1);
        let t = table();
        let mut out: *mut c_void = ptr::null_mut();
        let rc = unsafe { a20_uxfs_op_lookup(&t, DIR, name_ptr(b".."), &mut out) };
        assert_eq!(rc, 0);
        assert_eq!(out, PARENT);
        assert!(WORLD.with(|w| w.borrow().sent.is_empty()));
    }

    #[test]
    fn lookup_wire_decodes_reply() {
        reset();
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            b.reply_out0 = 42; // ino
            b.reply_out1 = UFS_FT_DIR; // type
            b.reply_out2 = (9u64 << 32) | 128; // mode | size
        });
        let t = table();
        let mut out: *mut c_void = ptr::null_mut();
        let rc = unsafe { a20_uxfs_op_lookup(&t, DIR, name_ptr(b"foo"), &mut out) };
        assert_eq!(rc, 0);
        assert_eq!(out, VN);
        let s = WORLD.with(|w| w.borrow().sent[0].clone());
        assert_eq!(s.opcode, UFS_OP_LOOKUP);
        assert_eq!(s.ino, 7);
        assert_eq!(s.name, b"foo".to_vec());
        assert_eq!(WORLD.with(|w| w.borrow().iget_ino), 42);
    }

    #[test]
    fn lookup_iget_failure_is_enomem() {
        reset();
        WORLD.with(|w| {
            w.borrow_mut().reply_out0 = 42;
            w.borrow_mut().iget_fail = true;
        });
        let t = table();
        let mut out: *mut c_void = ptr::null_mut();
        let rc = unsafe { a20_uxfs_op_lookup(&t, DIR, name_ptr(b"foo"), &mut out) };
        assert_eq!(rc, -ENOMEM);
    }

    #[test]
    fn stat_fills_kstat_and_refreshes_size() {
        reset();
        WORLD.with(|w| {
            w.borrow_mut().reply_out0 = 2048;
            w.borrow_mut().reply_out1 = (0o40755u64) << 32;
        });
        let t = table();
        let mut st = Kstat {
            st_dev: 1,
            st_ino: 1,
            st_mode: 1,
            st_nlink: 1,
            st_uid: 1,
            st_gid: 1,
            st_rdev: 1,
            st_size: 1,
            st_blksize: 1,
            st_blocks: 1,
            st_atime: 1,
            st_atime_nsec: 1,
            st_mtime: 1,
            st_mtime_nsec: 1,
            st_ctime: 1,
            st_ctime_nsec: 1,
        };
        let rc = unsafe { a20_uxfs_op_stat(&t, VN, &mut st as *mut Kstat as *mut c_void) };
        assert_eq!(rc, 0);
        assert_eq!(st.st_ino, 7);
        assert_eq!(st.st_size, 2048);
        assert_eq!(st.st_mode, 0o40755);
        assert_eq!(st.st_nlink, 1);
        assert_eq!(st.st_blocks, (2048 + 511) / 512);
        assert_eq!(WORLD.with(|w| w.borrow().size), 2048);
    }

    #[test]
    fn statfs_converts_bytes_to_blocks() {
        reset();
        WORLD.with(|w| {
            w.borrow_mut().reply_out0 = 5120;
            w.borrow_mut().reply_out1 = 2560;
        });
        let t = table();
        let mut st = Kstatfs {
            f_type: 0,
            f_bsize: 0,
            f_blocks: 0,
            f_bfree: 0,
            f_bavail: 0,
            f_files: 0,
            f_ffree: 0,
            f_fsid: 0,
            f_namelen: 0,
            f_frsize: 0,
            f_flags: 0,
        };
        let rc = unsafe { a20_uxfs_op_statfs(&t, VN, &mut st as *mut Kstatfs as *mut c_void) };
        assert_eq!(rc, 0);
        assert_eq!(st.f_blocks, 10);
        assert_eq!(st.f_bfree, 5);
        assert_eq!(st.f_bavail, 5);
        assert_eq!(st.f_type, STATFS_TYPE);
    }

    #[test]
    fn create_forgets_then_igets() {
        reset();
        WORLD.with(|w| w.borrow_mut().reply_out0 = 99);
        let t = table();
        let mut out: *mut c_void = ptr::null_mut();
        let rc =
            unsafe { a20_uxfs_op_create(&t, DIR, name_ptr(b"n"), 0o644, &mut out) };
        assert_eq!(rc, 0);
        let log = WORLD.with(|w| w.borrow().log.clone());
        let fi = log.iter().position(|l| l == "forget=99").unwrap();
        let ig = log.iter().position(|l| l == "iget=99").unwrap();
        assert!(fi < ig, "forget must precede iget");
        let s = WORLD.with(|w| w.borrow().sent[0].clone());
        assert_eq!(s.opcode, UFS_OP_CREATE);
        assert_eq!(s.arg0, 0o644);
    }

    #[test]
    fn mkdir_forgets_new_ino_on_success_only() {
        reset();
        WORLD.with(|w| {
            w.borrow_mut().reply_out0 = 55;
        });
        let t = table();
        let rc = unsafe { a20_uxfs_op_mkdir(&t, DIR, name_ptr(b"d"), 0o755) };
        assert_eq!(rc, 0);
        assert!(WORLD.with(|w| w.borrow().log.iter().any(|l| l == "forget=55")));

        reset();
        WORLD.with(|w| {
            w.borrow_mut().reply_status = -5;
            w.borrow_mut().reply_out0 = 55;
        });
        let rc = unsafe { a20_uxfs_op_mkdir(&t, DIR, name_ptr(b"d"), 0o755) };
        assert_eq!(rc, -5);
        assert!(!WORLD.with(|w| w.borrow().log.iter().any(|l| l.starts_with("forget="))));
    }

    #[test]
    fn truncate_sets_size_and_cache() {
        reset();
        let t = table();
        let rc = unsafe { a20_uxfs_op_truncate(&t, VN, 100) };
        assert_eq!(rc, 0);
        let log = WORLD.with(|w| w.borrow().log.clone());
        assert!(log.iter().any(|l| l == "pc_trunc=100"));
        assert!(log.iter().any(|l| l == "set_size=100"));
        assert_eq!(WORLD.with(|w| w.borrow().sent[0].arg0), 100);
    }

    #[test]
    fn writepage_clamps_to_file_size() {
        reset();
        WORLD.with(|w| w.borrow_mut().size = 5000);
        let t = table();
        let data = [1u8; 4096];
        /* index 1 => off 4096, size 5000 => len clamped to 904 */
        let rc = unsafe {
            a20_uxfs_op_writepage(&t, VN, 1, data.as_ptr() as *const c_void, 4096)
        };
        assert_eq!(rc, 0);
        let s = WORLD.with(|w| w.borrow().sent[0].clone());
        assert_eq!(s.opcode, UFS_OP_WRITE);
        assert_eq!(s.arg0, 4096);
        assert_eq!(s.arg1, 904);
        assert_eq!(s.payload.len(), 904);
    }

    #[test]
    fn writepage_beyond_size_is_noop() {
        reset();
        WORLD.with(|w| w.borrow_mut().size = 100);
        let t = table();
        let data = [1u8; 16];
        let rc = unsafe {
            a20_uxfs_op_writepage(&t, VN, 1, data.as_ptr() as *const c_void, 16)
        };
        assert_eq!(rc, 0);
        assert!(WORLD.with(|w| w.borrow().sent.is_empty()));
    }

    #[test]
    fn readpage_zero_fills_short_read() {
        reset();
        WORLD.with(|w| w.borrow_mut().reply_out0 = 2);
        let t = table();
        let mut data = [7u8; 8];
        let rc = unsafe {
            a20_uxfs_op_readpage(&t, VN, 0, data.as_mut_ptr() as *mut c_void, 8)
        };
        assert_eq!(rc, 0);
        assert_eq!(data[0], 7);
        assert_eq!(data[1], 7);
        for i in 2..8 {
            assert_eq!(data[i], 0);
        }
    }

    #[test]
    fn rename_sends_target_as_payload() {
        reset();
        let t = table();
        let rc = unsafe {
            a20_uxfs_op_rename(&t, DIR, name_ptr(b"old"), DIR, name_ptr(b"newname"))
        };
        assert_eq!(rc, 0);
        let s = WORLD.with(|w| w.borrow().sent[0].clone());
        assert_eq!(s.opcode, UFS_OP_RENAME);
        assert_eq!(s.arg0, 7); // new_dir ino
        assert_eq!(s.arg1, 7); // len("newname")
        assert_eq!(s.name, b"old".to_vec());
        assert_eq!(s.payload, b"newname".to_vec());
    }

    #[test]
    fn link_gated_on_v2_lifetime() {
        reset();
        WORLD.with(|w| w.borrow_mut().lt_is_v2 = false);
        let t = table();
        let rc = unsafe { a20_uxfs_op_link(&t, DIR, name_ptr(b"l"), VN) };
        assert_eq!(rc, -ENOSYS);
        assert!(WORLD.with(|w| w.borrow().sent.is_empty()));

        reset();
        let rc = unsafe { a20_uxfs_op_link(&t, DIR, name_ptr(b"l"), VN) };
        assert_eq!(rc, 0);
        let s = WORLD.with(|w| w.borrow().sent[0].clone());
        assert_eq!(s.opcode, UFS_OP_LINK);
        assert_eq!(s.arg0, 7); // target ino
    }

    #[test]
    fn link_cross_sb_is_exdev() {
        reset();
        let t = table();
        let rc = unsafe { a20_uxfs_op_link(&t, DIR, name_ptr(b"l"), ptr::null_mut()) };
        assert_eq!(rc, -EXDEV);
    }

    #[test]
    fn unlink_rmdir_sync_are_plain_wire() {
        reset();
        let t = table();
        assert_eq!(unsafe { a20_uxfs_op_unlink(&t, DIR, name_ptr(b"u")) }, 0);
        assert_eq!(unsafe { a20_uxfs_op_rmdir(&t, DIR, name_ptr(b"r")) }, 0);
        assert_eq!(unsafe { a20_uxfs_op_sync_vnode(&t, VN) }, 0);
        let ops = WORLD.with(|w| w.borrow().sent.iter().map(|r| r.opcode).collect::<Vec<_>>());
        assert_eq!(ops, vec![UFS_OP_UNLINK, UFS_OP_RMDIR, UFS_OP_SYNC]);
    }

    /* ------------------------ data plane (fops) ------------------------ */

    fn dp_reset() {
        reset();
        fc_reset();
    }

    #[test]
    fn file_read_advances_cookie_and_syncs_vfile() {
        dp_reset();
        let f = ftable();
        let mut buf = [0xAAu8; 16];
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            b.reply_out0 = 10;
            b.reply_payload = vec![b'A'; 10];
        });
        let rc = unsafe {
            a20_uxfs_file_read(&f, fc_ptr(), VN, buf.as_mut_ptr() as *mut c_char, 16)
        };
        assert_eq!(rc, 10);
        let s = WORLD.with(|w| w.borrow().sent[0].clone());
        assert_eq!(s.opcode, UFS_OP_READ);
        assert_eq!(s.arg0, 0); /* off */
        assert_eq!(s.arg1, 16); /* count */
        assert_eq!(FC.with(|f| f.borrow().off), 10);
        assert_eq!(WORLD.with(|w| w.borrow().vf_offset), 10);
        assert_eq!(&buf[..10], b"AAAAAAAAAA");
        assert_eq!(&buf[10..], &[0u8; 6]); /* short read zero-fills the tail */
    }

    #[test]
    fn file_read_short_zero_fills_tail() {
        dp_reset();
        let f = ftable();
        let mut buf = [0xAAu8; 8];
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            b.reply_out0 = 3;
            b.reply_payload = vec![b'A'; 3];
        });
        let rc = unsafe {
            a20_uxfs_file_read(&f, fc_ptr(), VN, buf.as_mut_ptr() as *mut c_char, 8)
        };
        assert_eq!(rc, 3);
        assert_eq!(&buf[..3], b"AAA");
        assert_eq!(&buf[3..], &[0u8; 5]); /* zero fill past the reply */
        assert_eq!(FC.with(|f| f.borrow().off), 3);
        assert_eq!(WORLD.with(|w| w.borrow().vf_offset), 3);
    }

    #[test]
    fn file_read_dir_is_eisdir() {
        dp_reset();
        let f = ftable();
        FC.with(|f| f.borrow_mut().is_dir = 1);
        let mut buf = [0u8; 4];
        let rc = unsafe {
            a20_uxfs_file_read(&f, fc_ptr(), VN, buf.as_mut_ptr() as *mut c_char, 4)
        };
        assert_eq!(rc, -EISDIR);
        assert!(WORLD.with(|w| w.borrow().sent.is_empty()));
    }

    #[test]
    fn file_write_grows_vnode_size() {
        dp_reset();
        let f = ftable();
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            b.reply_out0 = 5;
            b.size = 4; /* vnode currently smaller than the new end */
        });
        FC.with(|f| f.borrow_mut().off = 8);
        let data = b"hello";
        let rc = unsafe {
            a20_uxfs_file_write(&f, fc_ptr(), VN, data.as_ptr() as *const c_char, 5)
        };
        assert_eq!(rc, 5);
        let s = WORLD.with(|w| w.borrow().sent[0].clone());
        assert_eq!(s.opcode, UFS_OP_WRITE);
        assert_eq!(s.arg0, 8);
        assert_eq!(s.payload, b"hello");
        assert_eq!(FC.with(|f| f.borrow().off), 13);
        assert_eq!(WORLD.with(|w| w.borrow().vf_offset), 13);
        assert_eq!(WORLD.with(|w| w.borrow().size), 13); /* grown */
        assert!(WORLD.with(|w| w.borrow().log.iter().any(|l| l == "set_size=13")));
    }

    #[test]
    fn file_lseek_set_cur_end() {
        dp_reset();
        let f = ftable();
        WORLD.with(|w| w.borrow_mut().size = 100);
        let p = fc_ptr();
        let vf = VN;
        assert_eq!(unsafe { a20_uxfs_file_lseek(&f, p, vf, 40, SEEK_SET) }, 40);
        assert_eq!(unsafe { a20_uxfs_file_lseek(&f, p, vf, 5, SEEK_CUR) }, 45);
        assert_eq!(unsafe { a20_uxfs_file_lseek(&f, p, vf, -100, SEEK_END) }, 0);
        assert_eq!(unsafe { a20_uxfs_file_lseek(&f, p, vf, 0, 9) }, -EINVAL as i64);
        assert_eq!(unsafe { a20_uxfs_file_lseek(&f, p, vf, -1, SEEK_SET) }, -EINVAL as i64);
        assert_eq!(FC.with(|f| f.borrow().off), 0);
        assert_eq!(WORLD.with(|w| w.borrow().vf_offset), 0);
    }

    #[test]
    fn file_lseek_dir_has_no_limit() {
        dp_reset();
        let f = ftable();
        FC.with(|f| f.borrow_mut().is_dir = 1);
        /* ~0ul sentinel: SEEK_END lands one before the offset (C parity). */
        assert_eq!(unsafe { a20_uxfs_file_lseek(&f, fc_ptr(), VN, 10, SEEK_END) }, 9);
    }

    fn push_dirent(payload: &mut Vec<u8>, ino: u64, ty: u8, name: &[u8]) {
        payload.extend_from_slice(&ino.to_le_bytes());
        payload.push(ty);
        payload.push(name.len() as u8);
        payload.extend_from_slice(name);
    }

    fn read_dirent(buf: &[u8], at: usize) -> (u64, i64, u16, u8, Vec<u8>) {
        let ino = u64::from_le_bytes(buf[at..at + 8].try_into().unwrap());
        let off = i64::from_le_bytes(buf[at + 8..at + 16].try_into().unwrap());
        let reclen = u16::from_le_bytes(buf[at + 16..at + 18].try_into().unwrap());
        let ty = buf[at + 18];
        let scan_end = core::cmp::min(at + 19 + 64, buf.len());
        let name_end = buf[at + 19..scan_end]
            .iter()
            .position(|&c| c == 0)
            .unwrap()
            + at
            + 19;
        (ino, off, reclen, ty, buf[at + 19..name_end].to_vec())
    }

    #[test]
    fn readdir_decodes_entries_and_advances_cookie() {
        dp_reset();
        let f = ftable();
        FC.with(|f| f.borrow_mut().is_dir = 1);
        FC.with(|f| f.borrow_mut().off = 3); /* resume cookie */
        let mut pl = Vec::new();
        push_dirent(&mut pl, 11, UFS_FT_DIR as u8, b"dir");
        push_dirent(&mut pl, 12, UFS_FT_SYMLINK as u8, b"lnk");
        push_dirent(&mut pl, 13, 1, b"reg");
        WORLD.with(|w| w.borrow_mut().reply_payload = pl);

        let mut out = [0u8; 128];
        let mut scratch = [0u8; 4096];
        let rc = unsafe {
            a20_uxfs_file_readdir(&f, fc_ptr(), VN, out.as_mut_ptr() as *mut c_void, 128,
                                  scratch.as_mut_ptr(), 4096)
        };
        assert!(rc > 0);
        let total = rc as usize;
        let (ino, off, reclen, ty, name) = read_dirent(&out, 0);
        assert_eq!((ino, off, ty), (11, 4, DT_DIR));
        assert_eq!(name, b"dir");
        assert_eq!(reclen as usize, 24); /* align8(20+3+1) */
        let second = reclen as usize;
        let (ino2, off2, _, ty2, name2) = read_dirent(&out, second);
        assert_eq!((ino2, off2, ty2), (12, 5, DT_LNK));
        assert_eq!(name2, b"lnk");
        let third = second + read_dirent(&out, second).2 as usize;
        let (ino3, off3, _, ty3, name3) = read_dirent(&out, third);
        assert_eq!((ino3, off3, ty3), (13, 6, DT_REG));
        assert_eq!(name3, b"reg");
        assert_eq!(total, third + read_dirent(&out, third).2 as usize);
        assert_eq!(FC.with(|f| f.borrow().off), 6);
        assert_eq!(WORLD.with(|w| w.borrow().vf_offset), 6);
    }

    #[test]
    fn readdir_buffer_full_keeps_resume_cookie() {
        dp_reset();
        let f = ftable();
        FC.with(|f| f.borrow_mut().is_dir = 1);
        FC.with(|f| f.borrow_mut().off = 100);
        let mut pl = Vec::new();
        push_dirent(&mut pl, 11, UFS_FT_DIR as u8, b"aaaa");
        push_dirent(&mut pl, 12, UFS_FT_DIR as u8, b"bbbb");
        WORLD.with(|w| w.borrow_mut().reply_payload = pl);
        let mut out = [0u8; 32]; /* only room for the first entry */
        let mut scratch = [0u8; 4096];
        let rc = unsafe {
            a20_uxfs_file_readdir(&f, fc_ptr(), VN, out.as_mut_ptr() as *mut c_void, 32,
                                  scratch.as_mut_ptr(), 4096)
        };
        assert_eq!(rc as usize, 32);
        /* consumed exactly one entry -> resume at 101 */
        assert_eq!(FC.with(|f| f.borrow().off), 101);
    }

    #[test]
    fn readdir_truncated_tail_stops() {
        dp_reset();
        let f = ftable();
        FC.with(|f| f.borrow_mut().is_dir = 1);
        let mut pl = Vec::new();
        push_dirent(&mut pl, 11, UFS_FT_DIR as u8, b"ok");
        /* a torn trailing record: header claims 9 name bytes, only 2 present */
        pl.extend_from_slice(&12u64.to_le_bytes());
        pl.push(1);
        pl.push(9);
        pl.extend_from_slice(b"ab");
        WORLD.with(|w| w.borrow_mut().reply_payload = pl);
        let mut out = [0u8; 128];
        let mut scratch = [0u8; 4096];
        let rc = unsafe {
            a20_uxfs_file_readdir(&f, fc_ptr(), VN, out.as_mut_ptr() as *mut c_void, 128,
                                  scratch.as_mut_ptr(), 4096)
        };
        assert_eq!(rc as usize, 24); /* only the complete first entry */
        assert_eq!(FC.with(|f| f.borrow().off), 1);
    }

    #[test]
    fn readdir_non_dir_is_enotdir() {
        dp_reset();
        let f = ftable();
        let mut out = [0u8; 32];
        let mut scratch = [0u8; 4096];
        let rc = unsafe {
            a20_uxfs_file_readdir(&f, fc_ptr(), VN, out.as_mut_ptr() as *mut c_void, 32,
                                  scratch.as_mut_ptr(), 4096)
        };
        assert_eq!(rc, -ENOTDIR);
    }

    /* --------------------- namespace (mount/serve) --------------------- */

    const DEV: *mut c_void = 0x6000 as *mut c_void;
    const ROOT_VN: *mut c_void = 0x7000 as *mut c_void;
    const MNT: *mut c_void = 0x8000 as *mut c_void;
    const SRV: *mut c_void = 0x9000 as *mut c_void;
    const EP: *mut c_void = 0xA000 as *mut c_void;
    const NS_SLOTS: usize = 4;

    fn sb_new() -> UxfsSbView {
        UxfsSbView {
            ep: ptr::null_mut(),
            server: ptr::null_mut(),
            block_index: 0,
            next_req_id: 7,
            lifetime: UxfsLifetimeView {
                mode: 0,
                shutdown_mode: 0,
                close_token_mode: 0,
                close_confirm_mode: 0,
                generation: 0,
                server: ptr::null_mut(),
            },
            shutdown_started: 0,
            slot_reserved: 0,
            live_vnodes: 0,
            server_generation: 3,
        }
    }

    #[derive(Default)]
    struct NWorld {
        sbs: Vec<UxfsSbView>,
        path_kind: c_int, /* 1 dir, 0 non-dir, -ENOENT */
        dev_for_block: Vec<(c_int, *mut c_void)>,
        native: c_int,
        proc_get_ok: bool,
        iget_fail: bool,
        mount_alloc_ok: bool,
        neg_rc: c_int,
        shutdown_rc: c_int,
        shutdown_mode: u32,
        retry_safe: c_int,
        peer_closed: c_int,
        log: Vec<String>,
        vpriv_sb: *mut c_void,
    }

    thread_local! {
        static NW: RefCell<NWorld> = RefCell::new(NWorld::default());
    }

    fn ns_reset() {
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.sbs = (0..NS_SLOTS).map(|_| sb_new()).collect();
            b.path_kind = 1;
            b.dev_for_block = vec![(3, DEV)];
            b.native = 0;
            b.proc_get_ok = true;
            b.iget_fail = false;
            b.mount_alloc_ok = true;
            b.neg_rc = 0;
            b.shutdown_rc = 0;
            b.shutdown_mode = 1;
            b.retry_safe = 0;
            b.peer_closed = 0;
            b.log.clear();
            b.vpriv_sb = ptr::null_mut();
        });
    }

    fn ns_log_has(s: &str) -> bool {
        NW.with(|w| w.borrow().log.iter().any(|l| l == s))
    }

    extern "C" fn ns_sb_table() -> *mut c_void {
        NW.with(|w| w.borrow().sbs.as_ptr() as *mut c_void)
    }
    extern "C" fn ns_sb_stride() -> u64 {
        core::mem::size_of::<UxfsSbView>() as u64
    }
    extern "C" fn ns_sb_count() -> c_int {
        NS_SLOTS as c_int
    }
    extern "C" fn ns_lock() -> u64 {
        0
    }
    extern "C" fn ns_unlock(_f: u64) {}
    extern "C" fn ns_block_device(idx: c_int) -> *mut c_void {
        NW.with(|w| {
            w.borrow()
                .dev_for_block
                .iter()
                .find(|(i, _)| *i == idx)
                .map(|(_, d)| *d)
                .unwrap_or(ptr::null_mut())
        })
    }
    extern "C" fn ns_native(_d: *mut c_void) -> c_int {
        NW.with(|w| w.borrow().native)
    }
    extern "C" fn ns_path_is_dir(_p: *const c_char) -> c_int {
        NW.with(|w| w.borrow().path_kind)
    }
    extern "C" fn ns_iget(
        _sb: *mut c_void,
        _ino: u64,
        _t: c_int,
        _m: u32,
        _s: u64,
        _p: *mut c_void,
    ) -> *mut c_void {
        NW.with(|w| {
            if w.borrow().iget_fail {
                ptr::null_mut()
            } else {
                ROOT_VN
            }
        })
    }
    extern "C" fn ns_mount_alloc() -> *mut c_void {
        NW.with(|w| if w.borrow().mount_alloc_ok { MNT } else { ptr::null_mut() })
    }
    extern "C" fn ns_mount_init(
        _mnt: *mut c_void,
        _path: *const c_char,
        flags: u32,
        _root: *mut c_void,
        _sb: *mut c_void,
    ) {
        NW.with(|w| w.borrow_mut().log.push(format!("mount_init={}", flags)));
    }
    extern "C" fn ns_link(_mnt: *mut c_void) {
        NW.with(|w| w.borrow_mut().log.push("link".into()));
    }
    extern "C" fn ns_dcache(_x: *mut c_void) {
        NW.with(|w| w.borrow_mut().log.push("dcache".into()));
    }
    extern "C" fn ns_register_peek(_x: *mut c_void) {
        NW.with(|w| w.borrow_mut().log.push("register_peek".into()));
    }
    extern "C" fn ns_clear_sb(_sb: *mut c_void) {
        NW.with(|w| w.borrow_mut().log.push("clear_sb".into()));
    }
    extern "C" fn ns_vget(_v: *mut c_void) {
        NW.with(|w| w.borrow_mut().log.push("vget".into()));
    }
    extern "C" fn ns_vput(_v: *mut c_void) {
        NW.with(|w| w.borrow_mut().log.push("vput".into()));
    }
    extern "C" fn ns_fs_data(_v: *mut c_void) -> *mut c_void {
        NW.with(|w| w.borrow().vpriv_sb)
    }
    extern "C" fn ns_lifetime_reset(p: *mut c_void) {
        NW.with(|w| w.borrow_mut().log.push("lifetime_reset".into()));
        unsafe { *(p as *mut u32) = 0; }
    }
    extern "C" fn ns_proc_get(_t: *mut c_void) -> *mut c_void {
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.log.push("proc_get".into());
            if b.proc_get_ok {
                SRV
            } else {
                ptr::null_mut()
            }
        })
    }
    extern "C" fn ns_proc_put(_t: *mut c_void) {
        NW.with(|w| w.borrow_mut().log.push("proc_put".into()));
    }
    extern "C" fn ns_init_req_lock(_sb: *mut c_void) {
        NW.with(|w| w.borrow_mut().log.push("init_req_lock".into()));
    }
    extern "C" fn ns_detach(_sb: *mut c_void) {
        NW.with(|w| w.borrow_mut().log.push("detach".into()));
    }
    extern "C" fn ns_peer_closed(_ep: *mut c_void) -> c_int {
        NW.with(|w| w.borrow().peer_closed)
    }
    extern "C" fn ns_now() -> u64 {
        1000
    }
    extern "C" fn ns_negotiate(_sb: *mut c_void, _dl: u64) -> c_int {
        NW.with(|w| w.borrow().neg_rc)
    }
    extern "C" fn ns_shutdown_rpc(_sb: *mut c_void, _dl: u64, retry: *mut c_int) -> c_int {
        NW.with(|w| {
            let b = w.borrow();
            if !retry.is_null() {
                unsafe { *retry = b.retry_safe };
            }
            b.shutdown_rc
        })
    }
    extern "C" fn ns_mount_log(_p: *const c_char, _b: c_int) {
        NW.with(|w| w.borrow_mut().log.push("mount_log".into()));
    }

    fn ntable() -> A20UxfsNs {
        A20UxfsNs {
            sb_table: Some(ns_sb_table),
            sb_stride: Some(ns_sb_stride),
            sb_count: Some(ns_sb_count),
            lock_mounts: Some(ns_lock),
            unlock_mounts: Some(ns_unlock),
            block_device: Some(ns_block_device),
            device_has_native_mount: Some(ns_native),
            path_is_dir: Some(ns_path_is_dir),
            iget: Some(ns_iget),
            mount_alloc: Some(ns_mount_alloc),
            mount_init: Some(ns_mount_init),
            mount_link_tree: Some(ns_link),
            dcache_invalidate_all: Some(ns_dcache),
            identity_register_peek: Some(ns_register_peek),
            identity_clear_sb: Some(ns_clear_sb),
            vnode_get: Some(ns_vget),
            vnode_put: Some(ns_vput),
            vnode_fs_data: Some(ns_fs_data),
            lifetime_reset: Some(ns_lifetime_reset),
            proc_get: Some(ns_proc_get),
            proc_put: Some(ns_proc_put),
            slot_init_req_lock: Some(ns_init_req_lock),
            detach_server: Some(ns_detach),
            peer_closed: Some(ns_peer_closed),
            now_ns: Some(ns_now),
            negotiate_lifetime_until: Some(ns_negotiate),
            shutdown_rpc: Some(ns_shutdown_rpc),
            mount_log: Some(ns_mount_log),
        }
    }

    fn sb(i: usize) -> UxfsSbView {
        NW.with(|w| UxfsSbView { ..w.borrow().sbs[i].clone() })
    }

    #[test]
    fn serve_claims_free_slot_and_links_mount() {
        ns_reset();
        let n = ntable();
        let rc = unsafe {
            a20_uxfs_ns_serve_mount(&n, name_ptr(b"/mnt"), EP, SRV, 3, 0)
        };
        assert_eq!(rc, 0);
        let s = sb(0);
        assert_eq!(s.ep, EP);
        assert_eq!(s.server, SRV);
        assert_eq!(s.block_index, 3);
        assert_eq!(s.next_req_id, 0);
        assert_eq!(s.server_generation, 4); /* bumped */
        assert_eq!(s.slot_reserved, 0); /* released at the end */
        assert!(ns_log_has("mount_init=0"));
        assert!(ns_log_has("link"));
        assert!(ns_log_has("dcache"));
        assert!(ns_log_has("vget"));
        assert!(ns_log_has("mount_log"));
    }

    #[test]
    fn serve_readonly_flag_sets_mount_flag() {
        ns_reset();
        let n = ntable();
        assert_eq!(unsafe { a20_uxfs_ns_serve_mount(&n, name_ptr(b"/mnt"), EP, SRV, 3, 1) }, 0);
        assert!(ns_log_has("mount_init=1"));
    }

    #[test]
    fn serve_rejects_non_dir_and_missing_path() {
        ns_reset();
        let n = ntable();
        NW.with(|w| w.borrow_mut().path_kind = 0);
        assert_eq!(unsafe { a20_uxfs_ns_serve_mount(&n, name_ptr(b"/mnt"), EP, SRV, 3, 0) }, -ENOTDIR);
        NW.with(|w| w.borrow_mut().path_kind = -ENOENT);
        assert_eq!(unsafe { a20_uxfs_ns_serve_mount(&n, name_ptr(b"/mnt"), EP, SRV, 3, 0) }, -ENOENT);
    }

    #[test]
    fn serve_busy_on_native_or_existing_service() {
        ns_reset();
        let n = ntable();
        NW.with(|w| w.borrow_mut().native = 1);
        assert_eq!(unsafe { a20_uxfs_ns_serve_mount(&n, name_ptr(b"/mnt"), EP, SRV, 3, 0) }, -EBUSY);
        ns_reset();
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.sbs[1].ep = EP;
            b.sbs[1].block_index = 3;
            b.sbs[1].server = SRV;
        });
        assert_eq!(unsafe { a20_uxfs_ns_serve_mount(&n, name_ptr(b"/mnt"), EP, SRV, 3, 0) }, -EBUSY);
    }

    #[test]
    fn serve_exhausted_slots_is_emfile() {
        ns_reset();
        let n = ntable();
        NW.with(|w| {
            let mut b = w.borrow_mut();
            for i in 0..NS_SLOTS {
                b.sbs[i].ep = EP;
                b.sbs[i].block_index = 9 + i as c_int;
            }
        });
        assert_eq!(unsafe { a20_uxfs_ns_serve_mount(&n, name_ptr(b"/mnt"), EP, SRV, 3, 0) }, -EMFILE);
    }

    #[test]
    fn serve_proc_get_failure_releases_slot() {
        ns_reset();
        let n = ntable();
        NW.with(|w| w.borrow_mut().proc_get_ok = false);
        assert_eq!(unsafe { a20_uxfs_ns_serve_mount(&n, name_ptr(b"/mnt"), EP, SRV, 3, 0) }, -ESRCH);
        let s = sb(0);
        assert!(s.ep.is_null());
        assert_eq!(s.slot_reserved, 0);
    }

    #[test]
    fn serve_root_iget_failure_unwinds() {
        ns_reset();
        let n = ntable();
        NW.with(|w| w.borrow_mut().iget_fail = true);
        assert_eq!(unsafe { a20_uxfs_ns_serve_mount(&n, name_ptr(b"/mnt"), EP, SRV, 3, 0) }, -ENOMEM);
        let s = sb(0);
        assert!(s.ep.is_null());
        assert!(s.server.is_null());
        assert_eq!(s.slot_reserved, 0);
        assert!(ns_log_has("clear_sb"));
        assert!(ns_log_has("proc_put"));
    }

    #[test]
    fn serve_mount_alloc_failure_puts_root() {
        ns_reset();
        let n = ntable();
        NW.with(|w| w.borrow_mut().mount_alloc_ok = false);
        assert_eq!(unsafe { a20_uxfs_ns_serve_mount(&n, name_ptr(b"/mnt"), EP, SRV, 3, 0) }, -ENOMEM);
        assert!(ns_log_has("vput"));
        assert_eq!(sb(0).slot_reserved, 0);
    }

    #[test]
    fn device_owned_matches_live_service() {
        ns_reset();
        let n = ntable();
        assert_eq!(unsafe { a20_uxfs_ns_device_owned(&n, DEV) }, 0);
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.sbs[2].ep = EP;
            b.sbs[2].block_index = 3;
        });
        assert_eq!(unsafe { a20_uxfs_ns_device_owned(&n, DEV) }, 1);
    }

    #[test]
    fn owned_block_dev_matches_caller_server() {
        ns_reset();
        let n = ntable();
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.sbs[1].ep = EP;
            b.sbs[1].block_index = 3;
            b.sbs[1].server = SRV;
        });
        assert_eq!(unsafe { a20_uxfs_ns_owned_block_dev(&n, SRV, 3) }, DEV);
        assert!(unsafe { a20_uxfs_ns_owned_block_dev(&n, 0x1234 as *mut c_void, 3) }.is_null());
    }

    /* --------------------- namespace (unmount) --------------------- */

    fn unmount_ready() {
        ns_reset();
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.sbs[0].ep = EP;
            b.sbs[0].lifetime.shutdown_mode = b.shutdown_mode;
            let sbp = b.sbs.as_ptr() as *mut c_void;
            b.vpriv_sb = VPRIV.with(|v| {
                v.set(sbp as usize);
                v.as_ptr() as *mut c_void
            });
        });
    }

    thread_local! {
        static VPRIV: core::cell::Cell<usize> = const { core::cell::Cell::new(0) };
    }

    #[test]
    fn unmount_negotiate_failure_with_dead_peer_detaches() {
        unmount_ready();
        let n = ntable();
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.neg_rc = -EIO;
            b.peer_closed = 1;
        });
        assert_eq!(unsafe { a20_uxfs_ns_unmount(&n, 0xB000 as *mut c_void) }, 0);
        assert!(ns_log_has("detach"));
        assert_eq!(sb(0).shutdown_started, 1);
    }

    #[test]
    fn unmount_negotiate_failure_retryable() {
        unmount_ready();
        let n = ntable();
        NW.with(|w| w.borrow_mut().neg_rc = -ETIMEDOUT_TEST);
        let rc = unsafe { a20_uxfs_ns_unmount(&n, 0xB000 as *mut c_void) };
        assert_eq!(rc, -ETIMEDOUT_TEST);
        assert!(!ns_log_has("detach"));
        assert_eq!(sb(0).shutdown_started, 1); /* quiesced, retryable */
    }

    const ETIMEDOUT_TEST: c_int = 110;

    #[test]
    fn unmount_shutdown_ok_detaches() {
        unmount_ready();
        let n = ntable();
        assert_eq!(unsafe { a20_uxfs_ns_unmount(&n, 0xB000 as *mut c_void) }, 0);
        assert!(ns_log_has("detach"));
    }

    #[test]
    fn unmount_shutdown_retry_safe_reopens_admission() {
        unmount_ready();
        let n = ntable();
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.shutdown_rc = -EBUSY;
            b.retry_safe = 1;
        });
        assert_eq!(unsafe { a20_uxfs_ns_unmount(&n, 0xB000 as *mut c_void) }, -EBUSY);
        assert!(!ns_log_has("detach"));
        assert_eq!(sb(0).shutdown_started, 0); /* pre-teardown EBUSY reopened */
    }

    #[test]
    fn unmount_shutdown_ambiguous_ebusy_stays_quiesced() {
        unmount_ready();
        let n = ntable();
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.shutdown_rc = -EBUSY;
            b.retry_safe = 0;
        });
        assert_eq!(unsafe { a20_uxfs_ns_unmount(&n, 0xB000 as *mut c_void) }, -EBUSY);
        assert_eq!(sb(0).shutdown_started, 1);
    }

    #[test]
    fn unmount_v1_peer_without_shutdown_is_eopnotsupp() {
        unmount_ready();
        let n = ntable();
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.shutdown_mode = 0;
            b.sbs[0].lifetime.shutdown_mode = 0;
        });
        assert_eq!(unsafe { a20_uxfs_ns_unmount(&n, 0xB000 as *mut c_void) }, -EOPNOTSUPP);
        assert_eq!(sb(0).shutdown_started, 0);
    }

    #[test]
    fn unmount_v1_dead_peer_detaches() {
        unmount_ready();
        let n = ntable();
        NW.with(|w| {
            let mut b = w.borrow_mut();
            b.shutdown_mode = 0;
            b.sbs[0].lifetime.shutdown_mode = 0;
            b.peer_closed = 1;
        });
        assert_eq!(unsafe { a20_uxfs_ns_unmount(&n, 0xB000 as *mut c_void) }, 0);
        assert!(ns_log_has("detach"));
    }

    /* ------------------------- open/close lifecycle ------------------------- */

    const VFIL: *mut c_void = 0x9100 as *mut c_void;

    #[derive(Default)]
    struct LWorld {
        fc_alloc_ok: bool,
        vfile_alloc_ok: bool,
        vn_size: u64,
        is_dir: c_int,
        sb_confirm: c_int,
        token: u64,
        lifetime_open_rc: c_int,
        pc_writeback_rc: c_int,
        close_rc: c_int,
        close_acked: c_int,
        close_sent: c_int,
        confirm_rc: c_int,
        confirm_acked: c_int,
        confirm_sent: c_int,
        /* vfile model */
        vf_vnode: *mut c_void,
        vf_priv: *mut c_void,
        vf_flags: c_int,
        vf_capable: c_int,
        vf_offset: u64,
        attempt_sent: c_int,
        attempt_acked: c_int,
        attempt_retry: c_int,
        log: Vec<String>,
    }

    thread_local! {
        static LW: RefCell<LWorld> = RefCell::new(LWorld::default());
        static VP: core::cell::Cell<usize> = const { core::cell::Cell::new(0) };
    }

    fn lc_reset() {
        NW.with(|w| *w.borrow_mut() = NWorld::default());
        fc_reset();
        LW.with(|w| {
            let mut b = w.borrow_mut();
            *b = LWorld {
                fc_alloc_ok: true,
                vfile_alloc_ok: true,
                vf_vnode: VN,
                ..LWorld::default()
            };
        });
    }

    fn llog(s: &str) -> bool {
        LW.with(|w| w.borrow().log.iter().any(|l| l == s))
    }

    extern "C" fn lc_fc_alloc() -> *mut c_void {
        LW.with(|w| if w.borrow().fc_alloc_ok { fc_ptr() } else { ptr::null_mut() })
    }
    extern "C" fn lc_vfile_alloc() -> *mut c_void {
        LW.with(|w| if w.borrow().vfile_alloc_ok { VFIL } else { ptr::null_mut() })
    }
    extern "C" fn lc_vfile_free(_vf: *mut c_void) {
        LW.with(|w| w.borrow_mut().log.push("vfile_free".into()));
    }
    extern "C" fn lc_kfree(_p: *mut c_void) {
        LW.with(|w| w.borrow_mut().log.push("kfree".into()));
    }
    extern "C" fn lc_vn_vpriv(_vn: *mut c_void) -> *mut c_void {
        VP.with(|v| v.as_ptr() as *mut c_void)
    }
    extern "C" fn lc_vpriv_sb(_p: *mut c_void) -> *mut c_void {
        CTX
    }
    extern "C" fn lc_vpriv_is_dir(_p: *mut c_void) -> c_int {
        LW.with(|w| w.borrow().is_dir)
    }
    extern "C" fn lc_vn_ino(_vn: *const c_void) -> u64 {
        7
    }
    extern "C" fn lc_vn_size(_vn: *const c_void) -> u64 {
        LW.with(|w| w.borrow().vn_size)
    }
    extern "C" fn lc_lifetime_open(
        _sb: *mut c_void,
        _ino: u64,
        _flags: u64,
        token: *mut u64,
    ) -> c_int {
        LW.with(|w| {
            let b = w.borrow();
            if !token.is_null() {
                unsafe { *token = b.token };
            }
            b.lifetime_open_rc
        })
    }
    extern "C" fn lc_confirm_mode(_sb: *mut c_void) -> c_int {
        LW.with(|w| w.borrow().sb_confirm)
    }
    extern "C" fn lc_fc_init(
        _fc: *mut c_void,
        sb: *mut c_void,
        ino: u64,
        token: u64,
        confirm: c_int,
        is_dir: c_int,
        off: u64,
    ) {
        FC.with(|f| {
            let mut c = f.borrow_mut();
            c.sb = sb;
            c.ino = ino;
            c.close_token = token;
            c.close_confirm_mode = confirm as u8;
            c.is_dir = is_dir;
            c.off = off as usize;
        });
    }
    extern "C" fn lc_vf_init(
        _vf: *mut c_void,
        vn: *mut c_void,
        flags: c_int,
        capable: c_int,
        off: u64,
        priv_: *mut c_void,
    ) {
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.vf_vnode = vn;
            b.vf_flags = flags;
            b.vf_capable = capable;
            b.vf_offset = off;
            b.vf_priv = priv_;
            b.log.push("vf_init".into());
        });
    }
    extern "C" fn lc_fc_sb(_fc: *mut c_void) -> *mut c_void {
        FC.with(|f| f.borrow().sb)
    }
    extern "C" fn lc_fc_ino(_fc: *const c_void) -> u64 {
        FC.with(|f| f.borrow().ino)
    }
    extern "C" fn lc_fc_token(_fc: *const c_void) -> usize {
        FC.with(|f| f.borrow().close_token as usize)
    }
    extern "C" fn lc_fc_confirm(_fc: *const c_void) -> c_int {
        FC.with(|f| f.borrow().close_confirm_mode as c_int)
    }
    extern "C" fn lc_fc_accepted(_fc: *const c_void) -> c_int {
        FC.with(|f| f.borrow().close_accepted as c_int)
    }
    extern "C" fn lc_fc_set_accepted(_fc: *mut c_void, v: c_int) {
        FC.with(|f| f.borrow_mut().close_accepted = v as u8);
    }
    extern "C" fn lc_vf_vnode(_vf: *mut c_void) -> *mut c_void {
        LW.with(|w| w.borrow().vf_vnode)
    }
    extern "C" fn lc_vf_priv(_vf: *mut c_void) -> *mut c_void {
        LW.with(|w| w.borrow().vf_priv)
    }
    extern "C" fn lc_set_attempt(_vf: *mut c_void, sent: c_int, acked: c_int, retry: c_int) {
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.attempt_sent = sent;
            b.attempt_acked = acked;
            b.attempt_retry = retry;
        });
    }
    extern "C" fn lc_release(_vf: *mut c_void, _fc: *mut c_void) {
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.vf_priv = ptr::null_mut();
            b.log.push("release".into());
        });
    }
    extern "C" fn lc_pc_writeback(_vn: *mut c_void) -> c_int {
        LW.with(|w| w.borrow().pc_writeback_rc)
    }
    extern "C" fn lc_lifetime_close(
        _sb: *mut c_void,
        _ino: u64,
        _token: u64,
        acked: *mut c_int,
        sent: *mut c_int,
    ) -> c_int {
        LW.with(|w| {
            let b = w.borrow();
            if !acked.is_null() {
                unsafe { *acked = b.close_acked };
            }
            if !sent.is_null() {
                unsafe { *sent = b.close_sent };
            }
            b.close_rc
        })
    }
    extern "C" fn lc_lifetime_close_confirm(
        _sb: *mut c_void,
        _ino: u64,
        _token: u64,
        acked: *mut c_int,
        sent: *mut c_int,
    ) -> c_int {
        LW.with(|w| {
            let b = w.borrow();
            if !acked.is_null() {
                unsafe { *acked = b.confirm_acked };
            }
            if !sent.is_null() {
                unsafe { *sent = b.confirm_sent };
            }
            b.confirm_rc
        })
    }
    extern "C" fn lc_trace(_vf: *mut c_void, _fc: *mut c_void) {
        LW.with(|w| w.borrow_mut().log.push("trace".into()));
    }

    fn ltable() -> A20UxfsLifecycle {
        A20UxfsLifecycle {
            fc_alloc: Some(lc_fc_alloc),
            vfile_alloc: Some(lc_vfile_alloc),
            vfile_free: Some(lc_vfile_free),
            kfree: Some(lc_kfree),
            vn_vpriv: Some(lc_vn_vpriv),
            vpriv_sb: Some(lc_vpriv_sb),
            vpriv_is_dir: Some(lc_vpriv_is_dir),
            vn_ino: Some(lc_vn_ino),
            vn_size: Some(lc_vn_size),
            lifetime_open: Some(lc_lifetime_open),
            lifetime_confirm_mode: Some(lc_confirm_mode),
            fc_init: Some(lc_fc_init),
            vf_init: Some(lc_vf_init),
            fc_sb: Some(lc_fc_sb),
            fc_ino: Some(lc_fc_ino),
            fc_close_token: Some(lc_fc_token),
            fc_close_confirm_mode: Some(lc_fc_confirm),
            fc_close_accepted: Some(lc_fc_accepted),
            fc_set_close_accepted: Some(lc_fc_set_accepted),
            vf_vnode: Some(lc_vf_vnode),
            vf_priv: Some(lc_vf_priv),
            vf_set_close_attempt: Some(lc_set_attempt),
            fc_release: Some(lc_release),
            pc_writeback: Some(lc_pc_writeback),
            lifetime_close: Some(lc_lifetime_close),
            lifetime_close_confirm: Some(lc_lifetime_close_confirm),
            trace: Some(lc_trace),
        }
    }

    #[test]
    fn open_binds_token_confirm_and_append_offset() {
        lc_reset();
        let l = ltable();
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.token = 0xABC;
            b.sb_confirm = 1;
            b.vn_size = 4096;
        });
        let vf = unsafe { a20_uxfs_file_open(&l, VN, 0x402 /* O_RDWR|O_APPEND */) };
        assert_eq!(vf, VFIL);
        assert_eq!(FC.with(|f| f.borrow().close_token), 0xABC);
        assert_eq!(FC.with(|f| f.borrow().close_confirm_mode), 1);
        assert_eq!(FC.with(|f| f.borrow().off), 4096);
        let (vo, vc, vp) = LW.with(|w| {
            let b = w.borrow();
            (b.vf_offset, b.vf_capable, b.vf_priv)
        });
        assert_eq!(vo, 4096);
        assert_eq!(vc, 1);
        assert_eq!(vp, fc_ptr());
        assert!(llog("vf_init"));
    }

    #[test]
    fn open_without_token_is_not_confirm_capable() {
        lc_reset();
        let l = ltable();
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.token = 0;
            b.sb_confirm = 1;
        });
        let vf = unsafe { a20_uxfs_file_open(&l, VN, 0) };
        assert_eq!(vf, VFIL);
        assert_eq!(FC.with(|f| f.borrow().close_confirm_mode), 0);
        assert_eq!(LW.with(|w| w.borrow().vf_capable), 0);
    }

    #[test]
    fn open_lifetime_failure_frees_both() {
        lc_reset();
        let l = ltable();
        LW.with(|w| w.borrow_mut().lifetime_open_rc = -EIO);
        let vf = unsafe { a20_uxfs_file_open(&l, VN, 0) };
        assert!(vf.is_null());
        assert!(llog("vfile_free"));
        assert!(llog("kfree"));
        assert!(!llog("vf_init"));
    }

    #[test]
    fn open_vfile_alloc_failure_frees_fctx() {
        lc_reset();
        let l = ltable();
        LW.with(|w| w.borrow_mut().vfile_alloc_ok = false);
        let vf = unsafe { a20_uxfs_file_open(&l, VN, 0) };
        assert!(vf.is_null());
        assert!(llog("kfree"));
        assert!(!llog("vf_init"));
    }

    #[test]
    fn close_writeback_failure_is_retryable() {
        lc_reset();
        let l = ltable();
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.vf_priv = fc_ptr();
            b.pc_writeback_rc = -EIO;
        });
        assert_eq!(unsafe { a20_uxfs_file_close(&l, VFIL) }, -EIO);
        let (retry, priv_live) = LW.with(|w| {
            let b = w.borrow();
            (b.attempt_retry, !b.vf_priv.is_null())
        });
        assert_eq!(retry, 1);
        assert!(priv_live); /* context retained */
        assert!(!llog("release"));
    }

    #[test]
    fn close_non_confirm_success_releases() {
        lc_reset();
        let l = ltable();
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.vf_priv = fc_ptr();
            b.close_acked = 1;
            b.close_sent = 1;
        });
        FC.with(|f| f.borrow_mut().close_confirm_mode = 0);
        assert_eq!(unsafe { a20_uxfs_file_close(&l, VFIL) }, 0);
        assert!(llog("release"));
        assert!(LW.with(|w| w.borrow().vf_priv.is_null()));
    }

    #[test]
    fn close_failed_after_send_is_not_retry_safe() {
        lc_reset();
        let l = ltable();
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.vf_priv = fc_ptr();
            b.close_rc = -EIO;
            b.close_sent = 1;
            b.close_acked = 0;
        });
        assert_eq!(unsafe { a20_uxfs_file_close(&l, VFIL) }, -EIO);
        let (sent, retry) = LW.with(|w| {
            let b = w.borrow();
            (b.attempt_sent, b.attempt_retry)
        });
        assert_eq!(sent, 1);
        assert_eq!(retry, 0); /* on-wire, ambiguous: not retry safe */
        assert!(!llog("release"));
    }

    #[test]
    fn close_unsent_failure_is_retry_safe() {
        lc_reset();
        let l = ltable();
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.vf_priv = fc_ptr();
            b.close_rc = -EIO;
            b.close_sent = 0;
        });
        assert_eq!(unsafe { a20_uxfs_file_close(&l, VFIL) }, -EIO);
        assert_eq!(LW.with(|w| w.borrow().attempt_retry), 1);
    }

    #[test]
    fn close_confirm_success_releases() {
        lc_reset();
        let l = ltable();
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.vf_priv = fc_ptr();
            b.close_acked = 1;
            b.close_sent = 1;
            b.confirm_rc = 0;
            b.confirm_acked = 1;
            b.confirm_sent = 1;
        });
        FC.with(|f| f.borrow_mut().close_confirm_mode = 1);
        assert_eq!(unsafe { a20_uxfs_file_close(&l, VFIL) }, 0);
        assert!(llog("release"));
        assert_eq!(FC.with(|f| f.borrow().close_accepted), 1);
    }

    #[test]
    fn close_confirm_ambiguous_is_retry_safe_only_when_unsent() {
        lc_reset();
        let l = ltable();
        LW.with(|w| {
            let mut b = w.borrow_mut();
            b.vf_priv = fc_ptr();
            b.close_acked = 1;
            b.close_sent = 1;
            b.confirm_rc = 0;
            b.confirm_acked = 0;
            b.confirm_sent = 1;
        });
        FC.with(|f| f.borrow_mut().close_confirm_mode = 1);
        assert_eq!(unsafe { a20_uxfs_file_close(&l, VFIL) }, -EIO);
        let retry = LW.with(|w| w.borrow().attempt_retry);
        assert_eq!(retry, 0);
        assert!(!llog("release"));
    }
}
