/*
 * Host test for the cluster UART wire codec (kernel/cluster/uart.h).
 *
 * The codec is header-only pure C so it can be exercised without a kernel
 * build; every expected byte below was produced by the WB1 reference codec
 * (tools/cluster-ref/clframe.py, the golden source for 02-§9 interop) and
 * each group names the rule it pins:
 *
 *   CRC16   docs/cluster/02-wire-protocol.md §1 csum_kind=1
 *           (CRC-16/CCITT-FALSE; catalogue check 0x29B1)
 *   FNV-1a  docs/cluster/01-abi.md §节点哈希
 *           (anchors 0x69691905 / 0x360779f5, same ones clframe asserts)
 *   SLIP    docs/cluster/04-transports.md §4 variant table
 *   screen  02 §1 hard rules + 04 §4 csum_kind=1
 *
 * If one of these asserts fires after touching either the codec or the
 * reference, the two disagree and the disagreement is a bug until the
 * specs say otherwise (impl-prompts.md 故障处置表: the prose wins).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Relative path like x86_syscall_args_test.c: this header lives with its
 * implementation in kernel/cluster/, not under kernel/include/. */
#include "../../kernel/cluster/uart.h"

/* Host-CFLAGS-safe node id: an exact 16-byte copy with no NUL. */
static void set_demo_id(uint8_t *id)
{
    memcpy(id, "MCU-LEAF-DEMO-01", 16);
}

/* tools/cluster-ref/clframe.py: crc16_ccitt("123456789") == 0x29B1. */
static void test_crc16_catalogue(void)
{
    assert(a20_clx_crc16_ccitt((const uint8_t *)"123456789", 9) == 0x29B1);
    /* Init value with no input stays 0xFFFF (no final xor). */
    assert(a20_clx_crc16_ccitt((const uint8_t *)"", 0) == 0xFFFF);
}

/* 01-abi anchors, verified against clframe.fnv1a32. */
static void test_fnv1a32_anchors(void)
{
    uint8_t zeros[16] = { 0 };
    uint8_t ffs[16];
    memset(ffs, 0xFF, sizeof(ffs));
    assert(a20_clx_fnv1a32(zeros) == 0x69691905u);
    assert(a20_clx_fnv1a32(ffs) == 0x360779f5u);
}

/* clframe.slip_encode(b"\xC0\x01\xDB\x42") == c0 dbdc 01 dbdd 42 c0. */
static void test_slip_encode_escapes(void)
{
    const uint8_t in[4] = { 0xC0, 0x01, 0xDB, 0x42 };
    const uint8_t want[8] = { 0xC0, 0xDB, 0xDC, 0x01,
                              0xDB, 0xDD, 0x42, 0xC0 };
    uint8_t out[16];

    int n = a20_clx_slip_encode(in, 4, out, sizeof(out));
    assert(n == 8);
    assert(memcmp(out, want, 8) == 0);

    /* Worst case size must be declared honestly or the caller's buffer
     * arithmetic under-allocates. */
    assert(A20_CLX_SLIP_ENCODED_LEN(4) == 10);
    assert(a20_clx_slip_encode(in, 4, out, 9) == -1); /* would not fit */
}

/* Frame bytes produced by clframe.encode_frame(...) for a HELLO carrying
 * node id "MCU-LEAF-DEMO-01", tier MCU, caps LEAF, short addr 0x1234:
 * 434c00010000000000000000874107c800000000000000000000000020000801
 * 4d43552d4c4541462d44454d4f2d303100000104341202000001020304050607 3e9a */
static void test_frame_builder_matches_reference(void)
{
    uint8_t id[16];
    set_demo_id(id);
    uint8_t payload[A20_CLX_HELLO_LEN];
    uint8_t frame[66];
    const uint8_t nonce[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint16_t crc;

    a20_clx_build_hello_payload(id, 0, 0, A20_CLX_TIER_MCU, A20_CLX_CAP_LEAF,
                                0x1234, 0x0706050403020100ULL, payload);
    /* Nonce is little-endian in the payload. */
    assert(memcmp(payload + A20_CLX_HELLO_OFF_NONCE, nonce, 8) == 0);
    assert(payload[A20_CLX_HELLO_OFF_PROTO_MIN] == 0);
    assert(payload[A20_CLX_HELLO_OFF_PROTO_MAX] == 0);
    assert(payload[A20_CLX_HELLO_OFF_TIER] == 1);
    assert(payload[A20_CLX_HELLO_OFF_CAPS] == 4);
    assert(a20_clx_get_le16(payload + A20_CLX_HELLO_OFF_SHORT_ADDR) == 0x1234);
    assert(payload[A20_CLX_HELLO_OFF_LINK_ADDR_LEN] == 2);
    assert(payload[A20_CLX_HELLO_OFF_RESERVED] == 0);

    assert(a20_clx_build_frame(frame, sizeof(frame), A20_CLX_TYPE_HELLO, 0,
                               0, 0, a20_clx_fnv1a32(id), 0, 0, payload,
                               A20_CLX_HELLO_LEN) == 66);
    /* Field-by-field expectations (02 §1 offsets, LE): */
    assert(a20_clx_get_le16(frame + A20_CLX_OFF_MAGIC) == 0x4C43);
    assert(frame[0] == 0x43 && frame[1] == 0x4C); /* 'C', 'L' on the wire */
    assert(frame[A20_CLX_OFF_VER] == 0);
    assert(frame[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_HELLO);
    assert(a20_clx_get_le16(frame + A20_CLX_OFF_FLAGS) == 0);
    assert(a20_clx_get_le16(frame + A20_CLX_OFF_FRAG) == 0);
    assert(a20_clx_get_le32(frame + A20_CLX_OFF_TXID) == 0);
    assert(a20_clx_get_le32(frame + A20_CLX_OFF_SRC_HASH) ==
           a20_clx_fnv1a32(id));
    assert(a20_clx_get_le32(frame + A20_CLX_OFF_DST_HASH) == 0);
    assert(a20_clx_get_le32(frame + A20_CLX_OFF_DST_SLOT) == 0);
    assert(a20_clx_get_le32(frame + A20_CLX_OFF_SEQ) == 0);
    assert(a20_clx_get_le16(frame + A20_CLX_OFF_PAYLOAD_LEN) == 32);
    assert(frame[A20_CLX_OFF_TTL] == 8);
    assert(frame[A20_CLX_OFF_CSUM_KIND] == 1);
    assert(memcmp(frame + A20_CLX_OFF_PAYLOAD, payload, 32) == 0);
    crc = a20_clx_crc16_ccitt(frame, A20_CLX_HDR_LEN + 32);
    assert(a20_clx_get_le16(frame + 64) == crc);
    /* The trailer bytes the reference emitted for exactly this frame. */
    assert(frame[64] == 0x3E && frame[65] == 0x9A);
}

/* Round trip: every frame the builder makes survives the SLIP layer fed
 * one byte at a time, with inter-frame noise in the stream. */
static void test_slip_round_trip(void)
{
    uint8_t id[16];
    set_demo_id(id);
    uint8_t ping_payload[8];
    uint8_t hello[66], ping[42];
    uint8_t escaped[256];
    uint8_t buf[128];
    a20_clx_slip_rx_t rx;
    uint32_t i;
    int n, m, rc = 0, ping_seen = 0, hello_seen = 0;

    /* The HELLO payload is written straight into the frame buffer after
     * its header, then the frame is built around it. */
    a20_clx_build_hello_payload(id, 0, 0, A20_CLX_TIER_MCU, A20_CLX_CAP_LEAF,
                                0x1234, 42, hello + A20_CLX_OFF_PAYLOAD);
    n = a20_clx_build_frame(hello, sizeof(hello), A20_CLX_TYPE_HELLO, 0, 0, 0,
                            a20_clx_fnv1a32(id), 0, 0,
                            hello + A20_CLX_OFF_PAYLOAD, A20_CLX_HELLO_LEN);
    assert(n == 66);

    a20_clx_put_le64(ping_payload, 0xDEADBEEFCAFEBABEULL);
    m = a20_clx_build_frame(ping, sizeof(ping), A20_CLX_TYPE_PING, 0, 0, 0,
                            a20_clx_fnv1a32(id), a20_clx_fnv1a32(id), 0,
                            ping_payload, 8);
    assert(m == 42);

    /* One buffer holding noise, then the escaped PING, then the escaped
     * HELLO: the decoder must recover exactly both frames. */
    m = a20_clx_slip_encode(ping, 42, escaped, sizeof(escaped));
    assert(m > 0);
    n = a20_clx_slip_encode(hello, 66, escaped + m, sizeof(escaped) - (uint32_t)m);
    assert(n > 0);

    a20_clx_slip_rx_init(&rx, buf, sizeof(buf), 50);
    {
        uint8_t noise[3] = { 0x00, 0xFF, 0x55 };
        for (i = 0; i < 3; i++)
            assert(a20_clx_slip_rx_byte(&rx, noise[i], i + 1) == 0);
    }
    for (i = 0; i < (uint32_t)(m + n); i++) {
        rc = a20_clx_slip_rx_byte(&rx, escaped[i], 100 + i);
        if (rc == 1) {
            if (rx.len == 42 && rx.buf[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_PING) {
                assert(memcmp(rx.buf, ping, 42) == 0);
                ping_seen = 1;
            } else if (rx.len == 66 &&
                       rx.buf[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_HELLO) {
                assert(memcmp(rx.buf, hello, 66) == 0);
                hello_seen = 1;
            } else {
                assert(!"unexpected frame shape");
            }
        } else {
            assert(rc == 0);
        }
    }
    assert(ping_seen && hello_seen);
    /* Post-delivery state: hunting again; the delivered frame stays
     * readable in ->buf/->len by contract until the next head END. */
    assert(rx.in_frame == 0);
    assert(rx.len == 66);
}

/* An END right after a head END is the resync artifact, not a frame and
 * not a drop (clframe.slip_decode "empty"). */
static void test_slip_empty_frame_is_resync(void)
{
    uint8_t buf[32];
    a20_clx_slip_rx_t rx;

    a20_clx_slip_rx_init(&rx, buf, sizeof(buf), 50);
    assert(a20_clx_slip_rx_byte(&rx, 0xC0, 1) == 0);
    assert(a20_clx_slip_rx_byte(&rx, 0xC0, 2) == 0);
    assert(a20_clx_slip_rx_byte(&rx, 0xC0, 3) == 0);
    assert(a20_clx_slip_rx_byte(&rx, 0xC0, 4) == 0);
    assert(rx.in_frame == 1 && rx.len == 0);
}

/*
 * Final slip semantics (tools/cluster-ref/vectors/slip/, 2026-10): a bad
 * escape pair is consumed without producing a data byte while the half
 * frame STAYS ALIVE -- a raw END does not close an open escape
 * (slip-mal-badescape-01).
 */
static void test_slip_bad_escape_pair_consumed(void)
{
    uint8_t buf[32];
    a20_clx_slip_rx_t rx;
    int i, delivered = 0;

    a20_clx_slip_rx_init(&rx, buf, sizeof(buf), 50);
    assert(a20_clx_slip_rx_byte(&rx, 0xC0, 1) == 0);
    assert(a20_clx_slip_rx_byte(&rx, 0x01, 2) == 0);
    assert(a20_clx_slip_rx_byte(&rx, 0xDB, 3) == 0);
    /* DB 00: undefined pair -> consumed, no data byte, no reset. */
    assert(a20_clx_slip_rx_byte(&rx, 0x00, 4) == 0);
    assert(rx.in_frame == 1 && rx.len == 1);
    /* DB C0: END does not close an open escape; consumed as a bad pair. */
    assert(a20_clx_slip_rx_byte(&rx, 0xDB, 5) == 0);
    assert(a20_clx_slip_rx_byte(&rx, 0xC0, 6) == 0);
    assert(rx.in_frame == 1 && rx.len == 1);
    /* The frame continues after the bad pairs and delivers normally. */
    assert(a20_clx_slip_rx_byte(&rx, 0x02, 7) == 0);
    assert(a20_clx_slip_rx_byte(&rx, 0xC0, 8) == 1);
    delivered++;
    assert(delivered == 1);
    assert(rx.len == 2 && rx.buf[0] == 0x01 && rx.buf[1] == 0x02);

    /* Valid escapes still decode inside the same frame. */
    {
        uint8_t frame[3] = { 0x01, 0xC0, 0x02 };
        uint8_t wire[8];
        int n = a20_clx_slip_encode(frame, 3, wire, sizeof(wire));
        assert(n == 6);
        for (i = 0; i < n; i++) {
            int r = a20_clx_slip_rx_byte(&rx, wire[i], 10 + i);
            if (r != 1)
                assert(r == 0);
        }
        assert(rx.len == 3);
        assert(rx.buf[0] == 0x01 && rx.buf[1] == 0xC0 && rx.buf[2] == 0x02);
    }
}

/*
 * Overflow discards the half frame IMMEDIATELY (bytes voided, one -1) and
 * then re-syncs on the next END (slip-mal-oversize-05, 04 §4 "即刻丢弃").
 */
static void test_slip_oversize_immediate_discard(void)
{
    uint8_t buf[8];
    a20_clx_slip_rx_t rx;
    uint8_t good[8];
    int n, i, delivered = 0, drops = 0;

    a20_clx_slip_rx_init(&rx, buf, sizeof(buf), 50);
    assert(a20_clx_slip_rx_byte(&rx, 0xC0, 1) == 0);
    for (i = 0; i < 10; i++) {
        int r = a20_clx_slip_rx_byte(&rx, (uint8_t)(0x10 + i), 2 + i);
        if (r == -1)
            drops++;
        else
            assert(r == 0);
    }
    /* Exactly one discard report, at the byte that crossed the cap. */
    assert(drops == 1 && rx.overflow == 1);
    /* Closing END re-syncs without a second report... */
    assert(a20_clx_slip_rx_byte(&rx, 0xC0, 12) == 0);
    assert(rx.in_frame == 0 && rx.len == 0);
    /* ...and the next well-formed frame still arrives intact. */
    n = a20_clx_slip_encode((const uint8_t *)"\x01\x02", 2, good, sizeof(good));
    assert(n == 4);
    for (i = 0; i < n; i++) {
        int r = a20_clx_slip_rx_byte(&rx, good[i], 20 + i);
        if (r == 1)
            delivered++;
        else
            assert(r == 0);
    }
    assert(delivered == 1);
    assert(rx.len == 2 && rx.buf[0] == 0x01 && rx.buf[1] == 0x02);
}

/*
 * Replayed gold vectors (hex embedded from tools/cluster-ref/vectors/slip/,
 * generated by gen_vectors.py -- regenerate there, not here).  The decoder
 * must recover exactly the frames the vector manifest records.
 */
static void test_gold_slip_vectors(void)
{
    /* Wire streams taken verbatim from the named .slip.hex files. */
    static const char two_frames[] =
        "c0434c00030000000000000000b0cbdd9435818eae000000000000000008000801"
        "dbdddbdddbdddbdddbdddbdddbdcdbdcce0cc0"
        "c0434c00080000000000000000b0cbdd9435818eae010000000000000008000801"
        "87d61200000000004916c0";
    static const char badescape[] =
        "c0434c00030000000000000000b0cbdd9435818eae000000000000000008000801"
        "dbdddbdddbdddbdddbdddbdddbdcdbdcce0cc0db41c0";
    static const char budget[] =
        "c0434c000601000000dbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddb"
        "dddbdddbdddbdddbdddbdddbdddbdddbdd80000801dbdddbdddbdddbdddbdddb"
        "dddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddb"
        "dddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddb"
        "dddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddb"
        "dddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddb"
        "dddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddb"
        "dddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddb"
        "dddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdddb"
        "dddbdddbdddbdddbdddbdddbdddbdddbdddbdddbdd4b22c0";
    uint8_t wire[1024];
    uint8_t buf[512];
    a20_clx_slip_rx_t rx;
    uint64_t tick = 0;
    int i, delivered = 0, dropped = 0, last_len = 0;

    struct {
        const char *hex;
        int want_frames;
        int want_screen_ok; /* CL-layer screen verdict of the FIRST frame */
    } cases[3] = {
        { two_frames, 2, 1 },
        { badescape, 1, 1 },
        { budget, 1, 1 },
    };

    for (i = 0; i < 3; i++) {
        unsigned len = 0, j;
        char byte[3];
        int r;

        while (cases[i].hex[len * 2] && cases[i].hex[len * 2 + 1]) {
            byte[0] = cases[i].hex[len * 2];
            byte[1] = cases[i].hex[len * 2 + 1];
            byte[2] = 0;
            wire[len] = (uint8_t)strtoul(byte, NULL, 16);
            len++;
        }
        a20_clx_slip_rx_init(&rx, buf, sizeof(buf), 50);
        delivered = 0;
        dropped = 0;
        for (j = 0; j < len; j++) {
            r = a20_clx_slip_rx_byte(&rx, wire[j], ++tick);
            if (r == 1) {
                delivered++;
                last_len = (int)rx.len;
                assert(a20_clx_screen_frame(rx.buf, rx.len) ==
                       A20_CLX_SCREEN_OK);
            } else if (r < 0) {
                dropped++;
            }
        }
        assert(delivered == cases[i].want_frames);
        assert(dropped == 0);
    }
    assert(last_len == 162); /* budget vector's decoded frame size */
}

/* 04 §4: inter-byte silence strictly greater than 50 ms drops the half
 * frame and returns to hunting END; exactly 50 ms does not. */
static void test_slip_interbyte_timeout(void)
{
    uint8_t buf[32];
    a20_clx_slip_rx_t rx;

    a20_clx_slip_rx_init(&rx, buf, sizeof(buf), 50);
    assert(a20_clx_slip_rx_byte(&rx, 0xC0, 10) == 0);
    assert(a20_clx_slip_rx_byte(&rx, 0x01, 14) == 0);

    assert(a20_clx_slip_rx_timeout(&rx, 64) == 0);  /* 50 ms: still open  */
    assert(a20_clx_slip_rx_timeout(&rx, 65) == 1);  /* 51 ms: dropped     */
    assert(rx.in_frame == 0 && rx.len == 0);

    /* The decoder is usable again and hunting: a full frame decodes. */
    {
        uint8_t frame[2] = { 0x05, 0x06 };
        uint8_t wire[8]; /* worst case for 2 bytes is 6 */
        int n = a20_clx_slip_encode(frame, 2, wire, sizeof(wire));
        int i;
        assert(n == 4);
        for (i = 0; i < n; i++) {
            int r = a20_clx_slip_rx_byte(&rx, wire[i], 100 + i);
            if (r != 1)
                assert(r == 0);
        }
        assert(rx.len == 2);
    }
    /* Silence while hunting never triggers the timeout. */
    assert(a20_clx_slip_rx_timeout(&rx, 100000) == 0);
}

/* a20_clx_screen_frame: 02 §1 hard rules and 04 §4 csum_kind=1. */
static void test_screen_verdicts(void)
{
    uint8_t id[16];
    set_demo_id(id);
    uint8_t payload[4];
    uint8_t frame[38];
    int n;

    a20_clx_put_le32(payload, 0x11223344);
    n = a20_clx_build_frame(frame, sizeof(frame), A20_CLX_TYPE_CLOSE, 0, 0,
                            7, a20_clx_fnv1a32(id), a20_clx_fnv1a32(id), 5,
                            payload, 4);
    assert(n == 38);
    assert(a20_clx_screen_frame(frame, (uint32_t)n) == A20_CLX_SCREEN_OK);

    /* Flip one payload bit: CRC must catch it (04 §5, "CRC 丢弃"). */
    frame[A20_CLX_OFF_PAYLOAD] ^= 0x01;
    assert(a20_clx_screen_frame(frame, (uint32_t)n) ==
           A20_CLX_SCREEN_MALFORMED);
    frame[A20_CLX_OFF_PAYLOAD] ^= 0x01;
    assert(a20_clx_screen_frame(frame, (uint32_t)n) == A20_CLX_SCREEN_OK);

    /* payload_len disagrees with the received length (02 §1). */
    assert(a20_clx_screen_frame(frame, (uint32_t)n - 1) ==
           A20_CLX_SCREEN_MALFORMED);

    /* csum_kind=0 is refused on UART (04 §4). */
    frame[A20_CLX_OFF_CSUM_KIND] = 0;
    assert(a20_clx_screen_frame(frame, (uint32_t)n) ==
           A20_CLX_SCREEN_MALFORMED);
    frame[A20_CLX_OFF_CSUM_KIND] = 1;

    /* TTL exhausted in transit (02 §1) is a policy drop, not malformed. */
    frame[A20_CLX_OFF_TTL] = 0;
    assert(a20_clx_screen_frame(frame, (uint32_t)n) == A20_CLX_SCREEN_DROP);
    frame[A20_CLX_OFF_TTL] = 8;

    /* Bad magic and truncated frames are malformed. */
    frame[0] = 'X';
    assert(a20_clx_screen_frame(frame, (uint32_t)n) ==
           A20_CLX_SCREEN_MALFORMED);
    frame[0] = 0x43;
    assert(a20_clx_screen_frame(frame, 10) == A20_CLX_SCREEN_MALFORMED);
    assert(a20_clx_screen_frame(frame, A20_CLX_UART_MTU + 1) ==
           A20_CLX_SCREEN_MALFORMED);
}

/* The constants the whole subsystem leans on, pinned so a silent edit
 * cannot unsync the C side from the specs and the reference vectors. */
static void test_wire_constants(void)
{
    assert(A20_CLX_MAGIC == 0x4C43);
    assert(A20_CLX_HDR_LEN == 32);
    assert(A20_CLX_CRC_LEN == 2);
    assert(A20_CLX_UART_MTU == 256);
    assert(A20_CLX_UART_MAX_PAYLOAD == 222); /* 256 - 32 - 2 */
    assert(A20_CLX_UART_SHORT_UNASSIGNED == 0xFFFF);
    assert(A20_CLX_UART_INTERBYTE_MS == 50);
    assert(A20_CLX_UART_PING_PERIOD_MS == 5000);
    assert(A20_CLX_UART_LEAF_IDLE_MS == 50000);
    assert(A20_CLX_TYPE_ERROR == 11);
    assert(A20_CLX_FLAG_FRAGMENTED == 0x2);
    assert(A20_CLX_WERR_NOT_FOUND == 24);          /* ipc.h A20_ERR_NOT_FOUND */
    assert(A20_CLX_WERR_CLUSTER_UNSUPPORTED == 29); /* ipc.h, verified 2026-10 */
}

int main(void)
{
    test_crc16_catalogue();
    test_fnv1a32_anchors();
    test_slip_encode_escapes();
    test_frame_builder_matches_reference();
    test_slip_round_trip();
    test_slip_empty_frame_is_resync();
    test_slip_oversize_immediate_discard();
    test_slip_bad_escape_pair_consumed();
    test_slip_interbyte_timeout();
    test_gold_slip_vectors();
    test_screen_verdicts();
    test_wire_constants();
    printf("test_clx_uart: all codec assertions passed\n");
    return 0;
}
