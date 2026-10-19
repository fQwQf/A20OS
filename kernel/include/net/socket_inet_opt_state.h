/*
 * C ABI of the Rust INET socket option and buffer machinery
 * (kernel/net/socket_inet_opt.rs, selected by NET_SOCKET_INET_IMPL=rust).
 *
 * Rust owns the congestion-algorithm switch, the per-type buffer ceilings and
 * the value in force, the SO_SNDBUF/SO_RCVBUF application with its clamps and
 * the shrink-announce guard, the full TCP option application and callback
 * wiring, and the IP-option snapshot, retry loop, effective values and reapply.
 * C owns the pcb field access, the lwIP calls, the locks and the constants the
 * build configures.
 */
#ifndef _NET_SOCKET_INET_OPT_STATE_H
#define _NET_SOCKET_INET_OPT_STATE_H

#include <stddef.h>
#include <stdint.h>

/* The snapshot layout Rust reads and writes; the C struct it mirrors is
 * net_inet_ip_options_snapshot_t (ttl, tos, mc_ttl, mc_loop, generation). */
typedef struct net_inet_opt_snapshot {
    uint8_t ttl;
    uint8_t tos;
    uint8_t mc_ttl;
    uint8_t mc_loop;
    uint32_t generation;
} net_inet_opt_snapshot_t;

typedef struct net_inet_opt_view {
    void *s;
    int *sock_type;
    int *tcp_nodelay;
    uint8_t *tcp_congestion;
    uint32_t *snd_buf;
    uint32_t *rcv_buf;
    int *keepalive;
    int *keep_idle;
    int *keep_intvl;
    int *keep_cnt;
    int *reuseaddr;
    uint8_t *ip_ttl_set;
    uint8_t *ip_ttl;
    uint8_t *ip_tos_set;
    uint8_t *ip_tos;
    uint8_t *mc_ttl;
    uint8_t *mc_loop;
    uint32_t *ip_opts_generation;
    void **udp;
    void **raw;
    void **tcp;
} net_inet_opt_view_t;

void a20_net_inet_opt_view_init(void *s, net_inet_opt_view_t *out);

void a20_net_inet_opt_sock_lock(void *s, uint64_t *flags);
void a20_net_inet_opt_sock_unlock(void *s, uint64_t flags);
uint64_t a20_net_inet_opt_lane_lock(void *s);
void a20_net_inet_opt_lane_unlock(uint64_t flags);
uint64_t a20_net_inet_opt_shared_lock(void *pcb, int kind);
void a20_net_inet_opt_shared_unlock(void *pcb, int kind, uint64_t flags);
uint32_t a20_net_inet_opt_tcp_snd_buf(void);
uint32_t a20_net_inet_opt_tcp_wnd(void);
uint32_t a20_net_inet_opt_tcp_rcv_scale(void);
uint32_t a20_net_inet_opt_max_payload(void);
uint32_t a20_net_inet_opt_dgram_rcv_default(void);
int a20_net_inet_opt_pcb_is_listen(void *pcb);
void a20_net_inet_opt_set_snd_buf(void *pcb, uint32_t v);
void a20_net_inet_opt_set_wnd_limit(void *pcb, uint32_t v);
uint32_t a20_net_inet_opt_get_rcv_wnd(void *pcb);
void a20_net_inet_opt_set_rcv_wnd(void *pcb, uint32_t v);
uint32_t a20_net_inet_opt_get_rcv_ann_wnd(void *pcb);
void a20_net_inet_opt_recved0(void *pcb);
void a20_net_inet_opt_nagle_disable(void *pcb);
int a20_net_inet_opt_cong_get(void *pcb);
void a20_net_inet_opt_cong_set(void *pcb, int alg);
void a20_net_inet_opt_cubic_init(void *pcb);
void a20_net_inet_opt_set_keepalive(void *pcb);
void a20_net_inet_opt_set_reuseaddr(void *pcb);
void a20_net_inet_opt_set_keep_idle(void *pcb, uint32_t v);
void a20_net_inet_opt_set_keep_intvl(void *pcb, uint32_t v);
void a20_net_inet_opt_set_keep_cnt(void *pcb, uint32_t v);
void a20_net_inet_opt_set_callbacks(void *s, void *pcb);
void a20_net_inet_opt_apply_udp(void *pcb, uint8_t ttl, uint8_t tos,
                                uint8_t mc_ttl, uint8_t mc_loop);
void a20_net_inet_opt_apply_raw(void *pcb, uint8_t ttl, uint8_t tos,
                                uint8_t mc_ttl, uint8_t mc_loop);
void a20_net_inet_opt_apply_tcp(void *pcb, uint8_t ttl, uint8_t tos);

void a20_net_cong_apply_impl(void *pcb, uint8_t alg);
uint32_t a20_net_socket_buf_ceiling(void *s, int is_snd);
uint32_t a20_net_socket_buf_in_force(void *s, int is_snd);
void a20_net_inet_tcp_buf_apply(uint32_t snd_buf, uint32_t rcv_buf, void *pcb);
void a20_net_inet_tcp_apply_options(void *s, void *pcb);
void a20_net_inet_ip_options_snapshot(void *s, void *out);
uint64_t a20_net_inet_socket_ip_lane_lock(void *s, void *out);
void a20_net_inet_ip_effective(void *s, uint8_t *ttl, uint8_t *tos,
                               uint8_t *mc_ttl);
void a20_net_inet_ip_opts_apply_snapshot_locked(void *s, const void *opts);
void a20_net_inet_ip_opts_apply(void *s);

#endif /* _NET_SOCKET_INET_OPT_STATE_H */
