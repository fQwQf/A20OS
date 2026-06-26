use a20rust_support::lock::spinlock_t;
use core::ffi::{c_char, c_int, c_void};

pub const AF_UNIX: c_int = 1;
pub const SOCK_STREAM: c_int = 1;
pub const SOCK_SEQPACKET: c_int = 5;

pub const NET_SOCKADDR_MAX: usize = 128;
pub const NET_MAX_PAYLOAD: usize = 65535;
pub const MAX_PATH_LEN: usize = 512;
pub const NET_MAX_QUEUE: c_int = 128;

pub const O_RDWR: c_int = 2;
pub const O_CREAT: c_int = 0x40;
pub const O_EXCL: c_int = 0x80;

pub const S_IFMT: u32 = 0o170000;
pub const S_IFDIR: u32 = 0o040000;

pub const EEXIST: c_int = 17;
pub const ENOTDIR: c_int = 20;
pub const EINVAL: c_int = 22;
pub const ENAMETOOLONG: c_int = 36;
pub const ENOMEM: c_int = 12;
pub const EAGAIN: c_int = 11;
pub const EDESTADDRREQ: c_int = 89;
pub const EAFNOSUPPORT: c_int = 97;
pub const ENOTSOCK: c_int = 88;
pub const EADDRINUSE: c_int = 98;
pub const ECONNREFUSED: c_int = 111;

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
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
    pub alg_last: [u8; 2048],
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

#[repr(C)]
pub struct kstat_t {
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

unsafe extern "C" {
    pub static mut g_net_lock: spinlock_t;

    pub fn net_find_bound_socket_locked(
        domain: c_int,
        type_: c_int,
        addr: *const c_void,
        addrlen: usize,
    ) -> *mut net_socket_t;
    pub fn net_accept_queue_push_locked(
        listener: *mut net_socket_t,
        child: *mut net_socket_t,
    ) -> c_int;
    pub fn net_socket_alloc() -> *mut net_socket_t;
    pub fn net_socket_free(socket: *mut net_socket_t);
    pub fn net_socket_is_valid_locked(socket: *mut net_socket_t) -> c_int;
    pub fn net_enqueue_msg_locked(
        dst: *mut net_socket_t,
        buf: *const c_void,
        len: usize,
        addr: *const c_void,
        addrlen: usize,
    ) -> c_int;

    pub fn a20_unix_current_cwd() -> *const c_char;
    pub fn a20_unix_vfs_stat(path: *const c_char, st: *mut kstat_t) -> c_int;
    pub fn a20_unix_vfs_open(path: *const c_char, flags: c_int, mode: c_int) -> c_int;
    pub fn a20_unix_vfs_close(fd: c_int) -> c_int;
}
