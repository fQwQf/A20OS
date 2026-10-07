/*
 * Cluster transport contract (docs/cluster/04-transports.md §1) -- WA-owned
 * seam every transport plugs into (03-kernel-impl.md §2 transport.c row).
 *
 * Registrants (04-§1): loopback id 0 (kernel/cluster/loopback.c, all tiers),
 * UDP id 1 (kernel/cluster/udp.c, WA2), UART id 2 (kernel/cluster/uart.c,
 * WC1's head node wrapped here per 04-§4 实现期记录 note 4).
 */
#ifndef _CLUSTER_TRANSPORT_H
#define _CLUSTER_TRANSPORT_H

#include "core/types.h"

/* transport_id values (kernel/include/abi/native/types.h). */
#define A20_CLX_TRANSPORT_LOOPBACK_ID 0u
#define A20_CLX_TRANSPORT_UDP_ID      1u
#define A20_CLX_TRANSPORT_UART_ID     2u

/* 04-§1 flags. */
#define A20_CLX_TFL_RELIABLE_CAPABLE  (1u << 0)
#define A20_CLX_TFL_BROADCAST_CAPABLE (1u << 1)
#define A20_CLX_TFL_POLLING           (1u << 2)

#define A20_CLX_MAX_TRANSPORTS 8u
#define A20_CLX_LINK_ADDR_MAX  16u

typedef struct a20_clx_transport {
    uint32_t transport_id;     /* 0=loopback 1=udp 2=uart, fixed at注册 */
    uint32_t mtu;              /* 单帧上限，含帧头与 CRC (04-§1) */
    uint32_t flags;            /* A20_CLX_TFL_* */
    /* 发送一帧到下一跳链路地址；返回 0 或 -errno。允许丢帧（04-§1）。 */
    int  (*send)(void *ctx, const uint8_t *next_hop, uint32_t nh_len,
                 const void *frame, uint32_t len);
    /* POLLING 档收包泵；非 POLLING 档置 NULL。 */
    void (*poll)(void *ctx);
    void *ctx;
} a20_clx_transport_t;

/* 注册表：先注册先得，重复 id 拒绝（-A20_ERR_EXISTS 语义，返回 -1）。
 * 注册即触发 lazy core init（RX 线程等）。 */
int  a20_clx_transport_register(const a20_clx_transport_t *t);
const a20_clx_transport_t *a20_clx_transport_get(uint32_t transport_id);
uint32_t a20_clx_transport_mtu(uint32_t transport_id);

/* ---- 上行接口（04-§1，传输 -> cluster 核心唯一入口） -------------------- */

/*
 * 收一帧。可在（未来的）中断上下文调用：内部只入环形缓冲并唤醒 RX 线程，
 * 绝不拿 g_clx_lock。帧为传输层实收的完整 CL 帧（头+载荷+CRC）。
 */
void a20_clx_rx_frame(uint32_t transport_id, const uint8_t *next_hop_src,
                      uint32_t nh_len, const uint8_t *frame, uint32_t len);

/* 链路级事件，进事件订阅与路由失效判定（04-§1）。 */
void a20_clx_link_event(uint32_t transport_id, const uint8_t *next_hop,
                        uint32_t nh_len, int up);

/* ---- 链路计数（02-§10，cluster_link_status 的数据源） ------------------- */

/* RX 侧丢弃/畸形计数（无链路上下文时 nh 传 NULL）。 */
void a20_clx_link_rx_drop(uint32_t transport_id, const uint8_t *nh,
                          uint32_t nh_len);
void a20_clx_link_rx_malformed(uint32_t transport_id, const uint8_t *nh,
                               uint32_t nh_len);
void a20_clx_link_tx_drop(uint32_t transport_id, const uint8_t *nh,
                          uint32_t nh_len);

/* ---- loopback 虚拟节点注册表（04-§2，route.c 经此挂/摘虚拟节点） -------- */

#define A20_CLX_LOOPBACK_SELF_ADDR 0x00000000u

int a20_clx_loopback_vnode_add(const a20_node_id_t *node,
                               const uint8_t *next_hop, uint32_t nh_len);
int a20_clx_loopback_vnode_del(const a20_node_id_t *node);

#endif /* _CLUSTER_TRANSPORT_H */
