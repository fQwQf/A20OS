use a20rust_support::lock::spinlock_t;
use core::ffi::{c_char, c_int, c_long, c_void};

pub const AF_INET: c_int = 2;
pub const AF_INET6: c_int = 10;
pub const AF_ALG: c_int = 38;

pub const SOCK_STREAM: c_int = 1;
pub const SOCK_DGRAM: c_int = 2;
pub const SOCK_SEQPACKET: c_int = 5;

pub const NET_MAX_STREAM_PAYLOAD: usize = 2048;
pub const NET_MAX_PAYLOAD: usize = 65535;
pub const NET_MAX_QUEUE: c_int = 128;
pub const NET_SOCKADDR_MAX: usize = 128;
pub const PROC_BLOCKED: c_int = 3;

pub const EAGAIN: c_int = 11;
pub const ENOMEM: c_int = 12;
pub const ESPIPE: c_int = 29;
pub const EPIPE: c_int = 32;
pub const ENOTSOCK: c_int = 88;
pub const EDESTADDRREQ: c_int = 89;
pub const EMSGSIZE: c_int = 90;
pub const ECONNREFUSED: c_int = 111;
pub const ERESTARTSYS: c_int = 512;

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct vfile_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct vfile_ops_t {
    pub read: Option<extern "C" fn(*mut vfile_t, *mut c_char, usize) -> c_int>,
    pub write: Option<extern "C" fn(*mut vfile_t, *const c_char, usize) -> c_int>,
    pub lseek: Option<extern "C" fn(*mut vfile_t, c_long, c_int) -> c_long>,
    pub readdir: Option<extern "C" fn(*mut vfile_t, *mut c_void, usize) -> c_int>,
    pub ioctl: Option<extern "C" fn(*mut vfile_t, u64, *mut c_void) -> c_int>,
    pub close: Option<extern "C" fn(*mut vfile_t) -> c_int>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct net_bh_event_t {
    pub next: *mut net_bh_event_t,
    pub type_: c_int,
    pub err: c_int,
    pub len: usize,
    pub addr: [u8; NET_SOCKADDR_MAX],
    pub addrlen: usize,
    pub has_pktinfo: u8,
    pub has_hoplimit: u8,
    pub has_tclass: u8,
    pub pad: u8,
    pub pktinfo_ifindex: u32,
    pub pktinfo_addr: [u8; 16],
    pub hoplimit: u8,
    pub tclass: u8,
    pub __pad_meta: u16,
    pub data: [u8; NET_MAX_PAYLOAD],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct net_bh_ring_t {
    pub events: [net_bh_event_t; 16],
    pub head: u32,
    pub tail: u32,
}

#[repr(C)]
pub struct net_msg_t {
    pub next: *mut net_msg_t,
    pub len: usize,
    pub off: usize,
    pub addr: [u8; NET_SOCKADDR_MAX],
    pub addrlen: usize,
    pub has_pktinfo: u8,
    pub has_hoplimit: u8,
    pub has_tclass: u8,
    pub pad: u8,
    pub pktinfo_ifindex: u32,
    pub pktinfo_addr: [u8; 16],
    pub hoplimit: u8,
    pub tclass: u8,
    pub __pad_meta: u16,
    pub data: [u8; NET_MAX_PAYLOAD],
}

#[repr(C)]
pub struct net_socket_t {
    pub domain: c_int,
    pub type_: c_int,
    pub protocol: c_int,
    pub nonblock: c_int,
    pub closed: c_int,
    pub shut_rd: c_int,
    pub shut_wr: c_int,
    pub peer_closed: c_int,
    pub bound: c_int,
    pub connected: c_int,
    pub listening: c_int,
    pub local: [u8; NET_SOCKADDR_MAX],
    pub local_len: usize,
    pub peer_addr: [u8; NET_SOCKADDR_MAX],
    pub peer_len: usize,
    pub peer: *mut net_socket_t,
    pub rx_head: *mut net_msg_t,
    pub rx_tail: *mut net_msg_t,
    pub rx_count: c_int,
    pub waiter: *mut task_t,
    pub send_waiter: *mut task_t,
    pub udp: *mut c_void,
    pub raw: *mut c_void,
    pub tcp: *mut c_void,
    pub local_tcp: c_int,
    pub tcp_connecting: c_int,
    pub tcp_err: c_int,
    pub tcp_nodelay: c_int,
    pub reuseaddr: c_int,
    pub reuseport: c_int,
    pub keepalive: c_int,
    pub keep_idle: c_int,
    pub keep_intvl: c_int,
    pub keep_cnt: c_int,
    pub recv_timeout_ticks: u64,
    pub send_timeout_ticks: u64,
    pub ipv6_checksum_offset: c_int,
    pub icmp6_filter: [u32; 8],
    pub icmp6_filter_set: c_int,
    pub ipv6_recv_pktinfo: c_int,
    pub ipv6_recv_tclass: c_int,
    pub ipv6_recv_hoplimit: c_int,
    pub ipv6_recv_rthdr: c_int,
    pub ipv6_recv_hopopts: c_int,
    pub ipv6_recv_dstopts: c_int,
    pub ipv6_recv_err: c_int,
    pub ipv6_recv_2292_pktinfo: c_int,
    pub ipv6_recv_2292_hoplimit: c_int,
    pub ipv6_recv_2292_rthdr: c_int,
    pub ipv6_recv_2292_hopopts: c_int,
    pub ipv6_recv_2292_dstopts: c_int,
    pub bpf_prog_fd: c_int,
    pub alg_last: [u8; NET_MAX_STREAM_PAYLOAD],
    pub alg_last_len: usize,
    pub alg_type: [u8; 16],
    pub alg_name: [u8; 64],
    pub accept_next: *mut net_socket_t,
    pub accept_head: *mut net_socket_t,
    pub accept_tail: *mut net_socket_t,
    pub accept_count: c_int,
    pub in_registry: c_int,
    pub reg_idx: c_int,
    pub bh_ring: net_bh_ring_t,
    pub bh_connected: c_int,
    pub bh_closed: c_int,
    pub bh_error: c_int,
    pub bh_err_code: c_int,
    pub bh_tx_wake: c_int,
    pub bh_pending: c_int,
}

unsafe extern "C" {
    pub static mut g_net_lock: spinlock_t;

    pub fn timer_get_ticks() -> u64;

    pub fn vfile_alloc() -> *mut vfile_t;
    pub fn vfile_free(vf: *mut vfile_t);
    pub fn vfile_ref_init(vf: *mut vfile_t, refs: c_int);
    pub fn vfs_alloc_fd(vf: *mut vfile_t) -> c_int;
    pub fn vfs_get_file_ref(fd: c_int) -> *mut vfile_t;
    pub fn vfs_put_file_ref(fd: c_int, vf: *mut vfile_t);

    pub fn net_dequeue_msg_locked(
        s: *mut net_socket_t,
        buf: *mut c_void,
        len: usize,
        addr: *mut c_void,
        addrlen: *mut usize,
    ) -> c_int;
    pub fn net_enqueue_msg_locked(
        dst: *mut net_socket_t,
        buf: *const c_void,
        len: usize,
        addr: *const c_void,
        addrlen: usize,
    ) -> c_int;
    pub fn net_enqueue_msg_blocking(
        s: *mut net_socket_t,
        dst: *mut net_socket_t,
        buf: *const c_void,
        len: usize,
        addr: *const c_void,
        addrlen: usize,
        dontwait: c_int,
        timeout_ticks: u64,
    ) -> c_int;
    pub fn net_socket_is_valid_locked(s: *mut net_socket_t) -> c_int;
    pub fn net_find_bound_socket_locked(
        domain: c_int,
        type_: c_int,
        addr: *const c_void,
        addrlen: usize,
    ) -> *mut net_socket_t;
    pub fn net_unregister_socket_locked(s: *mut net_socket_t);
    pub fn net_inet_socket_destroy(s: *mut net_socket_t);
    pub fn net_socket_free(s: *mut net_socket_t);
    pub fn net_alloc_ephemeral_port_locked() -> u16;
    pub fn net_sockaddr_loopback(s: *mut net_socket_t, port: u16);
    pub fn net_task_has_unblocked_signal(t: *mut task_t) -> c_int;
    pub fn net_socket_wait_expired(s: *mut net_socket_t, start: u64, for_write: c_int) -> c_int;
    pub fn net_block_on_socket_locked(s: *mut net_socket_t, cur: *mut task_t);
    pub fn net_clear_socket_waiter(s: *mut net_socket_t, cur: *mut task_t);
    pub fn net_alg_socket_recv(s: *mut net_socket_t, buf: *mut c_void, len: usize) -> c_int;
    pub fn net_alg_socket_send(s: *mut net_socket_t, buf: *const c_void, len: usize) -> c_int;
    pub fn net_inet_sendto(
        s: *mut net_socket_t,
        buf: *const c_void,
        len: usize,
        flags: c_int,
        addr: *const c_void,
        addrlen: usize,
    ) -> c_int;
    pub fn net_tcp_recved(s: *mut net_socket_t, len: usize);
    pub fn net_msg_free(m: *mut net_msg_t);

    pub fn a20_socket_file_lwip_poll();
    pub fn a20_socket_file_proc_current() -> *mut task_t;
    pub fn a20_socket_file_sched();
    pub fn a20_socket_file_proc_make_ready(task: *mut task_t);
    pub fn a20_socket_file_task_state(task: *mut task_t) -> c_int;
    pub fn a20_socket_file_vfile_priv(vf: *mut vfile_t) -> *mut c_void;
    pub fn a20_socket_file_vfile_set_priv(vf: *mut vfile_t, priv_ptr: *mut c_void);
    pub fn a20_socket_file_vfile_ops_match(vf: *mut vfile_t, ops: *mut vfile_ops_t) -> c_int;
    pub fn a20_socket_file_vfile_init(
        vf: *mut vfile_t,
        ops: *mut vfile_ops_t,
        priv_ptr: *mut c_void,
        flags: c_int,
    );
}
