#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::cmp;
use core::ffi::{c_int, c_void};
use core::ptr;

use ffi::{
    ip_addr_t, net_bh_event_t, net_bh_ring_t, net_socket_t, pbuf, raw_pcb, tcp_pcb, udp_pcb,
    ERR_MEM, ERR_OK, NET_BH_RECV, NET_BH_RING_SIZE, NET_MAX_PAYLOAD, NET_MAX_SOCKETS,
    PROC_BLOCKED,
};

#[inline]
fn bh_ring_mask(x: u32) -> u32 {
    x & ((NET_BH_RING_SIZE as u32) - 1)
}

#[inline]
unsafe fn bh_ring_prepare(s: *mut net_socket_t) -> *mut net_bh_event_t {
    let ring = unsafe { ptr::addr_of_mut!((*s).bh_ring) };
    let head = unsafe { ffi::a20_socket_bh_atomic_load_u32_relaxed(ptr::addr_of!((*ring).head)) };
    let tail = unsafe { ffi::a20_socket_bh_atomic_load_u32_acquire(ptr::addr_of!((*ring).tail)) };
    if head.wrapping_sub(tail) >= NET_BH_RING_SIZE as u32 {
        return ptr::null_mut();
    }
    let event = unsafe { ptr::addr_of_mut!((*ring).events[bh_ring_mask(head) as usize]) };
    unsafe {
        ptr::write_bytes(event, 0, 1);
        (*event).type_ = NET_BH_RECV;
    }
    event
}

#[inline]
unsafe fn bh_ring_commit(s: *mut net_socket_t) {
    let ring = unsafe { ptr::addr_of_mut!((*s).bh_ring) };
    unsafe {
        ffi::a20_socket_bh_atomic_thread_fence_release();
        ffi::a20_socket_bh_atomic_fetch_add_u32_relaxed(ptr::addr_of_mut!((*ring).head), 1);
    }
}

#[inline]
unsafe fn bh_ring_consume(s: *mut net_socket_t) -> *mut net_bh_event_t {
    let ring = unsafe { ptr::addr_of_mut!((*s).bh_ring) };
    let head = unsafe { ffi::a20_socket_bh_atomic_load_u32_acquire(ptr::addr_of!((*ring).head)) };
    let tail = unsafe { ffi::a20_socket_bh_atomic_load_u32_relaxed(ptr::addr_of!((*ring).tail)) };
    if head == tail {
        return ptr::null_mut();
    }
    unsafe { ptr::addr_of_mut!((*ring).events[bh_ring_mask(tail) as usize]) }
}

#[inline]
unsafe fn bh_ring_consume_commit(s: *mut net_socket_t) {
    let ring = unsafe { ptr::addr_of_mut!((*s).bh_ring) };
    unsafe {
        ffi::a20_socket_bh_atomic_thread_fence_acquire();
        ffi::a20_socket_bh_atomic_fetch_add_u32_release(ptr::addr_of_mut!((*ring).tail), 1);
    }
}

#[inline]
unsafe fn net_inet_bh_schedule(s: *mut net_socket_t) {
    if s.is_null() {
        return;
    }
    let idx = unsafe { (*s).reg_idx };
    if !(0..NET_MAX_SOCKETS as c_int).contains(&idx) {
        return;
    }
    unsafe {
        ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!((*s).bh_pending), 1);
        ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!(ffi::g_net_bh_pending[idx as usize]), 1);
    }
}

#[inline]
unsafe fn wake_blocked(task: *mut ffi::task_t) {
    if !task.is_null() && unsafe { ffi::a20_socket_bh_task_state(task) } == PROC_BLOCKED {
        unsafe { ffi::a20_socket_bh_proc_make_ready(task) };
    }
}

unsafe fn fill_ipv6_meta(event: *mut net_bh_event_t) {
    if unsafe { ffi::a20_socket_bh_ip_current_is_v6() } == 0 {
        return;
    }
    unsafe {
        (*event).has_pktinfo = 1;
        (*event).pktinfo_ifindex = ffi::a20_socket_bh_ip_current_input_ifindex();
        ffi::a20_socket_bh_ip6_current_dest_addr_copy((*event).pktinfo_addr.as_mut_ptr());
        (*event).has_hoplimit = 1;
        (*event).hoplimit = ffi::a20_socket_bh_ip6_current_hoplimit();
        (*event).has_tclass = 1;
        (*event).tclass = ffi::a20_socket_bh_ip6_current_tclass();
    }
}

unsafe fn net_inet_bottom_half_process_socket_locked(s: *mut net_socket_t) {
    if unsafe { ffi::a20_socket_bh_atomic_exchange_int_acquire(ptr::addr_of_mut!((*s).bh_connected), 0) } != 0 {
        let err = unsafe { ffi::a20_socket_bh_atomic_load_int_relaxed(ptr::addr_of!((*s).bh_err_code)) };
        unsafe {
            (*s).tcp_connecting = 0;
            (*s).tcp_err = err;
            if err == ERR_OK {
                (*s).connected = 1;
            }
            wake_blocked((*s).waiter);
        }
    }

    if unsafe { ffi::a20_socket_bh_atomic_exchange_int_acquire(ptr::addr_of_mut!((*s).bh_error), 0) } != 0 {
        unsafe {
            (*s).tcp_connecting = 0;
            (*s).tcp_err = ffi::a20_socket_bh_atomic_load_int_relaxed(ptr::addr_of!((*s).bh_err_code));
            (*s).closed = 1;
            wake_blocked((*s).waiter);
        }
    }

    if unsafe { ffi::a20_socket_bh_atomic_exchange_int_acquire(ptr::addr_of_mut!((*s).bh_closed), 0) } != 0 {
        unsafe {
            (*s).closed = 1;
            wake_blocked((*s).waiter);
        }
    }

    loop {
        let event = unsafe { bh_ring_consume(s) };
        if event.is_null() {
            break;
        }
        unsafe {
            if (*s).closed == 0 {
                ffi::net_enqueue_msg_locked_meta(
                    s,
                    (*event).data.as_ptr().cast::<c_void>(),
                    (*event).len,
                    if (*event).addrlen != 0 {
                        (*event).addr.as_ptr().cast::<c_void>()
                    } else {
                        ptr::null()
                    },
                    (*event).addrlen,
                    event,
                );
            }
            bh_ring_consume_commit(s);
        }
    }

    if unsafe { ffi::a20_socket_bh_atomic_exchange_int_acquire(ptr::addr_of_mut!((*s).bh_tx_wake), 0) } != 0 {
        unsafe { wake_blocked((*s).send_waiter) };
    }
}

#[no_mangle]
pub unsafe extern "C" fn lwip_udp_recv_cb(
    arg: *mut c_void,
    _pcb: *mut udp_pcb,
    p: *mut pbuf,
    addr: *const ip_addr_t,
    port: u16,
) {
    let s = arg as *mut net_socket_t;
    if s.is_null() || p.is_null() {
        return;
    }

    let event = unsafe { bh_ring_prepare(s) };
    if event.is_null() {
        unsafe { ffi::pbuf_free(p) };
        return;
    }

    unsafe {
        let _ = ffi::net_lwip_ip_to_sockaddr(addr, port, (*event).addr.as_mut_ptr(), ptr::addr_of_mut!((*event).addrlen));
        fill_ipv6_meta(event);
        let len = cmp::min((*p).tot_len as usize, NET_MAX_PAYLOAD);
        let _ = ffi::pbuf_copy_partial(p, (*event).data.as_mut_ptr().cast::<c_void>(), len as u16, 0);
        (*event).len = len;
        bh_ring_commit(s);
        net_inet_bh_schedule(s);
        ffi::pbuf_free(p);
    }
}

#[no_mangle]
pub unsafe extern "C" fn lwip_raw_recv_cb(
    arg: *mut c_void,
    _pcb: *mut raw_pcb,
    p: *mut pbuf,
    addr: *const ip_addr_t,
) -> u8 {
    let s = arg as *mut net_socket_t;
    if s.is_null() || p.is_null() {
        return 0;
    }
    if unsafe { ffi::a20_socket_bh_raw_should_passthrough_icmp_echo(s, p) } != 0 {
        return 0;
    }

    let event = unsafe { bh_ring_prepare(s) };
    if event.is_null() {
        unsafe { ffi::pbuf_free(p) };
        return 0;
    }

    unsafe {
        let _ = ffi::net_lwip_ip_to_sockaddr(addr, 0, (*event).addr.as_mut_ptr(), ptr::addr_of_mut!((*event).addrlen));
        fill_ipv6_meta(event);
        let len = cmp::min((*p).tot_len as usize, NET_MAX_PAYLOAD);
        let _ = ffi::pbuf_copy_partial(p, (*event).data.as_mut_ptr().cast::<c_void>(), len as u16, 0);
        (*event).len = len;
        bh_ring_commit(s);
        net_inet_bh_schedule(s);
        ffi::pbuf_free(p);
    }
    1
}

#[no_mangle]
pub unsafe extern "C" fn lwip_tcp_connected_cb(
    arg: *mut c_void,
    _pcb: *mut tcp_pcb,
    err: i8,
) -> i8 {
    let s = arg as *mut net_socket_t;
    if s.is_null() {
        return ERR_OK as i8;
    }
    unsafe {
        ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!((*s).bh_err_code), err as c_int);
        ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!((*s).bh_connected), 1);
        net_inet_bh_schedule(s);
    }
    ERR_OK as i8
}

#[no_mangle]
pub unsafe extern "C" fn lwip_tcp_recv_cb(
    arg: *mut c_void,
    _pcb: *mut tcp_pcb,
    p: *mut pbuf,
    _err: i8,
) -> i8 {
    let s = arg as *mut net_socket_t;
    if s.is_null() {
        return ERR_OK as i8;
    }
    if p.is_null() {
        unsafe {
            ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!((*s).bh_closed), 1);
            net_inet_bh_schedule(s);
        }
        return ERR_OK as i8;
    }

    let mut off = 0usize;
    let total = unsafe { (*p).tot_len as usize };
    while off < total {
        let event = unsafe { bh_ring_prepare(s) };
        if event.is_null() {
            unsafe { ffi::pbuf_free(p) };
            return ERR_MEM as i8;
        }
        let chunk = cmp::min(total - off, NET_MAX_PAYLOAD);
        unsafe {
            let _ = ffi::pbuf_copy_partial(p, (*event).data.as_mut_ptr().cast::<c_void>(), chunk as u16, off as u16);
            (*event).len = chunk;
            bh_ring_commit(s);
        }
        off += chunk;
    }
    unsafe {
        net_inet_bh_schedule(s);
        ffi::pbuf_free(p);
    }
    ERR_OK as i8
}

#[no_mangle]
pub unsafe extern "C" fn lwip_tcp_err_cb(arg: *mut c_void, err: i8) {
    let s = arg as *mut net_socket_t;
    if s.is_null() {
        return;
    }
    unsafe {
        (*s).tcp = ptr::null_mut();
        ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!((*s).bh_err_code), err as c_int);
        ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!((*s).bh_error), 1);
        net_inet_bh_schedule(s);
    }
}

#[no_mangle]
pub unsafe extern "C" fn lwip_tcp_sent_cb(arg: *mut c_void, _pcb: *mut tcp_pcb, _len: u16) -> i8 {
    let s = arg as *mut net_socket_t;
    if s.is_null() {
        return ERR_OK as i8;
    }
    unsafe {
        ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!((*s).bh_tx_wake), 1);
        net_inet_bh_schedule(s);
    }
    ERR_OK as i8
}

#[no_mangle]
pub unsafe extern "C" fn net_inet_bottom_half_process_socket(s: *mut net_socket_t) {
    if s.is_null() {
        return;
    }
    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
    unsafe {
        if ffi::net_socket_is_valid_locked(s) != 0 && (*s).closed == 0 {
            net_inet_bottom_half_process_socket_locked(s);
        }
        ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!((*s).bh_pending), 0);
        let idx = (*s).reg_idx;
        if (0..NET_MAX_SOCKETS as c_int).contains(&idx) {
            ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!(ffi::g_net_bh_pending[idx as usize]), 0);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_inet_bottom_half_process_all() {
    let mut i = 0usize;
    while i < NET_MAX_SOCKETS {
        if unsafe { ffi::a20_socket_bh_atomic_load_int_acquire(ptr::addr_of!(ffi::g_net_bh_pending[i])) } == 0 {
            i += 1;
            continue;
        }
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::g_net_lock)) };
        unsafe {
            let s = ffi::g_sockets[i];
            if s.is_null() || ffi::net_socket_is_valid_locked(s) == 0 || (*s).closed != 0 {
                ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!(ffi::g_net_bh_pending[i]), 0);
                if !s.is_null() {
                    ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!((*s).bh_pending), 0);
                }
                i += 1;
                continue;
            }
            net_inet_bottom_half_process_socket_locked(s);
            ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!((*s).bh_pending), 0);
            ffi::a20_socket_bh_atomic_store_int_release(ptr::addr_of_mut!(ffi::g_net_bh_pending[i]), 0);
        }
        i += 1;
    }
}
