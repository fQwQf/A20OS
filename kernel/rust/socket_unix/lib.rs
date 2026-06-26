#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::cmp;
use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use ffi::{
    kstat_t, net_socket_t, AF_UNIX, EADDRINUSE, EAFNOSUPPORT, EAGAIN, ECONNREFUSED,
    EDESTADDRREQ, EEXIST, EINVAL, ENAMETOOLONG, ENOMEM, ENOTDIR, ENOTSOCK, MAX_PATH_LEN,
    NET_SOCKADDR_MAX, O_CREAT, O_EXCL, O_RDWR, S_IFDIR, S_IFMT, SOCK_SEQPACKET,
    SOCK_STREAM,
};

const FAMILY_SIZE: usize = core::mem::size_of::<u16>();
const UNIX_CREATE_MODE: c_int = 0o777;

#[inline]
unsafe fn read_u16_ne(ptr_u8: *const u8) -> u16 {
    unsafe { u16::from_ne_bytes([*ptr_u8, *ptr_u8.add(1)]) }
}

#[inline]
unsafe fn write_u16_ne(ptr_u8: *mut u8, value: u16) {
    let bytes = value.to_ne_bytes();
    unsafe {
        *ptr_u8 = bytes[0];
        *ptr_u8.add(1) = bytes[1];
    }
}

#[inline]
unsafe fn bounded_c_strlen(ptr_c: *const c_char, max: usize) -> usize {
    let mut len = 0usize;
    while len < max && unsafe { *ptr_c.add(len) } != 0 {
        len += 1;
    }
    len
}

#[inline]
unsafe fn copy_bytes(dst: *mut u8, src: *const u8, len: usize) {
    if len != 0 {
        unsafe { ptr::copy_nonoverlapping(src, dst, len) };
    }
}

#[inline]
unsafe fn write_c_string_trunc(dst: *mut c_char, dstsz: usize, src: *const u8, src_len: usize) {
    if dst.is_null() || dstsz == 0 {
        return;
    }
    let n = cmp::min(src_len, dstsz - 1);
    unsafe {
        copy_bytes(dst.cast::<u8>(), src, n);
        *dst.add(n) = 0;
    }
}

unsafe fn unix_path_make_absolute(path: *const c_char, out: *mut c_char, outsz: usize) -> c_int {
    if path.is_null() || out.is_null() || outsz == 0 || unsafe { *path } == 0 {
        return -EINVAL;
    }

    let out_u8 = out.cast::<u8>();
    let out_cap = outsz - 1;

    if unsafe { *path } as u8 == b'/' {
        let path_len = unsafe { bounded_c_strlen(path, MAX_PATH_LEN - 1) };
        unsafe {
            copy_bytes(out_u8, path.cast::<u8>(), cmp::min(path_len, out_cap));
            *out.add(out_cap) = 0;
        }
        return 0;
    }

    let cwd = unsafe { ffi::a20_unix_current_cwd() };
    let cwd_ptr = if cwd.is_null() {
        b"/\0".as_ptr().cast::<c_char>()
    } else {
        cwd
    };
    let cwd_len = unsafe { bounded_c_strlen(cwd_ptr, MAX_PATH_LEN - 1) };
    let path_len = unsafe { bounded_c_strlen(path, MAX_PATH_LEN - 1) };
    let cwd_is_root = cwd_len == 1 && unsafe { *cwd_ptr } as u8 == b'/';

    let mut written = 0usize;
    unsafe {
        if cwd_is_root {
            if written < out_cap {
                *out_u8.add(written) = b'/';
                written += 1;
            }
        } else {
            let copy_cwd = cmp::min(cwd_len, out_cap.saturating_sub(written));
            copy_bytes(out_u8.add(written), cwd_ptr.cast::<u8>(), copy_cwd);
            written += copy_cwd;
            if written < out_cap {
                *out_u8.add(written) = b'/';
                written += 1;
            }
        }
        let copy_path = cmp::min(path_len, out_cap.saturating_sub(written));
        copy_bytes(out_u8.add(written), path.cast::<u8>(), copy_path);
        *out.add(out_cap) = 0;
    }
    0
}

#[inline]
unsafe fn socket_copy_local(socket: *mut net_socket_t, src: *const u8, len: usize) {
    unsafe {
        copy_bytes((*socket).local.as_mut_ptr(), src, len);
        (*socket).local_len = len;
    }
}

#[inline]
unsafe fn socket_copy_peer_addr(socket: *mut net_socket_t, src: *const u8, len: usize) {
    unsafe {
        copy_bytes((*socket).peer_addr.as_mut_ptr(), src, len);
        (*socket).peer_len = len;
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_unix_sockaddr_prepare(
    addr: *const c_void,
    addrlen: usize,
    out: *mut u8,
    outlen: *mut usize,
    path_out: *mut c_char,
    path_outsz: usize,
) -> c_int {
    if addr.is_null()
        || out.is_null()
        || outlen.is_null()
        || addrlen < FAMILY_SIZE
        || addrlen > NET_SOCKADDR_MAX
    {
        return -EINVAL;
    }

    unsafe {
        copy_bytes(out, addr.cast::<u8>(), addrlen);
        *outlen = addrlen;
    }

    if unsafe { read_u16_ne(out) as c_int } != AF_UNIX {
        return -EAFNOSUPPORT;
    }
    if addrlen <= FAMILY_SIZE {
        return 0;
    }

    let path_len = addrlen - FAMILY_SIZE;
    let mut path = [0u8; MAX_PATH_LEN];
    let n = cmp::min(path_len, MAX_PATH_LEN - 1);
    unsafe {
        copy_bytes(path.as_mut_ptr(), addr.cast::<u8>().add(FAMILY_SIZE), n);
    }
    path[n] = 0;
    if path[0] == 0 {
        return 0;
    }

    let mut full = [0u8; MAX_PATH_LEN];
    let r = unsafe { unix_path_make_absolute(path.as_ptr().cast::<c_char>(), full.as_mut_ptr().cast::<c_char>(), full.len()) };
    if r < 0 {
        return r;
    }

    let full_len = unsafe { bounded_c_strlen(full.as_ptr().cast::<c_char>(), MAX_PATH_LEN - 1) } + 1;
    if FAMILY_SIZE + full_len > NET_SOCKADDR_MAX {
        return -ENAMETOOLONG;
    }

    unsafe {
        write_u16_ne(out, AF_UNIX as u16);
        copy_bytes(out.add(FAMILY_SIZE), full.as_ptr(), full_len);
        *outlen = FAMILY_SIZE + full_len;
        write_c_string_trunc(path_out, path_outsz, full.as_ptr(), full_len - 1);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_unix_path_parent_ok(path: *const c_char) -> c_int {
    if path.is_null() || unsafe { *path } == 0 {
        return 0;
    }

    let mut parent = [0u8; MAX_PATH_LEN];
    let rr = unsafe { unix_path_make_absolute(path, parent.as_mut_ptr().cast::<c_char>(), parent.len()) };
    if rr < 0 {
        return rr;
    }

    let plen = unsafe { bounded_c_strlen(parent.as_ptr().cast::<c_char>(), MAX_PATH_LEN - 1) };
    let mut slash_idx = None;
    let mut i = plen;
    while i > 0 {
        let idx = i - 1;
        if parent[idx] == b'/' {
            slash_idx = Some(idx);
            break;
        }
        i -= 1;
    }
    let Some(idx) = slash_idx else {
        return 0;
    };

    if idx == 0 {
        parent[1] = 0;
    } else {
        parent[idx] = 0;
    }

    let mut st = kstat_t {
        st_dev: 0,
        st_ino: 0,
        st_mode: 0,
        st_nlink: 0,
        st_uid: 0,
        st_gid: 0,
        st_rdev: 0,
        st_size: 0,
        st_blksize: 0,
        st_blocks: 0,
        st_atime: 0,
        st_atime_nsec: 0,
        st_mtime: 0,
        st_mtime_nsec: 0,
        st_ctime: 0,
        st_ctime_nsec: 0,
    };
    let r = unsafe { ffi::a20_unix_vfs_stat(parent.as_ptr().cast::<c_char>(), &mut st) };
    if r < 0 {
        return r;
    }
    if (st.st_mode & S_IFMT) == S_IFDIR {
        0
    } else {
        -ENOTDIR
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_unix_socket_bind(
    s: *mut net_socket_t,
    addr: *const c_void,
    addrlen: usize,
) -> c_int {
    if s.is_null() {
        return -ENOTSOCK;
    }

    let mut bind_addr = [0u8; NET_SOCKADDR_MAX];
    let mut bind_len = addrlen;
    let mut unix_path = [0u8; MAX_PATH_LEN];
    let mut unix_pathname = false;

    let ur = unsafe {
        net_unix_sockaddr_prepare(
            addr,
            addrlen,
            bind_addr.as_mut_ptr(),
            &mut bind_len,
            unix_path.as_mut_ptr().cast::<c_char>(),
            unix_path.len(),
        )
    };
    if ur < 0 {
        return ur;
    }

    if bind_len > FAMILY_SIZE && bind_addr[FAMILY_SIZE] != 0 {
        unix_pathname = true;
        let pr = unsafe { net_unix_path_parent_ok(bind_addr.as_ptr().add(FAMILY_SIZE).cast::<c_char>()) };
        if pr < 0 {
            return pr;
        }
        let mut st = kstat_t {
            st_dev: 0,
            st_ino: 0,
            st_mode: 0,
            st_nlink: 0,
            st_uid: 0,
            st_gid: 0,
            st_rdev: 0,
            st_size: 0,
            st_blksize: 0,
            st_blocks: 0,
            st_atime: 0,
            st_atime_nsec: 0,
            st_mtime: 0,
            st_mtime_nsec: 0,
            st_ctime: 0,
            st_ctime_nsec: 0,
        };
        if unsafe { ffi::a20_unix_vfs_stat(unix_path.as_ptr().cast::<c_char>(), &mut st) } == 0 {
            return -EADDRINUSE;
        }
    }

    {
        let _guard = unsafe { raw_irqsave_lock(core::ptr::addr_of_mut!(ffi::g_net_lock)) };
        if !unsafe {
            ffi::net_find_bound_socket_locked(AF_UNIX, (*s).type_, bind_addr.as_ptr().cast::<c_void>(), bind_len)
        }
        .is_null()
        {
            return -EADDRINUSE;
        }
        unsafe {
            socket_copy_local(s, bind_addr.as_ptr(), bind_len);
            (*s).bound = 1;
        }
    }

    if unix_pathname {
        let fd = unsafe {
            ffi::a20_unix_vfs_open(
                unix_path.as_ptr().cast::<c_char>(),
                O_CREAT | O_EXCL | O_RDWR,
                UNIX_CREATE_MODE,
            )
        };
        if fd < 0 {
            let _guard = unsafe { raw_irqsave_lock(core::ptr::addr_of_mut!(ffi::g_net_lock)) };
            unsafe {
                (*s).bound = 0;
                (*s).local_len = 0;
            }
            return if fd == -EEXIST { -EADDRINUSE } else { fd };
        }
        unsafe {
            ffi::a20_unix_vfs_close(fd);
        }
    }

    0
}

#[no_mangle]
pub unsafe extern "C" fn net_unix_socket_connect(
    s: *mut net_socket_t,
    addr: *const c_void,
    addrlen: usize,
) -> c_int {
    if s.is_null() {
        return -ENOTSOCK;
    }

    let mut peer_addr = [0u8; NET_SOCKADDR_MAX];
    let mut peer_len = addrlen;
    let ur = unsafe {
        net_unix_sockaddr_prepare(
            addr,
            addrlen,
            peer_addr.as_mut_ptr(),
            &mut peer_len,
            ptr::null_mut(),
            0,
        )
    };
    if ur < 0 {
        return ur;
    }

    let needs_child = unsafe { (*s).type_ == SOCK_STREAM || (*s).type_ == SOCK_SEQPACKET };
    let child = if needs_child {
        let child = unsafe { ffi::net_socket_alloc() };
        if child.is_null() {
            return -ENOMEM;
        }
        unsafe {
            ptr::write_bytes(child, 0, 1);
            (*child).domain = AF_UNIX;
            (*child).type_ = SOCK_STREAM;
            (*child).protocol = (*s).protocol;
            (*child).bpf_prog_fd = -1;
        }
        child
    } else {
        ptr::null_mut()
    };

    let mut free_child = false;
    let mut ret = 0;
    {
        let _guard = unsafe { raw_irqsave_lock(core::ptr::addr_of_mut!(ffi::g_net_lock)) };

        unsafe {
            socket_copy_peer_addr(s, peer_addr.as_ptr(), peer_len);
            (*s).connected = 1;
        }

        let listener = unsafe {
            ffi::net_find_bound_socket_locked(AF_UNIX, (*s).type_, peer_addr.as_ptr().cast::<c_void>(), peer_len)
        };
        if listener.is_null() {
            unsafe {
                (*s).connected = 0;
            }
            ret = -ECONNREFUSED;
            free_child = !child.is_null();
        } else if needs_child {
            let listening = unsafe { (*listener).listening != 0 };
            let queue_full = unsafe { (*listener).accept_count >= ffi::NET_MAX_QUEUE };
            if !listening || queue_full {
                unsafe {
                    (*s).connected = 0;
                }
                ret = if listening { -EAGAIN } else { -ECONNREFUSED };
                free_child = true;
            } else {
                unsafe {
                    (*child).bound = 1;
                    (*child).connected = 1;
                    (*child).peer = s;
                    (*s).peer = child;
                    socket_copy_local(child, (*listener).local.as_ptr(), (*listener).local_len);
                    socket_copy_peer_addr(child, (*s).local.as_ptr(), (*s).local_len);
                }
                let qr = unsafe { ffi::net_accept_queue_push_locked(listener, child) };
                if qr < 0 {
                    unsafe {
                        (*s).connected = 0;
                        (*s).peer = ptr::null_mut();
                    }
                    ret = qr;
                    free_child = true;
                }
            }
        } else {
            unsafe {
                (*s).peer = listener;
            }
        }
    }

    if free_child {
        unsafe {
            ffi::net_socket_free(child);
        }
    }
    ret
}

#[no_mangle]
pub unsafe extern "C" fn net_unix_socket_sendto(
    s: *mut net_socket_t,
    buf: *const c_void,
    len: usize,
    addr: *const c_void,
    addrlen: usize,
) -> c_int {
    if s.is_null() {
        return -ENOTSOCK;
    }

    let mut unix_addr = [0u8; NET_SOCKADDR_MAX];
    let mut unix_len = addrlen;
    let mut dst_addr = addr;
    let mut dst_len = addrlen;
    if !addr.is_null() {
        let ur = unsafe {
            net_unix_sockaddr_prepare(
                addr,
                addrlen,
                unix_addr.as_mut_ptr(),
                &mut unix_len,
                ptr::null_mut(),
                0,
            )
        };
        if ur < 0 {
            return ur;
        }
        dst_addr = unix_addr.as_ptr().cast::<c_void>();
        dst_len = unix_len;
    }

    let result;
    {
        let _guard = unsafe { raw_irqsave_lock(core::ptr::addr_of_mut!(ffi::g_net_lock)) };
        if dst_addr.is_null() && unsafe { (*s).connected != 0 } {
            dst_addr = unsafe { (*s).peer_addr.as_ptr().cast::<c_void>() };
            dst_len = unsafe { (*s).peer_len };
        }

        let mut dst = ptr::null_mut::<net_socket_t>();
        let peer = unsafe { (*s).peer };
        if !peer.is_null()
            && unsafe {
                (*s).type_ == SOCK_STREAM
                    || (*s).type_ == SOCK_SEQPACKET
                    || ffi::net_socket_is_valid_locked(peer) != 0
            }
        {
            dst = peer;
        } else {
            if !peer.is_null() {
                unsafe {
                    (*s).peer = ptr::null_mut();
                }
            }
            if !dst_addr.is_null() {
                dst = unsafe {
                    ffi::net_find_bound_socket_locked(AF_UNIX, (*s).type_, dst_addr, dst_len)
                };
            }
        }

        if dst.is_null() {
            result = if dst_addr.is_null() {
                -EDESTADDRREQ
            } else {
                -ECONNREFUSED
            };
        } else {
            result = unsafe {
                ffi::net_enqueue_msg_locked(dst, buf, len, (*s).local.as_ptr().cast::<c_void>(), (*s).local_len)
            };
        }
    }
    result
}
