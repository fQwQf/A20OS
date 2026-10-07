/*
 * Host integration test for the MCU leaf protocol face (kernel/mcu/leaf.c).
 *
 * This drives the REAL shipped leaf code -- init, poll, the SLIP decoder,
 * the HELLO/PING/CALL/CLOSE handlers, the 0xFFFF TX gate, the dedup cache
 * and the 50 s idle watchdog -- against a simulated head node built with
 * the same shared wire builders from kernel/cluster/uart.h.  The UART and
 * timer seams (uart_putc / uart_try_getc / timer_get_ticks /
 * riscv64_timer_freq) are stubbed with controllable queues so each scenario
 * can script exactly what the wire carries.
 *
 * Every expectation cites its rule:
 *   04-transports.md §4    HELLO passive answer, 0xFFFF gate, 50 s idle,
 *                          buffer sizing, frame types answered
 *   02-wire-protocol.md §2 PING/PONG echo, ERROR shape, CALL/REPLY txid
 *   02-wire-protocol.md §4 at-most-once dedup, slot lookup miss -> ERROR
 *   02-wire-protocol.md §8 CLOSE invalidates endpoint state
 *
 * The printf dance: leaf.c transitively includes core/klog.h, which
 * declares `void printf(...)` (freestanding kernel).  Renaming it for this
 * host TU keeps glibc's stdio out of the conflict without touching the
 * kernel header.
 */

/* Emulate the kernel's tier/arch selection for this host TU.  The leaf's
 * own code is arch-neutral C; riscv64 is picked as the header shim because
 * its arch headers are self-contained (armv7m's pulls a generated
 * board_config.h).  #ifndef-guarded so explicit -D flags win. */
#ifndef CONFIG_RISCV64
#define CONFIG_RISCV64 1
#endif
#ifndef RISCV64
#define RISCV64 1
#endif
#ifndef CONFIG_64BIT
#define CONFIG_64BIT 1
#endif
#ifndef CONFIG_MCU
#define CONFIG_MCU 1
#endif
#ifndef CONFIG_KLOG_BUF_SIZE
#define CONFIG_KLOG_BUF_SIZE 256
#endif
#ifndef CONFIG_CLUSTER_PROFILE
#define CONFIG_CLUSTER_PROFILE 1
#endif
#ifndef CONFIG_NR_CPUS
#define CONFIG_NR_CPUS 1
#endif

#define printf a20_kernel_klog_printf

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* Relative includes like x86_syscall_args_test.c; leaf.c is included (not
 * linked) so the whole protocol face is exercised in one TU. */
#include "../../kernel/mcu/leaf.h"
#include "../../kernel/mcu/leaf.c"

/* ------------------------------------------------------------------ */
/* Controllable UART + timer seams (leaf.c's entire external world)    */
/* ------------------------------------------------------------------ */

static uint8_t harness_rx_q[4096];
static unsigned harness_rx_head, harness_rx_tail;
static uint8_t harness_tx_q[4096];
static unsigned harness_tx_len;
static uint64_t harness_ticks;

uint64_t timer_get_ticks(void)
{
    return harness_ticks;
}

/* timer.h resolves TICKS_PER_SEC through this on riscv64 (QEMU: 10 MHz). */
uint64_t riscv64_timer_freq(void)
{
    return 10000000ULL;
}

void uart_putc(char c)
{
    assert(harness_tx_len < sizeof(harness_tx_q));
    harness_tx_q[harness_tx_len++] = (uint8_t)c;
}

int uart_try_getc(void)
{
    if (harness_rx_head == harness_rx_tail)
        return -1;
    return harness_rx_q[harness_rx_tail++ % sizeof(harness_rx_q)];
}

static void harness_reset(void)
{
    harness_rx_head = harness_rx_tail = 0;
    harness_tx_len = 0;
}

/* Queue raw wire bytes for the leaf to pump. */
static void harness_feed(const uint8_t *bytes, unsigned len)
{
    unsigned i;

    for (i = 0; i < len; i++) {
        assert(harness_rx_head - harness_rx_tail < sizeof(harness_rx_q));
        harness_rx_q[harness_rx_head++ % sizeof(harness_rx_q)] = bytes[i];
    }
}

/* One poll() turn of the leaf. */
static void harness_pump(void)
{
    a20_mcu_leaf_poll();
}

/* Decode what the leaf emitted this turn into exactly one frame; returns
 * its length via *out_len, or 0 when the leaf stayed silent. */
static unsigned harness_take_reply(uint8_t *frame, unsigned cap)
{
    a20_clx_slip_rx_t rx;
    unsigned consumed = 0;
    int got = 0;

    a20_clx_slip_rx_init(&rx, frame, cap, 1000);
    while (consumed < harness_tx_len) {
        int r = a20_clx_slip_rx_byte(&rx, harness_tx_q[consumed],
                                     harness_ticks);
        consumed++;
        if (r == 1) {
            got = 1;
            break; /* one reply per scripted exchange */
        }
        assert(r == 0);
    }
    if (!got)
        return 0;
    /* Drain any further queued bytes into the next turn's view. */
    memmove(harness_tx_q, harness_tx_q + consumed, harness_tx_len - consumed);
    harness_tx_len -= consumed;
    return rx.len;
}

/* ------------------------------------------------------------------ */
/* Simulated head node: built with the SAME shared builders uart.c uses */
/* ------------------------------------------------------------------ */

static uint8_t head_id[16];
static uint16_t head_assigned_addr = 0x0021;
static uint64_t head_nonce = 0x1111111122222222ULL;
static uint32_t head_txid = 100;

static void head_build_hello(uint8_t *frame, unsigned cap, uint16_t assign)
{
    uint8_t payload[A20_CLX_HELLO_LEN];

    a20_clx_build_hello_payload(head_id, 0, 0, A20_CLX_TIER_DEFAULT,
                                A20_CLX_CAP_RELAY, assign, head_nonce,
                                payload);
    /* Dial HELLO: dst unknown on a point-to-point wire (matches uart.c). */
    int n = a20_clx_build_frame(frame, cap, A20_CLX_TYPE_HELLO, 0, 0, 0,
                                a20_clx_fnv1a32(head_id), 0, 0, payload,
                                A20_CLX_HELLO_LEN);
    assert(n > 0);
}

static void head_slip_and_send(const uint8_t *frame, unsigned len)
{
    uint8_t wire[2 * A20_CLX_UART_MTU + 2];

    int n = a20_clx_slip_encode(frame, len, wire, sizeof(wire));
    assert(n > 0);
    harness_feed(wire, (unsigned)n);
}

static void head_build_ping(uint8_t *frame, unsigned cap, uint64_t ts_us)
{
    uint8_t payload[8];

    a20_clx_put_le64(payload, ts_us);
    /* Heartbeat frames address the leaf by its node hash (02 §1). */
    int n = a20_clx_build_frame(frame, cap, A20_CLX_TYPE_PING, 0, 0, 0,
                                a20_clx_fnv1a32(head_id),
                                a20_clx_fnv1a32(leaf_self_id), 0, payload, 8);
    assert(n > 0);
    (void)n;
}

static void head_build_call(uint8_t *frame, unsigned cap, uint32_t txid,
                            uint32_t slot, const uint8_t *payload,
                            uint16_t payload_len)
{
    int n = a20_clx_build_frame(frame, cap, A20_CLX_TYPE_CALL, 0, 0, txid,
                                a20_clx_fnv1a32(head_id),
                                a20_clx_fnv1a32(leaf_self_id), slot, payload,
                                payload_len);
    assert(n > 0);
}

static void head_build_close(uint8_t *frame, unsigned cap, uint32_t slot)
{
    int n = a20_clx_build_frame(frame, cap, A20_CLX_TYPE_CLOSE, 0, 0, 0,
                                a20_clx_fnv1a32(head_id),
                                a20_clx_fnv1a32(leaf_self_id), slot, NULL, 0);
    assert(n > 0);
}

/* ------------------------------------------------------------------ */
/* Scenarios                                                           */
/* ------------------------------------------------------------------ */

static void test_hello_handshake_assigns_address(void)
{
    uint8_t frame[A20_CLX_UART_MTU];
    uint8_t reply[A20_CLX_UART_MTU];
    a20_mcu_leaf_status_t st;
    unsigned rl;

    a20_mcu_leaf_init();
    harness_reset();
    assert(leaf_short_addr == A20_CLX_UART_SHORT_UNASSIGNED);

    head_build_hello(frame, sizeof(frame), head_assigned_addr);
    head_slip_and_send(frame, A20_CLX_HDR_LEN + A20_CLX_HELLO_LEN +
                                  A20_CLX_CRC_LEN);
    harness_pump();

    /* 04 §4: the leaf answers HELLO with HELLO_ACK and adopts the address. */
    a20_mcu_leaf_get_status(&st);
    assert(st.state == A20_MCU_LEAF_UP);
    assert(st.short_addr == head_assigned_addr);
    assert(st.counters.rx_frames == 1);
    assert(st.counters.hello_rejects == 0);

    rl = harness_take_reply(reply, sizeof(reply));
    assert(rl == A20_CLX_HDR_LEN + A20_CLX_HELLO_LEN + A20_CLX_CRC_LEN);
    assert(reply[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_HELLO_ACK);
    {
        const uint8_t *payload = reply + A20_CLX_OFF_PAYLOAD;
        /* 02 §6: HELLO_ACK payload mirrors HELLO; the leaf echoes the
         * assignment and carries its own identity + LEAF cap. */
        assert(memcmp(payload + A20_CLX_HELLO_OFF_NODE_ID, leaf_self_id,
                      16) == 0);
        assert(payload[A20_CLX_HELLO_OFF_TIER] == A20_CLX_TIER_MCU);
        assert(payload[A20_CLX_HELLO_OFF_CAPS] == A20_CLX_CAP_LEAF);
        assert(a20_clx_get_le16(payload + A20_CLX_HELLO_OFF_SHORT_ADDR) ==
               head_assigned_addr);
        assert(a20_clx_get_le32(reply + A20_CLX_OFF_DST_HASH) ==
               a20_clx_fnv1a32(head_id));
        assert(a20_clx_get_le32(reply + A20_CLX_OFF_SRC_HASH) ==
               a20_clx_fnv1a32(leaf_self_id));
        assert(payload[A20_CLX_HELLO_OFF_LINK_ADDR_LEN] == 2);
        /* The ACK must pass the transport screen byte-for-byte. */
        assert(a20_clx_screen_frame(reply, rl) == A20_CLX_SCREEN_OK);
    }
}

static void test_ping_pong_echo(void)
{
    uint8_t frame[A20_CLX_UART_MTU];
    uint8_t reply[A20_CLX_UART_MTU];
    unsigned rl;
    uint64_t ts = 0x0011223344556677ULL;

    harness_reset();
    head_build_ping(frame, sizeof(frame), ts);
    head_slip_and_send(frame, A20_CLX_HDR_LEN + 8 + A20_CLX_CRC_LEN);
    harness_pump();

    /* 02 §2: PONG echoes the PING payload verbatim. */
    rl = harness_take_reply(reply, sizeof(reply));
    assert(rl == A20_CLX_HDR_LEN + 8 + A20_CLX_CRC_LEN);
    assert(reply[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_PONG);
    assert(memcmp(reply + A20_CLX_OFF_PAYLOAD, frame + A20_CLX_OFF_PAYLOAD,
                  8) == 0);
}

static void test_call_dot_product_and_dedup(void)
{
    uint8_t frame[A20_CLX_UART_MTU];
    uint8_t reply[A20_CLX_UART_MTU];
    uint8_t payload[4 + 24];
    unsigned rl;
    uint32_t status;
    int64_t dot;

    /* n = 3, a = [1,2,3], b = [4,5,6] -> dot = 32. */
    a20_clx_put_le32(payload, 3);
    a20_clx_put_le32(payload + 4, 1);
    a20_clx_put_le32(payload + 8, 2);
    a20_clx_put_le32(payload + 12, 3);
    a20_clx_put_le32(payload + 16, 4);
    a20_clx_put_le32(payload + 20, 5);
    a20_clx_put_le32(payload + 24, 6);

    harness_reset();
    head_build_call(frame, sizeof(frame), head_txid,
                    A20_MCU_LEAF_OP_DOT_SLOT, payload, (uint16_t)sizeof(payload));
    head_slip_and_send(frame, A20_CLX_HDR_LEN + sizeof(payload) +
                                  A20_CLX_CRC_LEN);
    harness_pump();

    /* 02 §2: CALL_REPLY matches the request txid; operator reply is
     * u32 status + i64 dot (kernel/mcu/leaf.h contract). */
    rl = harness_take_reply(reply, sizeof(reply));
    assert(rl == A20_CLX_HDR_LEN + 12 + A20_CLX_CRC_LEN);
    assert(reply[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_CALL_REPLY);
    assert(a20_clx_get_le32(reply + A20_CLX_OFF_TXID) == head_txid);
    status = a20_clx_get_le32(reply + A20_CLX_OFF_PAYLOAD);
    dot = (int64_t)a20_clx_get_le64(reply + A20_CLX_OFF_PAYLOAD + 4);
    assert(status == 0);
    assert(dot == 32);

    /* 02 §4 at-most-once: the same (src_hash, txid) re-delivery re-sends
     * the cached reply and counts dedup_drops instead of re-executing. */
    harness_reset();
    head_slip_and_send(frame, A20_CLX_HDR_LEN + sizeof(payload) +
                                  A20_CLX_CRC_LEN);
    harness_pump();
    {
        uint8_t reply2[A20_CLX_UART_MTU];
        a20_mcu_leaf_status_t st;
        unsigned rl2 = harness_take_reply(reply2, sizeof(reply2));

        assert(rl2 == rl);
        assert(memcmp(reply2, reply, rl) == 0);
        a20_mcu_leaf_get_status(&st);
        assert(st.counters.dedup_drops == 1);
    }
}

static void test_call_negative_dot_and_malformed(void)
{
    uint8_t frame[A20_CLX_UART_MTU];
    uint8_t reply[A20_CLX_UART_MTU];
    uint8_t payload[4 + 8];
    unsigned rl;

    /* n = 1, a = [-3], b = [7] -> -21 (i32 sign extension both ways). */
    a20_clx_put_le32(payload, 1);
    a20_clx_put_le32(payload + 4, (uint32_t)-3);
    a20_clx_put_le32(payload + 8, 7);
    harness_reset();
    head_build_call(frame, sizeof(frame), head_txid + 1,
                    A20_MCU_LEAF_OP_DOT_SLOT, payload, (uint16_t)sizeof(payload));
    head_slip_and_send(frame, A20_CLX_HDR_LEN + sizeof(payload) +
                                  A20_CLX_CRC_LEN);
    harness_pump();
    rl = harness_take_reply(reply, sizeof(reply));
    assert(rl == A20_CLX_HDR_LEN + 12 + A20_CLX_CRC_LEN);
    assert(a20_clx_get_le32(reply + A20_CLX_OFF_PAYLOAD) == 0);
    assert((int64_t)a20_clx_get_le64(reply + A20_CLX_OFF_PAYLOAD + 4) == -21);

    /* Malformed operator payload: length disagrees with n -> the reply is
     * a CALL_REPLY carrying the application-level INVALID_ARGUMENT(12)
     * (kernel/mcu/leaf.h: operator status, not an ERROR frame). */
    a20_clx_put_le32(payload, 5); /* claims 5 vectors, carries 1 */
    harness_reset();
    head_build_call(frame, sizeof(frame), head_txid + 2,
                    A20_MCU_LEAF_OP_DOT_SLOT, payload, (uint16_t)sizeof(payload));
    head_slip_and_send(frame, A20_CLX_HDR_LEN + sizeof(payload) +
                                  A20_CLX_CRC_LEN);
    harness_pump();
    rl = harness_take_reply(reply, sizeof(reply));
    assert(rl == A20_CLX_HDR_LEN + 12 + A20_CLX_CRC_LEN);
    assert(reply[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_CALL_REPLY);
    assert(a20_clx_get_le32(reply + A20_CLX_OFF_PAYLOAD) == 12);
    assert(a20_clx_get_le32(reply + A20_CLX_OFF_TXID) == head_txid + 2);
}

static void test_call_unknown_slot_sends_error(void)
{
    uint8_t frame[A20_CLX_UART_MTU];
    uint8_t reply[A20_CLX_UART_MTU];
    uint8_t payload[4];
    unsigned rl;

    a20_clx_put_le32(payload, 0);
    harness_reset();
    head_build_call(frame, sizeof(frame), head_txid + 3, 7 /* unknown */,
                    payload, 4);
    head_slip_and_send(frame, A20_CLX_HDR_LEN + 4 + A20_CLX_CRC_LEN);
    harness_pump();

    /* 02 §4: "槽位查找失败 -> ERROR(errno, T)"; 01-abi errno NOT_FOUND. */
    rl = harness_take_reply(reply, sizeof(reply));
    assert(rl == A20_CLX_HDR_LEN + 8 + A20_CLX_CRC_LEN);
    assert(reply[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_ERROR);
    assert(a20_clx_get_le32(reply + A20_CLX_OFF_PAYLOAD) ==
           A20_CLX_WERR_NOT_FOUND);
    assert(a20_clx_get_le32(reply + A20_CLX_OFF_PAYLOAD + 4) ==
           head_txid + 3);
}

static void test_close_invalidates_cached_reply(void)
{
    uint8_t frame[A20_CLX_UART_MTU];
    uint8_t payload[4 + 8];
    a20_mcu_leaf_status_t st;

    /* Re-run one dot CALL, then CLOSE its slot; a repeat of the same txid
     * must re-execute (fresh reply, dedup_drops unchanged) because 02 §8
     * CLOSE tears the endpoint state down. */
    a20_clx_put_le32(payload, 0); /* n = 0 -> dot 0, exact length 4 */
    harness_reset();
    head_build_call(frame, sizeof(frame), head_txid + 4,
                    A20_MCU_LEAF_OP_DOT_SLOT, payload, 4);
    head_slip_and_send(frame, A20_CLX_HDR_LEN + 4 + A20_CLX_CRC_LEN);
    harness_pump();
    {
        uint8_t reply[A20_CLX_UART_MTU];
        assert(harness_take_reply(reply, sizeof(reply)) ==
               A20_CLX_HDR_LEN + 12 + A20_CLX_CRC_LEN);
    }

    harness_reset();
    head_build_close(frame, sizeof(frame), A20_MCU_LEAF_OP_DOT_SLOT);
    head_slip_and_send(frame, A20_CLX_HDR_LEN + A20_CLX_CRC_LEN);
    harness_pump();
    {
        uint8_t reply[A20_CLX_UART_MTU];
        /* 02 §8: CLOSE is a notification; the leaf sends nothing back. */
        assert(harness_take_reply(reply, sizeof(reply)) == 0);
    }

    harness_reset();
    head_build_call(frame, sizeof(frame), head_txid + 4,
                    A20_MCU_LEAF_OP_DOT_SLOT, payload, 4);
    head_slip_and_send(frame, A20_CLX_HDR_LEN + 4 + A20_CLX_CRC_LEN);
    harness_pump();
    {
        uint8_t reply[A20_CLX_UART_MTU];
        assert(harness_take_reply(reply, sizeof(reply)) ==
               A20_CLX_HDR_LEN + 12 + A20_CLX_CRC_LEN);
        a20_mcu_leaf_get_status(&st);
        assert(st.counters.dedup_drops == 1); /* not incremented again */
    }
}

static void test_unassigned_leaf_tx_gate_and_idle_watchdog(void)
{
    uint8_t frame[A20_CLX_UART_MTU];
    uint8_t reply[A20_CLX_UART_MTU];
    a20_mcu_leaf_status_t st;

    /* 04 §4: "50s 无 PING 回 DISCONNECTED，短地址作废".  Exactly 50 s is
     * not yet expired; one tick past is. */
    harness_ticks += MS_TO_TICKS(A20_CLX_UART_LEAF_IDLE_MS);
    harness_reset();
    harness_pump();
    a20_mcu_leaf_get_status(&st);
    assert(st.state == A20_MCU_LEAF_UP);
    assert(st.short_addr == head_assigned_addr);

    harness_ticks += 1;
    harness_reset();
    harness_pump();
    a20_mcu_leaf_get_status(&st);
    assert(st.state == A20_MCU_LEAF_DISCONNECTED);
    assert(st.short_addr == A20_CLX_UART_SHORT_UNASSIGNED);

    /* A PING arriving while DISCONNECTED is dropped, not served. */
    head_build_ping(frame, sizeof(frame), 1);
    head_slip_and_send(frame, A20_CLX_HDR_LEN + 8 + A20_CLX_CRC_LEN);
    harness_pump();
    a20_mcu_leaf_get_status(&st);
    assert(st.counters.rx_drops >= 1);
    assert(harness_take_reply(reply, sizeof(reply)) == 0); /* silence */

    /* Re-negotiation: a fresh HELLO may hand out a NEW short address and
     * the leaf must come back UP with it (WC1 acceptance: "恢复后 HELLO
     * 重协商成功（可能拿到新短地址）"). */
    head_assigned_addr = 0x0042;
    head_build_hello(frame, sizeof(frame), head_assigned_addr);
    head_slip_and_send(frame, A20_CLX_HDR_LEN + A20_CLX_HELLO_LEN +
                                  A20_CLX_CRC_LEN);
    harness_pump();
    a20_mcu_leaf_get_status(&st);
    assert(st.state == A20_MCU_LEAF_UP);
    assert(st.short_addr == 0x0042);
    {
        uint8_t ack[A20_CLX_UART_MTU];
        unsigned rl = harness_take_reply(ack, sizeof(ack));
        const uint8_t *payload = ack + A20_CLX_OFF_PAYLOAD;

        assert(rl == A20_CLX_HDR_LEN + A20_CLX_HELLO_LEN + A20_CLX_CRC_LEN);
        assert(a20_clx_get_le16(payload + A20_CLX_HELLO_OFF_SHORT_ADDR) ==
               0x0042);
    }
}

static void test_malformed_frames_are_counted_not_served(void)
{
    uint8_t frame[A20_CLX_UART_MTU];
    a20_mcu_leaf_status_t st;

    /* A HELLO whose src_hash does not match its payload identity is
     * rejected with hello_rejects and the leaf stays DISCONNECTED. */
    a20_mcu_leaf_init();
    harness_reset();
    head_build_hello(frame, sizeof(frame), 0x1234);
    frame[A20_CLX_OFF_SRC_HASH] ^= 0xFF; /* break the hash binding */
    {
        /* The CRC must be rebuilt so the frame passes the transport
         * screen and reaches the HELLO validation layer. */
        uint16_t crc = a20_clx_crc16_ccitt(frame, A20_CLX_HDR_LEN + 32);
        a20_clx_put_le16(frame + A20_CLX_HDR_LEN + 32, crc);
    }
    head_slip_and_send(frame, A20_CLX_HDR_LEN + A20_CLX_HELLO_LEN +
                                  A20_CLX_CRC_LEN);
    harness_pump();
    a20_mcu_leaf_get_status(&st);
    assert(st.state == A20_MCU_LEAF_DISCONNECTED);
    assert(st.counters.hello_rejects == 1);
    assert(harness_tx_len == 0); /* unassigned leaf may not send ERROR */
}

int main(void)
{
    static const char *hdr = "HEAD-DEMO-000001";

    memcpy(head_id, hdr, 16);

    test_hello_handshake_assigns_address();
    test_ping_pong_echo();
    test_call_dot_product_and_dedup();
    test_call_negative_dot_and_malformed();
    test_call_unknown_slot_sends_error();
    test_close_invalidates_cached_reply();
    test_unassigned_leaf_tx_gate_and_idle_watchdog();
    test_malformed_frames_are_counted_not_served();

    {
        const char *ok = "test_mcu_leaf: leaf protocol face passed\n";
        if (write(1, ok, strlen(ok)) < 0)
            return 1;
    }
    return 0;
}
