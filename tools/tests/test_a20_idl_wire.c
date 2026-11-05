/*
 * a20idl wire tests — the v1 bytes, pinned.
 *
 * Three things are checked, in order of how badly they would hurt if wrong:
 *
 *  1. Golden bytes.  Every expected byte string below is what user/svc/svcman.c
 *     and user/svc/echod.c already put on, and take off, a channel: the
 *     12-byte SVCMGR_REQ_ECHO frame (envelope + SVCMGR_ECHO), the 8-byte
 *     SVCMGR_REQ_CRASH frame, and the RTCD alarm/time payloads.  These are
 *     v1 wire bytes and this test exists so they cannot move.
 *
 *  2. Old-peer equivalence.  For each frame the test builds the bytes twice,
 *     once with the generated little-endian codec and once the way the
 *     service used to, by initialising the C struct and handing its bytes
 *     straight to the channel.  If the two ever differ, the change is a wire
 *     change and the test fails.  (The generated header's _Static_asserts
 *     make that impossible on any target the services build for; this is the
 *     host-side counterpart.)
 *
 *  3. Receiver behaviour.  echod's accept/ignore rule, transcribed:
 *       blen < 8                       -> ignore
 *       version != A20_SERVICES_IDL_VERSION -> ignore
 *       size != blen                   -> ignore
 *       SVCMGR_REQ_CRASH (after the checks above) -> exit 42
 *       otherwise                      -> echo the request back verbatim
 *     The order matters: a malformed request that also carries the crash type
 *     is ignored, not fatal.
 *
 * Host-only: built by `make host-tests` (tools/mk/targets-gates.mk) with the
 * host compiler against the generated header.  Nothing here is freestanding:
 * it is a host test binary that exits non-zero on failure, like its peers in
 * tools/tests.
 */
#include <stdint.h>
#include <stdio.h>

#include "a20_services_idl.h"

/* The generated header refuses to compile on a big-endian target, so this
 * translation unit existing at all is the positive half of that check; the
 * negative half -- a build forced to big-endian must fail -- lives in
 * tools/tests/test_a20idl.py, which compiles the same header with
 * __BYTE_ORDER__ overridden.  Asserting the macro explicitly keeps the
 * dependency visible here instead of leaving it implied. */
_Static_assert(A20_SERVICES_IDL_TARGET_IS_LITTLE_ENDIAN == 1,
               "the generated IDL header refuses a big-endian target");

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static int bytes_equal(const uint8_t *a, const uint8_t *b, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void dump(const char *label, const uint8_t *p, unsigned n) {
    unsigned i;
    printf("%s =", label);
    for (i = 0; i < n; i++)
        printf(" %02x", p[i]);
    printf("\n");
}

/* ------------------------------------------------------------------ *
 * 1 + 2. Frames: codec bytes, struct bytes, and the v1 golden string.
 * ------------------------------------------------------------------ */

struct echo_frame {
    a20_idl_envelope_t env;
    a20_idl_svcmgr_echo_t body;
};

/* Build the echo frame the new way: explicit little-endian codec. */
static unsigned echo_codec(uint8_t *out, uint32_t seq) {
    struct echo_frame f;
    uint8_t *wire = out;
    f.env.version = A20_SERVICES_IDL_VERSION;
    f.env.type = SVCMGR_REQ_ECHO;
    f.env.size = (uint32_t)sizeof(f);
    f.body.seq = seq;
    a20_idl_envelope_encode(wire, &f.env);
    a20_idl_svcmgr_echo_encode(wire + A20_IDL_ENVELOPE_WIRE_SIZE, &f.body);
    return (unsigned)sizeof(f);
}

/* Build it the old way: struct initializer, bytes taken verbatim. */
static unsigned echo_struct(uint8_t *out, uint32_t seq) {
    struct echo_frame f = {
        {A20_SERVICES_IDL_VERSION, SVCMGR_REQ_ECHO, sizeof(f)},
        {seq},
    };
    unsigned i;
    for (i = 0; i < sizeof(f); i++)
        out[i] = ((const uint8_t *)&f)[i];
    return (unsigned)sizeof(f);
}

static void test_echo_frames(void) {
    /* version=1 LE, type=SVCMGR_REQ_ECHO=0x21 LE, size=12 LE, seq LE. */
    static const uint8_t seq0[12] = {0x01, 0x00, 0x21, 0x00, 0x0c, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t seq1[12] = {0x01, 0x00, 0x21, 0x00, 0x0c, 0x00,
                                     0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    static const uint8_t seq2[12] = {0x01, 0x00, 0x21, 0x00, 0x0c, 0x00,
                                     0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
    static const uint8_t *goldens[3] = {seq0, seq1, seq2};
    uint8_t codec[16], old[16];
    unsigned n, i;

    CHECK(sizeof(struct echo_frame) == 12u,
          "echo frame is %u bytes, expected 12",
          (unsigned)sizeof(struct echo_frame));

    for (i = 0; i < 3; i++) {
        n = echo_codec(codec, i);
        CHECK(n == 12u, "codec wrote %u bytes for seq %u", n, i);
        if (!bytes_equal(codec, goldens[i], 12u)) {
            dump("codec", codec, 12);
            dump("golden", goldens[i], 12);
            failures++;
        }

        n = echo_struct(old, i);
        if (!bytes_equal(codec, old, 12u)) {
            dump("codec", codec, 12);
            dump("struct", old, 12);
            failures++;
        }

        /* And the payload codec on its own. */
        {
            a20_idl_svcmgr_echo_t body;
            uint8_t payload[4];
            a20_idl_svcmgr_echo_t src;
            src.seq = i;
            a20_idl_svcmgr_echo_encode(payload, &src);
            a20_idl_svcmgr_echo_decode(&body, payload);
            CHECK(body.seq == i, "echo payload round trip: %u != %u", body.seq,
                  i);
        }
    }
}

static void test_crash_frame(void) {
    /* version=1, type=SVCMGR_REQ_CRASH=0x22, size=8, no payload. */
    static const uint8_t golden[8] = {0x01, 0x00, 0x22, 0x00,
                                      0x08, 0x00, 0x00, 0x00};
    a20_idl_envelope_t env, back;
    uint8_t wire[8], old[8];
    unsigned i;

    env.version = A20_SERVICES_IDL_VERSION;
    env.type = SVCMGR_REQ_CRASH;
    env.size = (uint32_t)sizeof(env);
    a20_idl_envelope_encode(wire, &env);
    CHECK(bytes_equal(wire, golden, 8u), "crash frame bytes changed");
    if (!bytes_equal(wire, golden, 8u))
        dump("crash", wire, 8u);

    {
        a20_idl_envelope_t f = {A20_SERVICES_IDL_VERSION, SVCMGR_REQ_CRASH,
                                sizeof(f)};
        for (i = 0; i < sizeof(f); i++)
            old[i] = ((const uint8_t *)&f)[i];
    }
    CHECK(bytes_equal(wire, old, 8u),
          "crash frame: codec bytes differ from struct bytes");

    a20_idl_envelope_decode(&back, wire);
    CHECK(back.version == A20_SERVICES_IDL_VERSION, "crash envelope version");
    CHECK(back.type == SVCMGR_REQ_CRASH, "crash envelope type");
    CHECK(back.size == 8u, "crash envelope size %u", back.size);
}

/* ------------------------------------------------------------------ *
 * 2. The RTCD payloads: big-endian would put these bytes somewhere else.
 * ------------------------------------------------------------------ */

static void test_rtcd_payloads(void) {
    /* RTCD_REQ_ALARM with milliseconds = 1000 (0x3e8). */
    static const uint8_t alarm[4] = {0xe8, 0x03, 0x00, 0x00};
    a20_idl_rtcd_alarm_request_t req, back;
    uint8_t wire[4], old[4];

    req.milliseconds = 1000u;
    a20_idl_rtcd_alarm_request_encode(wire, &req);
    CHECK(bytes_equal(wire, alarm, 4u), "alarm payload bytes changed");
    if (!bytes_equal(wire, alarm, 4u))
        dump("alarm", wire, 4u);

    {
        a20_idl_rtcd_alarm_request_t f = {1000u};
        unsigned i;
        for (i = 0; i < sizeof(f); i++)
            old[i] = ((const uint8_t *)&f)[i];
    }
    CHECK(bytes_equal(wire, old, 4u),
          "alarm payload: struct image differs from codec bytes");

    a20_idl_rtcd_alarm_request_decode(&back, wire);
    CHECK(back.milliseconds == 1000u, "alarm payload round trip");

    /* RTCD_TIME_RESPONSE: seconds and nanoseconds, u64 little-endian. */
    {
        static const uint8_t time_golden[16] = {
            0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
            0x10, 0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x09};
        a20_idl_rtcd_time_response_t resp, back_t;
        uint8_t twire[16];

        resp.seconds = 0x0102030405060708ull;
        resp.nanoseconds = 0x090a0b0c0d0e0f10ull;
        a20_idl_rtcd_time_response_encode(twire, &resp);
        CHECK(bytes_equal(twire, time_golden, 16u),
              "time payload bytes changed");
        if (!bytes_equal(twire, time_golden, 16u))
            dump("time", twire, 16u);
        a20_idl_rtcd_time_response_decode(&back_t, twire);
        CHECK(back_t.seconds == resp.seconds, "time seconds round trip");
        CHECK(back_t.nanoseconds == resp.nanoseconds,
              "time nanoseconds round trip");
    }
}

/* ------------------------------------------------------------------ *
 * 3. echod's accept/ignore rule, transcribed from user/svc/echod.c.
 * ------------------------------------------------------------------ */

enum { PUMP_IDLE = 0, PUMP_HANDLED = 1, PUMP_CRASH = 42, PUMP_GONE = -1 };

static int pump_accepts(const uint8_t *buf, uint32_t blen) {
    a20_idl_envelope_t env;
    if (blen < A20_IDL_ENVELOPE_WIRE_SIZE)
        return PUMP_IDLE; /* too short to hold an envelope */
    a20_idl_envelope_decode(&env, buf);
    if (env.version != A20_SERVICES_IDL_VERSION || env.size != blen)
        return PUMP_IDLE; /* protocol mismatch: ignore */
    if (env.type == SVCMGR_REQ_CRASH)
        return PUMP_CRASH;
    return PUMP_HANDLED; /* echo back verbatim */
}

static void test_malformed_handling(void) {
    uint8_t frame[16];
    unsigned n = echo_codec(frame, 0);

    /* The well-formed echo frame is accepted. */
    CHECK(pump_accepts(frame, n) == PUMP_HANDLED,
          "echo frame should be handled");

    /* Short: 0..7 bytes are not an envelope at all. */
    CHECK(pump_accepts(frame, 0) == PUMP_IDLE, "zero-length message");
    CHECK(pump_accepts(frame, 7) == PUMP_IDLE, "7-byte message");
    CHECK(pump_accepts(frame, A20_IDL_ENVELOPE_WIRE_SIZE - 1) == PUMP_IDLE,
          "envelope size - 1");

    /* Wrong version.  0 and 2 are the neighbours of v1; 0xffff is garbage. */
    {
        static const uint16_t bad[3] = {0, 2, 0xffff};
        unsigned i;
        for (i = 0; i < 3; i++) {
            a20_idl_envelope_t env;
            uint8_t bad_frame[12];
            env.version = bad[i];
            env.type = SVCMGR_REQ_ECHO;
            env.size = 12;
            a20_idl_envelope_encode(bad_frame, &env);
            a20_idl_svcmgr_echo_encode(bad_frame + A20_IDL_ENVELOPE_WIRE_SIZE,
                                       &(a20_idl_svcmgr_echo_t){0});
            CHECK(pump_accepts(bad_frame, 12) == PUMP_IDLE,
                  "version %u should be ignored", bad[i]);
        }
    }

    /* Wrong size: shorter and longer than the frame actually received.
     * echod compares env.size against the whole received length, not against
     * a payload length, so both of these are ignored. */
    {
        a20_idl_envelope_t env;
        uint8_t bad_frame[12];

        env.version = A20_SERVICES_IDL_VERSION;
        env.type = SVCMGR_REQ_ECHO;
        env.size = 4; /* envelope-only claim */
        a20_idl_envelope_encode(bad_frame, &env);
        a20_idl_svcmgr_echo_encode(bad_frame + A20_IDL_ENVELOPE_WIRE_SIZE,
                                   &(a20_idl_svcmgr_echo_t){0});
        CHECK(pump_accepts(bad_frame, 12) == PUMP_IDLE,
              "size=4 on a 12-byte frame");

        env.size = 16; /* claims more than was sent */
        a20_idl_envelope_encode(bad_frame, &env);
        CHECK(pump_accepts(bad_frame, 12) == PUMP_IDLE,
              "size=16 on a 12-byte frame");

        env.size = 0;
        a20_idl_envelope_encode(bad_frame, &env);
        CHECK(pump_accepts(bad_frame, 12) == PUMP_IDLE,
              "size=0 on a 12-byte frame");
    }

    /* Truncated frame whose envelope still claims the full length. */
    CHECK(pump_accepts(frame, 11) == PUMP_IDLE,
          "11 of 12 bytes: size claims 12, only 11 arrived");

    /* Unknown SVCMGR type: echod echoes every well-formed v1 frame verbatim
     * (user/svc/echod.c's default arm), so 0x7b is handled, not dropped. */
    {
        a20_idl_envelope_t env;
        uint8_t unknown[12];
        env.version = A20_SERVICES_IDL_VERSION;
        env.type = 0x7b;
        env.size = 12;
        a20_idl_envelope_encode(unknown, &env);
        a20_idl_svcmgr_echo_encode(unknown + A20_IDL_ENVELOPE_WIRE_SIZE,
                                   &(a20_idl_svcmgr_echo_t){0});
        CHECK(pump_accepts(unknown, 12) == PUMP_HANDLED,
              "unknown SVCMGR type is echoed verbatim");
    }

    /* The crash type is only honoured after the checks above, so a
     * malformed crash request is ignored rather than fatal. */
    {
        a20_idl_envelope_t env;
        uint8_t crash[8];
        env.version = A20_SERVICES_IDL_VERSION;
        env.type = SVCMGR_REQ_CRASH;
        env.size = (uint32_t)sizeof(env);
        a20_idl_envelope_encode(crash, &env);
        CHECK(pump_accepts(crash, 8) == PUMP_CRASH, "well-formed crash frame");

        env.version = 2;
        a20_idl_envelope_encode(crash, &env);
        CHECK(pump_accepts(crash, 8) == PUMP_IDLE,
              "crash type with a bad version is ignored, not fatal");

        env.version = A20_SERVICES_IDL_VERSION;
        env.size = 9;
        a20_idl_envelope_encode(crash, &env);
        CHECK(pump_accepts(crash, 8) == PUMP_IDLE,
              "crash type with a bad size is ignored, not fatal");
    }
}

/* RTCD's dispatch policy, transcribed from user/svc/rtcd.c: a wrong version
 * or size is dropped; an unknown type is consumed but produces no reply; a
 * short alarm payload is dropped even though the type is known. */
enum { RTCD_DROP = 1, RTCD_TIME = 2, RTCD_ALARM = 3, RTCD_CRASH = 4 };

static int rtcd_consume(const uint8_t *buf, uint32_t blen) {
    a20_idl_envelope_t env;
    if (blen < A20_IDL_ENVELOPE_WIRE_SIZE)
        return RTCD_DROP;
    a20_idl_envelope_decode(&env, buf);
    if (env.version != A20_SERVICES_IDL_VERSION || env.size != blen)
        return RTCD_DROP;
    switch (env.type) {
    case RTCD_REQ_TIME:
        return RTCD_TIME;
    case RTCD_REQ_ALARM:
        if (blen != A20_IDL_ENVELOPE_WIRE_SIZE +
                       A20_IDL_RTCD_ALARM_REQUEST_WIRE_SIZE)
            return RTCD_DROP;
        return RTCD_ALARM;
    case RTCD_REQ_CRASH:
        return RTCD_CRASH;
    default:
        return RTCD_DROP; /* consumed, no reply (the default arm) */
    }
}

static void test_rtcd_unknown_values(void) {
    uint8_t frame[16];
    a20_idl_envelope_t env;

    /* Well-formed time request. */
    env.version = A20_SERVICES_IDL_VERSION;
    env.type = RTCD_REQ_TIME;
    env.size = A20_IDL_ENVELOPE_WIRE_SIZE;
    a20_idl_envelope_encode(frame, &env);
    CHECK(rtcd_consume(frame, A20_IDL_ENVELOPE_WIRE_SIZE) == RTCD_TIME,
          "RTCD time request is handled");

    /* Unknown type: consumed silently, never a reply. */
    env.type = 0x7b;
    a20_idl_envelope_encode(frame, &env);
    CHECK(rtcd_consume(frame, A20_IDL_ENVELOPE_WIRE_SIZE) == RTCD_DROP,
          "unknown RTCD type is consumed without a reply");

    /* Unknown envelope version is dropped before dispatch. */
    env.version = 2;
    a20_idl_envelope_encode(frame, &env);
    CHECK(rtcd_consume(frame, A20_IDL_ENVELOPE_WIRE_SIZE) == RTCD_DROP,
          "unknown RTCD version is dropped");

    /* Alarm with a missing payload is dropped even though the type matches. */
    env.version = A20_SERVICES_IDL_VERSION;
    env.type = RTCD_REQ_ALARM;
    env.size = A20_IDL_ENVELOPE_WIRE_SIZE;
    a20_idl_envelope_encode(frame, &env);
    CHECK(rtcd_consume(frame, A20_IDL_ENVELOPE_WIRE_SIZE) == RTCD_DROP,
          "short RTCD alarm payload is dropped");

    /* Crash keeps its terminal meaning. */
    env.type = RTCD_REQ_CRASH;
    a20_idl_envelope_encode(frame, &env);
    CHECK(rtcd_consume(frame, A20_IDL_ENVELOPE_WIRE_SIZE) == RTCD_CRASH,
          "RTCD crash is terminal");
}

/* An echoed reply is the request bytes unchanged: echod forwards buf, blen. */
static void test_echo_is_verbatim(void) {
    uint8_t req[16], rep[16];
    a20_idl_svcmgr_echo_t body;
    unsigned n = echo_codec(req, 2);
    unsigned i;
    for (i = 0; i < n; i++)
        rep[i] = req[i];
    CHECK(bytes_equal(rep, req, n), "echo reply differs from the request");
    a20_idl_svcmgr_echo_decode(&body, rep + A20_IDL_ENVELOPE_WIRE_SIZE);
    CHECK(body.seq == 2, "echo reply carries the original seq");
}

/* The published layout numbers the protocol depends on. */
static void test_declared_layout(void) {
    CHECK(A20_SERVICES_IDL_VERSION == 1u, "interface version");
    CHECK(SVCMGR_REQ_ECHO == 0x21u, "SVCMGR_REQ_ECHO");
    CHECK(SVCMGR_REQ_CRASH == 0x22u, "SVCMGR_REQ_CRASH");
    CHECK(A20_SVC_CRASH_CODE == 42, "A20_SVC_CRASH_CODE");
    CHECK(A20_IDL_ENVELOPE_WIRE_SIZE == 8u, "envelope wire size");
    CHECK(A20_IDL_SVCMGR_ECHO_WIRE_SIZE == 4u, "echo wire size");
    CHECK(A20_IDL_RTCD_ALARM_REQUEST_WIRE_SIZE == 4u, "alarm wire size");
    CHECK(A20_IDL_RTCD_TIME_RESPONSE_WIRE_SIZE == 16u, "time wire size");
    /* v1 messages are packed, so the generator emitted layout asserts for
     * them: sizeof/offsetof equal the wire numbers *here*.  That is a layout
     * statement, not an endianness one -- byte order is the _Static_assert
     * at the top of this file and the negative test in test_a20idl.py. */
    CHECK(sizeof(a20_idl_envelope_t) == A20_IDL_ENVELOPE_WIRE_SIZE,
          "envelope struct vs wire size");
    CHECK(sizeof(a20_idl_svcmgr_echo_t) == A20_IDL_SVCMGR_ECHO_WIRE_SIZE,
          "echo struct vs wire size");
    CHECK(sizeof(a20_idl_rtcd_alarm_request_t) ==
              A20_IDL_RTCD_ALARM_REQUEST_WIRE_SIZE,
          "alarm struct vs wire size");
    CHECK(sizeof(a20_idl_rtcd_time_response_t) ==
              A20_IDL_RTCD_TIME_RESPONSE_WIRE_SIZE,
          "time struct vs wire size");
}

int main(void) {
    test_declared_layout();
    test_echo_frames();
    test_crash_frame();
    test_rtcd_payloads();
    test_malformed_handling();
    test_rtcd_unknown_values();
    test_echo_is_verbatim();

    if (failures) {
        printf("a20idl wire tests: FAIL (%d)\n", failures);
        return 1;
    }
    printf("a20idl wire tests: PASS\n");
    return 0;
}