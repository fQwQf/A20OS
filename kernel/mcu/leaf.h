#ifndef _MCU_LEAF_H
#define _MCU_LEAF_H

/* MCU leaf protocol face. The head initiates discovery; a v1 leaf also
 * challenges the head before adopting its boot epoch. All storage is static.
 * Board byte I/O can be bound before the first handshake. */

#include "cluster/uart.h"
#include "cluster/operator.h"
#include "uapi/a20/cluster_work.h"

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

#define A20_MCU_LEAF_OP_DOT_SLOT A20_CLX_OPERATOR_DOT
#define A20_MCU_LEAF_WORK_SLOT 2u
#define A20_MCU_LEAF_SINK_SLOT 3u

/* One optional static receive-only service, without heap storage. Install
 * before discovery. The bounded callback returns an A20_ERR_* status, accepts
 * <=206 bytes, and must not retain the input pointer or re-enter the leaf.
 * A successful receipt means callback admission, not durable completion. */
int a20_mcu_leaf_set_sink(const char *name, uint32_t name_len,
                          uint32_t (*receive)(void *, const uint8_t *,
                                              uint32_t),
                          void *ctx);

/* Raw binary I/O; transmit returns 0 or -1, receive returns byte or -1.
 * Callbacks and context must outlive the leaf. Bind only while disconnected.
 * The default console seam exists for host fixtures; production must bind a
 * dedicated byte stream without terminal newline conversion. */
int a20_mcu_leaf_set_io(int (*send)(void *, uint8_t), int (*receive)(void *),
                        void *ctx);

/* Install a static-lifetime operator registry before the first handshake.
 * The leaf permits one admission, <=140 input bytes, <=168 output bytes,
 * and <=30 s cooperative execution. Process tasks require a hosted agent. */
int a20_mcu_leaf_set_agent(const a20_work_agent_t *agent);

/* Production board provisioning: stable unique identity and a nonzero boot
 * epoch that never repeats for this identity. Refuse changes while linked. */
int a20_mcu_leaf_configure(const uint8_t node_id[16], uint64_t boot_epoch);

/* Leaf link state, 04 §4: exactly two states. */
#define A20_MCU_LEAF_DISCONNECTED 0u
#define A20_MCU_LEAF_UP 1u

/*
 * Wire the leaf into the board bring-up: resets the protocol state, installs
 * the default demo identity and arms the decoder.  Safe to call before any
 * traffic.  The default identity is the printable string "MCU-LEAF-DEMO-01";
 * override with a20_mcu_leaf_set_identity() before the first HELLO lands.
 */
void a20_mcu_leaf_init(void);
void a20_mcu_leaf_disable(void);

/* Replace the leaf's 16-byte node identity (01-abi §节点标识: neither
 * all-zero (LOCAL) nor all-0xff (BROADCAST) may be used on the wire). */
void a20_mcu_leaf_set_identity(const uint8_t node_id[A20_CLX_NODE_ID_LEN]);

/* Drain at most 64 raw bytes and run handshake/resync/watchdog timers.
 * Call from one dedicated periodic thread; operator callbacks must be bounded
 * and cooperative. Production STM32 firmware pumps every 1 ms. */
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
