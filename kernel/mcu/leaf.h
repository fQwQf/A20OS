#ifndef _MCU_LEAF_H
#define _MCU_LEAF_H

/*
 * MCU leaf protocol face -- the leaf half of the cluster UART transport.
 *
 * WC1 track owns kernel/mcu/ (docs/cluster/impl-prompts.md 文件所有权表).
 * This file is the side a STM32-class leaf runs: it only ever ANSWERS.
 * The head-node counterpart lives in kernel/cluster/uart.c; both share the
 * wire codec in kernel/cluster/uart.h so the two ends cannot drift.
 *
 * Spec basis:
 *   docs/cluster/04-transports.md §4  leaf-side constraints: which frame
 *       types are answered, the DISCONNECTED/UP two-state machine, "50s 无
 *       PING 回 DISCONNECTED，短地址作废", the static 512 B RX / 320 B TX
 *       buffers, and the 0xFFFF unassigned-address rule.
 *   docs/cluster/02-wire-protocol.md §6  HELLO payload and short-address
 *       assignment; §2 frame types; §10 counters.
 *   docs/cluster/01-abi.md §能力位  CAP_LEAF: "只应答，不主动建链".
 *
 * Handshake direction (recorded here because 02 §6 can be read two ways):
 * the HEAD dials -- it sends HELLO carrying the assigned short address, the
 * leaf PASSIVELY answers HELLO_ACK.  This is the reading 01-abi's CAP_LEAF
 * ("不主动建链"), 04-§4's "HELLO/HELLO_ACK（被动应答）" and the WC1 brief's
 * "HELLO 被动应答" all agree on.  The 0xFFFF rule then lands as a TX gate:
 * while the leaf holds no assigned address its only lawful output is
 * HELLO-family frames, and since it never originates frames at all, the
 * gate in practice admits only the HELLO_ACK reply.
 *
 * Memory discipline (03-kernel-impl.md §3, MCU tier): every buffer here is
 * static; nothing in this file allocates.
 */

#include "cluster/uart.h"

/* ------------------------------------------------------------------ */
/* Built-in demo operator: u32 vector dot product.                     */
/*                                                                     */
/* CALL payload (dst_slot = A20_MCU_LEAF_OP_DOT_SLOT):                 */
/*     u32 n;  i32 a[n];  i32 b[n]     -- little-endian, exact length  */
/*                                        4 + 8n bytes.                */
/* CALL_REPLY payload:                                                 */
/*     u32 status;  i64 dot            -- 12 bytes, little-endian.     */
/*                                        status 0 = success; a nonzero*/
/*                                        value is an A20_ERR_* number */
/*                                        (kernel/include/ipc/ipc.h)   */
/*                                        reported at application      */
/*                                        level, NOT via the ERROR     */
/*                                        frame, whose errno space is  */
/*                                        cluster-only (02 §8).        */
/* The dot product accumulates in uint64 (defined wraparound); the i64  */
/* bit pattern is the answer.  UART MTU 256 caps n at 27               */
/* (4 + 8n <= 222 = MTU - 32 - 2).                                     */
/* ------------------------------------------------------------------ */

#define A20_MCU_LEAF_OP_DOT_SLOT 1u /* slot 0 is reserved (01-abi) */

/* Leaf link state, 04 §4: exactly two states. */
#define A20_MCU_LEAF_DISCONNECTED 0u
#define A20_MCU_LEAF_UP           1u

/*
 * Wire the leaf into the board bring-up: resets the protocol state, installs
 * the default demo identity and arms the decoder.  Safe to call before any
 * traffic.  The default identity is the printable string "MCU-LEAF-DEMO-01";
 * override with a20_mcu_leaf_set_identity() before the first HELLO lands.
 */
void a20_mcu_leaf_init(void);

/* Replace the leaf's 16-byte node identity (01-abi §节点标识: neither
 * all-zero (LOCAL) nor all-0xff (BROADCAST) may be used on the wire). */
void a20_mcu_leaf_set_identity(const uint8_t node_id[A20_CLX_NODE_ID_LEN]);

/*
 * The polling pump: drain the UART RX ring through the SLIP decoder, run
 * the 50 ms inter-byte resync and the 50 s idle watchdog.  Call from a
 * periodic thread context; it never blocks for long (one frame's worth of
 * uart_putc busy-wait is the worst case, ~30 ms at 115200 8N1 for a
 * full-MTU frame, 04 §4).
 *
 * Cadence note: the MCU console UART ring (kernel/mcu/uart.c) is 128 B; at
 * 115200 baud it fills in ~11 ms, so the pump period must stay well below
 * that or the ring overflows (the head sees a dropped frame, its timer
 * recovers).  The 5 ms stm32 peripheral-thread loop satisfies this.
 */
void a20_mcu_leaf_poll(void);

/* Observability: state, short address and the 02 §10 counter set. */
typedef struct a20_mcu_leaf_status {
    uint32_t state; /* A20_MCU_LEAF_* */
    uint16_t short_addr;
    a20_clx_link_counters_t counters;
} a20_mcu_leaf_status_t;

void a20_mcu_leaf_get_status(a20_mcu_leaf_status_t *out);

/* Test seam: feed one already-decoded CL frame (header + payload + CRC)
 * straight into the protocol face, bypassing the UART and the SLIP
 * decoder.  Used by host-side harnesses; production callers use poll(). */
void a20_mcu_leaf_on_frame(const uint8_t *frame, uint32_t len);

#endif /* _MCU_LEAF_H */
