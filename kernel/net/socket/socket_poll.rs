#![no_std]

//! The poll readiness computation: the two arms (channel-backed and legacy),
//! the HUP precedence over IN and OUT, the ERR precedence inside OUT, the
//! connected-but-peerless stream error, the connecting socket that reports
//! neither, and the deliberately weakly consistent read of the peer's queue
//! depth.
//!
//! Readiness is a precedence problem before it is anything else.  A peer that
//! closed must report HUP even when the caller asked only for POLLOUT, a
//! write side that is shut must not report OUT at all, and a connecting socket
//! must report neither OUT nor ERR -- each of those is one line in the order
//! below and a user-visible hang or spin if it is in the wrong place.

use core::ffi::c_void;
use core::ptr;

const AF_UNIX: i32 = 1;

const SOCK_STREAM: i32 = 1;

const POLLIN: i16 = 0x001;
const POLLOUT: i16 = 0x004;
const POLLERR: i16 = 0x008;
const POLLHUP: i16 = 0x010;

const ENOTSOCK: i32 = 88;

#[repr(C)]
pub struct PollView {
    pub s: *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub closed: *mut i32,
    pub shut_rd: *mut i32,
    pub shut_wr: *mut i32,
    pub peer_closed: *mut i32,
    pub connected: *mut i32,
    pub local_tcp: *mut i32,
    pub tcp_connecting: *mut i32,
    pub rx_count: *mut i32,
    pub ch_ep: *mut *mut c_void,
    pub ch_len: *mut u32,
    pub rx_head: *mut *mut c_void,
    pub accept_head: *mut *mut c_void,
    pub peer: *mut *mut c_void,
}

unsafe extern "C" {
    fn a20_net_poll_socket_from_vfile(vf: *mut c_void) -> *mut c_void;
    fn a20_net_poll_view_init(s: *mut c_void, out: *mut PollView);
    fn a20_net_poll_lock(s: *mut c_void) -> u64;
    fn a20_net_poll_unlock(s: *mut c_void, flags: u64);
    fn a20_net_poll_channel_readable(ch_ep: *mut c_void) -> i32;
    fn a20_net_poll_channel_writable(ch_ep: *mut c_void) -> i32;
    fn a20_net_poll_channel_peer_closed(ch_ep: *mut c_void) -> i32;
    fn a20_net_poll_peer_rx_count(peer: *mut c_void) -> i32;
    fn a20_net_poll_max_queue() -> i32;
    fn a20_net_poll_error_pending(s: *mut c_void) -> i32;
}

fn zero_view() -> PollView {
    PollView {
        s: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        closed: ptr::null_mut(),
        shut_rd: ptr::null_mut(),
        shut_wr: ptr::null_mut(),
        peer_closed: ptr::null_mut(),
        connected: ptr::null_mut(),
        local_tcp: ptr::null_mut(),
        tcp_connecting: ptr::null_mut(),
        rx_count: ptr::null_mut(),
        ch_ep: ptr::null_mut(),
        ch_len: ptr::null_mut(),
        rx_head: ptr::null_mut(),
        accept_head: ptr::null_mut(),
        peer: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> PollView {
    let mut v = zero_view();
    a20_net_poll_view_init(s, &mut v);
    v
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_poll_file(vf: *mut c_void, events: i16) -> i32 {
    let s = a20_net_poll_socket_from_vfile(vf);
    if s.is_null() {
        return -ENOTSOCK;
    }
    let v = view_of(s);
    let mut revents: i16 = 0;
    let irq = a20_net_poll_lock(s);
    if a20_net_poll_error_pending(s) != 0 { revents |= POLLERR; }

    if !(*v.ch_ep).is_null() {
        let ch_ep = *v.ch_ep;
        let ch_rd = a20_net_poll_channel_readable(ch_ep) != 0 || *v.ch_len > 0;
        let ch_wr = a20_net_poll_channel_writable(ch_ep) != 0;
        let ch_pc = a20_net_poll_channel_peer_closed(ch_ep) != 0;
        if *v.peer_closed != 0 || ch_pc {
            revents |= POLLHUP;
        }
        if (events & POLLIN) != 0
            && (!(*v.rx_head).is_null()
                || ch_rd
                || *v.closed != 0
                || *v.peer_closed != 0
                || ch_pc
                || *v.shut_rd != 0)
        {
            revents |= POLLIN;
        }
        if (events & POLLOUT) != 0 && *v.closed == 0 && *v.shut_wr == 0 {
            if *v.peer_closed != 0 || ch_pc {
                revents |= POLLERR;
            } else if (*v.domain == AF_UNIX || *v.local_tcp != 0)
                && *v.sock_type == SOCK_STREAM
                && *v.connected != 0
                && (*v.peer).is_null()
            {
                revents |= POLLERR;
            } else if ch_wr || *v.rx_count < a20_net_poll_max_queue() {
                revents |= POLLOUT;
            }
        }
        a20_net_poll_unlock(s, irq);
        return revents as i32;
    }

    if *v.peer_closed != 0 {
        revents |= POLLHUP;
    } else if *v.closed != 0 || (*v.shut_rd != 0 && *v.shut_wr != 0) {
        revents |= POLLHUP;
    }
    if (events & POLLIN) != 0
        && (!(*v.rx_head).is_null()
            || !(*v.accept_head).is_null()
            || *v.closed != 0
            || *v.peer_closed != 0
            || *v.shut_rd != 0)
    {
        revents |= POLLIN;
    }
    if (events & POLLOUT) != 0 && *v.closed == 0 && *v.shut_wr == 0 {
        if *v.peer_closed != 0 {
            revents |= POLLERR;
        } else if *v.tcp_connecting != 0 {
            /* A connecting socket reports neither: the connection is neither
             * writable nor broken yet. */
        } else if (*v.domain == AF_UNIX || *v.local_tcp != 0)
            && *v.sock_type == SOCK_STREAM
            && *v.connected != 0
            && (*v.peer).is_null()
        {
            revents |= POLLERR;
        } else if !(*v.peer).is_null()
            && *v.sock_type == SOCK_STREAM
            && a20_net_poll_peer_rx_count(*v.peer) >= a20_net_poll_max_queue()
        {
            /* Deliberately a weakly consistent read: the peer may well be in
             * another bucket, and taking it here would turn poll(2) into a
             * reader that contends with the peer's data path.  The only
             * consequence of reading a slightly stale depth is one extra or one
             * missing POLLOUT in this single call. */
        } else {
            revents |= POLLOUT;
        }
    }
    a20_net_poll_unlock(s, irq);
    revents as i32
}

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Sock {
        domain: i32,
        sock_type: i32,
        closed: i32,
        shut_rd: i32,
        shut_wr: i32,
        peer_closed: i32,
        connected: i32,
        local_tcp: i32,
        tcp_connecting: i32,
        pending_error: i32,
        error_count: i32,
        rx_count: i32,
        ch_ep: *mut c_void,
        ch_len: u32,
        rx_head: *mut c_void,
        accept_head: *mut c_void,
        peer: *mut c_void,
    }

    #[no_mangle]
    extern "C" fn a20_net_poll_error_pending(s: *mut c_void) -> i32 {
        let s = unsafe { &*(s as *const Sock) };
        (s.pending_error != 0 || s.error_count != 0) as i32
    }

    impl Sock {
        const fn new() -> Sock {
            Sock {
                domain: AF_UNIX,
                sock_type: SOCK_STREAM,
                closed: 0,
                shut_rd: 0,
                shut_wr: 0,
                peer_closed: 0,
                connected: 1,
                local_tcp: 0,
                tcp_connecting: 0,
                pending_error: 0,
                error_count: 0,
                rx_count: 0,
                ch_ep: 0x4000 as *mut c_void,
                ch_len: 0,
                rx_head: ptr::null_mut(),
                accept_head: ptr::null_mut(),
                peer: 0x5000 as *mut c_void,
            }
        }
    }

    static mut S_SOCK: Sock = Sock::new();
    static mut T_CH_RD: i32 = 0;
    static mut T_CH_WR: i32 = 1;
    static mut T_CH_PC: i32 = 0;
    static mut T_PEER_RX: i32 = 0;
    static mut T_MAX_QUEUE: i32 = 4;
    static mut T_FROM_VFILE: i32 = 1;
    static mut T_PEER_READS: i32 = 0;

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
        S_SOCK = Sock::new();
        T_CH_RD = 0;
        T_CH_WR = 1;
        T_CH_PC = 0;
        T_PEER_RX = 0;
        T_MAX_QUEUE = 4;
        T_FROM_VFILE = 1;
        T_PEER_READS = 0;
    }

    unsafe fn me() -> &'static mut Sock {
        &mut *(ptr::addr_of_mut!(S_SOCK) as *mut Sock)
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_poll_socket_from_vfile(_vf: *mut c_void) -> *mut c_void {
        if T_FROM_VFILE != 0 {
            ptr::addr_of_mut!(S_SOCK) as *mut c_void
        } else {
            ptr::null_mut()
        }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_poll_view_init(s: *mut c_void, out: *mut PollView) {
        let k = &mut *(s as *mut Sock);
        *out = PollView {
            s,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            closed: &mut k.closed,
            shut_rd: &mut k.shut_rd,
            shut_wr: &mut k.shut_wr,
            peer_closed: &mut k.peer_closed,
            connected: &mut k.connected,
            local_tcp: &mut k.local_tcp,
            tcp_connecting: &mut k.tcp_connecting,
            rx_count: &mut k.rx_count,
            ch_ep: &mut k.ch_ep,
            ch_len: &mut k.ch_len,
            rx_head: &mut k.rx_head,
            accept_head: &mut k.accept_head,
            peer: &mut k.peer,
        };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_poll_lock(_s: *mut c_void) -> u64 {
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_poll_unlock(_s: *mut c_void, _f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_poll_channel_readable(_e: *mut c_void) -> i32 {
        T_CH_RD
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_poll_channel_writable(_e: *mut c_void) -> i32 {
        T_CH_WR
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_poll_channel_peer_closed(_e: *mut c_void) -> i32 {
        T_CH_PC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_poll_peer_rx_count(_p: *mut c_void) -> i32 {
        T_PEER_READS += 1;
        T_PEER_RX
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_poll_max_queue() -> i32 {
        T_MAX_QUEUE
    }

    #[test]
    fn error_queue_reports_pollerr_even_with_no_requested_events() {
        unsafe {
            let _serial = serial();
            reset();
            me().ch_ep = ptr::null_mut();
            me().sock_type = 2;
            me().error_count = 1;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), 0), POLLERR as i32);
            me().pending_error = 111;
            me().error_count = 0;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN), POLLERR as i32);
            me().pending_error = 0;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), 0), 0);
        }
    }

    #[test]
    fn a_non_socket_vfile_is_enotsock() {
        unsafe {
            let _serial = serial();
            reset();
            T_FROM_VFILE = 0;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN), -ENOTSOCK);
        }
    }

    #[test]
    fn a_quiet_channel_reports_writable_and_not_readable() {
        unsafe {
            let _serial = serial();
            reset();
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN | POLLOUT),
                       (POLLOUT) as i32);
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN), 0);
        }
    }

    #[test]
    fn a_channel_with_data_or_a_closed_peer_reads_readable() {
        unsafe {
            let _serial = serial();
            reset();
            T_CH_RD = 1;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN), POLLIN as i32);
            reset();
            me().ch_len = 3;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN), POLLIN as i32);
            reset();
            me().rx_head = 0x1 as *mut c_void;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN), POLLIN as i32);
            reset();
            me().shut_rd = 1;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN), POLLIN as i32);
        }
    }

    #[test]
    fn a_channel_peer_close_reports_hup_with_or_without_pollin() {
        unsafe {
            let _serial = serial();
            reset();
            T_CH_PC = 1;
            /* HUP is unconditional: the caller asked only for POLLOUT. */
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLOUT),
                       (POLLHUP | POLLERR) as i32);
            /* POLLIN is set too, because a closed peer is readable (EOF). */
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN),
                       (POLLHUP | POLLIN) as i32);
        }
    }

    #[test]
    fn a_channel_stream_connected_without_a_peer_is_an_error_on_write() {
        unsafe {
            let _serial = serial();
            reset();
            me().peer = ptr::null_mut();
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLOUT),
                       POLLERR as i32);
            /* A datagram socket with no peer is not: it just writes. */
            reset();
            me().peer = ptr::null_mut();
            me().sock_type = 2;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLOUT),
                       POLLOUT as i32);
            /* A channel that cannot take more is not writable, and not an
             * error either. */
            reset();
            T_CH_WR = 0;
            me().rx_count = 4;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLOUT), 0);
        }
    }

    #[test]
    fn a_shut_or_closed_write_side_reports_no_out() {
        unsafe {
            let _serial = serial();
            reset();
            me().shut_wr = 1;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLOUT), 0);
            reset();
            me().closed = 1;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLOUT), 0);
        }
    }

    #[test]
    fn the_legacy_arm_reports_hup_for_a_closed_or_double_shut_socket() {
        unsafe {
            let _serial = serial();
            reset();
            me().ch_ep = ptr::null_mut();
            me().closed = 1;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN),
                       (POLLHUP | POLLIN) as i32);
            /* Both halves shut is HUP; one half is not. */
            reset();
            me().ch_ep = ptr::null_mut();
            me().shut_rd = 1;
            me().shut_wr = 1;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), 0), POLLHUP as i32);
            reset();
            me().ch_ep = ptr::null_mut();
            me().shut_wr = 1;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), 0), 0);
        }
    }

    #[test]
    fn the_legacy_arm_reads_a_queued_message_or_a_pending_accept() {
        unsafe {
            let _serial = serial();
            reset();
            me().ch_ep = ptr::null_mut();
            me().rx_head = 0x1 as *mut c_void;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN), POLLIN as i32);
            reset();
            me().ch_ep = ptr::null_mut();
            me().accept_head = 0x1 as *mut c_void;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN), POLLIN as i32);
            /* A quiet socket reports nothing. */
            reset();
            me().ch_ep = ptr::null_mut();
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN | POLLOUT),
                       POLLOUT as i32);
        }
    }

    #[test]
    fn the_legacy_arm_errors_on_a_closed_peer_before_anything_else() {
        unsafe {
            let _serial = serial();
            reset();
            me().ch_ep = ptr::null_mut();
            me().peer_closed = 1;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLIN | POLLOUT),
                       (POLLHUP | POLLIN | POLLERR) as i32);
        }
    }

    #[test]
    fn a_connecting_socket_reports_neither_out_nor_err() {
        unsafe {
            let _serial = serial();
            reset();
            me().ch_ep = ptr::null_mut();
            me().tcp_connecting = 1;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLOUT), 0);
        }
    }

    #[test]
    fn a_full_peer_queue_reports_no_out_and_reads_the_peer_depth() {
        unsafe {
            let _serial = serial();
            reset();
            me().ch_ep = ptr::null_mut();
            T_MAX_QUEUE = 4;
            T_PEER_RX = 4;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLOUT), 0);
            assert_eq!(T_PEER_READS, 1);
            /* Below the cap it is writable. */
            reset();
            me().ch_ep = ptr::null_mut();
            T_PEER_RX = 3;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLOUT),
                       POLLOUT as i32);
            /* A datagram socket never consults the peer's depth. */
            reset();
            me().ch_ep = ptr::null_mut();
            me().sock_type = 2;
            T_PEER_RX = 99;
            assert_eq!(a20_net_poll_file(ptr::null_mut(), POLLOUT),
                       POLLOUT as i32);
            assert_eq!(T_PEER_READS, 0);
        }
    }
}
