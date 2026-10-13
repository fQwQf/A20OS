"""Generator tests for tools/a20idl.py.

Two layers, deliberately.

The string layer pins what the generator *writes*: the packed little-endian
sizes and field offsets, the emitted text of a mixed-width message, and the
rejection paths.  Cheap and precise, and it catches wording drift.

The compile layer (EndiannessRestrictionTests) exists because the string layer
provably cannot: a u8 field used to emit `buf + OFF = src->f;`, which is not C,
and the golden that recorded it was never compiled either.  So the byte-order
restriction is tested by compiling the real generated header with
__BYTE_ORDER__ forced big-endian and requiring it to fail.  It is skipped when
no host compiler is available, matching tools/check-drm-abi.sh, which skips
when the headers it needs are absent.

Stdlib unittest, matching the rest of tools/tests (make check-a20-tests).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import a20idl


ROOT = Path(__file__).resolve().parents[2]
IDL = ROOT / "user" / "svc" / "a20_services.idl"
LIB = ROOT / "user" / "liba20rt"
HOST_CC = os.environ.get("HOST_CC", "cc")
RUSTC = os.environ.get("RUSTC", "rustc")

# Sizes and field offsets for interface version 1.  These are not invented
# here: 8/0/2/4 is the envelope user/svc/echod.c reads out of a channel
# message and user/svc/svcman.c writes, 4/0 is the SVCMGR echo payload
# svcman appends to that envelope, and 4/0 plus 16/0/8 are the RTCD alarm and
# time payloads.  Changing any of them changes v1 bytes.  All four are packed,
# which is why the generator emits layout asserts for them and why svcman and
# echod may keep passing struct memory around.
V1_LAYOUT = {
    "ENVELOPE": (8, {"VERSION": 0, "TYPE": 2, "SIZE": 4}),
    "RTCD_ALARM_REQUEST": (4, {"MILLISECONDS": 0}),
    "RTCD_TIME_RESPONSE": (16, {"SECONDS": 0, "NANOSECONDS": 8}),
    "SVCMGR_ECHO": (4, {"SEQ": 0}),
}

# MIXED is deliberately NOT packed on any of these targets: the u32 would sit
# at offset 4 in a host struct rather than at the wire offset 3, and the
# struct is 16 bytes rather than the 15 on the wire.  tools/tests/idl_mixed.idl
# compiles this one for real.
MIXED_WIRE = {"MIXED": (15, {"TAG": 0, "ID": 1, "COUNT": 3, "STAMP": 7}),
              "PACKED": (16, {"ID": 0, "KIND": 2, "COUNT": 4, "STAMP": 8})}


def synth(body: str) -> str:
    return f"interface synth version 9 {{\n{body}\n}}\n"


def compile_flags():
    """The project's host-test flags, from tools/targets-gates.mk defaults."""
    return ["-std=gnu99", "-O2", "-Wall", "-Wextra"]


def generate_from(body: str) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "synth.idl"
        src.write_text(body, encoding="ascii")
        return a20idl.generate(src)


class RepoIdlTests(unittest.TestCase):
    def setUp(self):
        self.text = a20idl.generate(IDL)

    def test_version_and_constants_are_unchanged(self):
        self.assertIn("#define A20_SERVICES_IDL_VERSION 1u", self.text)
        self.assertIn("#define SVCMGR_REQ_ECHO (0x21)", self.text)
        self.assertIn("#define SVCMGR_REQ_CRASH (0x22)", self.text)
        self.assertIn("#ifndef _A20_SERVICES_IDL_H", self.text)
        self.assertIn("#define _A20_SERVICES_IDL_H", self.text)

    def test_declared_wire_sizes_and_offsets_are_the_v1_ones(self):
        for message, (size, offsets) in V1_LAYOUT.items():
            self.assertIn(f"#define A20_IDL_{message}_WIRE_SIZE {size}u",
                          self.text)
            for field, offset in offsets.items():
                self.assertIn(f"#define A20_IDL_{message}_OFF_{field} {offset}u",
                              self.text)

    def test_every_v1_message_is_packed_so_the_layout_asserts_are_emitted(self):
        # These asserts are what let echod and svcman keep handing struct
        # memory to the channel.  They say nothing about byte order -- that is
        # the restriction further down the header.
        self.assertEqual(
            len(V1_LAYOUT),
            self.text.count(": struct size must equal the packed wire size"))
        self.assertEqual(
            sum(len(fields) for _, fields in V1_LAYOUT.values()),
            self.text.count(": must start at its wire offset"))

    def test_target_byte_order_is_restricted_explicitly(self):
        # Layout asserts cannot answer this, so the header has to, and it has
        # to leave a name a program can test.
        self.assertIn("__BYTE_ORDER__", self.text)
        self.assertIn("__ORDER_LITTLE_ENDIAN__", self.text)
        self.assertIn("interface version 1 is little-endian on the wire",
                      self.text)
        self.assertIn("#define A20_SERVICES_IDL_TARGET_IS_LITTLE_ENDIAN 1",
                      self.text)

    def test_wire_note_separates_the_codec_from_the_struct(self):
        self.assertIn("The generated struct next to them is a different thing",
                      self.text)
        self.assertIn("they say nothing about byte order", self.text)

    def test_encode_and_decode_helpers_exist_for_every_message(self):
        for message in V1_LAYOUT:
            lower = message.lower()
            self.assertIn(f"static inline void a20_idl_{lower}_encode(",
                          self.text)
            self.assertIn(f"static inline void a20_idl_{lower}_decode(",
                          self.text)

    def test_scalar_accessors_are_byte_wise_not_memcpy(self):
        self.assertIn(a20idl.PRIMITIVES, self.text)
        self.assertNotIn("a20_memcpy", self.text)

    def test_generation_is_deterministic(self):
        self.assertEqual(self.text, a20idl.generate(IDL))


class SyntheticIdlGoldenTests(unittest.TestCase):
    """Mixed widths in an order a host struct would pad, plus a packed one."""

    MIXED_BODY = (
        "  message MIXED {\n"
        "    u8 tag;\n"
        "    u16 id;\n"
        "    u32 count;\n"
        "    u64 stamp;\n"
        "  }\n"
        "  message PACKED {\n"
        "    u16 id;\n"
        "    u16 kind;\n"
        "    u32 count;\n"
        "    u64 stamp;\n"
        "  }"
    )

    # MIXED: 1 + 2 + 4 + 8 = 15 wire bytes.  A host struct is 16 with count at
    # 4, so no layout assert may be emitted and the u8 accessors must index
    # rather than form a (invalid) `buf + OFF = ...` assignment.
    MIXED_TAIL = """typedef struct {
    uint8_t tag;
    uint16_t id;
    uint32_t count;
    uint64_t stamp;
} a20_idl_mixed_t;

#define A20_IDL_MIXED_WIRE_SIZE 15u
#define A20_IDL_MIXED_OFF_TAG 0u
#define A20_IDL_MIXED_OFF_ID 1u
#define A20_IDL_MIXED_OFF_COUNT 3u
#define A20_IDL_MIXED_OFF_STAMP 7u"""

    MIXED_CODEC = """static inline void a20_idl_mixed_encode(uint8_t *buf, const a20_idl_mixed_t *src)
{
    buf[A20_IDL_MIXED_OFF_TAG] = src->tag;
    a20_idl_put_u16(buf + A20_IDL_MIXED_OFF_ID, src->id);
    a20_idl_put_u32(buf + A20_IDL_MIXED_OFF_COUNT, src->count);
    a20_idl_put_u64(buf + A20_IDL_MIXED_OFF_STAMP, src->stamp);
}"""

    MIXED_DECODE = """static inline void a20_idl_mixed_decode(a20_idl_mixed_t *dst, const uint8_t *buf)
{
    dst->tag = buf[A20_IDL_MIXED_OFF_TAG];
    dst->id = a20_idl_get_u16(buf + A20_IDL_MIXED_OFF_ID);
    dst->count = a20_idl_get_u32(buf + A20_IDL_MIXED_OFF_COUNT);
    dst->stamp = a20_idl_get_u64(buf + A20_IDL_MIXED_OFF_STAMP);
}"""

    def setUp(self):
        self.text = generate_from(synth(self.MIXED_BODY))

    def test_offsets_are_packed_not_aligned(self):
        # 8 + 1 + 4 + 4 + 8 + 2 with no alignment holes is the general case;
        # here MIXED is 1 + 2 + 4 + 8 = 15 and PACKED is 2 + 2 + 4 + 8 = 16,
        # both of which a host struct would pad.  The codec exists because the
        # header publishes the size instead of sizeof().
        for message, (size, offsets) in MIXED_WIRE.items():
            self.assertIn(f"#define A20_IDL_{message}_WIRE_SIZE {size}u",
                          self.text)
            for field, offset in offsets.items():
                self.assertIn(
                    f"#define A20_IDL_{message}_OFF_{field} {offset}u",
                    self.text)

    def test_mixed_tail_is_the_golden_text(self):
        self.assertIn(self.MIXED_TAIL, self.text)

    def test_u8_fields_index_the_buffer(self):
        # The bug this whole module exists to prevent: emitting
        # `buf + A20_IDL_MIXED_OFF_TAG = src->tag;`, which is not C.  Asserted
        # here so it is caught without a compiler, and compiled for real by
        # tools/tests/test_a20_idl_mixed.c.
        self.assertIn(self.MIXED_CODEC, self.text)
        self.assertIn(self.MIXED_DECODE, self.text)
        self.assertNotIn("buf + A20_IDL_MIXED_OFF_TAG =", self.text)

    def test_no_layout_assert_for_a_message_that_is_not_packed(self):
        # Emitting one would make the header uncompilable for this message,
        # which is the other half of the same bug.
        self.assertIn("no layout assert is emitted for it", self.text)
        self.assertNotIn(
            "_Static_assert(sizeof(a20_idl_mixed_t)", self.text)

    def test_layout_assert_for_a_message_that_is_packed(self):
        self.assertIn(
            "_Static_assert(sizeof(a20_idl_packed_t) == "
            "A20_IDL_PACKED_WIRE_SIZE,", self.text)

    def test_signed_fields_use_the_two_complement_accessors(self):
        body = synth("  message S {\n    i32 a;\n    i64 b;\n  }")
        text = generate_from(body)
        self.assertIn("a20_idl_put_i32(buf + A20_IDL_S_OFF_A, src->a);", text)
        self.assertIn("dst->b = a20_idl_get_i64(buf + A20_IDL_S_OFF_B);", text)


class SignedArithmeticTests(unittest.TestCase):
    """The put_i32/put_i64 magnitude step, which must not overflow.

    Written -(v + 1) + 1 rather than -(v) so INT32_MIN/INT64_MIN are
    representable, then negated in unsigned arithmetic.  tools/tests/
    test_a20_idl_mixed.c compiles and runs the real thing for the extremes.
    """

    def test_magnitude_form_avoids_negating_the_minimum(self):
        self.assertIn("(uint32_t)(-(v + 1)) + 1u", a20idl.PRIMITIVES)
        self.assertIn("(uint64_t)(-(v + 1)) + 1u", a20idl.PRIMITIVES)
        self.assertIn("(0u - magnitude)", a20idl.PRIMITIVES)

    def test_v1_message_codecs_use_the_unsigned_accessors(self):
        # The i32/i64 helpers are always defined -- the IDL grammar accepts
        # signed fields -- so what matters is that no *call site* uses them:
        # every interface version 1 field is unsigned.  Matching on the call
        # form ("a20_idl_put_i32(buf +") keeps this from tripping over the
        # definition itself.
        text = a20idl.generate(IDL)
        self.assertIn("a20_idl_put_u32(buf +", text)
        self.assertNotIn("a20_idl_put_i32(buf +", text)
        self.assertNotIn("a20_idl_put_i64(buf +", text)


@unittest.skipUnless(shutil.which(HOST_CC), f"{HOST_CC} not installed")
class EndiannessRestrictionTests(unittest.TestCase):
    """The restriction, tested by making the compiler enforce it.

    No amount of string matching can show that the #error actually fires, so
    this compiles the real generated header with __BYTE_ORDER__ forced to
    big-endian and requires the build to fail with our message.  The positive
    direction is the ordinary case: the same header compiles untouched.
    """

    ERROR_TEXT = "interface version 1 is little-endian on the wire"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.src = Path(self.tmp.name) / "probe.c"
        self.src.write_text('#include "a20_services_idl.h"\n'
                            "int main(void) { return 0; }\n", encoding="ascii")

    def compile(self, *extra):
        return subprocess.run(
            [HOST_CC, "-std=gnu99", "-c", str(self.src), "-o",
             str(Path(self.tmp.name) / "probe.o"),
             f"-I{LIB}", f"-I{ROOT / 'user' / 'svc'}", *extra],
            check=False, capture_output=True, text=True)

    def test_header_compiles_on_this_little_endian_host(self):
        result = self.compile()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_forced_big_endian_target_is_rejected(self):
        result = self.compile(f"-D__BYTE_ORDER__=__ORDER_BIG_ENDIAN__")
        self.assertNotEqual(result.returncode, 0,
                            "a big-endian target was allowed to build")
        self.assertIn(self.ERROR_TEXT, result.stderr)

    def test_a_missing_byte_order_macro_is_rejected_not_assumed(self):
        # Fail closed: a toolchain that cannot say what its byte order is gets
        # an error rather than an assumption.
        undef = "-U__BYTE_ORDER__"
        result = self.compile(undef)
        self.assertNotEqual(result.returncode, 0,
                            "an undeclared byte order was assumed to be little")
        self.assertIn("does not define __BYTE_ORDER__", result.stderr)


class RejectionTests(unittest.TestCase):
    def generate(self, body: str) -> str:
        return generate_from(body)

    def assertRejected(self, body: str, needle: str):
        with self.assertRaises(SystemExit) as caught:
            self.generate(body)
        self.assertIn(needle, str(caught.exception))

    def test_missing_interface_version(self):
        self.assertRejected("const A = 1;\n", "missing interface version")

    def test_duplicate_constant(self):
        self.assertRejected(synth("  const A = 1;\n  const A = 2;"),
                            "duplicate constant")

    def test_malformed_message_field(self):
        self.assertRejected(synth("  message M {\n    u7 bad;\n  }"),
                            "malformed message field")

    def test_empty_message_is_rejected(self):
        # An empty message generated invalid C before; now it is a named
        # failure, because every message must also emit a codec pair and a
        # layout decision that would be meaningless with no fields.
        self.assertRejected(synth("  message M {\n  }"), "empty message")

    def test_duplicate_field_in_message_is_rejected(self):
        # u16 a; u16 a; emitted a duplicate struct member and a redefined
        # offset macro, and the header only failed once something compiled it.
        self.assertRejected(
            synth("  message M {\n    u16 a;\n    u16 a;\n  }"),
            "duplicate field: a in message M")

    def test_field_names_colliding_only_by_case_are_rejected(self):
        # The quiet one.  `u16 a; u16 A;` is a legal struct, but the offset
        # macro is A20_IDL_M_OFF_A for both, the second definition wins, and
        # both fields encode through it.  A packed message trips the layout
        # assert; a non-packed one compiles clean and overwrites itself on
        # the wire, so the generator refuses both.
        for body in ("  message M {\n    u16 a;\n    u16 A;\n  }",
                     "  message M {\n    u8 a;\n    u8 A;\n    u32 b;\n  }"):
            with self.subTest(body=body):
                self.assertRejected(
                    synth(body),
                    "duplicate field: A in message M (same as a ignoring case)")

    def test_duplicate_message_name_is_rejected(self):
        # Two messages called M collide on the typedef, on every
        # A20_IDL_M_* macro and on the encode/decode pair.
        self.assertRejected(
            synth("  message M {\n    u16 a;\n  }\n"
                  "  message M {\n    u32 b;\n  }"),
            "duplicate message: M")

    def test_distinct_names_still_pass(self):
        # The checks above must not reject legal neighbours: a field and a
        # message may share a name, and field prefixes may differ only in
        # their tails.
        text = generate_from(synth(
            "  const A = 1;\n"
            "  message A {\n    u16 a;\n    u16 b;\n  }\n"
            "  message AB {\n    u32 ab;\n    u32 a;\n  }"))
        self.assertIn("a20_idl_a_t", text)
        self.assertIn("a20_idl_ab_t", text)
        self.assertIn("#define A20_IDL_A_OFF_A 0u", text)
        self.assertIn("#define A20_IDL_A_OFF_B 2u", text)
        self.assertIn("#define A20_IDL_AB_OFF_AB 0u", text)
        self.assertIn("#define A20_IDL_AB_OFF_A 4u", text)

    def test_unclosed_message_is_not_silently_dropped(self):
        # The block used to vanish from the header with no error at all,
        # which would drop a message from the wire contract silently.
        self.assertRejected("interface synth version 9 {\n"
                            "  message M {\n    u16 a;\n",
                            "unterminated message: M")

    def test_rust_backend_rejects_unrepresentable_special_identifiers(self):
        body = synth("  message BAD {\n    u8 self;\n  }")
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "bad.idl"
            source.write_text(body, encoding="ascii")
            with self.assertRaises(SystemExit) as caught:
                a20idl.generate_rust(source)
        self.assertIn("identifier cannot be represented: self", str(caught.exception))

    def test_rust_backend_rejects_unsupported_constant_expressions(self):
        body = synth("  const BAD = CALL(4);\n")
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "bad.idl"
            source.write_text(body, encoding="ascii")
            with self.assertRaises(SystemExit) as caught:
                a20idl.generate_rust(source)
        self.assertIn("unsupported IDL integer expression", str(caught.exception))


@unittest.skipUnless(shutil.which(RUSTC), "rustc is not installed")
class RustCodecTests(unittest.TestCase):
    """Compile generated Rust and check independently fixed wire bytes."""

    def test_mixed_width_rust_codec_matches_independent_golden(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            generated = root / "generated.rs"
            generated.write_text(a20idl.generate_rust(ROOT / "tools/tests/idl_mixed.idl"),
                                encoding="utf-8")
            test = root / "golden.rs"
            test.write_text(r'''#[path = "generated.rs"] mod idl;
#[test]
fn mixed_width_wire_golden() {
    let value = idl::MIXED { tag: 0x12, id: 0x3456,
        count: 0x789abcde, stamp: 0x0123456789abcdef };
    // Independently calculated packed LE layout: offsets 0,1,3,7.
    let golden: [u8; 15] = [0x12,0x56,0x34,0xde,0xbc,0x9a,0x78,
                            0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01];
    assert_eq!(idl::mixed_encode(&value), golden);
    assert_eq!(idl::mixed_decode(&golden), Some(value));
    assert_eq!(idl::mixed_decode(&golden[..14]), None);
}
#[test]
fn packed_wire_golden() {
    let value = idl::PACKED { id: 0x1234, kind: 0xabcd,
        count: 0x89abcdef, stamp: 0x0123456789abcdef };
    let golden: [u8; 16] = [0x34,0x12,0xcd,0xab,0xef,0xcd,0xab,0x89,
                            0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01];
    assert_eq!(idl::packed_encode(&value), golden);
    assert_eq!(idl::packed_decode(&golden), Some(value));
}
#[test]
fn signed_minimum_values_are_twos_complement_little_endian() {
    let value = idl::SIGNED { minimum32: i32::MIN, minimum64: i64::MIN };
    let golden: [u8; 12] = [0,0,0,0x80, 0,0,0,0,0,0,0,0x80];
    assert_eq!(idl::signed_encode(&value), golden);
    assert_eq!(idl::signed_decode(&golden), Some(value));
}
''', encoding="utf-8")
            binary = root / "golden"
            built = subprocess.run([RUSTC, "--edition=2021", "--test", str(test),
                                    "-o", str(binary)], text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("3 passed", ran.stdout)

    def test_rust_output_cli_is_additive_to_c_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            c_out, rust_out = Path(tmp) / "x.h", Path(tmp) / "x.rs"
            result = subprocess.run([sys.executable, str(ROOT / "tools/a20idl.py"),
                                     str(ROOT / "tools/tests/idl_mixed.idl"),
                                     str(c_out), "--rust-output", str(rust_out)],
                                   text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("a20_idl_mixed_encode", c_out.read_text())
            self.assertIn("pub fn mixed_encode", rust_out.read_text())

    def test_repository_rust_bindings_compile_and_resolve_sdk_constants(self):
        with tempfile.TemporaryDirectory() as tmp:
            generated = Path(tmp) / "services.rs"
            generated.write_text(a20idl.generate_rust(IDL), encoding="utf-8")
            self.assertIn("A20_SVC_ENDPOINT_SLOT: u64 = 104u64",
                          generated.read_text())
            self.assertIn("pub r#type: u16", generated.read_text())
            built = subprocess.run([RUSTC, "--edition=2021", "--crate-type", "lib",
                                    "--emit=obj", str(generated), "-o",
                                    str(Path(tmp) / "services.o")],
                                   text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)


if __name__ == "__main__":
    unittest.main()
