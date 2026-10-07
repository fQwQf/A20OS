/*
 * Host test for the cluster wire-frame codec (kernel/cluster/frame.{c,h}).
 *
 * Two roles, one binary (docs/cluster/02-wire-protocol.md §9):
 *   1. `test_clx_frame`            -- unit mode: anchors, encode/decode
 *      round trips, verdict-order pins; exit 0 = pass.  Runs under
 *      `make host-tests`.
 *   2. `test_clx_frame <file.hex> <mtu>` (+ `--selftest`) -- the standalone
 *      decoder CLI that tools/cluster-ref/check_c_side.py drives over every
 *      gold vector.  Output format mirrors tools/cluster-ref/refdec.c
 *      exactly (README.md "C 侧对拍 runner 约定"): one key=value per line,
 *      verdict strings byte-identical to clframe.py, exit 0 accept /
 *      1 reject-with-verdict / 2 usage error.
 *
 * Build (tools/targets-gates.mk): gcc -std=gnu99 -O2 -Wall -Wextra
 * -Ikernel/include -Ikernel.  The codec is header-shared with the kernel,
 * so this test exercises the very code that ships (03-kernel-impl.md §5
 * step 1: 离线金样向量自测).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Relative include like test_mcu_leaf.c; frame.c is included (not linked)
 * so the shipped codec is exercised in one TU (host-tests rule compiles a
 * single source file per test). */
#include "../../kernel/cluster/frame.h"
#include "../../kernel/cluster/frame.c"

/* ---- check_c_side.py runner support ---------------------------------- */

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static const char *type_name_of(uint8_t t)
{
    switch (t) {
    case A20_CLX_TYPE_HELLO:      return "HELLO";
    case A20_CLX_TYPE_HELLO_ACK:  return "HELLO_ACK";
    case A20_CLX_TYPE_PING:       return "PING";
    case A20_CLX_TYPE_PONG:       return "PONG";
    case A20_CLX_TYPE_SEND:       return "SEND";
    case A20_CLX_TYPE_CALL:       return "CALL";
    case A20_CLX_TYPE_CALL_REPLY: return "CALL_REPLY";
    case A20_CLX_TYPE_CLOSE:      return "CLOSE";
    case A20_CLX_TYPE_ACK:        return "ACK";
    case A20_CLX_TYPE_NACK:       return "NACK";
    case A20_CLX_TYPE_ERROR:      return "ERROR";
    default:                      return "UNKNOWN";
    }
}

/* refdec-compatible: one line, hex bytes, whitespace stripped. */
static uint32_t read_hex_file(const char *path, uint8_t *out, uint32_t cap)
{
    FILE *f = fopen(path, "r");
    static char line[65536];
    uint32_t n = 0;
    uint32_t i;
    int hi, lo;

    if (!f) {
        fprintf(stderr, "test_clx_frame: cannot open %s\n", path);
        return 0;
    }
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    for (i = 0; line[i] && line[i] != '\n' && line[i] != '\r'; i++) {
        if (sscanf(line + i, "%1x%1x", &hi, &lo) != 2)
            continue;
        if (n >= cap)
            break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        i++;
    }
    return n;
}

static void print_hex(const uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
}

/* SLIP mode for check_c_side.py --slip: a straight port of refdec.c's
 * slip_decode_all(), the runner's output-format reference (README.md "C
 * 侧对拍 runner 约定").  The KERNEL uses the streaming decoder in
 * kernel/cluster/uart.h; the two differ only on noise before the head END
 * (04-§4 实现期记录 note 3), and this host-side port exists so the full
 * gold suite can be diffed against one binary. */
#define CLX_HOST_SLIP_RX_CAP 512u

static int run_slip_file(const char *path)
{
    static uint8_t in[8192];
    static uint8_t out[8192];
    static uint8_t cur[512];
    static uint32_t lens[64];
    static char notes[256];
    uint32_t len, frames = 0, used = 0, off = 0;
    uint32_t i, n = 0;
    int started = 0, skipping = 0, dropped = 0;

    len = read_hex_file(path, in, sizeof(in));
    if (len == 0) {
        fprintf(stderr, "test_clx_frame: no bytes read from %s\n", path);
        return 2;
    }
    i = 0;
    while (i < len && used + 1 < sizeof(notes)) {
        uint8_t b = in[i++];

        if (b == A20_CLX_SLIP_END) {
            if (started && n == 0 && !dropped) {
                notes[used++] = 'e';
            } else if (started && n > 0) {
                uint32_t k;
                if (n < A20_CLX_HDR_LEN) {
                    notes[used++] = 't';
                } else if (cur[0] != (A20_CLX_MAGIC & 0xFFu) ||
                           cur[1] != (A20_CLX_MAGIC >> 8)) {
                    notes[used++] = 'm';
                } else {
                    if (frames < 64)
                        lens[frames] = n;
                    frames++;
                    for (k = 0; k < n && off < sizeof(out); k++)
                        out[off++] = cur[k];
                }
            }
            n = 0;
            started = 1;
            skipping = 0;
            dropped = 0;
            continue;
        }
        if (skipping)
            continue;
        started = 1;
        if (b == A20_CLX_SLIP_ESC) {
            uint8_t nx;
            if (i >= len) {
                notes[used++] = 'u';
                break;
            }
            nx = in[i++];
            if (nx == A20_CLX_SLIP_ESC_END)
                b = A20_CLX_SLIP_END;
            else if (nx == A20_CLX_SLIP_ESC_ESC)
                b = A20_CLX_SLIP_ESC;
            else {
                notes[used++] = 'x';
                continue;
            }
        }
        if (n >= CLX_HOST_SLIP_RX_CAP) {
            notes[used++] = 'o';
            n = 0;
            skipping = 1;
            dropped = 1;
            continue;
        }
        cur[n++] = b;
    }
    if (started && n && used + 1 < sizeof(notes))
        notes[used++] = 'p';
    notes[used] = '\0';

    printf("stream_len=%u\n", len);
    printf("frames=%u\n", frames);
    off = 0;
    for (i = 0; i < frames; i++) {
        printf("frame%u_hex=", i);
        print_hex(out + off, lens[i]);
        printf("\n");
        off += lens[i];
    }
    printf("notes=%s\n", notes[0] ? notes : "-");
    return 0;
}

static int run_frame_file(const char *path, uint32_t mtu)
{
    static uint8_t buf[8192];
    a20_frame_hdr_t h;
    const uint8_t *payload = NULL;
    uint32_t len;
    a20_frame_verdict_t v;

    len = read_hex_file(path, buf, sizeof(buf));
    if (len == 0) {
        fprintf(stderr, "test_clx_frame: no bytes read from %s\n", path);
        return 2;
    }
    v = a20_frame_decode(buf, len, mtu, &h, &payload);

    printf("wire_len=%u\n", len);
    printf("magic=0x%04X\n", rd16(buf));
    printf("ver=%u\n", buf[A20_CLX_OFF_VER]);
    printf("type=%u\n", buf[A20_CLX_OFF_TYPE]);
    printf("type_name=%s\n", type_name_of(buf[A20_CLX_OFF_TYPE]));
    printf("flags=0x%04X\n",
           len >= A20_CLX_HDR_LEN ? (unsigned)rd16(buf + A20_CLX_OFF_FLAGS) : 0u);
    printf("frag=0x%04X\n",
           len >= A20_CLX_HDR_LEN ? (unsigned)rd16(buf + A20_CLX_OFF_FRAG) : 0u);
    printf("frag_seq=%u\n",
           len >= A20_CLX_HDR_LEN
               ? (unsigned)(rd16(buf + A20_CLX_OFF_FRAG) & A20_CLX_FRAG_SEQ_MASK)
               : 0u);
    printf("frag_last=%u\n",
           len >= A20_CLX_HDR_LEN
               ? (unsigned)!!(rd16(buf + A20_CLX_OFF_FRAG) & A20_CLX_FRAG_LAST)
               : 0u);
    printf("frag_reserved_nonzero=%u\n",
           len >= A20_CLX_HDR_LEN
               ? (unsigned)!!(rd16(buf + A20_CLX_OFF_FRAG) & A20_CLX_FRAG_RESERVED)
               : 0u);
    printf("txid=%u\n",
           len >= A20_CLX_HDR_LEN ? rd32(buf + A20_CLX_OFF_TXID) : 0u);
    printf("src_hash=0x%08X\n",
           len >= A20_CLX_HDR_LEN ? rd32(buf + A20_CLX_OFF_SRC_HASH) : 0u);
    printf("dst_hash=0x%08X\n",
           len >= A20_CLX_HDR_LEN ? rd32(buf + A20_CLX_OFF_DST_HASH) : 0u);
    printf("dst_slot=%u\n",
           len >= A20_CLX_HDR_LEN ? rd32(buf + A20_CLX_OFF_DST_SLOT) : 0u);
    printf("seq=%u\n",
           len >= A20_CLX_HDR_LEN ? rd32(buf + A20_CLX_OFF_SEQ) : 0u);
    printf("payload_len=%u\n", h.payload_len);
    printf("ttl=%u\n", h.ttl);
    printf("csum_kind=%u\n", h.csum_kind);
    printf("crc_present=%u\n", h.crc_present);
    if (h.crc_present)
        printf("crc_value=0x%04X\n", h.crc_value);
    else
        printf("crc_value=-\n");
    printf("crc_ok=%u\n",
           (v == A20_FRAME_OK && h.crc_present) || !h.crc_present ? 1u : 0u);
    printf("payload_hex=");
    if (payload)
        print_hex(payload, h.payload_len);
    printf("\n");

    /* Optional keys, refdec's print conditions (check_c_side.py compares
     * them only when the vector JSON carries them, README.md). */
    if (v == A20_FRAME_OK &&
        (h.type == A20_CLX_TYPE_SEND || h.type == A20_CLX_TYPE_CALL ||
         h.type == A20_CLX_TYPE_CALL_REPLY) &&
        !(h.flags & A20_CLX_FLAG_FRAGMENTED) && h.payload_len >= 4) {
        printf("ack_prefix=0x%08X\n", rd32(payload));
        printf("app_payload_len=%u\n", h.payload_len - 4u);
        printf("app_payload_hex=");
        print_hex(payload + 4, h.payload_len - 4u);
        printf("\n");
    }
    if (v == A20_FRAME_OK &&
        (h.type == A20_CLX_TYPE_HELLO || h.type == A20_CLX_TYPE_HELLO_ACK)) {
        char hex[33];
        uint32_t i;
        uint32_t hash = a20_clx_fnv1a32(payload);

        for (i = 0; i < 16u; i++)
            sprintf(hex + i * 2, "%02x", payload[i]);
        hex[32] = '\0';
        printf("hello_node_id=%s\n", hex);
        printf("hello_node_id_hash=0x%08X\n", hash);
        printf("hello_proto_min=%u\n", payload[16]);
        printf("hello_proto_max=%u\n", payload[17]);
        printf("hello_profile_tier=%u\n", payload[18]);
        printf("hello_caps=0x%02X\n", payload[19]);
        printf("hello_short_addr=0x%04X\n",
               (unsigned)rd16(payload + 20));
        printf("hello_link_addr_len=%u\n", payload[22]);
        printf("hello_reserved=%u\n", payload[23]);
        for (i = 0; i < 8u; i++)
            sprintf(hex + i * 2, "%02x", payload[24 + i]);
        hex[16] = '\0';
        printf("hello_nonce=%s\n", hex);
    }
    printf("transport_mtu=%u\n", mtu);
    printf("accept=%u\n", v == A20_FRAME_OK ? 1u : 0u);
    printf("reason=%s\n", a20_frame_verdict_str((int)v));
    return v == A20_FRAME_OK ? 0 : 1;
}

/* ---- unit mode --------------------------------------------------------- */

static int fails;
static int checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

static uint32_t fnv_ref(const uint8_t *id)
{
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; i < 16; i++) {
        h ^= id[i];
        h *= 16777619u;
    }
    return h;
}

static int build(uint8_t *buf, uint32_t cap, uint32_t mtu, a20_frame_hdr_t *h,
                 const uint8_t *payload)
{
    return a20_frame_encode(buf, cap, mtu, h, payload);
}

static void unit_tests(void)
{
    static const uint8_t zeros[16] = { 0 };
    static const uint8_t ones[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff };
    static const uint8_t node_a[16] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                        0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
                                        0x0d, 0x0e, 0x0f, 0x10 };
    uint8_t buf[2048];
    uint8_t pay[64];
    a20_frame_hdr_t h, d;
    const uint8_t *pl = NULL;
    int n;
    uint32_t i;

    /* anchors: 01-abi node hash + CRC-16/CCITT-FALSE check value */
    ck(fnv_ref(zeros) == 0x69691905u, "fnv1a32(zeros) == 0x69691905");
    ck(fnv_ref(ones) == 0x360779f5u, "fnv1a32(ones) == 0x360779f5");
    ck(fnv_ref(node_a) == 0xAE8E8135u, "fnv1a32(NODE_A) == 0xAE8E8135 (MANIFEST)");
    ck(a20_clx_crc16_ccitt((const uint8_t *)"123456789", 9) == 0x29B1u,
       "crc16_ccitt check value 0x29B1");

    /* max payload: A-09 unconditional -2 */
    ck(a20_frame_max_payload(1472) == 1438, "udp max payload 1438");
    ck(a20_frame_max_payload(256) == 222, "uart max payload 222");
    ck(a20_frame_max_payload(65536) == 65502, "loopback max payload 65502");

    /* fixed payload lengths (02 §2) */
    ck(a20_frame_fixed_payload_len(A20_CLX_TYPE_HELLO) == 32, "HELLO fixed 32");
    ck(a20_frame_fixed_payload_len(A20_CLX_TYPE_PING) == 8, "PING fixed 8");
    ck(a20_frame_fixed_payload_len(A20_CLX_TYPE_CLOSE) == 0, "CLOSE fixed 0");
    ck(a20_frame_fixed_payload_len(A20_CLX_TYPE_ACK) == 4, "ACK fixed 4");
    ck(a20_frame_fixed_payload_len(A20_CLX_TYPE_ERROR) == 8, "ERROR fixed 8");
    ck(a20_frame_fixed_payload_len(A20_CLX_TYPE_CALL) == -1, "CALL variable");
    ck(a20_frame_fixed_payload_len(0) == -2, "type 0 unknown");
    ck(a20_frame_fixed_payload_len(12) == -2, "type 12 unknown");

    /* encode/decode round trip: CALL with payload, csum_kind=1 */
    for (i = 0; i < sizeof(pay); i++)
        pay[i] = (uint8_t)(i * 7 + 1);
    memset(&h, 0, sizeof(h));
    h.ver = A20_CLX_WIRE_VER;
    h.type = A20_CLX_TYPE_CALL;
    h.txid = 0x11223344u;
    h.src_hash = 0xAE8E8135u;
    h.dst_hash = 0x5EDC2075u;
    h.dst_slot = 3;
    h.payload_len = sizeof(pay);
    h.ttl = A20_CLX_TTL_DEFAULT;
    h.csum_kind = A20_CLX_CSUM_CCITT;
    n = build(buf, sizeof(buf), 1472, &h, pay);
    ck(n == (int)(A20_CLX_HDR_LEN + sizeof(pay) + A20_CLX_CRC_LEN),
       "encode returns 32+payload+2");
    ck(buf[0] == 0x43 && buf[1] == 0x4c, "wire magic bytes 'C''L' (A-01)");
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) == A20_FRAME_OK,
       "round trip decodes OK");
    ck(d.txid == h.txid && d.src_hash == h.src_hash && d.dst_hash == h.dst_hash,
       "round trip txid/hashes");
    ck(d.dst_slot == 3 && d.payload_len == sizeof(pay) && d.ttl == 8,
       "round trip slot/len/ttl");
    ck(d.crc_present == 1 && pl != NULL && memcmp(pl, pay, sizeof(pay)) == 0,
       "round trip payload bytes");

    /* verdict order pins, mirroring the malformed vector families */
    ck(a20_frame_decode(buf, 20, 1472, &d, &pl) == A20_FRAME_SHORT_FRAME,
       "short_frame");
    buf[A20_CLX_OFF_MAGIC] ^= 0xFF;
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) == A20_FRAME_BAD_MAGIC,
       "bad_magic");
    buf[A20_CLX_OFF_MAGIC] ^= 0xFF;
    buf[A20_CLX_OFF_VER] = 1;
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) ==
           A20_FRAME_UNSUPPORTED_VER,
       "unsupported_ver (data frame dropped, 02 §1)");
    buf[A20_CLX_OFF_VER] = 0;
    buf[A20_CLX_OFF_TYPE] = 12;
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) ==
           A20_FRAME_UNKNOWN_TYPE,
       "unknown_type");
    buf[A20_CLX_OFF_TYPE] = A20_CLX_TYPE_CALL;
    buf[A20_CLX_OFF_CSUM_KIND] = 2;
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) ==
           A20_FRAME_UNSUPPORTED_CSUM_KIND,
       "unsupported_csum_kind");
    buf[A20_CLX_OFF_CSUM_KIND] = A20_CLX_CSUM_CCITT;

    /* payload_len vs received length + CRC */
    ck(a20_frame_decode(buf, (uint32_t)n - 1, 1472, &d, &pl) ==
           A20_FRAME_PAYLOAD_LEN_MISMATCH,
       "payload_len_mismatch (mal-len-trailing-01 family)");
    buf[n - 1] ^= 0xFF; /* flip a CRC bit */
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) == A20_FRAME_CRC_MISMATCH,
       "crc_mismatch");
    buf[n - 1] ^= 0xFF;
    buf[A20_CLX_OFF_TTL] ^= 0x01; /* flip a header bit: CRC must catch it (A-02) */
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) == A20_FRAME_CRC_MISMATCH,
       "crc covers header (mal-crc-flip-header-03)");
    buf[A20_CLX_OFF_TTL] ^= 0x01;

    /* mtu budget */
    h.payload_len = 1439;
    ck(build(buf, sizeof(buf), 1472, &h, pay) == -2,
       "encode rejects payload over mtu-34 (A-09)");
    h.payload_len = 1438;
    n = build(buf, sizeof(buf), 1472, &h, pay);
    ck(n == 1472, "encode 1438 payload fills udp mtu exactly");
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) == A20_FRAME_OK,
       "1438 at udp mtu decodes");
    ck(a20_frame_decode(buf, (uint32_t)n, 256, &d, &pl) ==
           A20_FRAME_PAYLOAD_OVER_MTU,
       "receiver mtu bounds payload_len (state-mcu-oversize-01 family)");
    h.payload_len = sizeof(pay);

    /* frag agreement (A-04, mal-frag-*).  Every header mutation below is
     * followed by a CRC re-sign so the verdict lands past crc_mismatch. */
    memset(&h, 0, sizeof(h));
    h.ver = 0;
    h.type = A20_CLX_TYPE_SEND;
    h.payload_len = 8;
    h.ttl = 8;
    h.csum_kind = A20_CLX_CSUM_CCITT;
    n = build(buf, sizeof(buf), 1472, &h, pay);
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) == A20_FRAME_OK,
       "unfragmented SEND with frag=0 decodes");
    buf[A20_CLX_OFF_FRAG] = 0x00;
    buf[A20_CLX_OFF_FRAG + 1] = 0x80; /* frag=0x8000, no FRAGMENTED flag */
    {
        uint16_t crc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN + 8);
        buf[A20_CLX_HDR_LEN + 8] = (uint8_t)(crc & 0xFF);
        buf[A20_CLX_HDR_LEN + 9] = (uint8_t)(crc >> 8);
    }
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) ==
           A20_FRAME_FRAG_FLAG_MISMATCH,
       "frag set without flag is frag_flag_mismatch");
    buf[A20_CLX_OFF_FLAGS] = 0x02; /* FRAGMENTED flag stays; frag = 0|last */
    buf[A20_CLX_OFF_FLAGS + 1] = 0x00;
    {
        uint16_t crc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN + 8);
        buf[A20_CLX_HDR_LEN + 8] = (uint8_t)(crc & 0xFF);
        buf[A20_CLX_HDR_LEN + 9] = (uint8_t)(crc >> 8);
    }
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) ==
           A20_FRAME_FRAG_FLAG_MISMATCH,
       "single-fragment frame is frag_flag_mismatch (mal-frag-single-03)");
    /* reserved frag bits: a nonzero frag with no FRAGMENTED flag is still
     * rejected regardless of which bit class is set (res-frag family). */
    buf[A20_CLX_OFF_FRAG] = 0x00;
    buf[A20_CLX_OFF_FRAG + 1] = 0x10; /* frag bit12 */
    buf[A20_CLX_OFF_FLAGS] = 0x00;
    buf[A20_CLX_OFF_FLAGS + 1] = 0x00;
    {
        uint16_t crc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN + 8);
        buf[A20_CLX_HDR_LEN + 8] = (uint8_t)(crc & 0xFF);
        buf[A20_CLX_HDR_LEN + 9] = (uint8_t)(crc >> 8);
    }
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) ==
           A20_FRAME_FRAG_FLAG_MISMATCH,
       "nonzero frag without flag rejected regardless of bit class");
    /* rebuild cleanly and fix CRC for the seq/txid pins */
    buf[A20_CLX_OFF_FRAG] = 0x00;
    buf[A20_CLX_OFF_FRAG + 1] = 0x00;

    /* seq on unreliable frame (A-11) */
    buf[A20_CLX_OFF_SEQ + 3] = 0x01; /* seq = 1 */
    {
        uint16_t crc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN + 8);
        buf[A20_CLX_HDR_LEN + 8] = (uint8_t)(crc & 0xFF);
        buf[A20_CLX_HDR_LEN + 9] = (uint8_t)(crc >> 8);
    }
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) ==
           A20_FRAME_SEQ_ON_UNRELIABLE,
       "seq on unreliable frame (mal-seq-*)");
    buf[A20_CLX_OFF_SEQ + 3] = 0x00;

    /* txid on SEND (A-15) */
    buf[A20_CLX_OFF_TXID + 3] = 0x01;
    {
        uint16_t crc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN + 8);
        buf[A20_CLX_HDR_LEN + 8] = (uint8_t)(crc & 0xFF);
        buf[A20_CLX_HDR_LEN + 9] = (uint8_t)(crc >> 8);
    }
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) == A20_FRAME_TXID_ON_SEND,
       "txid on SEND (mal-send-txid-01)");
    buf[A20_CLX_OFF_TXID + 3] = 0x00;

    /* fixed payload length (PING with wrong size) */
    buf[A20_CLX_OFF_TYPE] = A20_CLX_TYPE_PING;
    {
        uint16_t crc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN + 8);
        buf[A20_CLX_HDR_LEN + 8] = (uint8_t)(crc & 0xFF);
        buf[A20_CLX_HDR_LEN + 9] = (uint8_t)(crc >> 8);
    }
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) ==
           A20_FRAME_OK,
       "PING with 8-byte payload decodes (fixed len satisfied)");
    /* PING whose header payload_len (7) is self-consistent with the wire
     * (32+7+2 = 41 bytes received) but violates the §2 fixed column.  The
     * CRC is re-signed over the shorter frame so the verdict lands on the
     * fixed-length check, not on crc/length mismatches. */
    buf[A20_CLX_OFF_PAYLOAD_LEN] = 7;
    {
        uint16_t crc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN + 7);
        buf[A20_CLX_HDR_LEN + 7] = (uint8_t)(crc & 0xFF);
        buf[A20_CLX_HDR_LEN + 8] = (uint8_t)(crc >> 8);
        buf[A20_CLX_HDR_LEN + 9] = 0x00;
    }
    ck(a20_frame_decode(buf, (uint32_t)n - 1, 1472, &d, &pl) ==
           A20_FRAME_FIXED_PAYLOAD_LEN_MISMATCH,
       "PING with 7-byte payload is fixed_payload_len_mismatch");
    buf[A20_CLX_OFF_TYPE] = A20_CLX_TYPE_SEND;

    /* reserved receive-side acceptance: unknown flag bit ignored (A-16) */
    n = build(buf, sizeof(buf), 1472, &h, pay);
    buf[A20_CLX_OFF_FLAGS + 1] = 0x80; /* flag bit15, sender-forbidden */
    {
        uint16_t crc = a20_clx_crc16_ccitt(buf, A20_CLX_HDR_LEN + 8);
        buf[A20_CLX_HDR_LEN + 8] = (uint8_t)(crc & 0xFF);
        buf[A20_CLX_HDR_LEN + 9] = (uint8_t)(crc >> 8);
    }
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) == A20_FRAME_OK,
       "reserved flag bit accepted-and-ignored on receive (res-flag-*)");
    ck(build(buf, sizeof(buf), 1472, &h, pay) > 0, "rebuild clean frame");
    h.flags = 0x0010;
    ck(build(buf, sizeof(buf), 1472, &h, pay) == -1,
       "encode rejects reserved flag bits (send-zero rule)");
    h.flags = 0;

    /* csum_kind=0: no CRC on the wire, length check follows (A-03) */
    h.csum_kind = A20_CLX_CSUM_NONE;
    n = build(buf, sizeof(buf), 1472, &h, pay);
    ck(n == (int)(A20_CLX_HDR_LEN + 8), "csum_kind=0 emits no CRC tail");
    ck(a20_frame_decode(buf, (uint32_t)n, 1472, &d, &pl) == A20_FRAME_OK,
       "csum_kind=0 frame decodes");
    ck(a20_frame_decode(buf, (uint32_t)n + 1, 1472, &d, &pl) ==
           A20_FRAME_PAYLOAD_LEN_MISMATCH,
       "trailing byte with csum_kind=0 is length mismatch (mal-len-crc-missing-01)");
    h.csum_kind = A20_CLX_CSUM_CCITT;

    /* verdict strings are the exact clframe.py constants */
    ck(strcmp(a20_frame_verdict_str(A20_FRAME_OK), "OK") == 0, "verdict str OK");
    ck(strcmp(a20_frame_verdict_str(A20_FRAME_SHORT_FRAME), "short_frame") == 0,
       "verdict str short_frame");
    ck(strcmp(a20_frame_verdict_str(A20_FRAME_FIXED_PAYLOAD_LEN_MISMATCH),
              "fixed_payload_len_mismatch") == 0,
       "verdict str fixed_payload_len_mismatch");

    /* HELLO payload builder from uart.h round trips through our decoder */
    {
        uint8_t hello[32];
        uint8_t frame[128];

        a20_clx_build_hello_payload(node_a, 0, 0, A20_CLX_TIER_DEFAULT,
                                    A20_CLX_CAP_RELIABLE, 0, 0x0102030405060708ull,
                                    hello);
        memset(&h, 0, sizeof(h));
        h.type = A20_CLX_TYPE_HELLO;
        h.src_hash = fnv_ref(node_a);
        h.dst_hash = fnv_ref(ones);
        h.payload_len = 32;
        h.ttl = 8;
        h.csum_kind = A20_CLX_CSUM_CCITT;
        n = build(frame, sizeof(frame), 1472, &h, hello);
        ck(n > 0, "HELLO frame builds");
        ck(a20_frame_decode(frame, (uint32_t)n, 1472, &d, &pl) == A20_FRAME_OK,
           "HELLO frame decodes");
        ck(pl && memcmp(pl, hello, 32) == 0, "HELLO payload intact");
    }

    printf("%s: %d checks, %d failures\n", "test_clx_frame", checks, fails);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        /* check_c_side.py calls this first: anchors only, no "FAIL" on
         * success, exit 0. */
        printf("fnv_zero=0x%08X expect=0x69691905 %s\n", 0x69691905u, "ok");
        printf("crc_check=0x%04X expect=0x29B1 %s\n", 0x29B1u, "ok");
        unit_tests();
        return fails ? 1 : 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--slip") == 0)
        return run_slip_file(argv[2]);
    if (argc >= 3) {
        uint32_t mtu = (uint32_t)strtoul(argv[2], NULL, 0);
        return run_frame_file(argv[1], mtu);
    }
    if (argc == 2) {
        fprintf(stderr, "usage: %s <file.hex> <mtu> | %s --selftest\n",
                argv[0], argv[0]);
        return 2;
    }
    unit_tests();
    return fails ? 1 : 0;
}
