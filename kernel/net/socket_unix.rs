#![no_std]

//! AF_UNIX: the address preparation, the bind and connect transactions, the
//! sendto transaction and the channel-backed data plane.
//!
//! The C side keeps the primitives this module calls: the socket lock (single
//! and the address-ordered pair), the liveness test, the reference and the free,
//! the socket allocator, the whole-table bound-socket scan, the current task's
//! credentials and cwd, the vfs stat/open/close that materialize a bound path,
//! the accept-queue push and the wakeup trio, the channel endpoint release and
//! the two channel calls, the stream-buffer allocator, the legacy receive-queue
//! enqueue and the wake-queue init/flush.
//!
//! Everything that is a *decision* is here: whether a bind publishes its address
//! before or after the filesystem node exists and what a failed node creation
//! rolls back; that the bound-socket scan and the listener lookup run with no net
//! lock held and come back with a reference; the order in which a connect wires
//! the accepted child, pushes it on the backlog and rolls the whole pairing back
//! when the push fails; which credentials a connecting end stamps on which peer;
//! that a stale peer back-pointer is dropped and the destination re-resolved by
//! address; that a stream write longer than one message is fragmented with the
//! SCM_RIGHTS fds on the final chunk; and that each channel chunk wakes the
//! reader before the writer may block on the next one.
//!
//! The socket's fields are reached through a borrowed view built by C, so no
//! struct ABI moves and the lock discipline is unchanged.

use core::ffi::c_void;
use core::ptr;

const AF_UNIX: i32 = 1;
const SOCK_STREAM: i32 = 1;
const SOCK_SEQPACKET: i32 = 5;

const NET_SOCKADDR_MAX: usize = 128;
const NET_MAX_PAYLOAD: usize = 65535;
const CH_MAX_DATA: usize = 65536;
const CH_NONBLOCK: u32 = 1;

/* Largest per-profile MAX_PATH_LEN; the effective limit is passed in. */
const PATH_BUF: usize = 512;

const A20_ERR_INTERRUPTED: i64 = 3;
const A20_ERR_WOULD_BLOCK: i64 = 18;
const A20_ERR_CANCELED: i64 = 20;

const EAGAIN: i32 = 11;
const EINTR: i32 = 4;
const EIO: i32 = 5;
const ENOMEM: i32 = 12;
const EEXIST: i32 = 17;
/* Only the host tests name this one: the S_IFDIR test itself is the C
 * primitive's, so the kernel build has no use for the constant. */
#[allow(dead_code)]
const ENOTDIR: i32 = 20;
const EINVAL: i32 = 22;
const EPIPE: i32 = 32;
const ENAMETOOLONG: i32 = 36;
const ENOTSOCK: i32 = 88;
const EDESTADDRREQ: i32 = 89;
const EAFNOSUPPORT: i32 = 97;
const EADDRINUSE: i32 = 98;
const ECONNREFUSED: i32 = 111;

/// The two-socket lock, opaque here: C fills in the ascending-address order.
#[repr(C)]
pub struct Pair {
    pub lo: *mut c_void,
    pub hi: *mut c_void,
    pub flags: u64,
}

impl Pair {
    fn zero() -> Pair {
        Pair { lo: ptr::null_mut(), hi: ptr::null_mut(), flags: 0 }
    }
}

/// Borrowed view of the fields these transactions read and write.
#[repr(C)]
pub struct UnixView {
    pub s: *mut c_void,
    pub net_ns: *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub protocol: *mut i32,
    pub nonblock: *mut i32,
    pub bound: *mut i32,
    pub connected: *mut i32,
    pub ever_connected: *mut i32,
    pub listening: *mut i32,
    pub accept_count: *mut i32,
    pub passcred: *mut i32,
    pub owner_pid: *mut i32,
    pub owner_uid: *mut i32,
    pub owner_gid: *mut i32,
    pub peer_pid: *mut i32,
    pub peer_uid: *mut i32,
    pub peer_gid: *mut i32,
    pub ch_cred_pid: *mut i32,
    pub ch_cred_uid: *mut i32,
    pub ch_cred_gid: *mut i32,
    pub local: *mut u8,
    pub local_len: *mut usize,
    pub peer_addr: *mut u8,
    pub peer_len: *mut usize,
    pub peer: *mut *mut c_void,
    pub accept_next: *mut *mut c_void,
    pub ch_ep: *mut *mut c_void,
    pub ch_buf: *mut *mut i8,
    pub ch_len: *mut u32,
}

unsafe extern "C" {
    fn a20_net_unix_view_init(s: *mut c_void, out: *mut UnixView);
    fn a20_net_unix_lock(s: *mut c_void, flags: *mut u64);
    fn a20_net_unix_unlock(s: *mut c_void, flags: u64);
    fn a20_net_unix_lock2(a: *mut c_void, b: *mut c_void, pair: *mut Pair);
    fn a20_net_unix_unlock2(pair: *mut Pair);
    fn a20_net_unix_is_live(s: *mut c_void) -> i32;
    fn a20_net_unix_ref(s: *mut c_void);
    fn a20_net_unix_free(s: *mut c_void);
    fn a20_net_unix_alloc() -> *mut c_void;
    fn a20_net_unix_find_bound(ns: *mut c_void, domain: i32, sock_type: i32,
                               addr: *const u8, addrlen: usize) -> *mut c_void;
    fn a20_net_unix_creds(pid: *mut i32, uid: *mut i32, gid: *mut i32) -> i32;
    fn a20_net_unix_cwd(out: *mut u8, outsz: usize);
    fn a20_net_unix_path_is_dir(path: *const i8) -> i32;
    fn a20_net_unix_path_stat(path: *const i8) -> i32;
    fn a20_net_unix_path_create(path: *const i8) -> i32;
    fn a20_net_unix_path_close(fd: i32);
    fn a20_net_unix_accept_push(listener: *mut c_void, child: *mut c_void) -> i32;
    fn a20_net_unix_accept_collect(listener: *mut c_void, wake_q: *mut c_void) -> i32;
    fn a20_net_unix_read_collect(s: *mut c_void, wake_q: *mut c_void) -> i32;
    fn a20_net_unix_read_wake_all(s: *mut c_void);
    fn a20_net_unix_channel_release(v: *const UnixView);
    fn a20_net_unix_channel_send(ep: *mut c_void, buf: *const u8, len: u32,
                                 flags: u32) -> i64;
    fn a20_net_unix_channel_recv(ep: *mut c_void, buf: *mut u8, cap: *mut u32) -> i64;
    fn a20_net_unix_chbuf(v: *const UnixView) -> *mut u8;
    fn a20_net_unix_enqueue(dst: *mut c_void, buf: *const u8, len: usize,
                            addr: *const u8, addrlen: usize) -> i32;
    fn a20_net_unix_enqueue_fds(dst: *mut c_void, buf: *const u8, len: usize,
                                addr: *const u8, addrlen: usize,
                                files: *mut *mut c_void, nfiles: i32) -> i32;
    fn a20_net_unix_wakeq_init(wake_q: *mut c_void);
    fn a20_net_unix_wakeq_flush(wake_q: *mut c_void);
}

fn zero_view() -> UnixView {
    UnixView {
        s: ptr::null_mut(),
        net_ns: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        protocol: ptr::null_mut(),
        nonblock: ptr::null_mut(),
        bound: ptr::null_mut(),
        connected: ptr::null_mut(),
        ever_connected: ptr::null_mut(),
        listening: ptr::null_mut(),
        accept_count: ptr::null_mut(),
        passcred: ptr::null_mut(),
        owner_pid: ptr::null_mut(),
        owner_uid: ptr::null_mut(),
        owner_gid: ptr::null_mut(),
        peer_pid: ptr::null_mut(),
        peer_uid: ptr::null_mut(),
        peer_gid: ptr::null_mut(),
        ch_cred_pid: ptr::null_mut(),
        ch_cred_uid: ptr::null_mut(),
        ch_cred_gid: ptr::null_mut(),
        local: ptr::null_mut(),
        local_len: ptr::null_mut(),
        peer_addr: ptr::null_mut(),
        peer_len: ptr::null_mut(),
        peer: ptr::null_mut(),
        accept_next: ptr::null_mut(),
        ch_ep: ptr::null_mut(),
        ch_buf: ptr::null_mut(),
        ch_len: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> UnixView {
    let mut v = zero_view();
    a20_net_unix_view_init(s, &mut v);
    v
}

/* --------------------------------------------------------------- strings */

/// The per-profile MAX_PATH_LEN is passed in; the module's own buffers are sized
/// for the largest profile and clamped to it, so a profile with a smaller limit
/// truncates exactly where the C it replaces did.
fn path_cap(max_path: usize) -> usize {
    if max_path == 0 || max_path > PATH_BUF { PATH_BUF } else { max_path }
}

unsafe fn slen(p: *const i8, cap: usize) -> usize {
    let mut n = 0usize;
    while n < cap && *p.add(n) != 0 {
        n += 1;
    }
    n
}

/// `strncpy` plus the explicit NUL the C version followed it with: at most
/// `outsz - 1` bytes are copied and the result is always terminated.
unsafe fn copy_cstr(out: *mut u8, outsz: usize, src: &[u8]) {
    if outsz == 0 {
        return;
    }
    let n = if src.len() < outsz - 1 { src.len() } else { outsz - 1 };
    ptr::copy_nonoverlapping(src.as_ptr(), out, n);
    *out.add(n) = 0;
}

/// `unix_path_make_absolute`: an absolute path is copied, a relative one is
/// joined onto the current working directory.  Both are bounded by `outsz` and
/// terminated, which is the truncation the C `strncpy`/`snprintf` pair performed.
unsafe fn make_absolute(path: *const i8, out: *mut u8, outsz: usize) -> i32 {
    if path.is_null() || out.is_null() || outsz == 0 {
        return -EINVAL;
    }
    let n = slen(path, outsz);
    if n == 0 {
        return -EINVAL;
    }
    let rel = core::slice::from_raw_parts(path as *const u8, n);
    if rel[0] == b'/' {
        copy_cstr(out, outsz, rel);
        return 0;
    }
    let mut cwd = [0u8; PATH_BUF];
    a20_net_unix_cwd(cwd.as_mut_ptr(), PATH_BUF);
    let clen = slen(cwd.as_ptr() as *const i8, PATH_BUF);
    let mut pos = 0usize;
    if clen == 1 && cwd[0] == b'/' {
        /* Root cwd joins as "/path", not "//path". */
        if pos + 1 < outsz {
            *out.add(pos) = b'/';
            pos += 1;
        }
    } else {
        for &b in &cwd[..clen] {
            if pos + 1 >= outsz {
                break;
            }
            *out.add(pos) = b;
            pos += 1;
        }
        if pos + 1 < outsz {
            *out.add(pos) = b'/';
            pos += 1;
        }
    }
    for &b in rel {
        if pos + 1 >= outsz {
            break;
        }
        *out.add(pos) = b;
        pos += 1;
    }
    *out.add(pos) = 0;
    0
}

/* ------------------------------------------------------- address preparation */

#[no_mangle]
pub unsafe extern "C" fn a20_net_unix_sockaddr_prepare(
    addr: *const c_void,
    addrlen: usize,
    out: *mut u8,
    outlen: *mut usize,
    path_out: *mut i8,
    path_outsz: usize,
    max_path: usize,
) -> i32 {
    if addr.is_null() || out.is_null() || outlen.is_null()
        || addrlen < 2
        || addrlen > NET_SOCKADDR_MAX
    {
        return -EINVAL;
    }
    ptr::copy_nonoverlapping(addr as *const u8, out, addrlen);
    *outlen = addrlen;
    if ptr::read_unaligned(out as *const u16) != AF_UNIX as u16 {
        return -EAFNOSUPPORT;
    }
    if addrlen <= 2 {
        return 0;
    }
    let cap = path_cap(max_path);
    let path_len = addrlen - 2;
    let n = if path_len < cap - 1 { path_len } else { cap - 1 };
    let mut path = [0u8; PATH_BUF];
    ptr::copy_nonoverlapping(out.add(2), path.as_mut_ptr(), n);
    path[n] = 0;
    if path[0] == 0 {
        return 0;
    }
    let mut full = [0u8; PATH_BUF];
    let r = make_absolute(path.as_ptr() as *const i8, full.as_mut_ptr(), cap);
    if r < 0 {
        return r;
    }
    let full_len = slen(full.as_ptr() as *const i8, cap) + 1;
    if 2 + full_len > NET_SOCKADDR_MAX {
        return -ENAMETOOLONG;
    }
    ptr::write_unaligned(out as *mut u16, AF_UNIX as u16);
    ptr::copy_nonoverlapping(full.as_ptr(), out.add(2), full_len);
    *outlen = 2 + full_len;
    if !path_out.is_null() && path_outsz > 0 {
        copy_cstr(path_out as *mut u8, path_outsz, &full[..full_len - 1]);
    }
    0
}

/// `net_unix_path_parent_ok`: absolutize, take the parent directory, and require
/// it to be a directory.  A path with no slash at all is accepted, as before.
#[no_mangle]
pub unsafe extern "C" fn a20_net_unix_path_parent_ok(path: *const i8,
                                                     max_path: usize) -> i32 {
    if path.is_null() || *path == 0 {
        return 0;
    }
    let cap = path_cap(max_path);
    let mut parent = [0u8; PATH_BUF];
    let r = make_absolute(path, parent.as_mut_ptr(), cap);
    if r < 0 {
        return r;
    }
    let n = slen(parent.as_ptr() as *const i8, cap);
    let mut slash: Option<usize> = None;
    for i in 0..n {
        if parent[i] == b'/' {
            slash = Some(i);
        }
    }
    match slash {
        None => return 0,
        Some(0) => parent[1] = 0,
        Some(i) => parent[i] = 0,
    }
    a20_net_unix_path_is_dir(parent.as_ptr() as *const i8)
}

/* ---------------------------------------------------------------------- bind */

#[no_mangle]
pub unsafe extern "C" fn a20_net_unix_socket_bind(s: *mut c_void, addr: *const c_void,
                                                  addrlen: usize,
                                                  max_path: usize) -> i32 {
    if s.is_null() {
        return -ENOTSOCK;
    }
    let v = view_of(s);
    let mut bind_addr = [0u8; NET_SOCKADDR_MAX];
    let mut bind_len = addrlen;
    let cap = path_cap(max_path);
    let mut unix_path = [0u8; PATH_BUF];
    let ur = a20_net_unix_sockaddr_prepare(addr, addrlen, bind_addr.as_mut_ptr(),
                                           &mut bind_len, unix_path.as_mut_ptr() as *mut i8,
                                           cap, max_path);
    if ur < 0 {
        return ur;
    }
    let mut pathname = false;
    if bind_len > 2 && bind_addr[2] != 0 {
        pathname = true;
        let pr = a20_net_unix_path_parent_ok(bind_addr.as_ptr().add(2) as *const i8,
                                             max_path);
        if pr < 0 {
            return pr;
        }
        if a20_net_unix_path_stat(unix_path.as_ptr() as *const i8) == 0 {
            return -EADDRINUSE;
        }
    }

    let mut pid = 0i32;
    let mut uid = 0i32;
    let mut gid = 0i32;
    let have_creds = a20_net_unix_creds(&mut pid, &mut uid, &mut gid) == 0;
    let sock_type = *v.sock_type;
    /* Whole-table scan: it takes one bucket at a time and comes back with a
     * reference, and it never mutates, so publishing the address afterwards
     * needs this socket's bucket alone. */
    let clash = a20_net_unix_find_bound(v.net_ns, AF_UNIX, sock_type,
                                        bind_addr.as_ptr(), bind_len);
    if !clash.is_null() {
        a20_net_unix_free(clash);
        return -EADDRINUSE;
    }
    let mut flags = 0u64;
    a20_net_unix_lock(s, &mut flags);
    if a20_net_unix_is_live(s) == 0 {
        a20_net_unix_unlock(s, flags);
        return -ENOTSOCK;
    }
    ptr::copy_nonoverlapping(bind_addr.as_ptr(), v.local, bind_len);
    *v.local_len = bind_len;
    *v.bound = 1;
    if have_creds {
        *v.owner_pid = pid;
        *v.owner_uid = uid;
        *v.owner_gid = gid;
    }
    a20_net_unix_unlock(s, flags);

    if pathname {
        let fd = a20_net_unix_path_create(unix_path.as_ptr() as *const i8);
        if fd < 0 {
            /* The address was published first, so a failed node creation has to
             * take it back out of the table before returning. */
            let mut undo = 0u64;
            a20_net_unix_lock(s, &mut undo);
            *v.bound = 0;
            *v.local_len = 0;
            a20_net_unix_unlock(s, undo);
            return if fd == -EEXIST { -EADDRINUSE } else { fd };
        }
        a20_net_unix_path_close(fd);
    }
    0
}

/* ------------------------------------------------------------------- connect */

#[no_mangle]
pub unsafe extern "C" fn a20_net_unix_socket_connect(s: *mut c_void,
                                                     addr: *const c_void,
                                                     addrlen: usize,
                                                     max_path: usize,
                                                     max_queue: i32) -> i32 {
    if s.is_null() {
        return -ENOTSOCK;
    }
    let v = view_of(s);
    let mut peer_addr = [0u8; NET_SOCKADDR_MAX];
    let mut peer_len = addrlen;
    let ur = a20_net_unix_sockaddr_prepare(addr, addrlen, peer_addr.as_mut_ptr(),
                                           &mut peer_len, ptr::null_mut(), 0, max_path);
    if ur < 0 {
        return ur;
    }

    let mut wake_q = WakeQ::new();
    a20_net_unix_wakeq_init(wake_q.as_mut_ptr());

    let sock_type = *v.sock_type;
    /* The listener lookup is a whole-table scan returning a reference, which is
     * what lets this socket's bucket be dropped before the pair is taken. */
    let listener = a20_net_unix_find_bound(v.net_ns, AF_UNIX, sock_type,
                                           peer_addr.as_ptr(), peer_len);
    if listener.is_null() {
        return -ECONNREFUSED;
    }

    let lv = view_of(listener);
    let mut child: *mut c_void = ptr::null_mut();
    let mut rc = 0i32;
    let mut pair = Pair::zero();
    a20_net_unix_lock2(s, listener, &mut pair);
    if a20_net_unix_is_live(s) == 0 || a20_net_unix_is_live(listener) == 0 {
        rc = -ECONNREFUSED;
    } else {
        ptr::copy_nonoverlapping(peer_addr.as_ptr(), v.peer_addr, peer_len);
        *v.peer_len = peer_len;
        *v.connected = 1;
        *v.ever_connected = 1;

        if sock_type == SOCK_STREAM || sock_type == SOCK_SEQPACKET {
            if *lv.listening == 0 || *lv.accept_count >= max_queue {
                *v.connected = 0;
                rc = if *lv.listening != 0 { -EAGAIN } else { -ECONNREFUSED };
            } else {
                /* Allocated under the two locks the single global lock used to
                 * cover, and not published in the table until an accepted socket
                 * is registered, so it owns no slot and the private writes below
                 * need no lock of their own. */
                child = a20_net_unix_alloc();
                if child.is_null() {
                    *v.connected = 0;
                    rc = -ENOMEM;
                } else {
                    let cv = view_of(child);
                    *cv.domain = AF_UNIX;
                    *cv.sock_type = *v.sock_type;
                    *cv.protocol = *v.protocol;
                    /* Linux inherits SO_PASSCRED on accepted AF_UNIX sockets. */
                    *cv.passcred = *lv.passcred;
                    *cv.bound = 1;
                    *cv.connected = 1;
                    *cv.ever_connected = 1;
                    *cv.peer = s;
                    *v.peer = child;
                    let ll = *lv.local_len;
                    ptr::copy_nonoverlapping(lv.local, cv.local, ll);
                    *cv.local_len = ll;
                    let sl = *v.local_len;
                    if sl >= 2 {
                        ptr::copy_nonoverlapping(v.local, cv.peer_addr, sl);
                        *cv.peer_len = sl;
                    } else {
                        /* The connector never bound a pathname.  Linux still
                         * reports such a peer as a sockaddr_un carrying only
                         * the family, with addrlen 2; handing back addrlen 0
                         * makes Go's syscall.Accept4 convert a zeroed
                         * sockaddr (family 0) and fail with EAFNOSUPPORT,
                         * which kills any Go unix-socket server on its first
                         * accept. */
                        ptr::write_unaligned(cv.peer_addr as *mut u16,
                                             AF_UNIX as u16);
                        *cv.peer_len = 2;
                    }
                    let qr = a20_net_unix_accept_push(listener, child);
                    if qr < 0 {
                        if !(*v.ch_ep).is_null() {
                            a20_net_unix_channel_release(&v);
                        }
                        if !(*cv.ch_ep).is_null() {
                            a20_net_unix_channel_release(&cv);
                        }
                        *v.connected = 0;
                        *v.peer = ptr::null_mut();
                        rc = qr;
                    } else {
                        a20_net_unix_accept_collect(listener, wake_q.as_mut_ptr());
                    }
                }
            }
        } else {
            /* Datagram and raw: no child, the two ends point at each other. */
            *v.peer = listener;
        }

        if rc == 0 {
            /* SO_PEERCRED: the accepted end's peer is the connecting task; the
             * connecting end's peer is the task that owns the bound socket. */
            let mut pid = 0i32;
            let mut uid = 0i32;
            let mut gid = 0i32;
            if a20_net_unix_creds(&mut pid, &mut uid, &mut gid) == 0 {
                let p = *v.peer;
                if !p.is_null() {
                    let pv = view_of(p);
                    if *pv.connected != 0 {
                        *pv.peer_pid = pid;
                        *pv.peer_uid = uid;
                        *pv.peer_gid = gid;
                    }
                }
                *v.peer_pid = *lv.owner_pid;
                *v.peer_uid = *lv.owner_uid;
                *v.peer_gid = *lv.owner_gid;
            }
        }
    }
    a20_net_unix_unlock2(&mut pair);
    /* Only the reference this function took.  s->peer and child->peer are plain
     * back-pointers with no reference of their own. */
    a20_net_unix_free(listener);
    if !child.is_null() && rc < 0 {
        a20_net_unix_free(child);
    }
    a20_net_unix_wakeq_flush(wake_q.as_mut_ptr());
    rc
}

/* -------------------------------------------------------------------- sendto */

#[no_mangle]
pub unsafe extern "C" fn a20_net_unix_socket_sendto(s: *mut c_void, buf: *const c_void,
                                                    len: usize, addr: *const c_void,
                                                    addrlen: usize,
                                                    files: *mut *mut c_void, nfiles: i32,
                                                    max_path: usize) -> i32 {
    if s.is_null() {
        return -ENOTSOCK;
    }
    let v = view_of(s);

    /* The destination is resolved first, outside any lock: it may come from a
     * whole-table scan, and the reference that scan hands back is what lets this
     * socket's bucket be dropped before the pair is taken. */
    let mut unix_addr = [0u8; NET_SOCKADDR_MAX];
    let mut dst_addr: *const u8 = ptr::null();
    let mut dst_len = 0usize;
    if !addr.is_null() {
        let mut ulen = addrlen;
        let ur = a20_net_unix_sockaddr_prepare(addr, addrlen, unix_addr.as_mut_ptr(),
                                               &mut ulen, ptr::null_mut(), 0, max_path);
        if ur < 0 {
            return ur;
        }
        dst_addr = unix_addr.as_ptr();
        dst_len = ulen;
    }

    let mut src_local = [0u8; NET_SOCKADDR_MAX];
    let mut dst: *mut c_void = ptr::null_mut();
    let mut flags = 0u64;
    a20_net_unix_lock(s, &mut flags);
    if a20_net_unix_is_live(s) == 0 {
        a20_net_unix_unlock(s, flags);
        return -ENOTSOCK;
    }
    if dst_addr.is_null() && *v.connected != 0 {
        /* Copied out rather than pointed at: the search below runs with no lock
         * held, so peer_addr must not be read after this section ends. */
        dst_len = *v.peer_len;
        ptr::copy_nonoverlapping(v.peer_addr, unix_addr.as_mut_ptr(), dst_len);
        dst_addr = unix_addr.as_ptr();
    }
    let src_local_len = *v.local_len;
    ptr::copy_nonoverlapping(v.local, src_local.as_mut_ptr(), src_local_len);
    let sock_type = *v.sock_type;
    let was_connected = *v.ever_connected != 0 || *v.connected != 0;
    let peer = *v.peer;
    let had_peer = !peer.is_null();
    /* Sampled under the lock, so it cannot be freed between the test and the
     * reference below. */
    if !peer.is_null()
        && (sock_type == SOCK_STREAM || sock_type == SOCK_SEQPACKET
            || a20_net_unix_is_live(peer) != 0)
    {
        a20_net_unix_ref(peer);
        dst = peer;
    }
    a20_net_unix_unlock(s, flags);

    if dst.is_null() && had_peer {
        /* The back-pointer is stale: drop it and fall back to a lookup. */
        let mut sf = 0u64;
        a20_net_unix_lock(s, &mut sf);
        if !(*v.peer).is_null() {
            *v.peer = ptr::null_mut();
        }
        a20_net_unix_unlock(s, sf);
    }
    if dst.is_null() && !dst_addr.is_null() {
        dst = a20_net_unix_find_bound(v.net_ns, AF_UNIX, sock_type, dst_addr, dst_len);
    }
    if dst.is_null() {
        if was_connected && (sock_type == SOCK_STREAM || sock_type == SOCK_SEQPACKET) {
            return -EPIPE;
        }
        return if !dst_addr.is_null() { -ECONNREFUSED } else { -EDESTADDRREQ };
    }

    let mut pair = Pair::zero();
    a20_net_unix_lock2(s, dst, &mut pair);
    if a20_net_unix_is_live(dst) == 0 {
        a20_net_unix_unlock2(&mut pair);
        a20_net_unix_free(dst);
        if sock_type == SOCK_STREAM || sock_type == SOCK_SEQPACKET {
            return -EPIPE;
        }
        return if !dst_addr.is_null() { -ECONNREFUSED } else { -EDESTADDRREQ };
    }
    let dv = view_of(dst);
    /* Channel-backed data plane: plain data goes through the internal channel
     * (which has its own locking, so the pair is dropped first).  SCM_RIGHTS
     * messages fall back to the legacy queue below. */
    if !(*dv.ch_ep).is_null() && !(!files.is_null() && nfiles > 0) {
        a20_net_unix_unlock2(&mut pair);
        let cr = ch_send(s, dst, buf, len);
        if cr >= 0 {
            a20_net_unix_read_wake_all(dst);
        }
        a20_net_unix_free(dst);
        return cr;
    }

    /* A stream carries a byte stream, so a write larger than one queued message
     * is split across several (Linux fragments it into skbs); the SCM_RIGHTS fds
     * ride on the final chunk.  On success the enqueued message owns the fd
     * references, on failure the caller still does. */
    let r: i32;
    if sock_type == SOCK_STREAM && len > NET_MAX_PAYLOAD {
        let mut p = buf as *const u8;
        let mut left = len;
        let mut acc = 0i32;
        while left > 0 {
            let n = if left > NET_MAX_PAYLOAD { NET_MAX_PAYLOAD } else { left };
            let rc = if left > n {
                a20_net_unix_enqueue(dst, p, n, src_local.as_ptr(), src_local_len)
            } else {
                a20_net_unix_enqueue_fds(dst, p, n, src_local.as_ptr(), src_local_len,
                                         files, nfiles)
            };
            if rc < 0 {
                if acc == 0 {
                    acc = rc;
                }
                break;
            }
            acc += rc;
            p = p.add(n);
            left -= n;
        }
        r = acc;
    } else {
        r = a20_net_unix_enqueue_fds(dst, buf as *const u8, len, src_local.as_ptr(),
                                     src_local_len, files, nfiles);
    }
    let mut wake_q = WakeQ::new();
    a20_net_unix_wakeq_init(wake_q.as_mut_ptr());
    if r >= 0 {
        a20_net_unix_read_collect(dst, wake_q.as_mut_ptr());
    }
    a20_net_unix_unlock2(&mut pair);
    a20_net_unix_wakeq_flush(wake_q.as_mut_ptr());
    a20_net_unix_free(dst);
    r
}

/* ---------------------------------------------------- channel-backed plane */

/// Chunked send over the internal channel.  The sender must send on its own
/// endpoint, so only the receiver's credentials are stamped and only the
/// receiver's lock is taken; each chunk wakes the reader, because the caller
/// only wakes the read queue once this whole write returns and a writer that
/// blocked part-way would leave bytes the reader needs to drain.
unsafe fn ch_send(s: *mut c_void, dst: *mut c_void, buf: *const c_void,
                  len: usize) -> i32 {
    let sv = view_of(s);
    let dv = view_of(dst);
    let mut flags = 0u64;
    a20_net_unix_lock(dst, &mut flags);
    let mut pid = 0i32;
    let mut uid = 0i32;
    let mut gid = 0i32;
    if a20_net_unix_creds(&mut pid, &mut uid, &mut gid) == 0 {
        *dv.ch_cred_pid = pid;
        *dv.ch_cred_uid = uid;
        *dv.ch_cred_gid = gid;
    }
    a20_net_unix_unlock(dst, flags);

    let ep = *sv.ch_ep;
    let chflags = if *sv.nonblock != 0 { CH_NONBLOCK } else { 0 };
    let mut p = buf as *const u8;
    let mut left = len;
    let mut sent = 0usize;
    while left > 0 {
        let n = if left > CH_MAX_DATA { CH_MAX_DATA } else { left };
        let r = a20_net_unix_channel_send(ep, p, n as u32, chflags);
        if r < 0 {
            let partial = sent as i32;
            if r == -A20_ERR_WOULD_BLOCK {
                return if sent > 0 { partial } else { -EAGAIN };
            }
            if r == -A20_ERR_CANCELED {
                return if sent > 0 { partial } else { -EPIPE };
            }
            if r == -A20_ERR_INTERRUPTED {
                return if sent > 0 { partial } else { -EINTR };
            }
            return if sent > 0 { partial } else { -EIO };
        }
        p = p.add(n);
        left -= n;
        sent += n;
        a20_net_unix_read_wake_all(dst);
    }
    sent as i32
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_unix_ch_send(s: *mut c_void, dst: *mut c_void,
                                              buf: *const c_void, len: usize) -> i32 {
    ch_send(s, dst, buf, len)
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_unix_ch_recv(s: *mut c_void, buf: *mut c_void,
                                              len: usize) -> i32 {
    let v = view_of(s);
    let out = buf as *mut u8;
    let mut total = 0usize;
    while total < len {
        if *v.ch_len > 0 {
            let cl = *v.ch_len as usize;
            let n = if cl < len - total { cl } else { len - total };
            ptr::copy_nonoverlapping((*v.ch_buf) as *const u8, out.add(total), n);
            total += n;
            if n < cl {
                ptr::copy((*v.ch_buf).add(n), *v.ch_buf, cl - n);
            }
            *v.ch_len -= n as u32;
            continue;
        }
        let b = a20_net_unix_chbuf(&v);
        if b.is_null() {
            return if total > 0 { total as i32 } else { -ENOMEM };
        }
        let mut cap = CH_MAX_DATA as u32;
        let r = a20_net_unix_channel_recv(*v.ch_ep, b, &mut cap);
        if r < 0 {
            if total > 0 {
                return total as i32;
            }
            if r == -A20_ERR_WOULD_BLOCK {
                return -EAGAIN;
            }
            /* Peer endpoint released: EOF. */
            if r == -A20_ERR_CANCELED {
                return 0;
            }
            if r == -A20_ERR_INTERRUPTED {
                return -EINTR;
            }
            return -EIO;
        }
        *v.ch_len = cap;
    }
    total as i32
}

/// Peek must not dequeue: one channel message is staged in the stream buffer and
/// left there for the next recv.
#[no_mangle]
pub unsafe extern "C" fn a20_net_unix_ch_peek(s: *mut c_void, buf: *mut c_void,
                                              len: usize) -> i32 {
    if s.is_null() {
        return -EINVAL;
    }
    let v = view_of(s);
    if (*v.ch_ep).is_null() || (buf.is_null() && len != 0) {
        return -EINVAL;
    }
    if len == 0 {
        return 0;
    }
    if *v.ch_len == 0 {
        let b = a20_net_unix_chbuf(&v);
        if b.is_null() {
            return -ENOMEM;
        }
        let mut cap = CH_MAX_DATA as u32;
        let r = a20_net_unix_channel_recv(*v.ch_ep, b, &mut cap);
        if r < 0 {
            if r == -A20_ERR_WOULD_BLOCK {
                return -EAGAIN;
            }
            if r == -A20_ERR_CANCELED {
                return 0;
            }
            if r == -A20_ERR_INTERRUPTED {
                return -EINTR;
            }
            return -EIO;
        }
        *v.ch_len = cap;
    }
    let cl = *v.ch_len as usize;
    let n = if cl < len { cl } else { len };
    ptr::copy_nonoverlapping((*v.ch_buf) as *const u8, buf as *mut u8, n);
    n as i32
}

/* ------------------------------------------------------------- wake queue */

/// proc_wake_q_t is 64 items of (task, seq, reason) plus a count; the size is
/// the caller's, this is only a scratch buffer it initializes and flushes.
#[repr(C, align(16))]
struct WakeQ([u8; 2048]);

impl WakeQ {
    fn new() -> WakeQ {
        WakeQ([0u8; 2048])
    }

    fn as_mut_ptr(&mut self) -> *mut c_void {
        self.0.as_mut_ptr() as *mut c_void
    }
}

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    /// A net_socket_t stand-in with the fields the view borrows, plus the
    /// liveness flag the primitives test.  Everything the primitives would do to
    /// real kernel state is scripted through the T_* slots below, so a test can
    /// drive one failure at a time and assert what was rolled back.
    #[repr(C)]
    struct Sock {
        domain: i32,
        sock_type: i32,
        protocol: i32,
        nonblock: i32,
        bound: i32,
        connected: i32,
        ever_connected: i32,
        listening: i32,
        accept_count: i32,
        passcred: i32,
        owner_pid: i32,
        owner_uid: i32,
        owner_gid: i32,
        peer_pid: i32,
        peer_uid: i32,
        peer_gid: i32,
        ch_cred_pid: i32,
        ch_cred_uid: i32,
        ch_cred_gid: i32,
        live: i32,
        local: [u8; 128],
        local_len: usize,
        peer_addr: [u8; 128],
        peer_len: usize,
        peer: *mut Sock,
        accept_next: *mut Sock,
        ch_ep: *mut c_void,
        ch_buf: *mut i8,
        ch_len: u32,
    }

    impl Sock {
        fn new(t: i32) -> Sock {
            Sock {
                domain: AF_UNIX,
                sock_type: t,
                protocol: 0,
                nonblock: 0,
                bound: 0,
                connected: 0,
                ever_connected: 0,
                listening: 0,
                accept_count: 0,
                passcred: 0,
                owner_pid: 0,
                owner_uid: 0,
                owner_gid: 0,
                peer_pid: 0,
                peer_uid: 0,
                peer_gid: 0,
                ch_cred_pid: 0,
                ch_cred_uid: 0,
                ch_cred_gid: 0,
                live: 1,
                local: [0; 128],
                local_len: 0,
                peer_addr: [0; 128],
                peer_len: 0,
                peer: ptr::null_mut(),
                accept_next: ptr::null_mut(),
                ch_ep: ptr::null_mut(),
                ch_buf: ptr::null_mut(),
                ch_len: 0,
            }
        }

        fn p(&mut self) -> *mut c_void {
            self as *mut Sock as *mut c_void
        }

        fn set_local(&mut self, path: &[u8]) {
            self.local[0] = AF_UNIX as u8;
            self.local[1] = 0;
            self.local[2..2 + path.len()].copy_from_slice(path);
            self.local_len = 2 + path.len() + 1;
        }

        fn set_peer_addr(&mut self, path: &[u8]) {
            self.peer_addr[0] = AF_UNIX as u8;
            self.peer_addr[1] = 0;
            self.peer_addr[2..2 + path.len()].copy_from_slice(path);
            self.peer_len = 2 + path.len() + 1;
        }
    }

    static mut T_CWD: [u8; 64] = [0; 64];
    static mut T_STAT: i32 = -2;
    static mut T_ISDIR: i32 = 0;
    static mut T_CREATE: i32 = 5;
    static mut T_ALLOC: *mut Sock = ptr::null_mut();
    static mut T_ALLOC_CALLS: i32 = 0;
    static mut T_FIND: *mut Sock = ptr::null_mut();
    static mut T_FIND_CALLS: i32 = 0;
    static mut T_PUSH: i32 = 0;
    static mut T_PUSH_CALLS: i32 = 0;
    static mut T_CREDS: i32 = 0;
    static mut T_FREE_CALLS: i32 = 0;
    static mut T_REF_CALLS: i32 = 0;
    static mut T_RELEASE_CALLS: i32 = 0;
    static mut T_LOCK_CALLS: i32 = 0;
    static mut T_LOCK2_CALLS: i32 = 0;
    static mut T_UNLOCK2_CALLS: i32 = 0;
    static mut T_WAKE_ALL: i32 = 0;
    static mut T_READ_COLLECT: i32 = 0;
    static mut T_ACCEPT_COLLECT: i32 = 0;
    static mut T_WAKEQ_FLUSH: i32 = 0;
    static mut T_CLOSE_FD: i32 = -99;
    static mut T_LAST_PATH: [u8; 128] = [0; 128];
    static mut T_ENQ_RC: i32 = 0;
    static mut T_ENQ_CALLS: i32 = 0;
    static mut T_ENQ_LENS: [usize; 8] = [0; 8];
    static mut T_ENQ_FDS: [i32; 8] = [0; 8];
    static mut T_CH_SEND_SCRIPT: [i64; 4] = [0; 4];
    static mut T_CH_SEND_AT: usize = 0;
    static mut T_CH_SEND_CALLS: i32 = 0;
    static mut T_CH_SEND_LENS: [u32; 4] = [0; 4];
    static mut T_CH_RECV_SCRIPT: [(i64, u32); 4] = [(0, 0); 4];
    static mut T_CH_RECV_AT: usize = 0;
    static mut T_CH_RECV_CALLS: i32 = 0;
    static mut T_CHBUF: [u8; 65536] = [0; 65536];

    /// The scripted slots above are one global set, so the harness must run one
    /// test at a time.  The guard releases on drop, including on a panic.
    static T_SERIAL: core::sync::atomic::AtomicBool =
        core::sync::atomic::AtomicBool::new(false);

    struct Serial;

    impl Drop for Serial {
        fn drop(&mut self) {
            T_SERIAL.store(false, core::sync::atomic::Ordering::Release);
        }
    }

    fn serial() -> Serial {
        while T_SERIAL.swap(true, core::sync::atomic::Ordering::Acquire) {
            core::hint::spin_loop();
        }
        Serial
    }

    unsafe fn reset() {
        T_CWD = [0; 64];
        T_CWD[0] = b'/';
        T_STAT = -2;
        T_ISDIR = 0;
        T_CREATE = 5;
        T_ALLOC = ptr::null_mut();
        T_ALLOC_CALLS = 0;
        T_FIND = ptr::null_mut();
        T_FIND_CALLS = 0;
        T_PUSH = 0;
        T_PUSH_CALLS = 0;
        T_CREDS = 0;
        T_FREE_CALLS = 0;
        T_REF_CALLS = 0;
        T_RELEASE_CALLS = 0;
        T_LOCK_CALLS = 0;
        T_LOCK2_CALLS = 0;
        T_UNLOCK2_CALLS = 0;
        T_WAKE_ALL = 0;
        T_READ_COLLECT = 0;
        T_ACCEPT_COLLECT = 0;
        T_WAKEQ_FLUSH = 0;
        T_CLOSE_FD = -99;
        T_LAST_PATH = [0; 128];
        T_ENQ_RC = 0;
        T_ENQ_CALLS = 0;
        T_ENQ_LENS = [0; 8];
        T_ENQ_FDS = [0; 8];
        T_CH_SEND_SCRIPT = [0; 4];
        T_CH_SEND_AT = 0;
        T_CH_SEND_CALLS = 0;
        T_CH_SEND_LENS = [0; 4];
        T_CH_RECV_SCRIPT = [(0, 0); 4];
        T_CH_RECV_AT = 0;
        T_CH_RECV_CALLS = 0;
        T_CHBUF = [0; 65536];
    }

    unsafe fn record_path(p: *const i8) {
        let n = slen(p, 127);
        T_LAST_PATH = [0; 128];
        ptr::copy_nonoverlapping(p as *const u8, T_LAST_PATH.as_mut_ptr(), n);
    }

    fn cstr(s: &str) -> &[u8] {
        s.as_bytes()
    }

    /// AF_UNIX sockaddr with `path` (and the trailing NUL a real caller sends).
    fn sa(path: &[u8]) -> ([u8; 128], usize) {
        let mut a = [0u8; 128];
        a[0] = AF_UNIX as u8;
        a[1] = 0;
        a[2..2 + path.len()].copy_from_slice(path);
        (a, 2 + path.len() + 1)
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_view_init(s: *mut c_void, out: *mut UnixView) {
        let k = &mut *(s as *mut Sock);
        *out = UnixView {
            s,
            net_ns: 1usize as *mut c_void,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            protocol: &mut k.protocol,
            nonblock: &mut k.nonblock,
            bound: &mut k.bound,
            connected: &mut k.connected,
            ever_connected: &mut k.ever_connected,
            listening: &mut k.listening,
            accept_count: &mut k.accept_count,
            passcred: &mut k.passcred,
            owner_pid: &mut k.owner_pid,
            owner_uid: &mut k.owner_uid,
            owner_gid: &mut k.owner_gid,
            peer_pid: &mut k.peer_pid,
            peer_uid: &mut k.peer_uid,
            peer_gid: &mut k.peer_gid,
            ch_cred_pid: &mut k.ch_cred_pid,
            ch_cred_uid: &mut k.ch_cred_uid,
            ch_cred_gid: &mut k.ch_cred_gid,
            local: k.local.as_mut_ptr(),
            local_len: &mut k.local_len,
            peer_addr: k.peer_addr.as_mut_ptr(),
            peer_len: &mut k.peer_len,
            peer: &mut k.peer as *mut *mut Sock as *mut *mut c_void,
            accept_next: &mut k.accept_next as *mut *mut Sock as *mut *mut c_void,
            ch_ep: &mut k.ch_ep,
            ch_buf: &mut k.ch_buf,
            ch_len: &mut k.ch_len,
        };
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_lock(_s: *mut c_void, _flags: *mut u64) {
        T_LOCK_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_unlock(_s: *mut c_void, _flags: u64) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_lock2(a: *mut c_void, b: *mut c_void,
                                            pair: *mut Pair) {
        T_LOCK2_CALLS += 1;
        (*pair).lo = a;
        (*pair).hi = b;
        (*pair).flags = 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_unlock2(pair: *mut Pair) {
        T_UNLOCK2_CALLS += 1;
        assert_eq!((*pair).flags, 1);
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_is_live(s: *mut c_void) -> i32 {
        (*(s as *const Sock)).live
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_ref(_s: *mut c_void) {
        T_REF_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_free(_s: *mut c_void) {
        T_FREE_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_alloc() -> *mut c_void {
        T_ALLOC_CALLS += 1;
        T_ALLOC as *mut c_void
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_find_bound(_ns: *mut c_void, _domain: i32,
                                                  _t: i32, _addr: *const u8,
                                                  _len: usize) -> *mut c_void {
        T_FIND_CALLS += 1;
        T_FIND as *mut c_void
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_creds(pid: *mut i32, uid: *mut i32,
                                            gid: *mut i32) -> i32 {
        if T_CREDS != 0 {
            return -1;
        }
        *pid = 1000;
        *uid = 1001;
        *gid = 1002;
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_cwd(out: *mut u8, outsz: usize) {
        let n = slen(T_CWD.as_ptr() as *const i8, 63);
        copy_cstr(out, outsz, &T_CWD[..n]);
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_path_is_dir(path: *const i8) -> i32 {
        record_path(path);
        T_ISDIR
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_path_stat(path: *const i8) -> i32 {
        record_path(path);
        T_STAT
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_path_create(path: *const i8) -> i32 {
        record_path(path);
        T_CREATE
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_path_close(fd: i32) {
        T_CLOSE_FD = fd;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_accept_push(_l: *mut c_void,
                                                  _c: *mut c_void) -> i32 {
        T_PUSH_CALLS += 1;
        T_PUSH
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_accept_collect(_l: *mut c_void,
                                                     _w: *mut c_void) -> i32 {
        T_ACCEPT_COLLECT += 1;
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_read_collect(_s: *mut c_void,
                                                   _w: *mut c_void) -> i32 {
        T_READ_COLLECT += 1;
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_read_wake_all(_s: *mut c_void) {
        T_WAKE_ALL += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_channel_release(v: *const UnixView) {
        T_RELEASE_CALLS += 1;
        *(*v).ch_ep = ptr::null_mut();
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_channel_send(_ep: *mut c_void, _buf: *const u8,
                                                   len: u32, _flags: u32) -> i64 {
        let at = T_CH_SEND_AT;
        if at < 4 {
            T_CH_SEND_LENS[at] = len;
            T_CH_SEND_AT += 1;
        }
        T_CH_SEND_CALLS += 1;
        if at < 4 { T_CH_SEND_SCRIPT[at] } else { T_CH_SEND_SCRIPT[3] }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_channel_recv(_ep: *mut c_void, buf: *mut u8,
                                                   cap: *mut u32) -> i64 {
        let at = T_CH_RECV_AT;
        T_CH_RECV_CALLS += 1;
        if at >= 4 {
            return -A20_ERR_WOULD_BLOCK;
        }
        T_CH_RECV_AT += 1;
        let (rc, len) = T_CH_RECV_SCRIPT[at];
        if rc == 0 {
            for i in 0..len as usize {
                *buf.add(i) = (i as u8) + 1;
            }
            *cap = len;
        }
        rc
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_chbuf(v: *const UnixView) -> *mut u8 {
        if (*(*v).ch_buf).is_null() {
            *(*v).ch_buf = T_CHBUF.as_mut_ptr() as *mut i8;
        }
        *(*v).ch_buf as *mut u8
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_enqueue(_d: *mut c_void, _b: *const u8,
                                              len: usize, _a: *const u8,
                                              _al: usize) -> i32 {
        if (T_ENQ_CALLS as usize) < 8 {
            T_ENQ_LENS[T_ENQ_CALLS as usize] = len;
            T_ENQ_FDS[T_ENQ_CALLS as usize] = 0;
        }
        T_ENQ_CALLS += 1;
        T_ENQ_RC
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_enqueue_fds(_d: *mut c_void, _b: *const u8,
                                                  len: usize, _a: *const u8,
                                                  _al: usize, _f: *mut *mut c_void,
                                                  nfiles: i32) -> i32 {
        if (T_ENQ_CALLS as usize) < 8 {
            T_ENQ_LENS[T_ENQ_CALLS as usize] = len;
            T_ENQ_FDS[T_ENQ_CALLS as usize] = nfiles;
        }
        T_ENQ_CALLS += 1;
        T_ENQ_RC
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_wakeq_init(_w: *mut c_void) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_unix_wakeq_flush(_w: *mut c_void) {
        T_WAKEQ_FLUSH += 1;
    }

    /* ------------------------------------------------ address preparation */

    #[test]
    fn prepare_rejects_bad_lengths_and_a_foreign_family() {
        unsafe {
            let _serial = serial();
            reset();
            let mut out = [0u8; 128];
            let mut olen = 0usize;
            let mut a = [0u8; 128];
            assert_eq!(a20_net_unix_sockaddr_prepare(a.as_ptr() as *const c_void, 1,
                        out.as_mut_ptr(), &mut olen, ptr::null_mut(), 0, 512), -EINVAL);
            assert_eq!(a20_net_unix_sockaddr_prepare(a.as_ptr() as *const c_void, 129,
                        out.as_mut_ptr(), &mut olen, ptr::null_mut(), 0, 512), -EINVAL);
            a[0] = 2; /* AF_INET */
            assert_eq!(a20_net_unix_sockaddr_prepare(a.as_ptr() as *const c_void, 4,
                        out.as_mut_ptr(), &mut olen, ptr::null_mut(), 0, 512),
                       -EAFNOSUPPORT);
            /* Family only: an unnamed address is accepted and left alone. */
            a[0] = AF_UNIX as u8;
            a[1] = 0;
            assert_eq!(a20_net_unix_sockaddr_prepare(a.as_ptr() as *const c_void, 2,
                        out.as_mut_ptr(), &mut olen, ptr::null_mut(), 0, 512), 0);
            assert_eq!(olen, 2);
        }
    }

    #[test]
    fn prepare_absolutizes_and_reports_a_name_too_long() {
        unsafe {
            let _serial = serial();
            reset();
            let mut out = [0u8; 128];
            let mut olen = 0usize;
            let mut path_out = [0i8; 64];
            T_CWD = [0; 64];
            let cwd = cstr("/home/u");
            ptr::copy_nonoverlapping(cwd.as_ptr(), T_CWD.as_mut_ptr(), cwd.len());

            let (a, alen) = sa(cstr("s"));
            assert_eq!(a20_net_unix_sockaddr_prepare(a.as_ptr() as *const c_void, alen,
                        out.as_mut_ptr(), &mut olen, path_out.as_mut_ptr(), 64, 512), 0);
            assert_eq!(olen, 2 + cstr("/home/u/s").len() + 1);
            assert_eq!(&out[2..olen], cstr("/home/u/s\0"));
            assert_eq!(&out[0..2], &[AF_UNIX as u8, 0]);
            assert_eq!(&path_out[..10], &[47, 104, 111, 109, 101, 47, 117, 47, 115, 0]);

            /* Root cwd joins without doubling the slash. */
            T_CWD = [0; 64];
            T_CWD[0] = b'/';
            let (a, alen) = sa(cstr("s"));
            assert_eq!(a20_net_unix_sockaddr_prepare(a.as_ptr() as *const c_void, alen,
                        out.as_mut_ptr(), &mut olen, ptr::null_mut(), 0, 512), 0);
            assert_eq!(&out[2..olen], cstr("/s\0"));

            /* An absolute path is copied, not joined. */
            T_CWD = [0; 64];
            let cwd = cstr("/home/u");
            ptr::copy_nonoverlapping(cwd.as_ptr(), T_CWD.as_mut_ptr(), cwd.len());
            let (a, alen) = sa(cstr("/tmp/x"));
            assert_eq!(a20_net_unix_sockaddr_prepare(a.as_ptr() as *const c_void, alen,
                        out.as_mut_ptr(), &mut olen, ptr::null_mut(), 0, 512), 0);
            assert_eq!(&out[2..olen], cstr("/tmp/x\0"));

            /* A relative name that only overflows once joined is refused: 60-char
             * cwd plus a 100-char name is 161 bytes, past NET_SOCKADDR_MAX. */
            T_CWD = [0; 64];
            for i in 0..60 { T_CWD[i] = b'a'; }
            let mut rel = [b'b'; 100];
            let (a, alen) = sa(&rel);
            assert_eq!(alen, 2 + 100 + 1);
            assert_eq!(a20_net_unix_sockaddr_prepare(a.as_ptr() as *const c_void, alen,
                        out.as_mut_ptr(), &mut olen, ptr::null_mut(), 0, 512),
                       -ENAMETOOLONG);
            /* The raw name still fits, which is why the length test is on the
             * joined result and not on the caller's addrlen. */
            rel[0] = b'/';
            let (a, alen) = sa(&rel);
            assert_eq!(a20_net_unix_sockaddr_prepare(a.as_ptr() as *const c_void, alen,
                        out.as_mut_ptr(), &mut olen, ptr::null_mut(), 0, 512), 0);

            /* max_path is the caller's MAX_PATH_LEN: a small profile truncates
             * where the large one would not, exactly as the C version did. */
            T_CWD = [0; 64];
            T_CWD[0] = b'/';
            let (a, alen) = sa(cstr("abcdefghij"));
            assert_eq!(a20_net_unix_sockaddr_prepare(a.as_ptr() as *const c_void, alen,
                        out.as_mut_ptr(), &mut olen, ptr::null_mut(), 0, 8), 0);
            assert_eq!(&out[2..olen], cstr("/abcdef\0"));
        }
    }

    #[test]
    fn parent_ok_requires_a_directory() {
        unsafe {
            let _serial = serial();
            reset();
            T_ISDIR = 0;
            assert_eq!(a20_net_unix_path_parent_ok(cstr("/tmp/x\0").as_ptr() as *const i8,
                                                  512), 0);
            assert_eq!(&T_LAST_PATH[..5], cstr("/tmp\0"));
            T_ISDIR = -ENOTDIR;
            assert_eq!(a20_net_unix_path_parent_ok(cstr("/tmp/x\0").as_ptr() as *const i8,
                                                  512), -ENOTDIR);
            /* The parent of a root-level name is the root itself. */
            T_ISDIR = 0;
            assert_eq!(a20_net_unix_path_parent_ok(cstr("/x\0").as_ptr() as *const i8,
                                                  512), 0);
            assert_eq!(&T_LAST_PATH[..2], cstr("/\0"));
            /* Empty path is accepted without a stat. */
            T_LAST_PATH = [0; 128];
            assert_eq!(a20_net_unix_path_parent_ok(cstr("\0").as_ptr() as *const i8,
                                                  512), 0);
            assert_eq!(T_LAST_PATH[0], 0);
        }
    }

    /* ----------------------------------------------------------------- bind */

    #[test]
    fn bind_publishes_the_address_and_creates_the_node() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new(SOCK_STREAM);
            let (a, alen) = sa(cstr("/tmp/sock"));
            assert_eq!(a20_net_unix_socket_bind(s.p(), a.as_ptr() as *const c_void, alen,
                                               512), 0);
            assert_eq!(s.bound, 1);
            assert_eq!(s.local_len, 2 + cstr("/tmp/sock").len() + 1);
            assert_eq!(&s.local[2..s.local_len], cstr("/tmp/sock\0"));
            assert_eq!(s.owner_pid, 1000);
            assert_eq!((s.owner_uid, s.owner_gid), (1001, 1002));
            assert_eq!(T_CLOSE_FD, 5);
            assert_eq!(T_FIND_CALLS, 1);
            /* The parent directory was checked and the node path stat'd first. */
            assert_eq!(&T_LAST_PATH[..9], cstr("/tmp/sock"));
        }
    }

    #[test]
    fn bind_rolls_the_address_back_when_the_node_cannot_be_created() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new(SOCK_STREAM);
            T_CREATE = -EEXIST;
            let (a, alen) = sa(cstr("/tmp/sock"));
            assert_eq!(a20_net_unix_socket_bind(s.p(), a.as_ptr() as *const c_void, alen,
                                               512), -EADDRINUSE);
            assert_eq!((s.bound, s.local_len), (0, 0));
            assert_eq!(T_CLOSE_FD, -99);

            /* A non-EEXIST failure is passed through and still rolled back. */
            reset();
            let mut s = Sock::new(SOCK_STREAM);
            T_CREATE = -ENOMEM;
            assert_eq!(a20_net_unix_socket_bind(s.p(), a.as_ptr() as *const c_void, alen,
                                               512), -ENOMEM);
            assert_eq!((s.bound, s.local_len), (0, 0));

            /* A name that already exists in the filesystem is refused before the
             * table is touched, and the address is never published. */
            reset();
            let mut s = Sock::new(SOCK_STREAM);
            T_STAT = 0;
            assert_eq!(a20_net_unix_socket_bind(s.p(), a.as_ptr() as *const c_void, alen,
                                               512), -EADDRINUSE);
            assert_eq!((s.bound, s.local_len, T_FIND_CALLS, T_LOCK_CALLS), (0, 0, 0, 0));
        }
    }

    #[test]
    fn bind_refuses_a_table_clash_and_a_dead_socket() {
        unsafe {
            let _serial = serial();
            reset();
            let mut other = Sock::new(SOCK_STREAM);
            let mut s = Sock::new(SOCK_STREAM);
            T_FIND = &mut other;
            let (a, alen) = sa(cstr("/tmp/sock"));
            assert_eq!(a20_net_unix_socket_bind(s.p(), a.as_ptr() as *const c_void, alen,
                                               512), -EADDRINUSE);
            assert_eq!(T_FREE_CALLS, 1);
            assert_eq!(s.bound, 0);

            reset();
            let mut s = Sock::new(SOCK_STREAM);
            s.live = 0;
            assert_eq!(a20_net_unix_socket_bind(s.p(), a.as_ptr() as *const c_void, alen,
                                               512), -ENOTSOCK);
            assert_eq!((s.bound, s.local_len), (0, 0));
        }
    }

    /* -------------------------------------------------------------- connect */

    #[test]
    fn connect_wires_the_child_the_backlog_and_the_peer_credentials() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new(SOCK_STREAM);
            listener.listening = 1;
            listener.passcred = 1;
            listener.owner_pid = 77;
            listener.owner_uid = 78;
            listener.owner_gid = 79;
            listener.set_local(cstr("/tmp/sock"));
            let mut child = Sock::new(SOCK_STREAM);
            let mut s = Sock::new(SOCK_STREAM);
            s.set_local(cstr("/tmp/cli"));
            T_FIND = &mut listener;
            T_ALLOC = &mut child;

            let (a, alen) = sa(cstr("/tmp/sock"));
            assert_eq!(a20_net_unix_socket_connect(s.p(), a.as_ptr() as *const c_void,
                        alen, 512, 4), 0);
            assert_eq!((s.connected, s.ever_connected), (1, 1));
            assert_eq!(s.peer, &mut child as *mut Sock);
            assert_eq!(&s.peer_addr[2..s.peer_len], cstr("/tmp/sock\0"));
            assert_eq!(child.peer, &mut s as *mut Sock);
            assert_eq!(child.domain, AF_UNIX);
            assert_eq!((child.bound, child.connected, child.ever_connected), (1, 1, 1));
            assert_eq!(child.passcred, 1);
            assert_eq!(&child.local[2..child.local_len], cstr("/tmp/sock\0"));
            assert_eq!(&child.peer_addr[2..child.peer_len], cstr("/tmp/cli\0"));
            assert_eq!(T_PUSH_CALLS, 1);
            assert_eq!(T_ACCEPT_COLLECT, 1);
            assert_eq!(T_WAKEQ_FLUSH, 1);
            /* The listener reference from the scan is dropped; the child's is
             * kept because the backlog owns it now. */
            assert_eq!(T_FREE_CALLS, 1);
            /* SO_PEERCRED: the accepted end reports the connecting task, the
             * connecting end reports the bound socket's owner. */
            assert_eq!((child.peer_pid, child.peer_uid, child.peer_gid),
                       (1000, 1001, 1002));
            assert_eq!((s.peer_pid, s.peer_uid, s.peer_gid), (77, 78, 79));
        }
    }

    #[test]
    fn connect_refuses_a_full_backlog_and_a_non_listener() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new(SOCK_STREAM);
            listener.listening = 1;
            listener.accept_count = 4;
            listener.set_local(cstr("/tmp/sock"));
            let mut s = Sock::new(SOCK_STREAM);
            s.set_local(cstr("/tmp/cli"));
            T_FIND = &mut listener;
            let (a, alen) = sa(cstr("/tmp/sock"));
            assert_eq!(a20_net_unix_socket_connect(s.p(), a.as_ptr() as *const c_void,
                        alen, 512, 4), -EAGAIN);
            assert_eq!(s.connected, 0);
            assert_eq!(T_ALLOC_CALLS, 0);
            assert_eq!(T_FREE_CALLS, 1);

            reset();
            let mut listener = Sock::new(SOCK_STREAM);
            listener.listening = 0;
            listener.set_local(cstr("/tmp/sock"));
            let mut s = Sock::new(SOCK_STREAM);
            T_FIND = &mut listener;
            assert_eq!(a20_net_unix_socket_connect(s.p(), a.as_ptr() as *const c_void,
                        alen, 512, 4), -ECONNREFUSED);
            assert_eq!(s.connected, 0);
            assert_eq!(T_ALLOC_CALLS, 0);
        }
    }

    #[test]
    fn connect_rolls_the_pairing_back_when_the_backlog_push_fails() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new(SOCK_STREAM);
            listener.listening = 1;
            listener.set_local(cstr("/tmp/sock"));
            let mut child = Sock::new(SOCK_STREAM);
            let mut s = Sock::new(SOCK_STREAM);
            s.set_local(cstr("/tmp/cli"));
            s.ch_ep = 8usize as *mut c_void;
            child.ch_ep = 9usize as *mut c_void;
            T_FIND = &mut listener;
            T_ALLOC = &mut child;
            T_PUSH = -ENOMEM;

            let (a, alen) = sa(cstr("/tmp/sock"));
            assert_eq!(a20_net_unix_socket_connect(s.p(), a.as_ptr() as *const c_void,
                        alen, 512, 4), -ENOMEM);
            assert_eq!((s.connected, s.ever_connected), (0, 1));
            assert!(s.peer.is_null());
            assert!(child.peer.is_null() || child.peer == &mut s as *mut Sock);
            /* Both channel endpoints are released and both fields cleared, and
             * the child the push did not take is freed. */
            assert_eq!(T_RELEASE_CALLS, 2);
            assert!(s.ch_ep.is_null() && child.ch_ep.is_null());
            assert_eq!(T_FREE_CALLS, 2);
            assert_eq!(T_ACCEPT_COLLECT, 0);
            assert_eq!(T_WAKEQ_FLUSH, 1);
        }
    }

    #[test]
    fn connect_a_datagram_peer_points_at_the_listener_without_a_child() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new(2);
            listener.listening = 1;
            listener.owner_pid = 77;
            listener.set_local(cstr("/tmp/dgram"));
            let mut s = Sock::new(2);
            s.set_local(cstr("/tmp/cli"));
            T_FIND = &mut listener;
            let (a, alen) = sa(cstr("/tmp/dgram"));
            assert_eq!(a20_net_unix_socket_connect(s.p(), a.as_ptr() as *const c_void,
                        alen, 512, 4), 0);
            assert_eq!(s.peer, &mut listener as *mut Sock);
            assert_eq!(T_ALLOC_CALLS, 0);
            assert_eq!(T_PUSH_CALLS, 0);
            assert_eq!(T_FREE_CALLS, 1);
            assert_eq!(s.peer_pid, 77);
        }
    }

    #[test]
    fn connect_without_a_listener_is_refused_before_any_lock() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new(SOCK_STREAM);
            let (a, alen) = sa(cstr("/tmp/sock"));
            assert_eq!(a20_net_unix_socket_connect(s.p(), a.as_ptr() as *const c_void,
                        alen, 512, 4), -ECONNREFUSED);
            assert_eq!(T_LOCK2_CALLS, 0);
            assert_eq!(T_WAKEQ_FLUSH, 0);
        }
    }

    /* --------------------------------------------------------------- sendto */

    #[test]
    fn sendto_needs_a_destination() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new(SOCK_STREAM);
            assert_eq!(a20_net_unix_socket_sendto(s.p(), cstr("hi").as_ptr() as *const c_void,
                        2, ptr::null(), 0, ptr::null_mut(), 0, 512), -EDESTADDRREQ);
            s.live = 0;
            let (a, alen) = sa(cstr("/tmp/sock"));
            assert_eq!(a20_net_unix_socket_sendto(s.p(), cstr("hi").as_ptr() as *const c_void,
                        2, a.as_ptr() as *const c_void, alen, ptr::null_mut(), 0, 512),
                       -ENOTSOCK);
        }
    }

    #[test]
    fn sendto_drops_a_stale_peer_and_falls_back_to_the_table() {
        unsafe {
            let _serial = serial();
            reset();
            let mut dead = Sock::new(2);
            dead.live = 0;
            let mut dst = Sock::new(2);
            let mut s = Sock::new(2);
            s.connected = 1;
            s.set_peer_addr(cstr("/tmp/dgram"));
            s.peer = &mut dead;
            T_FIND = &mut dst;

            assert_eq!(a20_net_unix_socket_sendto(s.p(), cstr("hi").as_ptr() as *const c_void,
                        2, ptr::null(), 0, ptr::null_mut(), 0, 512), 0);
            assert!(s.peer.is_null());
            assert_eq!(T_FIND_CALLS, 1);
            assert_eq!(T_ENQ_CALLS, 1);
            assert_eq!(T_FREE_CALLS, 1);
            assert_eq!(T_READ_COLLECT, 1);
        }
    }

    #[test]
    fn sendto_a_stream_socket_with_a_dead_peer_is_broken_pipe() {
        unsafe {
            let _serial = serial();
            reset();
            let mut dead = Sock::new(SOCK_STREAM);
            dead.live = 0;
            let mut s = Sock::new(SOCK_STREAM);
            s.connected = 1;
            s.set_peer_addr(cstr("/tmp/sock"));
            s.peer = &mut dead;

            assert_eq!(a20_net_unix_socket_sendto(s.p(), cstr("hi").as_ptr() as *const c_void,
                        2, ptr::null(), 0, ptr::null_mut(), 0, 512), -EPIPE);
            assert_eq!(T_FREE_CALLS, 1);
            assert_eq!(T_ENQ_CALLS, 0);
        }
    }

    #[test]
    fn sendto_fragments_a_long_stream_write_and_puts_the_fds_on_the_last_chunk() {
        unsafe {
            let _serial = serial();
            reset();
            let mut dst = Sock::new(SOCK_STREAM);
            let mut s = Sock::new(SOCK_STREAM);
            T_FIND = &mut dst;
            let mut files = [ptr::null_mut(); 1];
            let len = NET_MAX_PAYLOAD * 2 + 5;
            let buf = [7u8; NET_MAX_PAYLOAD * 2 + 5];
            let (a, alen) = sa(cstr("/tmp/sock"));

            assert_eq!(a20_net_unix_socket_sendto(s.p(), buf.as_ptr() as *const c_void,
                        len, a.as_ptr() as *const c_void, alen, files.as_mut_ptr(), 1,
                        512), 0);
            assert_eq!(T_ENQ_CALLS, 3);
            assert_eq!(T_ENQ_LENS[..3],
                       [NET_MAX_PAYLOAD, NET_MAX_PAYLOAD, 5]);
            assert_eq!(T_ENQ_FDS[..3], [0, 0, 1]);
            assert_eq!(T_LOCK2_CALLS, 1);
            assert_eq!(T_FREE_CALLS, 1);
        }
    }

    #[test]
    fn sendto_a_failed_fragment_returns_the_first_error_and_stops() {
        unsafe {
            let _serial = serial();
            reset();
            let mut dst = Sock::new(SOCK_STREAM);
            let mut s = Sock::new(SOCK_STREAM);
            T_FIND = &mut dst;
            T_ENQ_RC = -EAGAIN;
            let len = NET_MAX_PAYLOAD * 2 + 5;
            let buf = [7u8; NET_MAX_PAYLOAD * 2 + 5];
            let (a, alen) = sa(cstr("/tmp/sock"));

            assert_eq!(a20_net_unix_socket_sendto(s.p(), buf.as_ptr() as *const c_void,
                        len, a.as_ptr() as *const c_void, alen, ptr::null_mut(), 0, 512),
                       -EAGAIN);
            assert_eq!(T_ENQ_CALLS, 1);
            /* Nothing was enqueued, so the reader is not woken. */
            assert_eq!(T_READ_COLLECT, 0);
        }
    }

    #[test]
    fn sendto_prefers_the_channel_and_stamps_the_receivers_credentials() {
        unsafe {
            let _serial = serial();
            reset();
            let mut dst = Sock::new(SOCK_STREAM);
            dst.ch_ep = 7usize as *mut c_void;
            let mut s = Sock::new(SOCK_STREAM);
            s.ch_ep = 6usize as *mut c_void;
            s.peer = &mut dst;
            s.connected = 1;
            T_CH_SEND_SCRIPT[0] = 0;

            let n = a20_net_unix_socket_sendto(s.p(), cstr("hello").as_ptr() as *const c_void,
                        5, ptr::null(), 0, ptr::null_mut(), 0, 512);
            assert_eq!(n, 5);
            assert_eq!((dst.ch_cred_pid, dst.ch_cred_uid, dst.ch_cred_gid),
                       (1000, 1001, 1002));
            assert_eq!(T_ENQ_CALLS, 0);
            assert_eq!(T_REF_CALLS, 1);
            assert_eq!(T_FREE_CALLS, 1);
            /* One wake per delivered chunk from the send, one from sendto. */
            assert_eq!(T_WAKE_ALL, 2);
        }
    }

    /* ---------------------------------------------------- channel data plane */

    #[test]
    fn channel_send_chunks_and_reports_a_partial_count() {
        unsafe {
            let _serial = serial();
            reset();
            let mut dst = Sock::new(SOCK_STREAM);
            let mut s = Sock::new(SOCK_STREAM);
            s.ch_ep = 6usize as *mut c_void;
            s.nonblock = 1;
            T_CH_SEND_SCRIPT[0] = 0;
            T_CH_SEND_SCRIPT[1] = -A20_ERR_WOULD_BLOCK;
            let len = CH_MAX_DATA + 3;
            let buf = [3u8; CH_MAX_DATA + 3];
            assert_eq!(a20_net_unix_ch_send(s.p(), dst.p(), buf.as_ptr() as *const c_void,
                        len), CH_MAX_DATA as i32);
            assert_eq!(T_CH_SEND_CALLS, 2);
            assert_eq!(T_CH_SEND_LENS[..2], [CH_MAX_DATA as u32, 3]);
            assert_eq!(T_WAKE_ALL, 1);

            /* A canceled peer on the first chunk is EPIPE, not a partial count. */
            reset();
            let mut dst = Sock::new(SOCK_STREAM);
            let mut s = Sock::new(SOCK_STREAM);
            s.ch_ep = 6usize as *mut c_void;
            T_CH_SEND_SCRIPT[0] = -A20_ERR_CANCELED;
            assert_eq!(a20_net_unix_ch_send(s.p(), dst.p(), cstr("x").as_ptr() as *const c_void,
                        1), -EPIPE);
            assert_eq!(T_WAKE_ALL, 0);
        }
    }

    #[test]
    fn channel_recv_stages_across_calls_and_reports_eof() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new(SOCK_STREAM);
            s.ch_ep = 6usize as *mut c_void;
            T_CH_RECV_SCRIPT[0] = (0, 8);
            let mut out = [0u8; 8];
            assert_eq!(a20_net_unix_ch_recv(s.p(), out.as_mut_ptr() as *mut c_void, 4), 4);
            assert_eq!((s.ch_len, T_CH_RECV_CALLS), (4, 1));
            assert_eq!(out[..4], [1, 2, 3, 4]);
            /* The staged remainder is served without touching the channel. */
            assert_eq!(a20_net_unix_ch_recv(s.p(), out.as_mut_ptr() as *mut c_void, 4), 4);
            assert_eq!(out[..4], [5, 6, 7, 8]);
            assert_eq!((s.ch_len, T_CH_RECV_CALLS), (0, 1));
            /* Empty channel. */
            assert_eq!(a20_net_unix_ch_recv(s.p(), out.as_mut_ptr() as *mut c_void, 4),
                       -EAGAIN);
            /* Released peer endpoint is EOF, not an error. */
            reset();
            let mut s = Sock::new(SOCK_STREAM);
            s.ch_ep = 6usize as *mut c_void;
            T_CH_RECV_SCRIPT[0] = (-A20_ERR_CANCELED, 0);
            assert_eq!(a20_net_unix_ch_recv(s.p(), out.as_mut_ptr() as *mut c_void, 4), 0);
        }
    }

    #[test]
    fn channel_peek_does_not_consume_the_staged_message() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new(SOCK_STREAM);
            s.ch_ep = 6usize as *mut c_void;
            T_CH_RECV_SCRIPT[0] = (0, 8);
            let mut out = [0u8; 8];
            assert_eq!(a20_net_unix_ch_peek(s.p(), out.as_mut_ptr() as *mut c_void, 4), 4);
            assert_eq!((s.ch_len, T_CH_RECV_CALLS), (8, 1));
            assert_eq!(out[..4], [1, 2, 3, 4]);
            assert_eq!(a20_net_unix_ch_peek(s.p(), out.as_mut_ptr() as *mut c_void, 4), 4);
            assert_eq!((s.ch_len, T_CH_RECV_CALLS), (8, 1));
            /* Zero length is a no-op, and a socket with no channel is invalid. */
            assert_eq!(a20_net_unix_ch_peek(s.p(), out.as_mut_ptr() as *mut c_void, 0), 0);
            s.ch_ep = ptr::null_mut();
            assert_eq!(a20_net_unix_ch_peek(s.p(), out.as_mut_ptr() as *mut c_void, 4),
                       -EINVAL);
            assert_eq!(a20_net_unix_ch_peek(ptr::null_mut(), out.as_mut_ptr() as *mut c_void,
                        4), -EINVAL);
        }
    }
}

