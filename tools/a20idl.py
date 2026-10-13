#!/usr/bin/env python3
"""Generate a small, deterministic C constant header from A20 service IDL.

The initial IDL deliberately covers constants only. Message layout and
typed binding generation are the next extension; keeping the first grammar
small makes protocol versioning and generated-source checks usable now.

For every `message` the header now also carries the wire contract that used
to be implicit in the C struct: the packed little-endian size, the byte offset
of each field, and encode/decode helpers that read and write those bytes
explicitly.

Two things this header deliberately keeps apart, because they are not the same
claim:

  * The codec is the wire format.  It is byte-wise and explicit, so it emits
    interface version 1's little-endian bytes on any host.
  * The generated struct is host C layout.  Its sizeof and offsetof say which
    byte each field sits at -- they say nothing at all about byte order.  A
    struct holding fixed-width integers is host-endian by construction, so a
    big-endian host lays a struct out with exactly the right offsets and
    exactly the wrong bytes.

Where a message's field widths happen to make the host struct packed, the
header says so with _Static_asserts, which is what keeps the existing v1
conservatives (services that hand struct memory straight to a channel) sending
the same bytes as before.  Where they do not, no such assert is emitted and the
codec is the only correct path.  And because layout is not byte order, the
header additionally refuses to build on a big-endian target rather than let one
come up healthy and talk v1 in the wrong direction.

Every name the generator turns into a C identifier -- message names and field
names -- is checked for collisions before any of it is written out, including
names that differ only in case, because the offset macros are uppercased and a
collision there is silent rather than a compile error.
"""
import argparse
import ast
import re
from pathlib import Path


CONST = re.compile(r"^\s*const\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+);\s*$")
VERSION = re.compile(r"^\s*interface\s+[A-Za-z_][A-Za-z0-9_]*\s+version\s+(\d+)\s*\{")
MESSAGE = re.compile(r"^\s*message\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{\s*$")
FIELD = re.compile(r"^\s*(u8|u16|u32|u64|i32|i64)\s+([A-Za-z_][A-Za-z0-9_]*);\s*$")
TYPE = {"u8": "uint8_t", "u16": "uint16_t", "u32": "uint32_t",
        "u64": "uint64_t", "i32": "int32_t", "i64": "int64_t"}
WIDTH = {"u8": 1, "u16": 2, "u32": 4, "u64": 8,
         "i32": 4, "i64": 8}

# Little-endian scalar accessors.  Deliberately shift-and-or rather than a
# memcpy of the host object: the point is that the byte order is written down
# instead of inherited from the target.  The signed forms avoid the
# implementation-defined out-of-range unsigned conversions that plain
# (uint32_t)negative_int32 would rely on.
PRIMITIVES = """\
static inline uint16_t a20_idl_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t a20_idl_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t a20_idl_get_u64(const uint8_t *p)
{
    return (uint64_t)a20_idl_get_u32(p) |
           ((uint64_t)a20_idl_get_u32(p + 4) << 32);
}

static inline void a20_idl_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static inline void a20_idl_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static inline void a20_idl_put_u64(uint8_t *p, uint64_t v)
{
    a20_idl_put_u32(p, (uint32_t)(v & 0xffffffffu));
    a20_idl_put_u32(p + 4, (uint32_t)((v >> 32) & 0xffffffffu));
}

static inline void a20_idl_put_i32(uint8_t *p, int32_t v)
{
    /* |v| without overflowing on INT32_MIN, then two's complement in
     * unsigned arithmetic.  Writing -(v) would be undefined there. */
    uint32_t magnitude = (v < 0) ? (uint32_t)(-(v + 1)) + 1u : (uint32_t)v;
    a20_idl_put_u32(p, (v < 0) ? (0u - magnitude) : magnitude);
}

static inline void a20_idl_put_i64(uint8_t *p, int64_t v)
{
    uint64_t magnitude = (v < 0) ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    a20_idl_put_u64(p, (v < 0) ? (0u - magnitude) : magnitude);
}

static inline int32_t a20_idl_get_i32(const uint8_t *p)
{
    uint32_t u = a20_idl_get_u32(p);
    return (u <= (uint32_t)INT32_MAX)
               ? (int32_t)u
               : (int32_t)(u - (uint32_t)INT32_MIN) + INT32_MIN;
}

static inline int64_t a20_idl_get_i64(const uint8_t *p)
{
    uint64_t u = a20_idl_get_u64(p);
    return (u <= (uint64_t)INT64_MAX)
               ? (int64_t)u
               : (int64_t)(u - (uint64_t)INT64_MIN) + INT64_MIN;
}"""

# Interface version 1's byte order is a property of the wire, not of the
# target.  It is checked once, here, and only after the layout question above
# has been settled -- a sizeof/offsetof assert cannot answer it.
ENDIAN_NOTE = """\
/*
 * Target byte-order restriction for interface version 1.
 *
 * Version 1 defines its integers little-endian, and the codec above writes
 * those bytes explicitly, so the codec would be correct on a big-endian host
 * as well.  What would not be correct is everything else that touches a
 * message: a struct's memory is host-endian, and services routinely hand a
 * struct's bytes straight to a channel.  No v1 peer is big-endian, so a
 * big-endian target is refused at compile time instead of being allowed to
 * come up looking healthy and then send the wrong bytes.
 *
 * The check is fail-closed.  A toolchain that does not say what its byte
 * order is gets an error rather than an assumption, which matches the rest of
 * this repository's gates: a check that cannot run must say so, not pass.
 */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#  if (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#    error "a20idl: interface version 1 is little-endian on the wire, but this target is not; no v1 peer can talk to it"
#  endif
#else
#  error "a20idl: this toolchain does not define __BYTE_ORDER__, so the interface version 1 little-endian wire format cannot be verified"
#endif

/* Defined only because the check above passed.  A program that wants to be
 * explicit about relying on host-endian == wire-endian can test this rather
 * than trusting a layout assertion. */
#define A20_SERVICES_IDL_TARGET_IS_LITTLE_ENDIAN 1"""

WIRE_NOTE = """\
/*
 * Wire format, fixed for interface version 1 and unchanged by this header:
 * every field is a fixed-width integer encoded little-endian (unsigned, or
 * two's complement for the signed types), fields are packed in declaration
 * order with no padding, and a message occupies exactly the sum of its field
 * widths.  A20_IDL_*_WIRE_SIZE and A20_IDL_*_OFF_* are those numbers, and
 * a20_idl_*_encode/decode write and read exactly those bytes.
 *
 * The generated struct next to them is a different thing and is not the wire
 * format.  It is host C layout: sizeof() and offsetof() say where each field
 * starts and how big the object is, and they say nothing about byte order.  A
 * big-endian host gives this struct the same sizes and the same offsets and
 * the opposite bytes.  So:
 *
 *   - where a message's field widths make the host struct packed, the _Static_asserts
 *     below prove the struct occupies the wire bytes exactly, and code that
 *     passes struct memory to a channel is sending the v1 bytes.  That is a
 *     statement about offsets, not about byte order -- byte order is the
 *     separate check further down.
 *   - where they do not, no such assert is emitted and the struct is simply
 *     not the wire layout.  Build the bytes with the codec, or into a buffer
 *     sized A20_IDL_*_WIRE_SIZE.
 *
 * A field called `size` is ordinary data here.  What ENVELOPE.size *counts* --
 * the whole framed message, envelope included -- is protocol semantics rather
 * than layout, so it stays at the call sites that frame and validate it
 * (user/svc/svcman.c builds such a frame, user/svc/echod.c accepts or ignores
 * it).
 */"""


def wire_plan(fields):
    """Return (offset, ctype, width, name, idl_type) per field, packed in order."""
    plan = []
    offset = 0
    for idl_type, name in fields:
        width = WIDTH[idl_type]
        plan.append((offset, TYPE[idl_type], width, name, idl_type))
        offset += width
    return plan, offset


def struct_is_packed(plan, total):
    """Would a plain struct of these fields occupy exactly the packed layout?

    A fixed-width integer member is aligned to its own width, so the struct
    has no holes only when every prefix of the field list is already a
    multiple of the next field's width, and the total is a multiple of the
    widest field.  True for every message in interface version 1, which is why
    they can be asserted; false for e.g. {u64,u8,u32}, where the u32 would sit
    at offset 12 rather than 9.

    This decides whether a *layout* assertion is emitted.  It is not a claim
    about byte order, and a wrong answer here can only ever cost an assertion,
    never put wrong bytes on the wire: the codec does not consult it.
    """
    running = 0
    for _, _, width, _, _ in plan:
        running += width
        if running % width:
            return False
    widest = max((width for _, _, width, _, _ in plan), default=1)
    return total % widest == 0


def check_fields(source, message):
    """Reject field names that would collide once they are generated.

    Two identical names are a C error -- duplicate struct member -- which is
    loud but late: the header is written, and only compiling it finds out.
    Two names differing only in case are worse, because nothing is loud.
    `uint16_t a; uint16_t A;` is a legal struct, but the generator emits
    A20_IDL_M_OFF_A twice, the second definition wins, and both fields then
    encode and decode through that one offset.  For a packed message the
    layout assert trips on the first field; for one that is not packed there is
    no assert, the header compiles clean, and the two fields silently
    overwrite each other on the wire.
    """
    seen = {}
    for _, field in message[1]:
        key = field.lower()
        if key in seen:
            clash = "" if field == seen[key] else \
                f" (same as {seen[key]} ignoring case)"
            raise SystemExit(
                f"{source}: duplicate field: {field} in message {message[0]}"
                f"{clash}")
        seen[key] = field


def emit_message(lines, name, fields):
    lower = name.lower()
    ctype = f"a20_idl_{lower}_t"
    prefix = f"A20_IDL_{name}"
    plan, total = wire_plan(fields)
    packed = struct_is_packed(plan, total)

    lines.extend(["", "typedef struct {"])
    lines.extend(f"    {c} {n};" for _, c, _, n, _ in plan)
    lines.append(f"}} {ctype};")

    lines.extend([
        "",
        f"#define {prefix}_WIRE_SIZE {total}u",
    ])
    for offset, _, _, field, _ in plan:
        lines.append(f"#define {prefix}_OFF_{field.upper()} {offset}u")

    if packed:
        lines.extend([
            "",
            f"/* {ctype}: the host struct is the packed wire layout for this",
            f" * message, so these asserts make handing struct memory to a",
            f" * channel byte-equivalent to using the codec.  They pin size and",
            f" * field offsets; they do NOT pin byte order, which is checked",
            f" * once at the top of this header. */",
            "",
            f'_Static_assert(sizeof({ctype}) == {prefix}_WIRE_SIZE,',
            f'               "{ctype}: struct size must equal the packed wire size");',
        ])
        for _, _, _, field, _ in plan:
            lines.extend([
                f'_Static_assert(offsetof({ctype}, {field}) == '
                f'{prefix}_OFF_{field.upper()},',
                f'               "{ctype}.{field}: must start at its wire offset");',
            ])
    else:
        lines.extend([
            "",
            f"/* {ctype}: these field widths do not give a packed host struct, so",
            f" * sizeof() and offsetof() would NOT match {prefix}_WIRE_SIZE and",
            f" * no layout assert is emitted for it.  This struct is host layout;",
            f" * build wire bytes with a20_idl_{lower}_encode into a",
            f" * {prefix}_WIRE_SIZE-byte buffer. */",
        ])

    lines.extend([
        "",
        f"static inline void a20_idl_{lower}_encode(uint8_t *buf, const {ctype} *src)",
        "{",
    ])
    for _, _, width, field, idl_type in plan:
        at = f"{prefix}_OFF_{field.upper()}"
        if width == 1:
            lines.append(f"    buf[{at}] = src->{field};")
        else:
            lines.append(f"    a20_idl_put_{idl_type}(buf + {at}, src->{field});")
    lines.extend(["}", ""])

    lines.extend([
        f"static inline void a20_idl_{lower}_decode({ctype} *dst, const uint8_t *buf)",
        "{",
    ])
    for _, _, width, field, idl_type in plan:
        at = f"{prefix}_OFF_{field.upper()}"
        if width == 1:
            lines.append(f"    dst->{field} = buf[{at}];")
        else:
            lines.append(f"    dst->{field} = a20_idl_get_{idl_type}(buf + {at});")
    lines.append("}")


def parse_idl(source: Path):
    """Parse and validate once for all language backends."""
    version = None
    constants = []
    messages = []
    current_message = None
    for line in source.read_text(encoding="ascii").splitlines():
        if line.lstrip().startswith("//") or not line.strip():
            continue
        if version is None:
            match = VERSION.match(line)
            if match:
                version = int(match.group(1))
                continue
        if current_message is not None:
            if line.strip() == "}":
                if not current_message[1]:
                    raise SystemExit(
                        f"{source}: empty message: {current_message[0]}")
                check_fields(source, current_message)
                messages.append(current_message)
                current_message = None
                continue
            match = FIELD.match(line)
            if not match:
                raise SystemExit(f"{source}: malformed message field: {line}")
            current_message[1].append((match.group(1), match.group(2)))
            continue
        match = MESSAGE.match(line)
        if match:
            current_message = [match.group(1), []]
            continue
        match = CONST.match(line)
        if match:
            constants.append((match.group(1), match.group(2).strip()))
    if version is None:
        raise SystemExit(f"{source}: missing interface version")
    if current_message is not None:
        # An unterminated block used to be dropped silently, which would lose
        # a message from the header without failing anything.
        raise SystemExit(f"{source}: unterminated message: {current_message[0]}")
    if len({name for name, _ in constants}) != len(constants):
        raise SystemExit(f"{source}: duplicate constant")
    seen_messages = set()
    for name, _ in messages:
        # A repeated message name collides on the typedef, on every
        # A20_IDL_<NAME>_* macro, and on the encode/decode pair -- and the
        # collision only surfaces when something downstream compiles the
        # header, which is exactly the "silent until too late" shape the
        # per-message checks above exist to prevent.
        if name in seen_messages:
            raise SystemExit(f"{source}: duplicate message: {name}")
        seen_messages.add(name)
    return version, constants, messages


def generate(source: Path) -> str:
    version, constants, messages = parse_idl(source)
    lines = [
        "/* Generated by tools/a20idl.py; do not edit by hand. */",
        "#ifndef _A20_SERVICES_IDL_H",
        "#define _A20_SERVICES_IDL_H",
        "",
        '#include "a20_types.h"',
        "#include <stddef.h>",
        "",
        f"#define A20_SERVICES_IDL_VERSION {version}u",
    ]
    lines.extend(f"#define {name} ({value})" for name, value in constants)
    lines.extend(["", WIRE_NOTE, "", ENDIAN_NOTE, ""])
    lines.extend(PRIMITIVES.splitlines())
    for name, fields in messages:
        emit_message(lines, name, fields)
    lines.extend(["", "#endif", ""])
    return "\n".join(lines)


RUST_TYPE = {"u8": "u8", "u16": "u16", "u32": "u32", "u64": "u64",
             "i32": "i32", "i64": "i64"}
RUST_KEYWORDS = {"as", "break", "const", "continue", "crate", "else", "enum",
                 "extern", "false", "fn", "for", "if", "impl", "in", "let",
                 "loop", "match", "mod", "move", "mut", "pub", "ref", "return",
                 "self", "Self", "static", "struct", "super", "trait", "true",
                 "type", "unsafe", "use", "where", "while", "async", "await", "dyn",
                 "abstract", "become", "box", "do", "final", "macro", "override",
                 "priv", "typeof", "unsized", "virtual", "yield", "try", "gen"}
RUST_UNESCAPABLE = {"self", "Self", "super", "crate", "_"}


def rust_ident(name: str) -> str:
    if name in RUST_UNESCAPABLE:
        raise SystemExit(f"a20idl Rust backend: identifier cannot be represented: {name}")
    return f"r#{name}" if name in RUST_KEYWORDS else name


def _rust_const_expr(value: str, externals: dict[str, int]) -> str:
    """Translate the deliberately small integer-constant grammar to Rust.

    IDL constants currently use integer literals, arithmetic, and the one
    Native SDK constant A20_NATIVE_FD_HANDLE_BASE.  Parse rather than paste
    C text so an arbitrary C expression can never become generated Rust.
    """
    value = re.sub(r"(?<=[0-9a-fA-F])[uUlL]+\b", "", value)
    tree = ast.parse(value, mode="eval")

    def evaluate(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.Name) and node.id in externals:
            return externals[node.id]
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Sub, ast.Mult, ast.LShift, ast.RShift)):
            left, right = evaluate(node.left), evaluate(node.right)
            if isinstance(node.op, ast.Add): return left + right
            if isinstance(node.op, ast.Sub): return left - right
            if isinstance(node.op, ast.Mult): return left * right
            if isinstance(node.op, ast.LShift): return left << right
            return left >> right
        raise ValueError(f"unsupported IDL integer expression: {value}")

    result = evaluate(tree.body)
    if result < 0 or result > 0xFFFF_FFFF_FFFF_FFFF:
        raise ValueError(f"IDL constant is outside u64 range: {value}")
    return f"{result}u64"


def generate_rust(source: Path) -> str:
    """Emit host-layout Rust structs and explicit v1 little-endian codecs."""
    version, constants, messages = parse_idl(source)

    sdk_types = Path(__file__).resolve().parents[1] / "user/liba20rt/a20_types.h"
    sdk_text = sdk_types.read_text(encoding="utf-8")
    external_match = re.search(r"^#define\s+A20_NATIVE_FD_HANDLE_BASE\s+(\d+)[uUlL]*\s*$",
                               sdk_text, re.MULTILINE)
    if not external_match:
        raise SystemExit(f"{sdk_types}: cannot resolve A20_NATIVE_FD_HANDLE_BASE")
    externals = {"A20_NATIVE_FD_HANDLE_BASE": int(external_match.group(1))}
    lines = [
        "// Generated by tools/a20idl.py; do not edit by hand.",
        "// Wire values are packed, fixed-width, and little-endian. `repr(C)`",
        "// structs describe host layout only; the byte codecs define the wire.",
        "pub const A20_SERVICES_IDL_VERSION: u16 = %du16;" % version,
    ]
    for name, value in constants:
        try:
            expr = _rust_const_expr(value, externals)
        except (SyntaxError, ValueError) as exc:
            raise SystemExit(f"{source}: Rust constant {name}: {exc}") from exc
        lines.append(f"pub const {rust_ident(name)}: u64 = {expr};")
    for name, fields in messages:
        lower = name.lower()
        rust_name = rust_ident(name)
        plan, total = wire_plan(fields)
        lines.append("")
        lines.append("#[allow(non_camel_case_types)]")
        lines.append("#[repr(C)]")
        lines.append(f"#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]\npub struct {rust_name} {{")
        lines.extend(f"    pub {rust_ident(field)}: {RUST_TYPE[idl_type]}," for _, _, _, field, idl_type in plan)
        lines.append("}")
        lines.append(f"pub const {name}_WIRE_SIZE: usize = {total};")
        for offset, _, _, field, _ in plan:
            lines.append(f"pub const {name}_OFF_{field.upper()}: usize = {offset};")
        lines.append(f"pub fn {lower}_encode(src: &{rust_name}) -> [u8; {name}_WIRE_SIZE] {{")
        lines.append(f"    let mut out = [0u8; {name}_WIRE_SIZE];")
        for offset, _, width, field, idl_type in plan:
            if width == 1:
                lines.append(f"    out[{offset}] = src.{rust_ident(field)};")
            else:
                lines.append(f"    out[{offset}..{offset + width}].copy_from_slice(&src.{rust_ident(field)}.to_le_bytes());")
        lines.extend(["    out", "}"])
        lines.append(f"pub fn {lower}_decode(bytes: &[u8]) -> Option<{rust_name}> {{")
        lines.append(f"    if bytes.len() != {name}_WIRE_SIZE {{ return None; }}")
        lines.append("    Some(%s {" % rust_name)
        for offset, _, width, field, idl_type in plan:
            if width == 1:
                lines.append(f"        {rust_ident(field)}: bytes[{offset}],")
            else:
                lines.append(f"        {rust_ident(field)}: {RUST_TYPE[idl_type]}::from_le_bytes(bytes[{offset}..{offset + width}].try_into().ok()?),")
        lines.extend(["    })", "}"])
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--rust-output", type=Path,
                        help="also emit Rust structs and v1 wire codecs")
    args = parser.parse_args()
    args.output.write_text(generate(args.source), encoding="ascii")
    if args.rust_output:
        args.rust_output.write_text(generate_rust(args.source), encoding="utf-8")


if __name__ == "__main__":
    main()
