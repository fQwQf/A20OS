/*
 * Compiled test for generated IDL code -- mixed field widths.
 *
 * tools/tests/test_a20idl.py pins what the generator *writes*.  That cannot
 * catch an emitter that writes valid-looking text that will not compile, which
 * is exactly how a u8 field used to slip through: the emitted line was
 * `buf + A20_IDL_MIXED_OFF_TAG = src->tag;`, which no compiler accepts, and
 * every assertion upstream of it was a string comparison against a golden that
 * had never been through a compiler either.  So this test does the other half:
 * the build generates a header from tools/tests/idl_mixed.idl, compiles it with
 * the project's host flags, and runs it.
 *
 * The header is generated into $(TMPDIR) by tools/targets-gates.mk and included
 * below; nothing here reads a checked-in artifact.
 *
 * What it pins:
 *
 *  1. Mixed widths encode to the declared packed size in little-endian byte
 *     order.  MIXED is {u8,u16,u32,u64} = 15 bytes at offsets 0/1/3/7, while
 *     the host struct is 16 bytes with the u32 at offset 4.  A codec that
 *     deferred to host layout could not produce these bytes.
 *  2. Decode is the exact inverse, checked field by field against values that
 *     differ in every byte position -- including 0x80, which is the byte a
 *     sign-extension slip shows up in.
 *  3. The layout assertion is emitted where it is sound (PACKED) and withheld
 *     where it is not (MIXED).  MIXED's absence is asserted by the fact that
 *     this file compiles at all; PACKED's presence by a _Static_assert.
 *  4. The target byte-order restriction is live: the header defines
 *     A20_SERVICES_IDL_TARGET_IS_LITTLE_ENDIAN only because it passed.
 *     tools/tests/test_a20idl.py compiles this same header with
 *     __BYTE_ORDER__ forced big-endian and requires it to fail.
 *
 * Built and run by `make host-tests`.
 */
#include <stdint.h>
#include <stdio.h>

#include "a20idl_mixed.h"

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

/* (4) The endianness gate passed for this translation unit; if it had not,
 * this file would not have compiled at all. */
_Static_assert(A20_SERVICES_IDL_TARGET_IS_LITTLE_ENDIAN == 1,
               "the generated header refuses a big-endian target");

/* (3) PACKED's layout assertion is present and holds: this only compiles
 * because the generator emitted the sizeof/offsetof asserts for it. */
_Static_assert(sizeof(a20_idl_packed_t) == A20_IDL_PACKED_WIRE_SIZE,
               "PACKED struct size must equal its wire size");
_Static_assert(offsetof(a20_idl_packed_t, stamp) == A20_IDL_PACKED_OFF_STAMP,
               "PACKED stamp must start at its wire offset");

static void test_declared_layout(void) {
    CHECK(A20_SERVICES_IDL_VERSION == 1u, "version");

    CHECK(A20_IDL_MIXED_WIRE_SIZE == 15u, "MIXED wire size %u",
          A20_IDL_MIXED_WIRE_SIZE);
    CHECK(A20_IDL_MIXED_OFF_TAG == 0u, "MIXED tag offset");
    CHECK(A20_IDL_MIXED_OFF_ID == 1u, "MIXED id offset");
    CHECK(A20_IDL_MIXED_OFF_COUNT == 3u, "MIXED count offset");
    CHECK(A20_IDL_MIXED_OFF_STAMP == 7u, "MIXED stamp offset");

    CHECK(A20_IDL_PACKED_WIRE_SIZE == 16u, "PACKED wire size");
    CHECK(A20_IDL_PACKED_OFF_ID == 0u, "PACKED id offset");
    CHECK(A20_IDL_PACKED_OFF_KIND == 2u, "PACKED kind offset");
    CHECK(A20_IDL_PACKED_OFF_COUNT == 4u, "PACKED count offset");
    CHECK(A20_IDL_PACKED_OFF_STAMP == 8u, "PACKED stamp offset");

    /* The fixture's whole premise: MIXED's struct is NOT the wire layout, so
     * the generator must not have asserted that it was. */
    CHECK(sizeof(a20_idl_mixed_t) != A20_IDL_MIXED_WIRE_SIZE,
          "MIXED struct (%u bytes) is the same size as its wire form (%u); the "
          "fixture no longer exercises a non-packed struct",
          (unsigned)sizeof(a20_idl_mixed_t), A20_IDL_MIXED_WIRE_SIZE);
    CHECK(offsetof(a20_idl_mixed_t, count) != A20_IDL_MIXED_OFF_COUNT,
          "MIXED struct has count at its wire offset; the fixture no longer "
          "exercises a non-packed struct");
}

/* (1) Encode: the whole point is that these bytes are not the struct's. */
static void test_mixed_encode_golden(void) {
    /* tag=0x80, id=0x1234, count=0xdeadbeef, stamp=0x0102030405060708 */
    static const uint8_t golden[A20_IDL_MIXED_WIRE_SIZE] = {
        0x80,                                          /* tag   */
        0x34, 0x12,                                    /* id    */
        0xef, 0xbe, 0xad, 0xde,                        /* count */
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 /* stamp */
    };
    a20_idl_mixed_t m;
    uint8_t wire[A20_IDL_MIXED_WIRE_SIZE];

    m.tag = 0x80u;
    m.id = 0x1234u;
    m.count = 0xdeadbeefu;
    m.stamp = 0x0102030405060708ull;

    a20_idl_mixed_encode(wire, &m);
    CHECK(bytes_equal(wire, golden, sizeof(golden)),
          "MIXED encode is not the golden little-endian byte string");
    if (!bytes_equal(wire, golden, sizeof(golden))) {
        dump("encode", wire, sizeof(golden));
        dump("golden", golden, sizeof(golden));
    }
}

/* (1, cont.) Every field boundary hit with a high byte that is non-zero, so a
 * big-endian or partially-swapped emitter cannot pass. */
static void test_mixed_encode_widths(void) {
    static const struct {
        uint8_t tag;
        uint16_t id;
        uint32_t count;
        uint64_t stamp;
        uint8_t expect[A20_IDL_MIXED_WIRE_SIZE];
    } cases[] = {
        /* tag=00 id=0100 count=00000100 stamp=0000000000000100 */
        {0x00,
         0x0100,
         0x00000100u,
         0x0000000000000100ull,
         {0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00}},
        /* tag=ff id=ff00 count=ffff0000 stamp=ffffffffffff0000 */
        {0xff,
         0xff00,
         0xffff0000u,
         0xffffffffffff0000ull,
         {0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xff}},
        /* tag=01 id=0080 count=80000000 stamp=8000000000000000 */
        {0x01,
         0x0080,
         0x80000000u,
         0x8000000000000000ull,
         {0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x80}},
        /* tag=7f id=00ff count=000000ff stamp=00000000000000ff */
        {0x7f,
         0x00ff,
         0x000000ffu,
         0x00000000000000ffull,
         {0x7f, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00}},
    };
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        a20_idl_mixed_t m;
        uint8_t wire[A20_IDL_MIXED_WIRE_SIZE];
        m.tag = cases[i].tag;
        m.id = cases[i].id;
        m.count = cases[i].count;
        m.stamp = cases[i].stamp;
        a20_idl_mixed_encode(wire, &m);
        CHECK(bytes_equal(wire, cases[i].expect, A20_IDL_MIXED_WIRE_SIZE),
              "MIXED encode case %u is not little-endian", i);
        if (!bytes_equal(wire, cases[i].expect, A20_IDL_MIXED_WIRE_SIZE))
            dump("encode", wire, A20_IDL_MIXED_WIRE_SIZE);
    }
}

/* (2) Decode: the inverse, field by field. */
static void test_mixed_decode_golden(void) {
    static const uint8_t golden[A20_IDL_MIXED_WIRE_SIZE] = {
        0x80, 0x34, 0x12, 0xef, 0xbe, 0xad, 0xde, 0x08,
        0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    a20_idl_mixed_t m;

    a20_idl_mixed_decode(&m, golden);
    CHECK(m.tag == 0x80u, "tag 0x%02x", m.tag);
    CHECK(m.id == 0x1234u, "id 0x%04x", (unsigned)m.id);
    CHECK(m.count == 0xdeadbeefu, "count 0x%08lx", (unsigned long)m.count);
    CHECK(m.stamp == 0x0102030405060708ull, "stamp 0x%016llx",
          (unsigned long long)m.stamp);
}

/* (2, cont.) Round trip over values that stress each width's high bytes. */
static void test_round_trip(void) {
    static const uint64_t stamps[4] = {0ull, 1ull, 0x0102030405060708ull,
                                       0xffffffffffffffffull};
    unsigned i;
    for (i = 0; i < 4; i++) {
        a20_idl_mixed_t out, back;
        uint8_t wire[A20_IDL_MIXED_WIRE_SIZE];
        out.tag = (uint8_t)(0x80u + i);
        out.id = (uint16_t)(0x1234u + i);
        out.count = (uint32_t)(0xdeadbe00u + i);
        out.stamp = stamps[i];
        a20_idl_mixed_encode(wire, &out);
        a20_idl_mixed_decode(&back, wire);
        CHECK(back.tag == out.tag && back.id == out.id &&
                  back.count == out.count && back.stamp == out.stamp,
              "MIXED round trip failed for stamp index %u", i);
    }
}

/* The packed message still has to agree with the wire byte string, and -- this
 * is the property the v1 services rely on -- so does its plain struct image. */
static void test_packed_golden(void) {
    /* id=0x0201 kind=0x0403 count=0x08070605 stamp=0x100f0e0d0c0b0a09 --
     * 16 bytes at offsets 0/2/4/8, every byte non-zero, so no position in the
     * message is unproven and a short literal cannot zero-fill past the end. */
    static const uint8_t golden[A20_IDL_PACKED_WIRE_SIZE] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
    a20_idl_packed_t p, back;
    uint8_t wire[A20_IDL_PACKED_WIRE_SIZE];

    p.id = 0x0201u;
    p.kind = 0x0403u;
    p.count = 0x08070605u;
    p.stamp = 0x100f0e0d0c0b0a09ull;

    a20_idl_packed_encode(wire, &p);
    CHECK(bytes_equal(wire, golden, sizeof(golden)), "PACKED encode bytes");
    if (!bytes_equal(wire, golden, sizeof(golden)))
        dump("packed", wire, sizeof(golden));

    /* Safe because the generator asserted this message's layout AND the
     * header checked the target byte order: on this target the struct's own
     * bytes are the wire bytes. */
    CHECK(bytes_equal((const uint8_t *)&p, golden, sizeof(golden)),
          "PACKED struct image differs from the wire bytes");

    a20_idl_packed_decode(&back, wire);
    CHECK(back.id == p.id && back.kind == p.kind && back.count == p.count &&
              back.stamp == p.stamp,
          "PACKED round trip");
}

/* Scalar accessors, including the signed pair and the extreme values. */
static void test_scalar_accessors(void) {
    uint8_t buf[8];
    a20_idl_mixed_t m;

    a20_idl_put_u16(buf, 0x0102u);
    CHECK(buf[0] == 0x02u && buf[1] == 0x01u, "put_u16 is little-endian");
    CHECK(a20_idl_get_u16(buf) == 0x0102u, "get_u16 round trip");

    a20_idl_put_u32(buf, 0x01020304u);
    CHECK(buf[0] == 0x04u && buf[3] == 0x01u, "put_u32 is little-endian");
    CHECK(a20_idl_get_u32(buf) == 0x01020304u, "get_u32 round trip");

    a20_idl_put_u64(buf, 0x0102030405060708ull);
    CHECK(buf[0] == 0x08u && buf[7] == 0x01u, "put_u64 is little-endian");
    CHECK(a20_idl_get_u64(buf) == 0x0102030405060708ull, "get_u64 round trip");

    a20_idl_put_i32(buf, -2);
    CHECK(buf[0] == 0xfeu && buf[1] == 0xffu && buf[2] == 0xffu &&
              buf[3] == 0xffu,
          "put_i32(-2) is little-endian two's complement");
    CHECK(a20_idl_get_i32(buf) == -2, "get_i32 round trip");

    a20_idl_put_i32(buf, (-2147483647 - 1));
    CHECK(a20_idl_get_i32(buf) == (-2147483647 - 1), "get_i32(INT32_MIN)");

    a20_idl_put_i64(buf, -2);
    CHECK(buf[0] == 0xfeu && buf[7] == 0xffu, "put_i64(-2) two's complement");
    CHECK(a20_idl_get_i64(buf) == -2, "get_i64 round trip");

    a20_idl_put_i64(buf, (-9223372036854775807LL - 1));
    CHECK(a20_idl_get_i64(buf) == (-9223372036854775807LL - 1),
          "get_i64(INT64_MIN)");

    /* A u8 field is a byte, not a small signed integer, and the other three
     * widths are exactly as wide as they claim.  MIXED needs its own buffer:
     * writing it through the 8-byte scratch buf above would be the very
     * host-layout confusion this test exists to catch. */
    {
        uint8_t wide[A20_IDL_MIXED_WIRE_SIZE];
        unsigned i;
        m.tag = 0x80u;
        m.id = 0x0000u;
        m.count = 0x00000000u;
        m.stamp = 0x0000000000000000ull;
        a20_idl_mixed_encode(wide, &m);
        CHECK(wide[0] == 0x80u, "a u8 field must not be sign-extended");
        for (i = 1; i < A20_IDL_MIXED_WIRE_SIZE; i++)
            CHECK(wide[i] == 0x00u,
                  "zero-valued field leaked into wire byte %u", i);
    }
}

int main(void) {
    test_declared_layout();
    test_mixed_encode_golden();
    test_mixed_encode_widths();
    test_mixed_decode_golden();
    test_round_trip();
    test_packed_golden();
    test_scalar_accessors();

    if (failures) {
        printf("a20idl mixed-width tests: FAIL (%d)\n", failures);
        return 1;
    }
    printf("a20idl mixed-width tests: PASS\n");
    return 0;
}