/*
 * C ABI of the Rust socket option state machine
 * (kernel/net/socket/socket_options.rs, selected by NET_SOCKET_OPTIONS_IMPL=rust).
 *
 * Rust owns the option policy and the per-option state transitions; C owns the
 * primitives it calls (the socket lock, the lwIP control barrier, usercopy, the
 * pidfd allocator, the IGMP/netif lookup, the pcb option application) and the
 * borrowed view of the socket's option fields defined below. No struct ABI
 * moves: `net_option_view_init()` fills a table of pointers into the existing
 * `net_socket_t`.
 *
 * Return conventions:
 *   a20_net_setsockopt_state  >= 0: a mask of what the caller must re-apply
 *                                  after this returns (0 = nothing). Bits 0..8
 *                                  are the net_tcp_options_apply_current()
 *                                  groups and are passed straight to it; bit 9
 *                                  (NET_OPT_APPLY_IP_OPTS) means
 *                                  net_inet_ip_opts_apply(). Every return is
 *                                  either a non-negative mask or a negative
 *                                  -errno, so no mask bit may be the sign bit.
 *                             < 0: -errno.
 *   a20_net_getsockopt_state  0: `rep` describes what to copy out; < 0: -errno.
 */
#ifndef _NET_SOCKET_OPTIONS_STATE_H
#define _NET_SOCKET_OPTIONS_STATE_H

#include <stddef.h>
#include <stdint.h>

/* What a getsockopt reports.  `kind` selects the shape:
 *   0 int       val0
 *   1 string    text[0..len)
 *   2 cred      val0/val1/val2 = pid/uid/gid
 *   3 timeval   val0/val1 = sec/usec, each a `long`
 *   4 pidfd     val0 = peer pid; the caller creates the descriptor
 * `len` is the byte count after the caller's *optlen clamp, which is part of
 * the ABI and therefore decided in Rust. */
#define NET_OPT_REPORT_INT     0
#define NET_OPT_REPORT_STRING  1
#define NET_OPT_REPORT_CRED    2
#define NET_OPT_REPORT_TIMEVAL 3
#define NET_OPT_REPORT_PIDFD   4
#define NET_OPT_REPORT_TIMEVAL64 5

typedef struct net_opt_report {
    int32_t  kind;
    size_t   len;
    int64_t  val0;
    int64_t  val1;
    int64_t  val2;
    uint8_t  text[16];
} net_opt_report_t;

/*
 * Borrowed view of the option-bearing fields of net_socket_t.  The layout must
 * match `NetOptionView` in kernel/net/socket/socket_options.rs field for field; the
 * order is the reason both are written out rather than derived.
 */
typedef struct net_option_view {
    void       *s;
    const int  *domain;
    const int  *sock_type;
    const int  *protocol;
    const int  *connected;
    const int  *bound;
    const int  *listening;
    uint8_t    *ip_ttl;
    uint8_t    *ip_tos;
    uint8_t    *ip_ttl_set;
    uint8_t    *ip_tos_set;
    uint8_t    *mc_ttl;
    uint8_t    *mc_loop;
    uint32_t   *ip_opts_generation;
    int        *ipv6_checksum_offset;
    int        *ipv6_v6only;
    /* First of twelve *contiguous* `int` RECV flags; socket_control.c asserts
     * the layout, because the Rust side indexes this run. */
    int        *ipv6_recv_flags;
    uint32_t   *icmp6_filter;
    int        *icmp6_filter_set;
    uint8_t    *tcp_congestion;
    int        *tcp_nodelay;
    int        *keepalive;
    int        *keep_idle;
    int        *keep_intvl;
    int        *keep_cnt;
    uint32_t   *tcp_opts_generation;
    uint32_t   *snd_buf;
    uint32_t   *rcv_buf;
    int        *reuseaddr;
    int        *reuseport;
    int        *broadcast;
    uint64_t   *recv_timeout_ticks;
    uint64_t   *send_timeout_ticks;
    int        *passcred;
    int        *ipv4_recv_pktinfo;
    const int32_t *peer_pid;
    const int32_t *peer_uid;
    const int32_t *peer_gid;
    int         long_size;
    /* TICKS_PER_SEC as the compiled target defines it.  On riscv64 that is a
     * runtime function of the timebase, so it cannot be a Rust constant. */
    uint64_t    ticks_per_sec;
} net_option_view_t;

/* pcb-apply groups, mirroring NET_TCP_APPLY_* in socket_control.c. */
#define NET_OPT_APPLY_PCB_MASK 0x1ff
/* Re-run net_inet_ip_opts_apply(); see the comment above. */
#define NET_OPT_APPLY_IP_OPTS  (1 << 9)

/* Primitives the Rust module calls back into.  Defined next to the option state
 * machine in kernel/net/socket/socket_control.c, so the lock discipline they encode
 * stays with the code that used to open-code it. */
void     a20_net_option_lock(void *s, uint64_t *flags);
void     a20_net_option_unlock(void *s, uint64_t flags);
uint32_t a20_net_option_buf_ceiling(void *s, int is_snd);
int a20_net_option_recverr(void *s, int family, int value, int set);
int      a20_net_option_group_membership(void *s, const void *mreq, int join);
int      a20_net_option_tcp_live(void *s, int which);
int      a20_net_option_ip_effective(void *s, int which);
int      a20_net_option_notconn_pidfd(void);

int a20_net_setsockopt_state(const net_option_view_t *v, int level,
                             int optname, const void *optval, size_t optlen);
int a20_net_getsockopt_state(const net_option_view_t *v, int level,
                             int optname, size_t optlen, net_opt_report_t *rep);

#endif /* _NET_SOCKET_OPTIONS_STATE_H */
