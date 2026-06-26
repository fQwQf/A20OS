#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::cmp;
use core::ffi::c_int;
use core::ptr;

use ffi::{
    net_bh_event_t, net_msg_t, net_recv_meta_t, net_socket_t, task_t, AF_ALG, EAGAIN,
    EMSGSIZE, ENOTCONN, ENOTSOCK, ERESTARTSYS, EINVAL, MSG_DONTWAIT, NET_MAX_PAYLOAD,
    NET_MAX_QUEUE, NET_SOCKADDR_MAX, PROC_BLOCKED, SOCK_DGRAM, SOCK_STREAM,
};

#[inline]
unsafe fn socket_wake_waiter_locked(socket: *mut net_socket_t) {
    let waiter = unsafe { (*socket).waiter };
    if !waiter.is_null() && unsafe { ffi::a20_task_state_value(waiter) } == PROC_BLOCKED {
        unsafe { ffi::a20_proc_make_ready_task(waiter) };
    }
}

#[inline]
unsafe fn block_on_queue_space_locked(socket: *mut net_socket_t, current: *mut task_t) {
    unsafe {
        (*socket).send_waiter = current;
        ffi::a20_proc_set_wake_time_task(current, current_ticks() + ffi::a20_net_wait_ticks_value());
        ffi::a20_task_set_state_value(current, PROC_BLOCKED);
    }
}

#[inline]
unsafe fn wake_queue_space_waiter_locked(socket: *mut net_socket_t) {
    let waiter = unsafe { (*socket).send_waiter };
    if !waiter.is_null() && unsafe { ffi::a20_task_state_value(waiter) } == PROC_BLOCKED {
        unsafe { ffi::a20_proc_make_ready_task(waiter) };
    }
}

#[inline]
unsafe fn clear_queue_space_waiter(socket: *mut net_socket_t, current: *mut task_t) {
    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
    unsafe {
        if !socket.is_null() && (*socket).closed == 0 && (*socket).send_waiter == current {
            (*socket).send_waiter = ptr::null_mut();
        }
        ffi::a20_proc_set_wake_time_task(current, 0);
    }
}

#[inline]
unsafe fn block_on_socket_locked(socket: *mut net_socket_t, current: *mut task_t) {
    unsafe {
        (*socket).waiter = current;
        ffi::a20_proc_set_wake_time_task(current, current_ticks() + ffi::a20_net_wait_ticks_value());
        ffi::a20_task_set_state_value(current, PROC_BLOCKED);
    }
}

#[inline]
unsafe fn clear_socket_waiter(socket: *mut net_socket_t, current: *mut task_t) {
    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
    unsafe {
        if !socket.is_null() && (*socket).in_registry != 0 && (*socket).waiter == current {
            (*socket).waiter = ptr::null_mut();
        }
        ffi::a20_proc_set_wake_time_task(current, 0);
    }
}

#[inline]
unsafe fn current_ticks() -> u64 {
    extern "C" {
        fn timer_get_ticks() -> u64;
    }
    unsafe { timer_get_ticks() }
}

#[inline]
unsafe fn copy_meta_from_event(msg: *mut net_msg_t, meta: *const net_bh_event_t) {
    unsafe {
        (*msg).has_pktinfo = (*meta).has_pktinfo;
        (*msg).has_hoplimit = (*meta).has_hoplimit;
        (*msg).has_tclass = (*meta).has_tclass;
        (*msg).pktinfo_ifindex = (*meta).pktinfo_ifindex;
        ptr::copy_nonoverlapping((*meta).pktinfo_addr.as_ptr(), (*msg).pktinfo_addr.as_mut_ptr(), 16);
        (*msg).hoplimit = (*meta).hoplimit;
        (*msg).tclass = (*meta).tclass;
    }
}

#[inline]
unsafe fn copy_meta_to_recv(msg: *mut net_msg_t, meta: *mut net_recv_meta_t) {
    unsafe {
        (*meta).has_pktinfo = (*msg).has_pktinfo;
        (*meta).has_hoplimit = (*msg).has_hoplimit;
        (*meta).has_tclass = (*msg).has_tclass;
        (*meta).pktinfo_ifindex = (*msg).pktinfo_ifindex;
        ptr::copy_nonoverlapping((*msg).pktinfo_addr.as_ptr(), (*meta).pktinfo_addr.as_mut_ptr(), 16);
        (*meta).hoplimit = (*msg).hoplimit;
        (*meta).tclass = (*msg).tclass;
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_enqueue_msg_locked_meta(
    dst: *mut net_socket_t,
    buf: *const core::ffi::c_void,
    len: usize,
    addr: *const core::ffi::c_void,
    addrlen: usize,
    meta: *const net_bh_event_t,
) -> c_int {
    if dst.is_null() || unsafe { (*dst).closed != 0 } {
        return -ENOTCONN;
    }
    if len > NET_MAX_PAYLOAD {
        return -EMSGSIZE;
    }
    if unsafe { (*dst).rx_count >= NET_MAX_QUEUE } {
        return -EAGAIN;
    }
    let bpf_fd = unsafe { (*dst).bpf_prog_fd };
    if bpf_fd >= 0 {
        unsafe { ffi::a20_bpf_run_socket_filter(bpf_fd) };
    }

    let msg = unsafe { ffi::net_msg_alloc() };
    if msg.is_null() {
        return -EAGAIN;
    }

    unsafe {
        (*msg).next = ptr::null_mut();
        (*msg).len = len;
        (*msg).off = 0;
        (*msg).addrlen = 0;
        (*msg).has_pktinfo = 0;
        (*msg).has_hoplimit = 0;
        (*msg).has_tclass = 0;
        (*msg).pad = 0;
        (*msg).pktinfo_ifindex = 0;
        (*msg).pktinfo_addr = [0; 16];
        (*msg).hoplimit = 0;
        (*msg).tclass = 0;
        (*msg).__pad_meta = 0;
        if len != 0 {
            ptr::copy_nonoverlapping(buf.cast::<u8>(), (*msg).data.as_mut_ptr(), len);
        }

        if !addr.is_null() && addrlen != 0 {
            let capped = cmp::min(addrlen, NET_SOCKADDR_MAX);
            if capped != 0 {
                ptr::copy_nonoverlapping(addr.cast::<u8>(), (*msg).addr.as_mut_ptr(), capped);
            }
            (*msg).addrlen = capped;
        }
        if !meta.is_null() {
            copy_meta_from_event(msg, meta);
        }

        if !(*dst).rx_tail.is_null() {
            (*(*dst).rx_tail).next = msg;
        } else {
            (*dst).rx_head = msg;
        }
        (*dst).rx_tail = msg;
        (*dst).rx_count += 1;
        socket_wake_waiter_locked(dst);
    }

    len as c_int
}

#[no_mangle]
pub unsafe extern "C" fn net_enqueue_msg_locked(
    dst: *mut net_socket_t,
    buf: *const core::ffi::c_void,
    len: usize,
    addr: *const core::ffi::c_void,
    addrlen: usize,
) -> c_int {
    unsafe { net_enqueue_msg_locked_meta(dst, buf, len, addr, addrlen, ptr::null()) }
}

#[no_mangle]
pub unsafe extern "C" fn net_enqueue_msg_blocking(
    s: *mut net_socket_t,
    dst: *mut net_socket_t,
    buf: *const core::ffi::c_void,
    len: usize,
    addr: *const core::ffi::c_void,
    addrlen: usize,
    dontwait: c_int,
    timeout_ticks: u64,
) -> c_int {
    let start = unsafe { current_ticks() };
    loop {
        let guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
        let ret = unsafe {
            if s.is_null() || (*s).in_registry == 0 || (*s).closed != 0 {
                Some(-ENOTCONN)
            } else if dst.is_null() || (*dst).in_registry == 0 || (*dst).closed != 0 {
                Some(-ENOTCONN)
            } else if (*s).connected != 0 && (*s).peer != dst && (*s).type_ != SOCK_DGRAM {
                Some(-ENOTCONN)
            } else {
                None
            }
        };
        if let Some(code) = ret {
            drop(guard);
            return code;
        }

        let enqueue_res = unsafe { net_enqueue_msg_locked(dst, buf, len, addr, addrlen) };
        if enqueue_res != -EAGAIN || dontwait != 0 {
            drop(guard);
            return enqueue_res;
        }

        let current = unsafe { ffi::a20_proc_current_task() };
        if current.is_null() {
            drop(guard);
            return -EAGAIN;
        }
        if unsafe { ffi::a20_task_has_unblocked_signal(current) } != 0 {
            drop(guard);
            return -ERESTARTSYS;
        }

        let now = unsafe { current_ticks() };
        if timeout_ticks != 0 && (now as i64 - start.wrapping_add(timeout_ticks) as i64) >= 0 {
            drop(guard);
            return -EAGAIN;
        }

        let socket_type = unsafe { (*s).type_ };
        if timeout_ticks == 0 && socket_type == SOCK_DGRAM {
            let deadline = start.wrapping_add(unsafe { ffi::a20_ms_to_ticks_value(200) });
            if (now as i64 - deadline as i64) >= 0 {
                drop(guard);
                return -EAGAIN;
            }
        }
        if timeout_ticks == 0 && socket_type == SOCK_STREAM {
            let deadline = start.wrapping_add(unsafe { ffi::a20_ms_to_ticks_value(5000) });
            if (now as i64 - deadline as i64) >= 0 {
                drop(guard);
                return -EAGAIN;
            }
        }

        unsafe { block_on_queue_space_locked(dst, current) };
        drop(guard);
        unsafe { ffi::a20_sched_yield() };
        unsafe { clear_queue_space_waiter(dst, current) };
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_dequeue_msg_locked_meta(
    s: *mut net_socket_t,
    buf: *mut core::ffi::c_void,
    len: usize,
    addr: *mut core::ffi::c_void,
    addrlen: *mut usize,
    meta: *mut net_recv_meta_t,
) -> c_int {
    if s.is_null() {
        return -EAGAIN;
    }

    let msg = unsafe { (*s).rx_head };
    if msg.is_null() {
        return unsafe {
            if (*s).closed != 0 || (*s).peer_closed != 0 || (*s).shut_rd != 0 {
                0
            } else {
                -EAGAIN
            }
        };
    }

    let avail = unsafe { (*msg).len - (*msg).off };
    let n = cmp::min(avail, len);
    unsafe {
        if n != 0 {
            ptr::copy_nonoverlapping((*msg).data.as_ptr().add((*msg).off), buf.cast::<u8>(), n);
        }
        if !addr.is_null() && !addrlen.is_null() && *addrlen > 0 {
            let copy_len = cmp::min((*msg).addrlen, *addrlen);
            if copy_len != 0 {
                ptr::copy_nonoverlapping((*msg).addr.as_ptr(), addr.cast::<u8>(), copy_len);
            }
            *addrlen = copy_len;
        }
        if !meta.is_null() {
            copy_meta_to_recv(msg, meta);
        }
    }

    if unsafe { (*s).type_ == SOCK_STREAM } && n < avail {
        unsafe { (*msg).off += n };
        return n as c_int;
    }

    unsafe {
        (*s).rx_head = (*msg).next;
        if (*s).rx_head.is_null() {
            (*s).rx_tail = ptr::null_mut();
        }
        (*s).rx_count -= 1;
        ffi::net_msg_free(msg);
        socket_wake_waiter_locked(s);
        wake_queue_space_waiter_locked(s);
    }
    n as c_int
}

#[no_mangle]
pub unsafe extern "C" fn net_dequeue_msg_locked(
    s: *mut net_socket_t,
    buf: *mut core::ffi::c_void,
    len: usize,
    addr: *mut core::ffi::c_void,
    addrlen: *mut usize,
) -> c_int {
    unsafe { net_dequeue_msg_locked_meta(s, buf, len, addr, addrlen, ptr::null_mut()) }
}

#[no_mangle]
pub unsafe extern "C" fn net_recvfrom_meta(
    gfd: c_int,
    buf: *mut core::ffi::c_void,
    len: usize,
    flags: c_int,
    addr: *mut core::ffi::c_void,
    addrlen: *mut usize,
    meta: *mut net_recv_meta_t,
) -> c_int {
    let socket = unsafe { ffi::a20_net_socket_from_file(gfd) };
    if socket.is_null() {
        return -ENOTSOCK;
    }
    if unsafe { (*socket).domain == AF_ALG } {
        extern "C" {
            fn net_alg_socket_recv(s: *mut net_socket_t, buf: *mut core::ffi::c_void, len: usize) -> c_int;
        }
        return unsafe { net_alg_socket_recv(socket, buf, len) };
    }

    let dontwait = (flags & MSG_DONTWAIT) != 0;
    let start = unsafe { current_ticks() };

    loop {
        unsafe { ffi::a20_net_lwip_poll() };
        let guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
        let mut result = unsafe { net_dequeue_msg_locked_meta(socket, buf, len, addr, addrlen, meta) };
        if result > 0 && unsafe { (*socket).type_ == SOCK_STREAM } {
            let mut total = result as usize;
            while total < len && unsafe { !(*socket).rx_head.is_null() } {
                let nr = unsafe {
                    net_dequeue_msg_locked_meta(
                        socket,
                        buf.cast::<u8>().add(total).cast(),
                        len - total,
                        ptr::null_mut(),
                        ptr::null_mut(),
                        ptr::null_mut(),
                    )
                };
                if nr <= 0 {
                    break;
                }
                total += nr as usize;
            }
            result = total as c_int;
        }

        let terminal = unsafe {
            result != -EAGAIN
                || (*socket).nonblock != 0
                || dontwait
                || (*socket).closed != 0
                || (*socket).peer_closed != 0
                || (*socket).shut_rd != 0
        };
        if terminal {
            if result == -EAGAIN
                && unsafe { (*socket).closed != 0 || (*socket).peer_closed != 0 || (*socket).shut_rd != 0 }
            {
                result = 0;
            }
            let recved = if result > 0 && unsafe { (*socket).type_ == SOCK_STREAM } {
                result as usize
            } else {
                0
            };
            drop(guard);
            if recved != 0 {
                unsafe { ffi::a20_net_tcp_recved(socket, recved) };
            }
            return result;
        }

        let current = unsafe { ffi::a20_proc_current_task() };
        if current.is_null() {
            drop(guard);
            return -EAGAIN;
        }
        if unsafe { ffi::a20_task_has_unblocked_signal(current) } != 0 {
            drop(guard);
            return -ERESTARTSYS;
        }
        if unsafe { ffi::a20_net_socket_wait_expired(socket, start, 0) } != 0 {
            drop(guard);
            return -EAGAIN;
        }

        unsafe { block_on_socket_locked(socket, current) };
        drop(guard);
        unsafe { ffi::a20_sched_yield() };
        unsafe { clear_socket_waiter(socket, current) };
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_accept_queue_push_locked(
    listener: *mut net_socket_t,
    child: *mut net_socket_t,
) -> c_int {
    if listener.is_null() || child.is_null() || unsafe { (*listener).listening == 0 } {
        return -EINVAL;
    }
    if unsafe { (*listener).accept_count >= NET_MAX_QUEUE } {
        return -EAGAIN;
    }

    unsafe {
        (*child).accept_next = ptr::null_mut();
        if !(*listener).accept_tail.is_null() {
            (*(*listener).accept_tail).accept_next = child;
        } else {
            (*listener).accept_head = child;
        }
        (*listener).accept_tail = child;
        (*listener).accept_count += 1;
        socket_wake_waiter_locked(listener);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_accept_queue_pop_locked(
    listener: *mut net_socket_t,
) -> *mut net_socket_t {
    if listener.is_null() {
        return ptr::null_mut();
    }
    let child = unsafe { (*listener).accept_head };
    if child.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        (*listener).accept_head = (*child).accept_next;
        if (*listener).accept_head.is_null() {
            (*listener).accept_tail = ptr::null_mut();
        }
        (*listener).accept_count -= 1;
        (*child).accept_next = ptr::null_mut();
    }
    child
}

#[no_mangle]
pub unsafe extern "C" fn net_recvfrom(
    gfd: c_int,
    buf: *mut core::ffi::c_void,
    len: usize,
    flags: c_int,
    addr: *mut core::ffi::c_void,
    addrlen: *mut usize,
) -> c_int {
    unsafe { net_recvfrom_meta(gfd, buf, len, flags, addr, addrlen, ptr::null_mut()) }
}
