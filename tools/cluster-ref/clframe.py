#!/usr/bin/env python3
"""A20OS cluster wire-format reference codec (host side).

This is the golden reference for ``kernel/cluster/frame.c`` (WA1) and the MCU
leaf decoder (WC1).  Authority for every constant below, quoted by section:

* ``docs/cluster/02-wire-protocol.md`` -- frame layout, message types, flags,
  CALL lifecycle, fragmentation, HELLO negotiation, reliability, counters.
* ``docs/cluster/01-abi.md``           -- node hash, reserved node ids, caps,
  errno names/numbers, limits.
* ``docs/cluster/04-transports.md``    -- SLIP variant (section 4), MTUs.
* ``docs/cluster/05-userspace.md``     -- TLV payload convention (section 3).
* ``docs/cluster/03-kernel-impl.md``   -- section 2 style constraints for the
  C encoder this file is the reference for: pure functions, no locks, no
  allocation (the caller supplies the buffer), explicit per-field read/write,
  never a struct memory image.

Style constraint mirrored from 03-02 on purpose: every field goes through
``struct.pack``/``int.from_bytes`` one at a time.  No ``ctypes``, no memory
views, no ``int.from_bytes(buffer)`` on a whole frame, no reliance on native
alignment.  A field-by-field encoder is the only thing that stays correct when
the kernel, the userspace clusterd and an STM32 leaf disagree about struct
padding.

Assumptions (every one of them is listed with its section reference in
README.md and in the acceptance report) are marked ``ASSUMPTION A-nn``.
"""

from __future__ import annotations

import struct
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

# ---------------------------------------------------------------------------
# 02-1  frame header
# ---------------------------------------------------------------------------

MAGIC = 0x4C43
"""02-1: "magic u16  fixed 0x4C43 ('CL')".  Stored little-endian, so the first
byte on the wire is 0x43 ('C') and the second 0x4C ('L')."""

WIRE_VER = 0
"""02-1: "ver u8  protocol version, v0 = 0"."""

HEADER_LEN = 32
"""02-1: "fixed 32-byte header + payload + optional CRC16 trailer"."""

#: 02-1 offsets, exported so the C side and the vector manifest quote the same
#: table instead of re-typing magic numbers.
FIELD_OFFSETS = {
    "magic": 0,
    "ver": 2,
    "type": 3,
    "flags": 4,
    "frag": 6,
    "txid": 8,
    "src_hash": 12,
    "dst_hash": 16,
    "dst_slot": 20,
    "seq": 24,
    "payload_len": 28,
    "ttl": 30,
    "csum_kind": 31,
    "payload": 32,
}

# 02-2  message types
TYPE_HELLO = 1
TYPE_HELLO_ACK = 2
TYPE_PING = 3
TYPE_PONG = 4
TYPE_SEND = 5
TYPE_CALL = 6
TYPE_CALL_REPLY = 7
TYPE_CLOSE = 8
TYPE_ACK = 9
TYPE_NACK = 10
TYPE_ERROR = 11

TYPE_NAMES = {
    TYPE_HELLO: "HELLO",
    TYPE_HELLO_ACK: "HELLO_ACK",
    TYPE_PING: "PING",
    TYPE_PONG: "PONG",
    TYPE_SEND: "SEND",
    TYPE_CALL: "CALL",
    TYPE_CALL_REPLY: "CALL_REPLY",
    TYPE_CLOSE: "CLOSE",
    TYPE_ACK: "ACK",
    TYPE_NACK: "NACK",
    TYPE_ERROR: "ERROR",
}
TYPE_BY_NAME = {v: k for k, v in TYPE_NAMES.items()}

#: 02-2 payload column.  ``None`` means "application defined, opaque bytes".
FIXED_PAYLOAD_LEN = {
    TYPE_HELLO: 32,  # 02-6
    TYPE_HELLO_ACK: 32,  # 02-2 "same as HELLO"
    TYPE_PING: 8,  # 02-2 8B send timestamp
    TYPE_PONG: 8,  # 02-2 echoes the PING payload
    TYPE_SEND: None,  # 02-2 application payload
    TYPE_CALL: None,  # 02-2 application payload
    TYPE_CALL_REPLY: None,  # 02-2 application payload
    TYPE_CLOSE: 0,  # 02-2 "empty"
    TYPE_ACK: 4,  # 02-2 4B cumulative_seq
    TYPE_NACK: 4,  # 02-2 4B missing_seq
    TYPE_ERROR: 8,  # 02-2 4B errno + 4B orig_txid
}

#: Types whose payload is a variable-length application message, i.e. the only
#: ones a cumulative ACK may be piggybacked on.  ASSUMPTION A-06 (02-7 says
#: "any reverse frame", which would break the fixed-length payload column of
#: 02-2 -- see README).
PIGGYBACK_HOST_TYPES = frozenset({TYPE_SEND, TYPE_CALL, TYPE_CALL_REPLY})

#: 02-7 "piggybacked on the first 4 bytes of the payload of any reverse frame
#: (the receiver strips it first when flags bit0 is set)".  ASSUMPTION A-05:
#: the prefix is present on non-fragmented RELIABLE frames only, so that
#: 02-5 "UDP fragment payload = 1438" stays exact.
ACK_PREFIX_LEN = 4

# 02-3  flags
FLAG_RELIABLE = 1 << 0
FLAG_FRAGMENTED = 1 << 1
FLAG_BROADCAST = 1 << 2
FLAG_COMPRESSED = 1 << 3

FLAG_NAMES = {
    FLAG_RELIABLE: "RELIABLE",
    FLAG_FRAGMENTED: "FRAGMENTED",
    FLAG_BROADCAST: "BROADCAST",
    FLAG_COMPRESSED: "COMPRESSED",
}
FLAG_BIT = {v: k for k, v in FLAG_NAMES.items()}

#: Flags documented by 02-3.  Bits 4..15 are reserved: 02-1 "reserved
#: bits/fields must be sent as zero; the receiver must NOT reject a non-zero
#: reserved bit (forward compatibility), it must ignore it".
KNOWN_FLAGS_MASK = FLAG_RELIABLE | FLAG_FRAGMENTED | FLAG_BROADCAST | FLAG_COMPRESSED

#: 02-5 frag field: "low 12 bits = fragment index (from 0), bit15 = last-fragment
#: flag, bits 12-14 reserved = 0".
FRAG_SEQ_MASK = 0x0FFF
FRAG_LAST = 0x8000
FRAG_RESERVED_MASK = 0x7000

#: 02-1 "initial value 8".
DEFAULT_TTL = 8

#: 02-1 csum_kind.
CSUM_NONE = 0
CSUM_CCITT = 1

# ---------------------------------------------------------------------------
# 01-abi  identity, capabilities, errno, limits
# ---------------------------------------------------------------------------

NODE_ID_LEN = 16

NODE_ID_LOCAL = bytes(16)
"""01-abi node identity: all-zero == A20_NODE_ID_LOCAL, "addressing (LOCAL,
slot) must take the local fast path, serializing it is forbidden"."""

NODE_ID_BROADCAST = b"\xff" * 16
"""01-abi: all-0xff == A20_NODE_ID_BROADCAST, SERVER tier + SEND only; an MCU
tier node must drop it."""

FNV1A_OFFSET = 2166136261
FNV1A_PRIME = 16777619

CAP_RELAY = 1 << 0
CAP_RELIABLE = 1 << 1
CAP_LEAF = 1 << 2
CAP_ALL = CAP_RELAY | CAP_RELIABLE | CAP_LEAF
CAP_NAMES = {CAP_RELAY: "RELAY", CAP_RELIABLE: "RELIABLE", CAP_LEAF: "LEAF"}

PROFILE_MCU = 1
PROFILE_DEFAULT = 2
PROFILE_SERVER = 3
PROFILE_NAMES = {
    PROFILE_MCU: "MCU",
    PROFILE_DEFAULT: "DEFAULT",
    PROFILE_SERVER: "SERVER",
}

#: 01-abi "new errno" + the "existing errno reuse" table at the end of the same
#: document: 01-abi calls it A20_ERR_INVALID_ARGS but the tree implements
#: A20_ERR_INVALID_ARGUMENT (12); A20_ERR_RESOURCE_LIMIT -> A20_ERR_NO_SPACE
#: (13).  The vectors carry the implemented names, per 01-abi's arbitration
#: order ("code reality > subsystem spec").
ERRNO = {
    "A20_ERR_NO_ENTRY": 2,
    "A20_ERR_EXISTS": 10,
    "A20_ERR_INVALID_ARGUMENT": 12,
    "A20_ERR_NO_SPACE": 13,
    "A20_ERR_NOT_FOUND": 24,
    "A20_ERR_NODE_UNREACHABLE": 26,
    "A20_ERR_CLUSTER_TIMEOUT": 27,
    "A20_ERR_REMOTE_CLOSED": 28,
    "A20_ERR_CLUSTER_UNSUPPORTED": 29,
}
ERRNO_BY_VALUE = {v: k for k, v in ERRNO.items()}

#: 02-8 "the ERROR frame errno field uses only the cluster errnos defined in
#: 01-abi ... an unknown code is treated as A20_ERR_CLUSTER_UNSUPPORTED".
#: ASSUMPTION A-08 widens the set to the codes 01-abi itself names as cluster
#: connect/call outcomes, which includes A20_ERR_NOT_FOUND(24) -- 02-2 already
#: requires NOT_FOUND for a slot lookup miss, which contradicts a literal
#: reading of 02-8.  See README.
WIRE_ERRNO_SET = frozenset(
    {
        ERRNO["A20_ERR_NOT_FOUND"],
        ERRNO["A20_ERR_NODE_UNREACHABLE"],
        ERRNO["A20_ERR_CLUSTER_TIMEOUT"],
        ERRNO["A20_ERR_REMOTE_CLOSED"],
        ERRNO["A20_ERR_CLUSTER_UNSUPPORTED"],
    }
)

#: 01-abi limits table (mirrored by A20_LIMIT_CLX_* in
#: kernel/include/abi/native/resource.h).
LIMITS = {
    "routes": {PROFILE_MCU: 4, PROFILE_DEFAULT: 256, PROFILE_SERVER: 4096},
    "slots": {PROFILE_MCU: 4, PROFILE_DEFAULT: 64, PROFILE_SERVER: 256},
    "remote_eps": {PROFILE_MCU: 2, PROFILE_DEFAULT: 128, PROFILE_SERVER: 1024},
    "reasm_bytes": {PROFILE_MCU: 0, PROFILE_DEFAULT: 256 * 1024, PROFILE_SERVER: 4 * 1024 * 1024},
    "inflight_call": {PROFILE_MCU: 1, PROFILE_DEFAULT: 64, PROFILE_SERVER: 512},
    # 02-4 dedup window, 02-5 reassembly timeout.
    "dedup_window": {PROFILE_MCU: 256, PROFILE_DEFAULT: 256, PROFILE_SERVER: 256},
    "reasm_timeout_ms": {PROFILE_MCU: 0, PROFILE_DEFAULT: 5000, PROFILE_SERVER: 5000},
    "call_deadline_ms": {PROFILE_MCU: 30000, PROFILE_DEFAULT: 30000, PROFILE_SERVER: 30000},
}

# ---------------------------------------------------------------------------
# 02-10  counters (append only, never renamed)
# ---------------------------------------------------------------------------

COUNTERS = (
    "tx_frames",
    "rx_frames",
    "tx_drops",
    "rx_drops",
    "rx_malformed",
    "retransmits",
    "reasm_timeouts",
    "reasm_evicted",
    "dedup_drops",
    "hello_rejects",
)
assert len(COUNTERS) == 10

# ---------------------------------------------------------------------------
# 04-1/3/4/5  transports
# ---------------------------------------------------------------------------

TRANSPORT_LOOPBACK = 0
TRANSPORT_UDP = 1
TRANSPORT_UART = 2

TRANSPORT_NAME = {
    TRANSPORT_LOOPBACK: "loopback",
    TRANSPORT_UDP: "udp",
    TRANSPORT_UART: "uart",
}
TRANSPORT_ID = {v: k for k, v in TRANSPORT_NAME.items()}

#: 04-5 comparison table (and 04-3 / 04-4).  "mtu" is the single-frame limit
#: including the 32-byte header and the CRC, per 04-1.
TRANSPORT_MTU = {
    TRANSPORT_LOOPBACK: 65536,
    TRANSPORT_UDP: 1472,
    TRANSPORT_UART: 256,
}

#: 02-1 "payload_len: payload byte count, <= transport MTU - header - CRC".
#: ASSUMPTION A-09: the 2 CRC bytes are subtracted unconditionally, exactly as
#: the sentence reads, so the bound holds even when csum_kind == 0.
def max_payload_len(mtu: int) -> int:
    return mtu - HEADER_LEN - 2


MAX_PAYLOAD = {TRANSPORT_NAME[tid]: max_payload_len(mtu) for tid, mtu in TRANSPORT_MTU.items()}

#: 02-5 "UDP tier MTU 1472 -> fragment payload 1438".
UDP_FRAGMENT_PAYLOAD = MAX_PAYLOAD["udp"]
#: 04-4 MCU messages may not be fragmented: 256 - 32 - 2.
UART_FRAGMENT_PAYLOAD = MAX_PAYLOAD["uart"]

#: 04-4 SLIP framing.
SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD
#: 04-4 "single receive buffer 512B ... single send buffer 320B (max 2x escape
#: expansion scenario at the frame limit plus margin)".
UART_RX_BUF = 512
UART_TX_BUF = 320
#: 04-4 "partial frame timeout: >50ms of silence between bytes drops the half
#: frame".
SLIP_INTERBYTE_TIMEOUT_MS = 50

# ---------------------------------------------------------------------------
# 05-3  TLV payload convention
# ---------------------------------------------------------------------------

TLV_TAG_CLUSTERD = (0x0001, 0x00FF)
TLV_TAG_JOBD = (0x0100, 0x01FF)
TLV_TAG_APP = (0x8000, 0xFFFF)
TLV_ALIGN = 4
"""05-3 "[tag:u16][len:u16][value:len bytes] ... aligned to 4 bytes".

ASSUMPTION A-07: the alignment is applied per element, i.e. every element
starts on a 4-byte boundary and its trailing pad is ``(-len) % 4`` bytes that
are NOT counted by ``len``.  The alternative reading (pad only the whole TLV
sequence) produces different bytes for the same message; see README.
"""


# ---------------------------------------------------------------------------
# primitives
# ---------------------------------------------------------------------------


def fnv1a32(node_id_bytes: bytes) -> int:
    """01-abi "node hash": FNV-1a 32 over the 16 node-id bytes, offset basis
    2166136261, prime 16777619, ``h = (h ^ b) * 16777619`` byte by byte.

    Anchors fixed by W0 and asserted in selftest.py:
    all-zero -> 0x69691905, all-0xff -> 0x360779f5.
    """
    if len(node_id_bytes) != NODE_ID_LEN:
        raise ValueError(f"node id must be {NODE_ID_LEN} bytes, got {len(node_id_bytes)}")
    h = FNV1A_OFFSET
    for b in node_id_bytes:
        h = (h ^ b) & 0xFFFFFFFF
        h = (h * FNV1A_PRIME) & 0xFFFFFFFF
    return h


def crc16_ccitt(data: bytes, init: int = 0xFFFF, poly: int = 0x1021) -> int:
    """02-1 ``csum_kind=1``: CRC16-CCITT, poly 0x1021, init 0xFFFF, no
    reflection, no final xor.  This is the CRC-16/CCITT-FALSE parameterisation
    (check value of "123456789" is 0x29B1 -- asserted in selftest.py) so the
    MCU side can name a catalogue entry instead of a bit loop.
    """
    crc = init & 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ poly) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def tlv_pad_len(value_len: int) -> int:
    """05-3 trailing pad of one TLV element (ASSUMPTION A-07)."""
    return (-value_len) % TLV_ALIGN


def tlv_element_len(value_len: int) -> int:
    """Wire size of one TLV element including its pad."""
    return 4 + value_len + tlv_pad_len(value_len)


def tlv_encode(items: Sequence[Tuple[int, bytes]]) -> bytes:
    """05-3 encoder.  ``items`` is an ordered list of ``(tag, value)``; strings
    are passed without a NUL by the caller."""
    out = bytearray()
    for tag, value in items:
        if not 0 <= tag <= 0xFFFF:
            raise ValueError(f"tag {tag:#x} out of range")
        if len(value) > 0xFFFF:
            raise ValueError(f"value of {len(value)} bytes exceeds u16 len")
        out += struct.pack("<HH", tag, len(value))
        out += bytes(value)
        out += b"\x00" * tlv_pad_len(len(value))
    return bytes(out)


class TlvError(ValueError):
    """05-3 / 06-1 P-单元-5: truncated header or truncated value."""


def tlv_decode(buf: bytes) -> List[Tuple[int, bytes]]:
    """05-3 decoder.

    06-1 P-单元-5 requires "truncation, skipping unknown tags" to be covered, so
    an unknown tag (outside the three ranges of 05-3) is *skipped*, not an
    error.  A short read is an error.
    """
    items: List[Tuple[int, bytes]] = []
    off = 0
    while off + 4 <= len(buf):
        tag, length = struct.unpack_from("<HH", buf, off)
        off += 4
        if off + length > len(buf):
            raise TlvError(
                f"tlv 0x{tag:04x} claims {length} bytes, {len(buf) - off} available"
            )
        items.append((tag, bytes(buf[off : off + length])))
        off += length + tlv_pad_len(length)
    if off != len(buf):
        raise TlvError(f"{len(buf) - off} trailing bytes after last tlv element")
    return items


def tlv_is_known_tag(tag: int) -> bool:
    """05-3 tag space.  Anything else is an unknown tag to be skipped."""
    return (
        TLV_TAG_CLUSTERD[0] <= tag <= TLV_TAG_CLUSTERD[1]
        or TLV_TAG_JOBD[0] <= tag <= TLV_TAG_JOBD[1]
        or TLV_TAG_APP[0] <= tag <= TLV_TAG_APP[1]
    )


# ---------------------------------------------------------------------------
# 04-4  SLIP variant
# ---------------------------------------------------------------------------


def slip_encode(frame: bytes) -> bytes:
    """04-4: one END at the head and one at the tail, 0xDB escaped
    (DB DC -> C0, DB DD -> DB).  The head END doubles as the resync point.
    """
    out = bytearray((SLIP_END,))
    for b in frame:
        if b == SLIP_END:
            out += bytes((SLIP_ESC, SLIP_ESC_END))
        elif b == SLIP_ESC:
            out += bytes((SLIP_ESC, SLIP_ESC_ESC))
        else:
            out.append(b)
    out.append(SLIP_END)
    return bytes(out)


class SlipError(ValueError):
    """Unterminated escape, or a frame that does not fit the receive buffer."""


def slip_decode(stream: bytes, max_frame: int = UART_RX_BUF) -> Tuple[List[bytes], List[str]]:
    """04-4 byte-stream decoder.

    Returns ``(frames, notes)``.  ``frames`` holds the CL frames recovered from
    the stream; ``notes`` is a list of 04-4-level diagnostics:

    ``empty``       two consecutive ENDs -- the head-END resync artifact, no
                    frame and no counter (04-4 "the head END doubles as the
                    resync point"; ASSUMPTION A-10)
    ``unterminated_escape``  trailing lone ESC at the end of the stream
    ``truncated``    the frame has < 32 bytes (a CL-frame level concern, but the
                    SLIP layer is the only place a half frame is visible)
    ``oversize``     the decoded frame does not fit ``max_frame`` bytes: the
                    half frame is dropped and the receiver resyncs on the next
                    END (04-4 static 512B receive buffer)
    ``bad_magic``    the unescaped bytes do not start with 0x43 0x4C
    """
    frames: List[bytes] = []
    notes: List[str] = []
    cur = bytearray()
    started = False
    i = 0
    n = len(stream)

    def finish() -> None:
        nonlocal cur, started
        if not started:
            return
        if len(cur) == 0:
            notes.append("empty")
            cur = bytearray()
            started = False
            return
        if len(cur) > max_frame:
            notes.append("oversize")
        elif len(cur) < HEADER_LEN:
            notes.append("truncated")
        elif cur[0] != (MAGIC & 0xFF) or cur[1] != (MAGIC >> 8) & 0xFF:
            notes.append("bad_magic")
        else:
            frames.append(bytes(cur))
        cur = bytearray()
        started = False

    while i < n:
        b = stream[i]
        i += 1
        if b == SLIP_END:
            finish()
            started = True  # a fresh head END also arms the delimiter
            continue
        started = True
        if b == SLIP_ESC:
            if i >= n:
                notes.append("unterminated_escape")
                cur = bytearray()
                started = False
                break
            nxt = stream[i]
            i += 1
            if nxt == SLIP_ESC_END:
                cur.append(SLIP_END)
            elif nxt == SLIP_ESC_ESC:
                cur.append(SLIP_ESC)
            else:
                notes.append("bad_escape")
        else:
            cur.append(b)
    if started and cur:
        # 04-4 partial-frame timeout: >50ms of silence drops the half frame.
        notes.append("partial_frame")
    return frames, notes


# ---------------------------------------------------------------------------
# 02-6  HELLO payload (fixed 32 bytes)
# ---------------------------------------------------------------------------

HELLO_PAYLOAD_LEN = 32

HELLO_OFFSETS = {
    "node_id": 0,
    "proto_min": 16,
    "proto_max": 17,
    "profile_tier": 18,
    "caps": 19,
    "short_addr": 20,
    "link_addr_len": 22,
    "reserved": 23,
    "nonce": 24,
}


def encode_hello_payload(
    node_id: bytes,
    proto_min: int,
    proto_max: int,
    profile_tier: int,
    caps: int,
    short_addr: int,
    link_addr_len: int,
    nonce: bytes,
    reserved: int = 0,
) -> bytes:
    """02-6 HELLO payload, 32 bytes fixed.

    ``short_addr`` is u16 little-endian (ASSUMPTION A-05b: 02-6 does not repeat
    02-1's little-endian rule for payload fields, and 05-3 says "integers
    little-endian" for the only other multi-byte payload convention).

    ``nonce`` is 8 opaque bytes.  02-6 only ever compares a nonce for equality
    ("a HELLO carrying our own nonce is judged a self-loop"), so it is never
    read as an integer and its byte order cannot disagree.
    """
    if len(node_id) != NODE_ID_LEN:
        raise ValueError("node_id must be 16 bytes")
    if len(nonce) != 8:
        raise ValueError("nonce must be 8 bytes")
    out = bytearray()
    out += bytes(node_id)
    out += struct.pack("<BBBB", proto_min, proto_max, profile_tier, caps)
    out += struct.pack("<H", short_addr)
    out += struct.pack("<BB", link_addr_len, reserved)
    out += bytes(nonce)
    assert len(out) == HELLO_PAYLOAD_LEN
    return bytes(out)


def decode_hello_payload(payload: bytes) -> Dict[str, object]:
    """02-6 HELLO payload reader, field by field."""
    if len(payload) != HELLO_PAYLOAD_LEN:
        raise ValueError(f"HELLO payload must be {HELLO_PAYLOAD_LEN} bytes")
    node_id = bytes(payload[0:16])
    proto_min, proto_max, profile_tier, caps = struct.unpack_from("<BBBB", payload, 16)
    short_addr, = struct.unpack_from("<H", payload, 20)
    link_addr_len, reserved = struct.unpack_from("<BB", payload, 22)
    nonce = bytes(payload[24:32])
    if profile_tier == PROFILE_MCU:
        # 04-4: 0xFFFF is the unassigned power-on value and only HELLO may use it.
        short_state = "unassigned" if short_addr == 0xFFFF else "assigned"
    elif short_addr == 0:
        # 02-6: "UART tier only ... everything else puts 0".
        short_state = "not_used_off_uart"
    else:
        short_state = f"unexpected_off_uart:0x{short_addr:04X}"
    return {
        "node_id": node_id.hex(),
        "node_id_hash": f"0x{fnv1a32(node_id):08X}",
        "proto_min": proto_min,
        "proto_max": proto_max,
        "profile_tier": profile_tier,
        "profile_name": PROFILE_NAMES.get(profile_tier, "UNKNOWN"),
        "caps": caps,
        "cap_names": [n for b, n in CAP_NAMES.items() if caps & b],
        "short_addr": short_addr,
        "short_addr_state": short_state,
        "link_addr_len": link_addr_len,
        "reserved": reserved,
        "nonce": nonce.hex(),
    }


# ---------------------------------------------------------------------------
# 02-1  frame encode / decode
# ---------------------------------------------------------------------------


class Frame:
    """One decoded CL frame.  Plain attribute bag on purpose: the C side has no
    equivalent of a rich object, and the JSON form is the vector contract."""

    __slots__ = (
        "magic",
        "ver",
        "type",
        "flags",
        "frag",
        "txid",
        "src_hash",
        "dst_hash",
        "dst_slot",
        "seq",
        "payload_len",
        "ttl",
        "csum_kind",
        "payload",
        "wire_len",
        "crc_present",
        "crc_value",
        "crc_ok",
        "ack_prefix",
        "app_payload",
        "app_offset",
    )

    def __init__(self, **kw):
        for slot in self.__slots__:
            setattr(self, slot, kw.get(slot))
        self.payload = bytes(self.payload or b"")
        self.app_payload = bytes(self.app_payload or b"")
        if self.flags is None:
            self.flags = 0
        if self.frag is None:
            self.frag = 0
        if self.type is None:
            self.type = 0

    # -- derived fields ----------------------------------------------------
    @property
    def type_name(self) -> str:
        return TYPE_NAMES.get(self.type, "UNKNOWN")

    @property
    def flags_set(self) -> Dict[str, bool]:
        return {name: bool(self.flags & bit) for name, bit in FLAG_BIT.items()}

    @property
    def frag_seq(self) -> int:
        return self.frag & FRAG_SEQ_MASK

    @property
    def frag_last(self) -> bool:
        return bool(self.frag & FRAG_LAST)

    @property
    def frag_reserved_nonzero(self) -> bool:
        return bool(self.frag & FRAG_RESERVED_MASK)

    @property
    def is_fragment(self) -> bool:
        return bool(self.flags & FLAG_FRAGMENTED)

    @property
    def is_reliable(self) -> bool:
        return bool(self.flags & FLAG_RELIABLE)

    @property
    def unknown_flag_bits(self) -> int:
        return self.flags & ~KNOWN_FLAGS_MASK

    def hello(self) -> Optional[Dict[str, object]]:
        if self.type in (TYPE_HELLO, TYPE_HELLO_ACK):
            return decode_hello_payload(self.payload)
        return None

    def expected_wire_len(self) -> int:
        return len(self.payload) + HEADER_LEN + (2 if self.csum_present else 0)

    def hex(self) -> str:
        return encode_frame(
            type=self.type,
            ver=self.ver,
            magic=self.magic,
            flags=self.flags,
            frag=self.frag,
            txid=self.txid,
            src_hash=self.src_hash,
            dst_hash=self.dst_hash,
            dst_slot=self.dst_slot,
            seq=self.seq,
            ttl=self.ttl,
            csum_kind=self.csum_kind,
            payload=self.payload,
        ).hex()

    def header_json(self) -> Dict[str, object]:
        return {
            "magic": f"0x{self.magic:04X}",
            "ver": self.ver,
            "type": self.type,
            "type_name": self.type_name,
            "flags": f"0x{self.flags:04X}",
            "flags_set": self.flags_set,
            "unknown_flag_bits": f"0x{self.unknown_flag_bits:04X}",
            "frag": f"0x{self.frag:04X}",
            "frag_seq": self.frag_seq,
            "frag_last": self.frag_last,
            "frag_reserved_nonzero": self.frag_reserved_nonzero,
            "txid": self.txid,
            "src_hash": f"0x{self.src_hash:08X}",
            "dst_hash": f"0x{self.dst_hash:08X}",
            "dst_slot": self.dst_slot,
            "seq": self.seq,
            "payload_len": self.payload_len,
            "ttl": self.ttl,
            "csum_kind": self.csum_kind,
        }


def encode_frame(
    *,
    type: int,
    src_hash: int,
    dst_hash: int,
    payload: bytes = b"",
    flags: int = 0,
    frag: int = 0,
    txid: int = 0,
    dst_slot: int = 0,
    seq: int = 0,
    ttl: int = DEFAULT_TTL,
    csum_kind: int = CSUM_CCITT,
    ver: int = WIRE_VER,
    magic: int = MAGIC,
) -> bytes:
    """02-1 encoder.  Explicit per-field little-endian writes, no struct image.

    The CRC (02-1 "appended as 2 bytes at the tail of the payload") covers the
    whole 32-byte header plus the payload, and ``payload_len`` counts the
    payload only -- i.e. it excludes both the header and the CRC, which is what
    makes 02-1's "payload_len must match the length actually received by the
    transport" checkable as ``len(buf) == 32 + payload_len + crc_len``.
    """
    payload = bytes(payload)
    out = bytearray()
    out += struct.pack("<H", magic)                       # offset 0
    out += struct.pack("<B", ver)                         # offset 2
    out += struct.pack("<B", type)                        # offset 3
    out += struct.pack("<H", flags)                       # offset 4
    out += struct.pack("<H", frag)                        # offset 6
    out += struct.pack("<I", txid)                        # offset 8
    out += struct.pack("<I", src_hash)                    # offset 12
    out += struct.pack("<I", dst_hash)                    # offset 16
    out += struct.pack("<I", dst_slot)                    # offset 20
    out += struct.pack("<I", seq)                         # offset 24
    out += struct.pack("<H", len(payload))                # offset 28
    out += struct.pack("<B", ttl)                         # offset 30
    out += struct.pack("<B", csum_kind)                   # offset 31
    out += payload                                        # offset 32
    if csum_kind == CSUM_CCITT:
        out += struct.pack("<H", crc16_ccitt(bytes(out)))
    return bytes(out)


#: Decoder verdicts.  A C decoder must be able to produce these strings; the
#: vector JSONs and check_c_side.py both use them.
R_OK = "OK"
R_SHORT = "short_frame"
R_MAGIC = "bad_magic"
R_VER = "unsupported_ver"
R_TYPE = "unknown_type"
R_LEN = "payload_len_mismatch"
R_OVERFLOW = "payload_len_over_mtu"
R_CSUM_KIND = "unsupported_csum_kind"
R_CRC = "crc_mismatch"
R_CRC_MISSING = "crc_missing"
R_FRAG = "frag_flag_mismatch"
R_SEQ = "seq_on_unreliable_frame"
R_TXID = "txid_on_send"
R_FIXED_LEN = "fixed_payload_len_mismatch"

#: Layer-2 verdicts (dispatch / HELLO validation, 03-02 puts these outside the
#: pure frame decoder: they need peer state that a stateless function does not
#: have).
D_LOCAL_DST = "dst_is_local"
D_BCAST_PROFILE = "broadcast_forbidden_on_profile"
D_BCAST_TYPE = "broadcast_flag_on_non_send"
D_RELIABLE_NO_CAP = "reliable_without_negotiated_cap"
D_FRAG_PROFILE = "fragment_forbidden_on_profile"
D_TTL = "ttl_expired"
D_DUP = "duplicate_seq"
D_NO_ROUTE = "no_route"
D_WRONG_STATE = "unexpected_type_for_state"
H_NOT_NEGOTIABLE = "hello_proto_not_negotiable"
H_HASH_MISMATCH = "hello_hash_mismatch"
H_HASH_CLASH = "hello_hash_clash"
H_SELF_LOOP = "hello_nonce_self_loop"
H_RESERVED_NODE_ID = "hello_reserved_node_id"
H_BAD_CAPS = "hello_caps_not_legal"
H_RELAY_ON_LEAF = "hello_relay_on_leaf"
H_LEN = "hello_payload_len"
E_UNKNOWN_ERRNO = "error_errno_unknown"
E_NO_TRANSACTION = "error_orig_txid_not_inflight"
S_REASM_TIMEOUT = "reasm_timeout"
S_REASM_EVICT = "reasm_cache_evicted"


def decode_frame(
    buf: bytes,
    *,
    mtu: int = TRANSPORT_MTU[TRANSPORT_UDP],
    local_ver: int = WIRE_VER,
) -> Tuple[Optional[Frame], Dict[str, object]]:
    """02-1 decoder, layer 1 (pure, stateless, no allocation beyond the result).

    ``mtu`` is the receiving transport's single-frame limit (04-1), because
    02-1 bounds ``payload_len`` by it.

    Returns ``(frame_or_None, diag)``.  ``diag`` always carries a per-field
    status for magic / ver / type / flags / frag / reserved / payload_len /
    csum_kind / crc plus the single overall ``reason``.  Malformed vectors
    assert on those, which is why they are reported individually instead of as
    a single boolean.
    """
    n = len(buf)
    diag: Dict[str, object] = {
        "wire_len": n,
        "magic": "unchecked",
        "ver": "unchecked",
        "type": "unchecked",
        "flags": "unchecked",
        "frag": "unchecked",
        "reserved": "unchecked",
        "payload_len": "unchecked",
        "csum_kind": "unchecked",
        "crc": "unchecked",
        "reason": R_OK,
    }
    if n < HEADER_LEN:
        diag["reason"] = R_SHORT
        diag["magic"] = "short_frame"
        return None, diag

    magic, = struct.unpack_from("<H", buf, 0)
    ver = buf[2]
    ftype = buf[3]
    flags, = struct.unpack_from("<H", buf, 4)
    frag, = struct.unpack_from("<H", buf, 6)
    txid, = struct.unpack_from("<I", buf, 8)
    src_hash, = struct.unpack_from("<I", buf, 12)
    dst_hash, = struct.unpack_from("<I", buf, 16)
    dst_slot, = struct.unpack_from("<I", buf, 20)
    seq, = struct.unpack_from("<I", buf, 24)
    payload_len, = struct.unpack_from("<H", buf, 28)
    ttl = buf[30]
    csum_kind = buf[31]

    diag["magic"] = "ok" if magic == MAGIC else "bad_magic"
    diag["ver"] = "ok" if ver == local_ver else "unsupported_ver"
    diag["type"] = "ok" if ftype in TYPE_NAMES else "unknown_type"
    diag["flags"] = "ok"
    diag["frag"] = "ok"
    diag["reserved"] = "ok"
    diag["payload_len"] = "unchecked"
    diag["csum_kind"] = "ok" if csum_kind in (CSUM_NONE, CSUM_CCITT) else "unsupported"
    diag["crc"] = "absent"

    # -- hard rules first, cheapest and most decisive ones in order --------
    if magic != MAGIC:
        diag["reason"] = R_MAGIC
        return None, diag
    # 02-1 "magic/ver mismatch -> drop + count; a ver higher than local is
    # handled during HELLO (section 6), data frames are dropped silently".
    if ver != local_ver:
        diag["reason"] = R_VER
        return None, diag
    if ftype not in TYPE_NAMES:
        diag["reason"] = R_TYPE
        return None, diag
    if csum_kind not in (CSUM_NONE, CSUM_CCITT):
        diag["reason"] = R_CSUM_KIND
        return None, diag
    if payload_len > max_payload_len(mtu):
        diag["payload_len"] = "over_mtu"
        diag["reason"] = R_OVERFLOW
        return None, diag

    crc_present = csum_kind == CSUM_CCITT
    crc_len = 2 if crc_present else 0
    # 02-1 "payload_len must match the length actually received by the
    # transport -> drop the frame + count rx_malformed".
    if n != HEADER_LEN + payload_len + crc_len:
        diag["payload_len"] = "mismatch"
        diag["reason"] = R_LEN
        return None, diag
    diag["payload_len"] = "ok"

    if crc_present:
        got, = struct.unpack_from("<H", buf, HEADER_LEN + payload_len)
        want = crc16_ccitt(buf[: HEADER_LEN + payload_len])
        diag["crc"] = "ok" if got == want else "mismatch"
        diag["crc_value"] = f"0x{got:04X}"
        diag["crc_computed"] = f"0x{want:04X}"
        if got != want:
            diag["reason"] = R_CRC
            return None, diag
    else:
        got = None
        want = None

    payload = bytes(buf[HEADER_LEN : HEADER_LEN + payload_len])

    # -- flags / frag consistency (02-3 + 02-5) ---------------------------
    fragmented = bool(flags & FLAG_FRAGMENTED)
    frag_seq = frag & FRAG_SEQ_MASK
    frag_last = bool(frag & FRAG_LAST)
    if fragmented:
        # 02-5 "fragment indices increase from 0, the last fragment sets
        # bit15".  A single-fragment message therefore does not exist: one
        # frame that fits is sent with FRAGMENTED clear (ASSUMPTION A-04).
        if frag_seq == 0 and frag_last:
            diag["frag"] = "single_fragmented"
            diag["reason"] = R_FRAG
            return None, diag
    else:
        if frag != 0:
            diag["frag"] = "frag_without_flag"
            diag["reason"] = R_FRAG
            return None, diag

    if not (flags & FLAG_RELIABLE) and seq != 0:
        # 02-1 "seq ... unreliable tier puts 0" + ASSUMPTION A-11: only frames
        # that opted into RELIABLE consume a sequence number.
        diag["flags"] = "seq_without_reliable"
        diag["reason"] = R_SEQ
        return None, diag

    if ftype == TYPE_SEND and txid != 0:
        # 02-1 "SEND puts 0".
        diag["flags"] = "txid_on_send"
        diag["reason"] = R_TXID
        return None, diag

    fixed = FIXED_PAYLOAD_LEN[ftype]
    if fixed is not None and payload_len != fixed:
        # 02-2 payload column (fixed sizes) + 02-6 (HELLO is fixed 32 bytes).
        diag["payload_len"] = "fixed_size_mismatch"
        diag["reason"] = R_FIXED_LEN
        return None, diag

    # -- reserved-bit reporting: never a reject (02-1) ---------------------
    reserved_bits = 0
    if flags & ~KNOWN_FLAGS_MASK:
        reserved_bits |= flags & ~KNOWN_FLAGS_MASK
    if frag & FRAG_RESERVED_MASK:
        reserved_bits |= frag & FRAG_RESERVED_MASK
    diag["reserved"] = "nonzero_ignored" if reserved_bits else "ok"

    frame = Frame(
        magic=magic,
        ver=ver,
        type=ftype,
        flags=flags,
        frag=frag,
        txid=txid,
        src_hash=src_hash,
        dst_hash=dst_hash,
        dst_slot=dst_slot,
        seq=seq,
        payload_len=payload_len,
        ttl=ttl,
        csum_kind=csum_kind,
        payload=payload,
        wire_len=n,
        crc_present=crc_present,
        crc_value=got,
        crc_ok=True,
    )

    # -- ACK piggyback split (02-7, ASSUMPTION A-05) ----------------------
    # Non-fragmented RELIABLE frames of a variable-payload type start with the
    # 4-byte cumulative ACK; fragments do not (02-5 keeps the fragment payload
    # at exactly MTU-32-2).
    if (
        fragmented
        or not frame.is_reliable
        or ftype not in PIGGYBACK_HOST_TYPES
        or payload_len < ACK_PREFIX_LEN
    ):
        frame.app_offset = 0
        frame.app_payload = payload
        frame.ack_prefix = None
    else:
        frame.app_offset = ACK_PREFIX_LEN
        frame.ack_prefix = struct.unpack_from("<I", payload, 0)[0]
        frame.app_payload = payload[ACK_PREFIX_LEN:]

    return frame, diag


# ---------------------------------------------------------------------------
# layer 2: dispatch and HELLO validation (02-5, 02-6, 02-7, 01-abi)
# ---------------------------------------------------------------------------


def validate_dispatch(
    frame: Frame,
    *,
    profile: int,
    peer_caps: int,
    peer_ver: int = WIRE_VER,
    state: str = "UP",
    seq_seen: bool = False,
) -> Tuple[Optional[str], Optional[int]]:
    """Policy layer for an otherwise well-formed frame.

    Returns ``(reason_or_None, counter_or_None)`` -- both ``None`` means accept.
    Counter names come from 02-10 and nothing else.

    ``ASSUMPTION A-12``: the 02-10 counter taxonomy is made decidable as

    * ``rx_malformed`` -- the frame violates the wire format (what 02-1 already
      assigns it to), decoded by :func:`decode_frame`;
    * ``rx_drops``     -- the frame is well formed but the receiver refuses it
      for state or policy reasons;
    * ``dedup_drops``  -- 02-7 duplicate sequence;
    * ``hello_rejects``-- 02-6 negotiation failure.
    """
    if frame.ttl == 0:
        # 02-1 "decrement by 1 per hop, drop when it reaches 0"; 01-abi maps a
        # TTL exhaustion to A20_ERR_NODE_UNREACHABLE.  ASSUMPTION A-13: the
        # decrement belongs to the forwarding hop, so a frame that *arrives*
        # with ttl == 0 has already exhausted its budget.
        return D_TTL, "rx_drops"
    if frame.dst_hash == fnv1a32(NODE_ID_LOCAL):
        # 01-abi node identity: "(LOCAL, slot) must take the local fast path,
        # serializing it is forbidden", so a frame addressed to the all-zero
        # node id is a protocol violation and never reaches the local path.
        return D_LOCAL_DST, "rx_drops"
    if frame.flags & FLAG_BROADCAST:
        if frame.dst_hash != fnv1a32(NODE_ID_BROADCAST):
            return D_BCAST_TYPE, "rx_drops"
        if frame.type != TYPE_SEND:
            # 02-3 "BROADCAST: dst_hash is the broadcast hash (SERVER tier
            # SEND only)".
            return D_BCAST_TYPE, "rx_drops"
        if profile == PROFILE_MCU:
            # 01-abi "an MCU tier node receiving it must drop it"; 02-5
            # "the MCU tier ... A20_ERR_CLUSTER_UNSUPPORTED".
            return D_BCAST_PROFILE, "rx_drops"
    if frame.is_fragment and profile == PROFILE_MCU:
        # 02-5 "the MCU tier forbids fragmentation (FRAGMENTED frames are
        # dropped and counted)".
        return D_FRAG_PROFILE, "rx_drops"
    if frame.is_reliable and not (peer_caps & CAP_RELIABLE):
        # 02-3 "RELIABLE ... only allowed when both ends negotiated
        # CAP_RELIABLE".
        return D_RELIABLE_NO_CAP, "rx_drops"
    if frame.is_reliable and seq_seen:
        # 02-7 "duplicate frames are dropped but the ACK is resent".
        return D_DUP, "dedup_drops"
    return None, None


def validate_hello(
    payload: bytes,
    *,
    local_ver: int = WIRE_VER,
    local_nonce: bytes = b"",
    known_hashes: Optional[Dict[int, bytes]] = None,
    expect_src_hash: Optional[int] = None,
) -> Tuple[Optional[str], Optional[str], Optional[int]]:
    """02-6 HELLO/HELLO_ACK validation.

    Returns ``(reason, reply_errno_name, counter)``.  ``reason is None`` means
    the link may go UP.  A non-None ``reply_errno`` means the node must answer
    with an ERROR frame (02-6 "any failure -> ERROR frame + state DOWN");
    ``reply_errno is None`` means drop silently, which is the self-loop case
    (answering yourself is pointless -- ASSUMPTION A-14).
    """
    if len(payload) != HELLO_PAYLOAD_LEN:
        return H_LEN, "A20_ERR_CLUSTER_UNSUPPORTED", "hello_rejects"
    h = decode_hello_payload(payload)
    # node_id_hash is rendered as a string for the JSON documents; compare the
    # integer form against the header field (01-abi: src_hash carries
    # fnv1a32(src node id), an integer).
    node_hash_int = fnv1a32(bytes.fromhex(str(h["node_id"])))
    if expect_src_hash is not None and node_hash_int != expect_src_hash:
        # 02-6 "hash clash check" on the sender's own consistency: the header
        # carries fnv1a32(node_id) (01-abi), so a HELLO whose header hash is
        # not the hash of the full id it carries is either corrupt or forged.
        return H_HASH_MISMATCH, "A20_ERR_CLUSTER_UNSUPPORTED", "hello_rejects"
    if h["node_id"] == NODE_ID_LOCAL.hex() or h["node_id"] == NODE_ID_BROADCAST.hex():
        # 01-abi: both values are reserved and must never be a remote identity.
        return H_RESERVED_NODE_ID, "A20_ERR_CLUSTER_UNSUPPORTED", "hello_rejects"
    if local_nonce and h["nonce"] == local_nonce.hex():
        # 02-6 "a HELLO carrying our own nonce is judged a self-loop, rejected".
        return H_SELF_LOOP, None, "hello_rejects"
    if h["caps"] & ~CAP_ALL:
        # 01-abi rejects caps with unknown bits at set_self; 02-6 requires
        # "caps consistent with the tier".
        return H_BAD_CAPS, "A20_ERR_CLUSTER_UNSUPPORTED", "hello_rejects"
    if h["profile_tier"] not in PROFILE_NAMES:
        return H_BAD_CAPS, "A20_ERR_CLUSTER_UNSUPPORTED", "hello_rejects"
    if h["profile_tier"] == PROFILE_MCU and (h["caps"] & CAP_RELAY):
        # 02-6 "caps consistent with the tier (a leaf must not set RELAY)".
        return H_RELAY_ON_LEAF, "A20_ERR_CLUSTER_UNSUPPORTED", "hello_rejects"
    lo = max(h["proto_min"], 0)
    hi = min(h["proto_max"], local_ver)
    if lo > hi:
        # 02-6 "non-empty proto intersection (the largest value of the
        # intersection takes effect)"; 01-abi stability rule: a peer that
        # cannot be negotiated with gets an ERROR frame and the link is refused.
        return H_NOT_NEGOTIABLE, "A20_ERR_CLUSTER_UNSUPPORTED", "hello_rejects"
    if known_hashes is not None:
        # known_hashes maps hash -> full node id (see classify()/gen_vectors);
        # keys are integers, same domain as node_hash_int.
        other = known_hashes.get(node_hash_int)
        if other is not None and other.hex() != h["node_id"]:
            # 01-abi node hash: "two ends with different full IDs but the same
            # hash -> the later one is rejected with an ERROR frame".
            return H_HASH_CLASH, "A20_ERR_CLUSTER_UNSUPPORTED", "hello_rejects"
    return None, None, None


def hello_src_hash_check(frame: Frame) -> bool:
    """02-6/01-abi consistency: the header's ``src_hash`` must be the hash of
    the full node id carried in the HELLO payload.  Returns True when equal."""
    if frame.type not in (TYPE_HELLO, TYPE_HELLO_ACK):
        raise ValueError("hello_src_hash_check on a non-HELLO frame")
    if len(frame.payload) != HELLO_PAYLOAD_LEN:
        return False
    return fnv1a32(bytes.fromhex(decode_hello_payload(frame.payload)["node_id"])) == frame.src_hash


# ---------------------------------------------------------------------------
# 02-5  fragmentation helpers
# ---------------------------------------------------------------------------


def fragment_payload(payload: bytes, transport: str, *, ack_prefix_len: int = 0) -> List[bytes]:
    """02-5 split a message into per-fragment payloads of exactly
    ``MTU - 32 - 2`` bytes (1438 on UDP), the last one shorter.

    ``ack_prefix_len`` is rejected unless zero: ASSUMPTION A-05 forbids an ACK
    prefix inside a fragment, because 02-5 states the fragment payload size as
    ``MTU - 32 - 2`` with no room for it.
    """
    if ack_prefix_len:
        raise ValueError("ASSUMPTION A-05: no ACK piggyback inside fragments")
    mtu = TRANSPORT_MTU[TRANSPORT_ID[transport]]
    chunk = max_payload_len(mtu)
    if len(payload) <= chunk:
        return [payload]
    return [payload[i : i + chunk] for i in range(0, len(payload), chunk)]


def frag_field(seq: int, last: bool) -> int:
    """02-5 frag encoding: low 12 bits index, bit15 last flag."""
    if not 0 <= seq <= FRAG_SEQ_MASK:
        raise ValueError(f"fragment index {seq} exceeds the 12-bit field")
    return (seq & FRAG_SEQ_MASK) | (FRAG_LAST if last else 0)


def reassemble(chunks: Sequence[Tuple[int, bytes]]) -> bytes:
    """02-5 "the reassembly cache is a per-(src_hash, txid) bitmap plus a
    concatenation buffer; when every fragment is in, it is reassembled into one
    message" -- fragments may arrive out of order, so they are sorted by index.
    """
    out = bytearray()
    for _, data in sorted(chunks, key=lambda c: c[0]):
        out += data
    return bytes(out)


# ---------------------------------------------------------------------------
# the one authoritative receiver-side verdict
# ---------------------------------------------------------------------------

#: Default receiver context.  A DEFAULT tier node (03-4 ``CLUSTER_PROFILE=2``)
#: on a link whose peer negotiated CAP_RELIABLE, link state UP, v0 on both
#: ends.
DEFAULT_CTX = {
    "mtu": TRANSPORT_MTU[TRANSPORT_UDP],
    "profile": PROFILE_DEFAULT,
    "local_ver": WIRE_VER,
    "peer_caps": CAP_RELIABLE,
    "state": "UP",
    "seq_seen": False,
    "local_nonce": b"",
    "known_hashes": None,
    "inflight_txids": None,
    "leaf_strict": False,
}


def classify(buf: bytes, ctx: Optional[Dict[str, object]] = None) -> Dict[str, object]:
    """Single authoritative verdict for one received byte string.

    Runs layer 1 (:func:`decode_frame`) then layer 2 (:func:`validate_dispatch`
    / :func:`validate_hello`) and returns one record:

    ``accept``    bool -- the frame is dispatched
    ``layer``     "frame" | "dispatch" | "hello" | "error" -- where it stopped
    ``reason``    a verdict string from the tables above (R_OK when accepted)
    ``counter``   a 02-10 counter name to bump, or None
    ``errno``     errno name for the error the caller observes, or None
    ``reply``     the ERROR frame the receiver must send back, or None

    The generator, selftest.py and check_c_side.py all call this one function,
    so the C side has exactly one thing to reimplement.
    """
    c = dict(DEFAULT_CTX)
    if ctx:
        c.update(ctx)
    frame, diag = decode_frame(
        buf, mtu=int(c["mtu"]), local_ver=int(c["local_ver"])
    )
    out: Dict[str, object] = {
        "frame": frame,
        "diag": diag,
        "frame_reason": diag["reason"],
        "accept": False,
        "layer": "frame",
        "reason": diag["reason"],
        "counter": None,
        "errno": None,
        "reply": None,
    }
    if frame is None:
        # 02-1: magic / ver / payload_len / CRC failures all land here and the
        # only counter 02-1 ever names is rx_malformed.
        out["counter"] = "rx_malformed"
        if diag["reason"] == R_VER and frame_is_hello_shaped(buf):
            # 02-1 "a ver higher than local is handled during the HELLO phase
            # (section 6)": a data frame is dropped silently, a HELLO gets an
            # ERROR frame plus hello_rejects.
            out.update(
                layer="hello",
                reason=H_NOT_NEGOTIABLE,
                counter="hello_rejects",
                errno="A20_ERR_CLUSTER_UNSUPPORTED",
                reply="ERROR(A20_ERR_CLUSTER_UNSUPPORTED, orig_txid=0)",
            )
        return out

    if frame.type in (TYPE_HELLO, TYPE_HELLO_ACK):
        if int(c["profile"]) == PROFILE_MCU and frame.type == TYPE_HELLO:
            # 04-4 "a leaf only answers passively, it never dials out"; a leaf
            # that dials is dropped rather than negotiated.
            out.update(layer="dispatch", reason=D_WRONG_STATE, counter="rx_drops")
            return out
        reason, reply_errno, counter = validate_hello(
            frame.payload,
            local_ver=int(c["local_ver"]),
            local_nonce=bytes(c["local_nonce"]),  # type: ignore[arg-type]
            known_hashes=c["known_hashes"],  # type: ignore[arg-type]
            expect_src_hash=frame.src_hash,
        )
        if reason is not None:
            out.update(layer="hello", reason=reason, counter=counter, errno=reply_errno)
            if reply_errno:
                out["reply"] = f"ERROR({reply_errno}, orig_txid={frame.txid})"
            return out
        out.update(
            accept=True,
            layer="hello",
            reason=R_OK,
            counter="rx_frames",
            negotiated={
                "proto_effective": min(int(decode_hello_payload(frame.payload)["proto_max"]),
                                       int(c["local_ver"])),
                "peer_caps": decode_hello_payload(frame.payload)["caps"],
                "peer_profile": decode_hello_payload(frame.payload)["profile_tier"],
            },
        )
        return out

    reason, counter = validate_dispatch(
        frame,
        profile=int(c["profile"]),
        peer_caps=int(c["peer_caps"]),
        peer_ver=int(c["local_ver"]),
        state=str(c["state"]),
        seq_seen=bool(c["seq_seen"]),
    )
    if reason is not None:
        out.update(layer="dispatch", reason=reason, counter=counter)
        # Only TTL exhaustion surfaces a local errno: 01-abi lists "TTL
        # exhausted" under A20_ERR_NODE_UNREACHABLE, and the local caller is
        # waiting on exactly this transaction.  A frame dropped for any other
        # receive-side reason produces no local errno -- the *sender* learns
        # through its deadline (A20_ERR_CLUSTER_TIMEOUT), and the MCU
        # A20_ERR_CLUSTER_UNSUPPORTED of 01-abi belongs to the local API that
        # refuses to build the frame in the first place.
        if reason == D_TTL:
            out["errno"] = "A20_ERR_NODE_UNREACHABLE"
        return out

    if frame.type == TYPE_ERROR:
        errno_value, orig_txid = struct.unpack_from("<II", frame.payload, 0)
        inflight = c.get("inflight_txids")
        if inflight is not None and orig_txid not in inflight:  # type: ignore[operator]
            # 02-4 "a late REPLY finds no transaction by txid -> drop and count"
            # (the same rule the ERROR reply follows: it maps a remote error back
            # to a caller that no longer exists).
            out.update(layer="error", reason=E_NO_TRANSACTION, counter="rx_drops")
            return out
        out["accept"] = True
        out["counter"] = "rx_frames"
        out["layer"] = "error"
        if errno_value not in WIRE_ERRNO_SET:
            # 02-8 "an unknown code is treated as A20_ERR_CLUSTER_UNSUPPORTED".
            out["reason"] = E_UNKNOWN_ERRNO
            out["errno"] = "A20_ERR_CLUSTER_UNSUPPORTED"
            return out
        out["reason"] = R_OK
        out["errno"] = ERRNO_BY_VALUE[errno_value]
        return out

    if int(c["profile"]) == PROFILE_MCU and frame.type in (TYPE_SEND, TYPE_ACK, TYPE_NACK):
        # 04-4 "on receiving SEND/ACK/NACK/fragment frames: drop and count,
        # not treated as an error".
        out.update(layer="dispatch", reason=D_WRONG_STATE, counter="rx_drops")
        return out
    if int(c["profile"]) == PROFILE_MCU and str(c["state"]) == "DISCONNECTED" and frame.type in (
        TYPE_PING,
        TYPE_PONG,
    ):
        # 04-4 "the leaf side has only the DISCONNECTED/UP state machine; after
        # 50s without a PING it returns to DISCONNECTED and the short address is
        # invalidated".
        out.update(layer="dispatch", reason=D_WRONG_STATE, counter="rx_drops")
        return out

    out.update(accept=True, layer="dispatch", reason=R_OK, counter="rx_frames")
    return out


def frame_is_hello_shaped(buf: bytes) -> bool:
    """True when the byte string claims to be a HELLO/HELLO_ACK at offset 3,
    even if the rest of the frame failed to decode."""
    return len(buf) >= 4 and buf[3] in (TYPE_HELLO, TYPE_HELLO_ACK)


__all__ = [name for name in dir() if not name.startswith("_")]
