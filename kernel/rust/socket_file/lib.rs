#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::ffi::{c_char, c_int, c_long, c_void};
use core::ptr;

use ffi::{
    net_msg_t, net_socket_t, task_t, vfile_ops_t, vfile_t, AF_ALG, AF_INET, AF_INET6, EAGAIN,
    ECONNREFUSED, EDESTADDRREQ, EMSGSIZE, ENOMEM, ENOTSOCK, EPIPE, ERESTARTSYS, ESPIPE,
    NET_MAX_PAYLOAD, NET_MAX_QUEUE, PROC_BLOCKED, SOCK_DGRAM, SOCK_SEQPACKET, SOCK_STREAM,
};

#[inline]
unsafe fn socket_from_vfile(vf: *mut vfile_t) -> *mut net_socket_t {
    if vf.is_null() {
        ptr::null_mut()
    } else {
        unsafe { ffi::a20_socket_file_vfile_priv(vf) as *mut net_socket_t }
    }
}

#[inline]
unsafe fn wake_if_blocked(task: *mut task_t) {
    if !task.is_null() && unsafe { ffi::a20_socket_file_task_state(task) } == PROC_BLOCKED {
        unsafe { ffi::a20_socket_file_proc_make_ready(task) };
    }
}

extern "C" fn net_vfile_read(vf: *mut vfile_t, buf: *mut c_char, count: usize) -> c_int {
    let s = unsafe { socket_from_vfile(vf) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if unsafe { (*s).domain } == AF_ALG {
        return unsafe { ffi::net_alg_socket_recv(s, buf.cast::<c_void>(), count) };
    }

    let start = unsafe { ffi::timer_get_ticks() };
    loop {
        unsafe { ffi::a20_socket_file_lwip_poll() };
        let guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
        let mut r = unsafe {
            ffi::net_dequeue_msg_locked(s, buf.cast::<c_void>(), count, ptr::null_mut(), ptr::null_mut())
        };
        if r > 0 && unsafe { (*s).type_ } == SOCK_STREAM {
            let mut total = r as usize;
            while total < count && unsafe { !(*s).rx_head.is_null() } {
                let nr = unsafe {
                    ffi::net_dequeue_msg_locked(
                        s,
                        buf.cast::<u8>().add(total).cast::<c_void>(),
                        count - total,
                        ptr::null_mut(),
                        ptr::null_mut(),
                    )
                };
                if nr <= 0 {
                    break;
                }
                total += nr as usize;
            }
            r = total as c_int;
        }

        let terminal = unsafe {
            r != -EAGAIN
                || (*s).nonblock != 0
                || (*s).closed != 0
                || (*s).peer_closed != 0
                || (*s).shut_rd != 0
        };
        if terminal {
            if r == -EAGAIN
                && unsafe { (*s).closed != 0 || (*s).peer_closed != 0 || (*s).shut_rd != 0 }
            {
                r = 0;
            }
            let recved = if r > 0 && unsafe { (*s).type_ } == SOCK_STREAM {
                r as usize
            } else {
                0
            };
            drop(guard);
            if recved != 0 {
                unsafe { ffi::net_tcp_recved(s, recved) };
            }
            return r;
        }

        let cur = unsafe { ffi::a20_socket_file_proc_current() };
        if cur.is_null() {
            drop(guard);
            return -EAGAIN;
        }
        if unsafe { ffi::net_task_has_unblocked_signal(cur) } != 0 {
            drop(guard);
            return -ERESTARTSYS;
        }
        if unsafe { ffi::net_socket_wait_expired(s, start, 0) } != 0 {
            drop(guard);
            return -EAGAIN;
        }
        unsafe { ffi::net_block_on_socket_locked(s, cur) };
        drop(guard);
        unsafe { ffi::a20_socket_file_sched() };
        unsafe { ffi::net_clear_socket_waiter(s, cur) };
    }
}

extern "C" fn net_vfile_write(vf: *mut vfile_t, buf: *const c_char, count: usize) -> c_int {
    let s = unsafe { socket_from_vfile(vf) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if count > NET_MAX_PAYLOAD && unsafe { (*s).type_ } != SOCK_STREAM {
        return -EMSGSIZE;
    }
    if unsafe { (*s).domain } == AF_ALG {
        return unsafe { ffi::net_alg_socket_send(s, buf.cast::<c_void>(), count) };
    }
    if unsafe {
        ((*s).domain == AF_INET || (*s).domain == AF_INET6)
            && (!(*s).udp.is_null() || !(*s).raw.is_null() || !(*s).tcp.is_null())
    } {
        return unsafe { ffi::net_inet_sendto(s, buf.cast::<c_void>(), count, 0, ptr::null(), 0) };
    }

    let mut dst: *mut net_socket_t;
    {
        let guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
        if unsafe { (*s).closed != 0 || (*s).shut_wr != 0 } {
            drop(guard);
            return -EPIPE;
        }
        if unsafe { (*s).bound == 0 && ((*s).domain == AF_INET || (*s).domain == AF_INET6) } {
            let port = unsafe { ffi::net_alloc_ephemeral_port_locked() };
            unsafe { ffi::net_sockaddr_loopback(s, port) };
        }
        dst = unsafe { (*s).peer };
        if !dst.is_null()
            && unsafe { (*s).type_ != SOCK_STREAM && (*s).type_ != SOCK_SEQPACKET }
            && unsafe { ffi::net_socket_is_valid_locked(dst) } == 0
        {
            unsafe { (*s).peer = ptr::null_mut() };
            dst = ptr::null_mut();
        }
        if dst.is_null() && unsafe { (*s).connected != 0 } {
            dst = unsafe {
                ffi::net_find_bound_socket_locked(
                    (*s).domain,
                    (*s).type_,
                    (*s).peer_addr.as_ptr().cast::<c_void>(),
                    (*s).peer_len,
                )
            };
        }
        if dst.is_null() {
            let err = if unsafe { (*s).connected != 0 } {
                -ECONNREFUSED
            } else {
                -EDESTADDRREQ
            };
            drop(guard);
            return err;
        }

        if count <= NET_MAX_PAYLOAD {
            if unsafe { (*dst).rx_count >= NET_MAX_QUEUE && (*s).nonblock == 0 } {
                drop(guard);
                return unsafe {
                    ffi::net_enqueue_msg_blocking(
                        s,
                        dst,
                        buf.cast::<c_void>(),
                        count,
                        (*s).local.as_ptr().cast::<c_void>(),
                        (*s).local_len,
                        (*s).nonblock,
                        (*s).send_timeout_ticks,
                    )
                };
            }
            let r = unsafe {
                ffi::net_enqueue_msg_locked(
                    dst,
                    buf.cast::<c_void>(),
                    count,
                    (*s).local.as_ptr().cast::<c_void>(),
                    (*s).local_len,
                )
            };
            drop(guard);
            return r;
        }
        drop(guard);
    }

    let mut total = 0usize;
    while total < count {
        let mut chunk = count - total;
        if chunk > NET_MAX_PAYLOAD {
            chunk = NET_MAX_PAYLOAD;
        }
        let r = unsafe {
            ffi::net_enqueue_msg_blocking(
                s,
                dst,
                buf.cast::<u8>().add(total).cast::<c_void>(),
                chunk,
                (*s).local.as_ptr().cast::<c_void>(),
                (*s).local_len,
                (*s).nonblock,
                (*s).send_timeout_ticks,
            )
        };
        if r < 0 {
            return if total != 0 { total as c_int } else { r };
        }
        total += r as usize;
    }
    total as c_int
}

extern "C" fn net_vfile_lseek(_vf: *mut vfile_t, _offset: c_long, _whence: c_int) -> c_long {
    -ESPIPE as c_long
}

static G_NET_OPS: vfile_ops_t = vfile_ops_t {
    read: Some(net_vfile_read),
    write: Some(net_vfile_write),
    lseek: Some(net_vfile_lseek),
    readdir: None,
    ioctl: None,
    close: Some(net_socket_close_file),
};

#[no_mangle]
pub extern "C" fn net_socket_from_file(gfd: c_int) -> *mut net_socket_t {
    let vf = unsafe { ffi::vfs_get_file_ref(gfd) };
    if vf.is_null() {
        return ptr::null_mut();
    }
    let s = if unsafe {
        ffi::a20_socket_file_vfile_ops_match(vf, ptr::addr_of!(G_NET_OPS) as *mut vfile_ops_t)
    } != 0
    {
        unsafe { ffi::a20_socket_file_vfile_priv(vf) as *mut net_socket_t }
    } else {
        ptr::null_mut()
    };
    unsafe { ffi::vfs_put_file_ref(gfd, vf) };
    s
}

#[no_mangle]
pub extern "C" fn net_socket_install_file(s: *mut net_socket_t, flags: c_int) -> c_int {
    let vf = unsafe { ffi::vfile_alloc() };
    if vf.is_null() {
        return -ENOMEM;
    }
    unsafe {
        ffi::vfile_ref_init(vf, 1);
        ffi::a20_socket_file_vfile_init(
            vf,
            ptr::addr_of!(G_NET_OPS) as *mut vfile_ops_t,
            s.cast::<c_void>(),
            flags,
        );
    }

    let gfd = unsafe { ffi::vfs_alloc_fd(vf) };
    if gfd < 0 {
        let _ = net_socket_close_file(vf);
        unsafe { ffi::vfile_free(vf) };
        return gfd;
    }
    gfd
}

#[no_mangle]
pub extern "C" fn net_socket_close_file(vf: *mut vfile_t) -> c_int {
    let s = unsafe { socket_from_vfile(vf) };
    if s.is_null() {
        return 0;
    }

    unsafe { ffi::net_inet_socket_destroy(s) };

    let mut send_waiter = ptr::null_mut();
    let mut peer_waiter = ptr::null_mut();
    let mut peer_send_waiter = ptr::null_mut();
    let mut self_waiter = ptr::null_mut();
    let mut msgs: *mut net_msg_t;
    let mut accepted: *mut net_socket_t;

    {
        let guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
        unsafe {
            ffi::net_unregister_socket_locked(s);
            send_waiter = (*s).send_waiter;
            let peer = (*s).peer;
            if !peer.is_null()
                && ((*s).type_ == SOCK_STREAM
                    || (*s).type_ == SOCK_SEQPACKET
                    || ffi::net_socket_is_valid_locked(peer) != 0)
                && (*peer).peer == s
            {
                (*peer).peer = ptr::null_mut();
                (*peer).peer_closed = 1;
                peer_waiter = (*peer).waiter;
                peer_send_waiter = (*peer).send_waiter;
            }
            (*s).closed = 1;
            self_waiter = (*s).waiter;
            msgs = (*s).rx_head;
            (*s).rx_head = ptr::null_mut();
            (*s).rx_tail = ptr::null_mut();
            accepted = (*s).accept_head;
            (*s).accept_head = ptr::null_mut();
            (*s).accept_tail = ptr::null_mut();
            (*s).accept_count = 0;
        }
        drop(guard);
    }

    unsafe {
        wake_if_blocked(send_waiter);
        wake_if_blocked(peer_waiter);
        wake_if_blocked(peer_send_waiter);
        wake_if_blocked(self_waiter);
    }

    while !msgs.is_null() {
        let next = unsafe { (*msgs).next };
        unsafe { ffi::net_msg_free(msgs) };
        msgs = next;
    }

    while !accepted.is_null() {
        let next = unsafe { (*accepted).accept_next };
        let mut child_peer_waiter = ptr::null_mut();
        let mut child_peer_send_waiter = ptr::null_mut();
        {
            let guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
            unsafe {
                (*accepted).closed = 1;
                let peer = (*accepted).peer;
                if !peer.is_null() && (*peer).peer == accepted {
                    (*peer).peer = ptr::null_mut();
                    (*peer).peer_closed = 1;
                    child_peer_waiter = (*peer).waiter;
                    child_peer_send_waiter = (*peer).send_waiter;
                }
            }
            drop(guard);
        }
        unsafe {
            wake_if_blocked(child_peer_waiter);
            wake_if_blocked(child_peer_send_waiter);
            ffi::net_inet_socket_destroy(accepted);
        }
        {
            let guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
            unsafe { ffi::net_unregister_socket_locked(accepted) };
            drop(guard);
        }
        unsafe { ffi::net_socket_free(accepted) };
        accepted = next;
    }

    unsafe {
        ffi::net_socket_free(s);
        ffi::a20_socket_file_vfile_set_priv(vf, ptr::null_mut());
    }
    0
}
