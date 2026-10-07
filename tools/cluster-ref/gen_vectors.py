#!/usr/bin/env python3
"""Generate the gold vectors required by docs/cluster/02-wire-protocol.md 9.

Run ``python3 tools/cluster-ref/gen_vectors.py`` to rewrite ``vectors/``.  The
committed tree is what the C side diffs against; selftest.py regenerates the
same bytes in memory and fails if the tree has drifted, so this file is the
single source of truth for the corpus.

Requirements from 02-9, mapped onto directories:

  valid/       02-9(1) >= 2 legal frames per message type: .hex + .json decode
  malformed/   02-9(2) >= 20 malformed frames with the expected behaviour
  reserved/    02-9(2) "non-zero reserved bits" -- 02-1 forbids rejecting them
  state/       02-9(2) counters that only a stateful receiver can produce
  slip/        04-4 SLIP variant, the WC1 diffing basis

Every vector's expectation is *evaluated* by clframe.classify() while it is
generated, so a wrong expectation is a generation error rather than a silent
bad expectation.
"""

from __future__ import annotations

import json
import os
import struct
import sys
from typing import Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import clframe as C  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
VECDIR = os.path.join(HERE, "vectors")

# ---------------------------------------------------------------------------
# participants
# ---------------------------------------------------------------------------

NODE_A = bytes(range(1, 17))  # 01..10, head / SERVER tier
NODE_B = bytes([0xA1 + i for i in range(15)] + [0xB0])  # peer / DEFAULT tier
LEAF = bytes([0xDE, 0xAD, 0xBE, 0xEF] + [0] * 11 + [0x01])  # MCU leaf

H_A = C.fnv1a32(NODE_A)  # 0xae8e8135
H_B = C.fnv1a32(NODE_B)  # 0x5edc2075
H_LEAF = C.fnv1a32(LEAF)  # 0x94ddcbb0
H_BCAST = C.fnv1a32(C.NODE_ID_BROADCAST)  # 0x360779f5
H_LOCAL = C.fnv1a32(C.NODE_ID_LOCAL)  # 0x69691905

NONCE_A = bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08])
NONCE_B = bytes([0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18])
NONCE_LEAF = bytes([0xFE, 0xED, 0xFA, 0xCE, 0xC0, 0xDB, 0xDC, 0xDD])  # forces SLIP escapes

TIER = {
    "loopback": C.PROFILE_DEFAULT,
    "udp": C.PROFILE_DEFAULT,
    "uart": C.PROFILE_MCU,
}

#: Receiver roles.  A frame's expectation depends on *who* receives it (the MCU
#: rules in 02-5 / 04-4 are receiver-side), so every vector names its receiver
#: explicitly instead of deriving it from the transport.
RX_CTX = {
    # B, a DEFAULT tier peer reached over UDP; its link was negotiated with a
    # peer that advertised CAP_RELIABLE.
    "peer_udp": {
        "mtu": C.TRANSPORT_MTU[C.TRANSPORT_UDP],
        "profile": C.PROFILE_DEFAULT,
        "local_ver": C.WIRE_VER,
        "peer_caps": C.CAP_RELIABLE,
        "state": "UP",
    },
    # A, the SERVER tier head reached over UDP (its HELLO peer is a SERVER too).
    "head_udp": {
        "mtu": C.TRANSPORT_MTU[C.TRANSPORT_UDP],
        "profile": C.PROFILE_SERVER,
        "local_ver": C.WIRE_VER,
        "peer_caps": C.CAP_RELIABLE | C.CAP_RELAY,
        "state": "UP",
    },
    # B still in HELLO_SENT: it has no negotiated caps yet, so HELLO validation
    # is what a received HELLO runs (02-6).
    "peer_udp_hello": {
        "mtu": C.TRANSPORT_MTU[C.TRANSPORT_UDP],
        "profile": C.PROFILE_DEFAULT,
        "local_ver": C.WIRE_VER,
        "peer_caps": 0,
        "state": "HELLO_SENT",
    },
    # A in HELLO_SENT over UDP, i.e. it has sent a HELLO and is waiting for the
    # HELLO_ACK / the peer's HELLO.
    "head_udp_hello": {
        "mtu": C.TRANSPORT_MTU[C.TRANSPORT_UDP],
        "profile": C.PROFILE_SERVER,
        "local_ver": C.WIRE_VER,
        "peer_caps": 0,
        "state": "HELLO_SENT",
    },
    # A, the head, in HELLO_SENT over UART: a leaf HELLO arrives here.
    "head_uart_hello": {
        "mtu": C.TRANSPORT_MTU[C.TRANSPORT_UART],
        "profile": C.PROFILE_SERVER,
        "local_ver": C.WIRE_VER,
        "peer_caps": 0,
        "state": "HELLO_SENT",
    },
    # A, the head, reached over UART; its peer is a leaf that advertises
    # CAP_LEAF only (01-abi capability bits).
    "head_uart": {
        "mtu": C.TRANSPORT_MTU[C.TRANSPORT_UART],
        "profile": C.PROFILE_SERVER,
        "local_ver": C.WIRE_VER,
        "peer_caps": C.CAP_LEAF,
        "state": "UP",
    },
    # the MCU leaf itself, over UART
    "leaf_uart": {
        "mtu": C.TRANSPORT_MTU[C.TRANSPORT_UART],
        "profile": C.PROFILE_MCU,
        "local_ver": C.WIRE_VER,
        "peer_caps": C.CAP_LEAF,
        "state": "UP",
    },
    "leaf_uart_down": {
        "mtu": C.TRANSPORT_MTU[C.TRANSPORT_UART],
        "profile": C.PROFILE_MCU,
        "local_ver": C.WIRE_VER,
        "peer_caps": C.CAP_LEAF,
        "state": "DISCONNECTED",
    },
    # loopback virtual node
    "loopback": {
        "mtu": C.TRANSPORT_MTU[C.TRANSPORT_LOOPBACK],
        "profile": C.PROFILE_DEFAULT,
        "local_ver": C.WIRE_VER,
        "peer_caps": C.CAP_RELIABLE,
        "state": "UP",
    },
}


def hexs(data: bytes) -> str:
    return bytes(data).hex()


# ---------------------------------------------------------------------------
# the record
# ---------------------------------------------------------------------------


class Vec:
    """One vector, plus the machine-checked expectation."""

    def __init__(
        self,
        vid: str,
        category: str,
        title: str,
        sections: Sequence[str],
        *,
        transport: str,
        rx: str = "peer_udp",
        assumptions: Sequence[str] = (),
        notes: str = "",
        raw: Optional[bytes] = None,
        ctx: Optional[Dict[str, object]] = None,
        mutations: Optional[List[Dict[str, object]]] = None,
        payload_view: Optional[Dict[str, object]] = None,
        slip_stream: Optional[bytes] = None,
        wire_frame: bool = True,
        precondition: str = "",
        followup: str = "",
    ) -> None:
        self.vid = vid
        self.category = category
        self.title = title
        self.sections = list(sections)
        self.transport = transport
        self.rx = rx
        self.assumptions = list(assumptions)
        self.notes = notes
        self.raw = raw
        self.ctx = ctx
        self.mutations = mutations or []
        self.payload_view = payload_view
        self.slip_stream = slip_stream
        self.wire_frame = wire_frame
        self.precondition = precondition
        self.followup = followup
        self.dir = {
            "valid": "valid",
            "malformed": "malformed",
            "reserved": "reserved",
            "state": "state",
            "slip": "slip",
        }[category]
        self.result: Optional[Dict[str, object]] = None
        self.expect: Optional[Dict[str, object]] = None

    # -- evaluation --------------------------------------------------------
    def receiver_ctx(self) -> Dict[str, object]:
        c = dict(RX_CTX[self.rx])
        if self.ctx:
            c.update(self.ctx)
        return c

    def evaluate(self) -> Dict[str, object]:
        if self.slip_stream is not None:
            frames, notes = C.slip_decode(self.slip_stream)
            return {
                "slip_frames": [f.hex() for f in frames],
                "slip_notes": notes,
                "slip_stream_len": len(self.slip_stream),
            }
        res = C.classify(self.raw, self.receiver_ctx())
        self.result = res
        return res

    # -- serialisation -----------------------------------------------------
    def document(self) -> Dict[str, object]:
        doc: Dict[str, object] = {
            "id": self.vid,
            "category": self.category,
            "title": self.title,
            "doc_sections": self.sections,
            "transport": {
                "id": C.TRANSPORT_ID[self.transport],
                "name": self.transport,
                "mtu": C.TRANSPORT_MTU[C.TRANSPORT_ID[self.transport]],
                "max_payload_len": C.MAX_PAYLOAD[self.transport],
            },
            "receiver": {
                "role": self.rx,
                "profile_tier": int(self.receiver_ctx()["profile"]),
                "profile_name": C.PROFILE_NAMES[int(self.receiver_ctx()["profile"])],
                "state": self.receiver_ctx()["state"],
                "peer_caps": int(self.receiver_ctx()["peer_caps"]),
                "local_ver": int(self.receiver_ctx()["local_ver"]),
            },
            "mutations": self.mutations,
        }
        if self.assumptions:
            doc["assumptions"] = self.assumptions
        if self.notes:
            doc["notes"] = self.notes
        if self.precondition:
            doc["precondition"] = self.precondition
        if self.followup:
            doc["followup"] = self.followup
        if not self.wire_frame:
            doc["wire_frame"] = None
            doc["expect"] = self.expect
            return doc
        if self.slip_stream is not None:
            doc["slip"] = self.evaluate()
            doc["frame_hex"] = self.raw.hex()
            doc["expect"] = self.expect
            return doc
        res = self.result if self.result is not None else self.evaluate()
        frame: Optional[C.Frame] = res["frame"]  # type: ignore[assignment]
        diag = res["diag"]
        if frame is not None:
            doc["frame"] = frame.header_json()
            doc["frame"]["wire_len"] = frame.wire_len
            doc["frame"]["crc_present"] = frame.crc_present
            doc["frame"]["crc_value"] = (
                f"0x{frame.crc_value:04X}" if frame.crc_value is not None else None
            )
            doc["frame"]["crc_ok"] = frame.crc_ok
            doc["payload"] = {
                "len": frame.payload_len,
                "hex": frame.payload.hex(),
                "app_offset": frame.app_offset,
                "app_len": len(frame.app_payload),
                "app_hex": frame.app_payload.hex(),
                "ack_prefix": frame.ack_prefix,
            }
            hello = frame.hello()
            if hello is not None:
                doc["payload"]["hello"] = hello
            if self.payload_view == {"kind": "tlv"}:
                doc["payload"]["view"] = {
                    "kind": "tlv",
                    "doc": "05-userspace.md 3",
                    "items": [
                        {"tag": f"0x{tag:04X}", "len": len(val), "hex": val.hex()}
                        for tag, val in C.tlv_decode(frame.app_payload)
                    ],
                }
        else:
            doc["frame"] = None
            doc["raw_header"] = self.raw[: C.HEADER_LEN].hex()
        doc["diagnostics"] = diag
        doc["expect"] = self.expect
        return doc


# ---------------------------------------------------------------------------
# registry
# ---------------------------------------------------------------------------

VECTORS: List[Vec] = []


def add(vec: Vec) -> Vec:
    VECTORS.append(vec)
    return vec


def check(cond: bool, message: str) -> None:
    if not cond:
        raise SystemExit(f"gen_vectors: {message}")


# ---------------------------------------------------------------------------
# payload helpers
# ---------------------------------------------------------------------------

TLV_ECHO_REQ = [(0x8001, b"ping"), (0x8002, struct.pack("<I", 7))]
TLV_ECHO_RESP = [(0x8003, struct.pack("<I", 2048))]
TLV_DOT_REQ = [(0x8001, b"dot"), (0x8002, struct.pack("<I", 8))]
TLV_CLUSTERD = [(0x0001, struct.pack("<I", 42)), (0x0002, b"clusterd/resolve")]
TLV_JOBD_SHARD = [
    (0x0101, struct.pack("<Q", 0x1122334455667788)),
    (0x0103, b"ops.dot"),
    (0x0104, bytes(range(64))),
]


def app_payload(items: Sequence[Tuple[int, bytes]]) -> bytes:
    return C.tlv_encode(items)


def reliable_payload(ack: int, items: Sequence[Tuple[int, bytes]]) -> bytes:
    """02-7 piggyback: cumulative ACK first, then the application message."""
    return struct.pack("<I", ack) + app_payload(items)


def hello_payload(
    node_id: bytes,
    tier: int,
    caps: int,
    nonce: bytes,
    *,
    short_addr: int = 0,
    link_addr_len: int = 6,
    proto_min: int = 0,
    proto_max: int = 0,
    reserved: int = 0,
) -> bytes:
    return C.encode_hello_payload(
        node_id,
        proto_min,
        proto_max,
        tier,
        caps,
        short_addr,
        link_addr_len,
        nonce,
        reserved,
    )


# ---------------------------------------------------------------------------
# mutation helpers -- each records itself for the vector JSON
# ---------------------------------------------------------------------------


def m_set(offset: int, value: int, doc: str, why: str) -> Tuple[Dict[str, object], bytes]:
    return (
        {"op": "set", "offset": offset, "to": f"0x{value:02X}", "doc": doc, "why": why},
        bytes((value,)),
    )


def mutate(base: bytes, *muts: Dict[str, object]) -> Tuple[bytes, List[Dict[str, object]]]:
    """Apply mutation descriptors to a base frame; returns (bytes, records)."""
    buf = bytearray(base)
    records: List[Dict[str, object]] = []
    for m in muts:
        op = m["op"]
        if op == "set":
            off = int(m["offset"])  # type: ignore[arg-type]
            buf[off] = int(m["value"])  # type: ignore[arg-type]
            m["was"] = f"0x{base[off]:02X}"
            records.append(m)
        elif op == "set16":
            # a 16-bit little-endian field (02-1: every multi-byte field is LE)
            off = int(m["offset"])  # type: ignore[arg-type]
            val = int(m["value"])  # type: ignore[arg-type]
            old, = struct.unpack_from("<H", bytes(base), off)
            struct.pack_into("<H", buf, off, val)
            m["was"] = f"0x{old:04X}"
            records.append(m)
        elif op == "flip_bit":
            bit = int(m["bit"])  # type: ignore[arg-type]
            off, shift = divmod(bit, 8)
            buf[off] ^= 1 << shift
            m["offset"] = off
            m["was"] = f"0x{base[off]:02X}"
            m["to"] = f"0x{buf[off]:02X}"
            records.append(m)
        elif op == "truncate":
            keep = int(m["keep"])  # type: ignore[arg-type]
            m["was_len"] = len(base)
            buf = buf[:keep]
            records.append(m)
        elif op == "append":
            extra = bytes.fromhex(str(m["bytes"]))
            m["was_len"] = len(base)
            buf += extra
            records.append(m)
        elif op == "drop_tail":
            keep = int(m["keep"])  # type: ignore[arg-type]
            m["was_len"] = len(base)
            buf = buf[:keep]
            records.append(m)
        else:  # pragma: no cover - generator bug
            raise SystemExit(f"unknown mutation {op}")
    return bytes(buf), records


def rec(op: str, doc: str, why: str, **kw) -> Dict[str, object]:
    d: Dict[str, object] = {"op": op, "doc": doc, "why": why}
    d.update(kw)
    return d


def fix_crc(buf: bytes) -> bytes:
    """Recompute the 02-1 CRC trailer.

    Header-field violations (reserved bits, frag/flag mismatch, seq on an
    unreliable frame ...) must be mutated *with a valid CRC*, otherwise the
    receiver stops at the CRC check and the vector never reaches the rule it
    exists to pin.  The record notes this explicitly so nobody mistakes the
    vector for a bit-flip test -- the ``mal-crc-*`` vectors are the ones that
    leave the CRC broken.
    """
    if len(buf) < C.HEADER_LEN:
        return buf
    csum_kind = buf[31]
    if csum_kind != C.CSUM_CCITT:
        return buf
    payload_len, = struct.unpack_from("<H", buf, 28)
    if len(buf) != C.HEADER_LEN + payload_len + 2:
        return buf
    body = buf[: C.HEADER_LEN + payload_len]
    return body + struct.pack("<H", C.crc16_ccitt(body))


# ---------------------------------------------------------------------------
# 02-9(1)  valid frames, >= 2 per message type
# ---------------------------------------------------------------------------


def gen_valid() -> None:
    S1 = ["02-wire-protocol.md 1"]
    S2 = ["02-wire-protocol.md 2"]
    S3 = ["02-wire-protocol.md 3"]
    S5 = ["02-wire-protocol.md 5"]
    S6 = ["02-wire-protocol.md 6"]
    S7 = ["02-wire-protocol.md 7"]
    S8 = ["02-wire-protocol.md 8"]
    S10 = ["02-wire-protocol.md 10"]
    ABIN = ["01-abi.md node hash"]
    T3 = ["04-transports.md 3"]
    T4 = ["04-transports.md 4"]
    T5 = ["04-transports.md 5"]
    U3 = ["05-userspace.md 3"]

    def valid(
        vid: str,
        title: str,
        sections: Sequence[str],
        transport: str,
        *,
        rx: str = "peer_udp",
        tlv: bool = False,
        assumptions: Sequence[str] = (),
        notes: str = "",
        **frame_kw,
    ) -> Vec:
        raw = C.encode_frame(**frame_kw)
        v = add(
            Vec(
                vid,
                "valid",
                title,
                list(sections),
                transport=transport,
                rx=rx,
                assumptions=assumptions,
                notes=notes,
                raw=raw,
                payload_view={"kind": "tlv"} if tlv else None,
            )
        )
        res = v.evaluate()
        check(res["accept"], f"{vid}: legal frame was not accepted ({res['reason']})")
        check(res["counter"] == "rx_frames", f"{vid}: unexpected counter {res['counter']}")
        fr: C.Frame = res["frame"]  # type: ignore[assignment]
        check(
            fr.wire_len <= C.TRANSPORT_MTU[C.TRANSPORT_ID[transport]],
            f"{vid}: {fr.wire_len} B exceeds the {transport} MTU",
        )
        v.expect = {
            "frame": {"accept": True, "reason": C.R_OK},
            "dispatch": {
                "action": "accept",
                "reason": C.R_OK,
                "counters": ["rx_frames"],
                "errno": None,
                "reply": None,
            },
        }
        return v

    # ---- HELLO ----------------------------------------------------------
    valid(
        "valid-hello-default-01",
        "A -> B HELLO over UDP, DEFAULT tier, CAP_RELIABLE",
        S1 + S2 + S6 + S7 + ABIN + T3,
        "udp",
        rx="peer_udp_hello",
        type=C.TYPE_HELLO,
        src_hash=H_A,
        dst_hash=H_B,
        payload=hello_payload(NODE_A, C.PROFILE_DEFAULT, C.CAP_RELIABLE, NONCE_A, link_addr_len=6),
        ttl=C.DEFAULT_TTL,
        csum_kind=C.CSUM_CCITT,
    )
    valid(
        "valid-hello-default-02",
        "B -> A HELLO, SERVER tier with CAP_RELAY|CAP_RELIABLE",
        S1 + S2 + S6 + ABIN,
        "udp",
        rx="head_udp_hello",
        type=C.TYPE_HELLO,
        src_hash=H_B,
        dst_hash=H_A,
        payload=hello_payload(NODE_B, C.PROFILE_SERVER, C.CAP_RELIABLE | C.CAP_RELAY, NONCE_B, link_addr_len=6),
        csum_kind=C.CSUM_CCITT,
    )
    valid(
        "valid-hello-default-03",
        "A -> B HELLO with csum_kind=0 (CRC optional off UDP)",
        S1 + S2 + S6 + T3,
        "udp",
        rx="peer_udp_hello",
        type=C.TYPE_HELLO,
        src_hash=H_A,
        dst_hash=H_B,
        payload=hello_payload(NODE_A, C.PROFILE_DEFAULT, C.CAP_RELIABLE, NONCE_A),
        csum_kind=C.CSUM_NONE,
        notes="02-1 csum_kind=0: no CRC trailer, wire_len == 32 + payload_len.",
    )
    valid(
        "valid-hello-uart-01",
        "leaf -> A HELLO over UART, short_addr 0xFFFF (unassigned power-on value)",
        S1 + S2 + S6 + ABIN + T4,
        "uart",
        rx="head_uart_hello",
        type=C.TYPE_HELLO,
        src_hash=H_LEAF,
        dst_hash=H_A,
        payload=hello_payload(
            LEAF, C.PROFILE_MCU, C.CAP_LEAF, NONCE_LEAF, short_addr=0xFFFF, link_addr_len=2
        ),
        csum_kind=C.CSUM_CCITT,
        notes="04-4: csum_kind=1 is mandatory on UART; 0xFFFF short_addr means "
        "unassigned and only HELLO may be sent.",
    )
    valid(
        "valid-hello-ack-default-01",
        "B -> A HELLO_ACK confirming the v0 intersection",
        S1 + S2 + S6 + S7,
        "udp",
        rx="head_udp",
        type=C.TYPE_HELLO_ACK,
        src_hash=H_B,
        dst_hash=H_A,
        payload=hello_payload(NODE_B, C.PROFILE_DEFAULT, C.CAP_RELIABLE, NONCE_B),
        csum_kind=C.CSUM_CCITT,
    )
    valid(
        "valid-hello-ack-uart-01",
        "A -> leaf HELLO_ACK with short_addr 0x0007 assigned by the head",
        S1 + S2 + S6 + T4,
        "uart",
        rx="leaf_uart",
        type=C.TYPE_HELLO_ACK,
        src_hash=H_A,
        dst_hash=H_LEAF,
        payload=hello_payload(
            NODE_A,
            C.PROFILE_SERVER,
            C.CAP_RELAY | C.CAP_RELIABLE,
            NONCE_A,
            short_addr=0x0007,
            link_addr_len=2,
        ),
        csum_kind=C.CSUM_CCITT,
        notes="02-6: UART tier also completes short address allocation; the head "
        "assigns the leaf's 16-bit link address here.",
    )

    # ---- PING / PONG ----------------------------------------------------
    valid(
        "valid-ping-01",
        "A -> B PING, 8 B monotonic timestamp (us), ttl 8",
        S1 + S2 + S6,
        "udp",
        rx="peer_udp",
        type=C.TYPE_PING,
        src_hash=H_A,
        dst_hash=H_B,
        payload=struct.pack("<Q", 1_234_567),
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-05b"],
        notes="02-2/02-6: PING every 1 s while UP/SUSPECT; the timestamp is a "
        "local monotonic clock reading in microseconds, little-endian.",
    )
    valid(
        "valid-ping-02",
        "B -> A PING at t = 1 s, ttl 8",
        S1 + S2 + S6,
        "udp",
        rx="head_udp",
        type=C.TYPE_PING,
        src_hash=H_B,
        dst_hash=H_A,
        payload=struct.pack("<Q", 1_000_000),
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-05b"],
    )
    valid(
        "valid-pong-01",
        "B -> A PONG echoing valid-ping-01 verbatim",
        S1 + S2 + S6,
        "udp",
        rx="head_udp",
        type=C.TYPE_PONG,
        src_hash=H_B,
        dst_hash=H_A,
        payload=struct.pack("<Q", 1_234_567),
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-05b"],
        notes="02-2: PONG echoes the PING payload, so the two frames differ only "
        "in type, src/dst and seq of the header.",
    )
    valid(
        "valid-pong-02",
        "B -> A PONG with ttl=1 (last hop budget)",
        S1 + S6,
        "udp",
        rx="head_udp",
        type=C.TYPE_PONG,
        src_hash=H_B,
        dst_hash=H_A,
        payload=struct.pack("<Q", 1_000_000),
        ttl=1,
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-13"],
        notes="A-13: the TTL decrement belongs to the forwarding hop, so a frame "
        "arriving with ttl=1 is still delivered.",
    )

    # ---- SEND -----------------------------------------------------------
    valid(
        "valid-send-01",
        "A -> B SEND, txid 0, application TLV payload (unreliable)",
        S1 + S2 + S3 + U3,
        "udp",
        rx="peer_udp",
        tlv=True,
        type=C.TYPE_SEND,
        src_hash=H_A,
        dst_hash=H_B,
        payload=app_payload(TLV_ECHO_REQ),
        csum_kind=C.CSUM_CCITT,
    )
    valid(
        "valid-send-02",
        "A -> B SEND RELIABLE seq=1, payload starts with the 4 B cumulative ACK",
        S1 + S2 + S3 + S7 + U3,
        "udp",
        rx="peer_udp",
        tlv=True,
        type=C.TYPE_SEND,
        src_hash=H_A,
        dst_hash=H_B,
        payload=reliable_payload(7, TLV_ECHO_REQ),
        flags=C.FLAG_RELIABLE,
        seq=1,
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-05"],
    )
    valid(
        "valid-send-broadcast-01",
        "A -> broadcast SEND, SERVER tier only, csum_kind=0",
        S1 + S2 + S3 + ABIN,
        "udp",
        rx="peer_udp",
        type=C.TYPE_SEND,
        src_hash=H_A,
        dst_hash=H_BCAST,
        payload=app_payload(TLV_CLUSTERD),
        flags=C.FLAG_BROADCAST,
        csum_kind=C.CSUM_NONE,
        notes="01-abi: BROADCAST node id is only legal on SERVER tier SEND; "
        "dst_hash is fnv1a32(0xff*16) = 0x360779f5.",
    )

    # ---- CALL / CALL_REPLY ---------------------------------------------
    valid(
        "valid-call-01",
        "A -> B CALL txid=1000 seq=1 slot=3, reliable, TLV request",
        S1 + S2 + S3 + S4 if False else S1 + S2 + S3 + ["02-wire-protocol.md 4"] + U3,
        "udp",
        rx="peer_udp",
        tlv=True,
        type=C.TYPE_CALL,
        src_hash=H_A,
        dst_hash=H_B,
        payload=reliable_payload(7, TLV_DOT_REQ),
        flags=C.FLAG_RELIABLE,
        txid=1000,
        dst_slot=3,
        seq=1,
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-05"],
    )
    valid(
        "valid-call-02",
        "A -> B CALL txid=0xFFFFFFFF (txid upper boundary), seq=2",
        S1 + S2 + S3 + ["02-wire-protocol.md 4"],
        "udp",
        rx="peer_udp",
        tlv=True,
        type=C.TYPE_CALL,
        src_hash=H_A,
        dst_hash=H_B,
        payload=reliable_payload(7, TLV_ECHO_REQ),
        flags=C.FLAG_RELIABLE,
        txid=0xFFFFFFFF,
        dst_slot=3,
        seq=2,
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-05"],
        notes="02-4: txid is allocated monotonically per endpoint and skips "
        "in-flight values on wraparound.",
    )
    # A 4300 B jobd shard (05-4 JOB TLV: 0x0101 job_id, 0x0103 payload,
    # 0x0104 data_shards) splits into 1438 + 1438 + 1424 on UDP -- 02-5's
    # "fragment payload = MTU - 32 - 2" and the "64 KiB ~= 46 fragments"
    # arithmetic both hold.
    job_shard = bytes((i * 7 + 3) & 0xFF for i in range(4272))
    big = app_payload(
        [
            (0x0101, struct.pack("<Q", 0x1122334455667788)),
            (0x0103, b"ops.dot"),
            (0x0104, job_shard),
        ]
    )
    check(len(big) == 4300, f"fragmented CALL payload is {len(big)} B, expected 4300")
    chunks = C.fragment_payload(big, "udp")
    check([len(c) for c in chunks] == [1438, 1438, 1424], "unexpected fragment sizes")
    for idx, chunk in enumerate(chunks[:3]):
        last = idx == len(chunks[:3]) - 1
        valid(
            f"valid-call-frag-{idx + 1:02d}",
            f"A -> B CALL txid=1001 seq=3 fragment {idx} "
            f"({len(chunk)} B, {'last' if last else 'not last'})",
            S1 + S2 + S3 + S5,
            "udp",
            type=C.TYPE_CALL,
            src_hash=H_A,
            dst_hash=H_B,
            payload=chunk,
            flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED,
            frag=C.frag_field(idx, last),
            txid=1001,
            dst_slot=3,
            seq=3,
            csum_kind=C.CSUM_CCITT,
            assumptions=["A-04", "A-05"],
            notes="02-5: fragment payload is exactly MTU-32-2 = 1438 B and no ACK "
            "piggyback rides inside a fragment (A-05).",
        )
    big_reply = app_payload([(0x0103, bytes((i * 11 + 5) & 0xFF for i in range(1500)))])
    check(len(big_reply) == 1504, f"fragmented REPLY payload is {len(big_reply)} B")
    reply_chunks = C.fragment_payload(big_reply, "udp")
    for idx, chunk in enumerate(reply_chunks):
        last = idx == len(reply_chunks) - 1
        valid(
            f"valid-call-reply-frag-{idx + 1:02d}",
            f"B -> A CALL_REPLY txid=2001 seq=5 fragment {idx} "
            f"({len(chunk)} B, {'last' if last else 'not last'})",
            S1 + S2 + S5 + S7,
            "udp",
            type=C.TYPE_CALL_REPLY,
            src_hash=H_B,
            dst_hash=H_A,
            payload=chunk,
            flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED,
            frag=C.frag_field(idx, last),
            txid=2001,
            seq=5,
            csum_kind=C.CSUM_CCITT,
            assumptions=["A-04", "A-05"],
        )
    valid(
        "valid-call-reply-01",
        "B -> A CALL_REPLY txid=1000 seq=1, TLV result",
        S1 + S2 + S7 + U3,
        "udp",
        rx="head_udp",
        tlv=True,
        type=C.TYPE_CALL_REPLY,
        src_hash=H_B,
        dst_hash=H_A,
        payload=reliable_payload(1, TLV_ECHO_RESP),
        flags=C.FLAG_RELIABLE,
        txid=1000,
        seq=1,
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-05"],
        notes="02-4: the REPLY carries the CALL's txid; 02-4 also requires the "
        "service side to answer a retransmitted CALL from its cache without "
        "executing twice.",
    )
    valid(
        "valid-call-reply-02",
        "B -> A CALL_REPLY txid=0xFFFFFFFF seq=2, csum_kind=0",
        S1 + S2 + S7,
        "udp",
        rx="head_udp",
        tlv=True,
        type=C.TYPE_CALL_REPLY,
        src_hash=H_B,
        dst_hash=H_A,
        payload=reliable_payload(2, TLV_ECHO_RESP),
        flags=C.FLAG_RELIABLE,
        txid=0xFFFFFFFF,
        seq=2,
        csum_kind=C.CSUM_NONE,
        assumptions=["A-05"],
    )

    # ---- CLOSE ----------------------------------------------------------
    valid(
        "valid-close-01",
        "B -> A CLOSE slot=3, empty payload",
        S1 + S2 + S8,
        "udp",
        rx="head_udp",
        type=C.TYPE_CLOSE,
        src_hash=H_B,
        dst_hash=H_A,
        dst_slot=3,
        csum_kind=C.CSUM_CCITT,
        notes="02-8: the proxy endpoint calls peer_shutdown, later local calls "
        "get A20_ERR_REMOTE_CLOSED.",
    )
    valid(
        "valid-close-02",
        "A -> B CLOSE slot=3, ttl=1, csum_kind=0",
        S1 + S2 + S8,
        "udp",
        rx="peer_udp",
        type=C.TYPE_CLOSE,
        src_hash=H_A,
        dst_hash=H_B,
        dst_slot=3,
        ttl=1,
        csum_kind=C.CSUM_NONE,
        assumptions=["A-13"],
    )

    # ---- ACK / NACK -----------------------------------------------------
    valid(
        "valid-ack-01",
        "A -> B bare ACK cumulative_seq=3",
        S1 + S2 + S7 + S10,
        "udp",
        rx="peer_udp",
        type=C.TYPE_ACK,
        src_hash=H_A,
        dst_hash=H_B,
        payload=struct.pack("<I", 3),
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-11"],
        notes="02-7: bare ACK goes out every 200 ms or every 8 frames when "
        "there is no reverse traffic; a bare ACK does not consume a seq (A-11).",
    )
    valid(
        "valid-ack-02",
        "B -> A bare ACK cumulative_seq=0 (nothing acknowledged yet)",
        S1 + S2 + S7,
        "udp",
        rx="head_udp",
        type=C.TYPE_ACK,
        src_hash=H_B,
        dst_hash=H_A,
        payload=struct.pack("<I", 0),
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-11", "A-14"],
        notes="A-14: cumulative_seq=0 is ambiguous -- it also means 'seq 0 "
        "received' because 02-7 restarts seq at 0. See README.",
    )
    valid(
        "valid-nack-01",
        "A -> B NACK missing_seq=7 (optional gap acceleration)",
        S1 + S2 + S7,
        "udp",
        rx="peer_udp",
        type=C.TYPE_NACK,
        src_hash=H_A,
        dst_hash=H_B,
        payload=struct.pack("<I", 7),
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-11"],
        notes="02-2/02-7: NACK is an optional optimisation; v0 may implement "
        "ACK + timeout retransmission only.",
    )
    valid(
        "valid-nack-02",
        "B -> A NACK missing_seq=0xFFFFFFFF",
        S1 + S2 + S7,
        "udp",
        rx="head_udp",
        type=C.TYPE_NACK,
        src_hash=H_B,
        dst_hash=H_A,
        payload=struct.pack("<I", 0xFFFFFFFF),
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-11"],
    )

    # ---- ERROR ----------------------------------------------------------
    valid(
        "valid-error-01",
        "B -> A ERROR NOT_FOUND(24) orig_txid=1002 (slot lookup miss)",
        S1 + S2 + ["02-wire-protocol.md 4"] + S8 + ["01-abi.md errnos"],
        "udp",
        rx="head_udp",
        type=C.TYPE_ERROR,
        src_hash=H_B,
        dst_hash=H_A,
        payload=struct.pack("<II", C.ERRNO["A20_ERR_NOT_FOUND"], 1002),
        csum_kind=C.CSUM_CCITT,
        assumptions=["A-08"],
        notes="02-4: a slot/service lookup failure answers ERROR(errno, "
        "orig_txid) so the caller thread gets the errno.",
    )
    valid(
        "valid-error-02",
        "B -> A ERROR CLUSTER_UNSUPPORTED(29) orig_txid=0xFFFFFFFF, csum_kind=0",
        S1 + S2 + S8,
        "udp",
        rx="head_udp",
        type=C.TYPE_ERROR,
        src_hash=H_B,
        dst_hash=H_A,
        payload=struct.pack(
            "<II", C.ERRNO["A20_ERR_CLUSTER_UNSUPPORTED"], 0xFFFFFFFF
        ),
        csum_kind=C.CSUM_NONE,
        assumptions=["A-08"],
    )


# ---------------------------------------------------------------------------
# 02-9(2)  malformed frames
# ---------------------------------------------------------------------------


def gen_malformed() -> None:
    S1 = ["02-wire-protocol.md 1"]
    S2 = ["02-wire-protocol.md 2"]
    S3 = ["02-wire-protocol.md 3"]
    S4 = ["02-wire-protocol.md 4"]
    S5 = ["02-wire-protocol.md 5"]
    S6 = ["02-wire-protocol.md 6"]
    S7 = ["02-wire-protocol.md 7"]
    S8 = ["02-wire-protocol.md 8"]
    S10 = ["02-wire-protocol.md 10"]
    ABIN = ["01-abi.md node identity"]
    ABINH = ["01-abi.md node hash"]
    T2 = ["04-transports.md 2"]
    T3 = ["04-transports.md 3"]
    T4 = ["04-transports.md 4"]

    base_ping = C.encode_frame(
        type=C.TYPE_PING,
        src_hash=H_A,
        dst_hash=H_B,
        payload=struct.pack("<Q", 1_234_567),
        csum_kind=C.CSUM_CCITT,
    )
    base_send = C.encode_frame(
        type=C.TYPE_SEND,
        src_hash=H_A,
        dst_hash=H_B,
        payload=app_payload(TLV_ECHO_REQ),
        csum_kind=C.CSUM_CCITT,
    )
    base_hello = C.encode_frame(
        type=C.TYPE_HELLO,
        src_hash=H_A,
        dst_hash=H_B,
        payload=hello_payload(NODE_A, C.PROFILE_DEFAULT, C.CAP_RELIABLE, NONCE_A),
        csum_kind=C.CSUM_CCITT,
    )
    base_frag = C.encode_frame(
        type=C.TYPE_CALL,
        src_hash=H_A,
        dst_hash=H_B,
        payload=bytes(C.UDP_FRAGMENT_PAYLOAD),
        flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED,
        frag=C.frag_field(2, True),
        txid=1001,
        dst_slot=3,
        seq=3,
        csum_kind=C.CSUM_CCITT,
    )

    def mal(
        vid: str,
        title: str,
        sections: Sequence[str],
        base: bytes,
        transport: str,
        *,
        muts: Sequence[Dict[str, object]] = (),
        raw: Optional[bytes] = None,
        fix: bool = True,
        rx: str = "peer_udp",
        layer: str = "frame",
        reason: str,
        counter: Optional[str],
        errno: Optional[str] = None,
        reply: Optional[str] = None,
        accept: bool = False,
        assumptions: Sequence[str] = (),
        notes: str = "",
        ctx: Optional[Dict[str, object]] = None,
        payload_view: Optional[Dict[str, object]] = None,
        expect_reason_override: Optional[str] = None,
    ) -> Vec:
        if raw is None:
            raw, records = mutate(base, *muts)
            if fix:
                fixed = fix_crc(raw)
                if fixed != raw:
                    records.append(
                        rec(
                            "recrc",
                            "02-1",
                            "CRC recomputed so the frame reaches the rule under test",
                        )
                    )
                    raw = fixed
        else:
            records = list(muts)
        v = add(
            Vec(
                vid,
                "malformed",
                title,
                list(sections),
                transport=transport,
                rx=rx,
                assumptions=assumptions,
                notes=notes,
                raw=raw,
                mutations=records,
                ctx=ctx,
                payload_view=payload_view,
            )
        )
        res = v.evaluate()
        got_reason = res["reason"]
        want_reason = expect_reason_override or reason
        check(
            bool(res["accept"]) == accept,
            f"{vid}: accept={res['accept']}, expected {accept} (reason {got_reason})",
        )
        check(
            got_reason == want_reason,
            f"{vid}: reason={got_reason}, expected {want_reason}",
        )
        check(
            res["counter"] == counter,
            f"{vid}: counter={res['counter']}, expected {counter}",
        )
        if errno is not None:
            check(res["errno"] == errno, f"{vid}: errno={res['errno']}, expected {errno}")
        v.expect = {
            "frame": {
                "accept": bool(res["frame"] is not None),
                "reason": res["frame_reason"],
            },
            "dispatch": {
                "layer": layer,
                "action": "accept" if accept else "drop",
                "reason": got_reason,
                "counters": [counter] if counter else [],
                "errno": errno,
                "reply": reply if reply else res["reply"],
            },
        }
        return v

    # -- magic -----------------------------------------------------------
    mal(
        "mal-magic-wrong-01",
        "magic 0x4C44 instead of 0x4C43",
        S1 + S10,
        base_ping,
        "udp",
        muts=[rec("set", "02-1", "wrong magic", offset=0, value=0x44)],
        reason=C.R_MAGIC,
        counter="rx_malformed",
    )
    mal(
        "mal-magic-swapped-02",
        "magic bytes written as 0x4C 0x43, i.e. u16 0x434C (big-endian spelling)",
        S1 + S10,
        base_ping,
        "udp",
        muts=[
            rec("set", "02-1", "byte-swapped magic", offset=0, value=0x4C),
            rec("set", "02-1", "byte-swapped magic", offset=1, value=0x43),
        ],
        reason=C.R_MAGIC,
        counter="rx_malformed",
        notes="02-1 says multi-byte fields are little-endian and magic is "
        "0x4C43, so a legal frame starts with the bytes 0x43 0x4C ('C','L'). "
        "This vector fails if either side spells the magic big-endian. It is "
        "the vector that settles the 'CL' vs 0x4C43 wording of 02-1.",
    )
    mal(
        "mal-magic-zero-03",
        "magic all zero (noise on the wire)",
        S1 + S10,
        base_ping,
        "udp",
        muts=[rec("set", "02-1", "magic zeroed", offset=0, value=0x00),
              rec("set", "02-1", "magic zeroed", offset=1, value=0x00)],
        reason=C.R_MAGIC,
        counter="rx_malformed",
    )
    # -- truncation / length --------------------------------------------
    mal(
        "mal-short-01",
        "only 20 bytes: shorter than the 32-byte header",
        S1 + S10,
        base_ping,
        "udp",
        muts=[rec("truncate", "02-1", "half a header", keep=20)],
        fix=False,
        reason=C.R_SHORT,
        counter="rx_malformed",
    )
    mal(
        "mal-short-02",
        "31 bytes: one byte short of a complete header",
        S1 + S10,
        base_ping,
        "udp",
        muts=[rec("truncate", "02-1", "one byte short", keep=31)],
        fix=False,
        reason=C.R_SHORT,
        counter="rx_malformed",
    )
    mal(
        "mal-len-under-01",
        "payload_len=8 but 20 payload bytes present (trailer lost)",
        S1 + S10,
        base_send,
        "udp",
        muts=[rec("set", "02-1", "payload_len too small", offset=28, value=8)],
        fix=False,
        reason=C.R_LEN,
        counter="rx_malformed",
        notes="02-1: payload_len not matching the length the transport actually "
        "received -> drop + rx_malformed.",
    )
    mal(
        "mal-hello-len-05",
        "HELLO with a 24 B payload although 02-6 fixes 32 B",
        S2 + S6 + S10,
        base_hello,
        "udp",
        raw=C.encode_frame(
            type=C.TYPE_HELLO,
            src_hash=H_A,
            dst_hash=H_B,
            payload=hello_payload(
                NODE_A, C.PROFILE_DEFAULT, C.CAP_RELIABLE, NONCE_A
            )[:24],
            csum_kind=C.CSUM_CCITT,
        ),
        reason=C.R_FIXED_LEN,
        counter="rx_malformed",
        notes="02-6 fixes the HELLO payload at 32 bytes. A second copy of this "
        "check lives in the HELLO validator (02-6 layer) and is unreachable "
        "from a single frame, which is intentional defence in depth.",
    )
    mal(
        "mal-len-trailing-01",
        "4 garbage bytes appended after the CRC trailer",
        S1 + S10,
        base_ping,
        "udp",
        muts=[rec("append", "02-1", "trailing garbage", bytes="deadbeef")],
        fix=False,
        reason=C.R_LEN,
        counter="rx_malformed",
    )
    mal(
        "mal-len-crc-missing-01",
        "csum_kind=1 but the 2 CRC bytes are absent",
        S1 + S10,
        base_ping,
        "udp",
        muts=[rec("drop_tail", "02-1", "CRC trailer missing", keep=C.HEADER_LEN + 8)],
        fix=False,
        reason=C.R_LEN,
        counter="rx_malformed",
    )
    over = C.encode_frame(
        type=C.TYPE_SEND,
        src_hash=H_A,
        dst_hash=H_B,
        payload=bytes(C.MAX_PAYLOAD["udp"] + 2),
        csum_kind=C.CSUM_CCITT,
    )
    mal(
        "mal-len-overflow-udp-01",
        "payload_len=1440 > UDP MTU - 32 - 2 = 1438 (full 1474 B datagram)",
        S1 + S10 + T3,
        over,
        "udp",
        fix=False,
        reason=C.R_OVERFLOW,
        counter="rx_malformed",
        notes="02-1 bounds payload_len by the transport MTU, so the decoder "
        "needs the receiving transport's MTU as an input (04-1 supplies it).",
    )
    # -- version ---------------------------------------------------------
    mal(
        "mal-ver-high-data-01",
        "ver=1 on a PING: a data frame is dropped silently",
        S1 + S6 + S10,
        base_ping,
        "udp",
        muts=[rec("set", "02-1", "ver above local", offset=2, value=1)],
        reason=C.R_VER,
        counter="rx_malformed",
        notes="02-1: magic/ver mismatch -> drop + count; a ver higher than local "
        "is handled in the HELLO phase, data frames are dropped silently.",
    )
    mal(
        "mal-ver-high-hello-02",
        "ver=1 on a HELLO: ERROR frame + hello_rejects",
        S1 + S6 + S10 + ["01-abi.md ABI stability"],
        base_hello,
        "udp",
        muts=[rec("set", "02-1", "ver above local", offset=2, value=1)],
        layer="hello",
        reason=C.H_NOT_NEGOTIABLE,
        counter="hello_rejects",
        errno="A20_ERR_CLUSTER_UNSUPPORTED",
        reply="ERROR(A20_ERR_CLUSTER_UNSUPPORTED, orig_txid=0)",
        notes="02-1 hands a higher ver to the HELLO phase; 02-6 turns the "
        "failure into an ERROR frame and state DOWN; 01-abi stability rules "
        "allow exactly a one-version compatibility window.",
    )
    # -- CRC -------------------------------------------------------------
    mal(
        "mal-crc-flip-01",
        "one bit flipped in the CRC trailer",
        S1 + S10 + T2,
        base_ping,
        "udp",
        muts=[rec("flip_bit", "02-1", "CRC bit flip", bit=(C.HEADER_LEN + 8) * 8 + 3)],
        fix=False,
        reason=C.R_CRC,
        counter="rx_malformed",
    )
    mal(
        "mal-crc-flip-payload-02",
        "one payload bit flipped, header untouched (loopback corrupt hook)",
        S1 + S10 + T2,
        base_send,
        "udp",
        muts=[rec("flip_bit", "02-1", "payload bit flip", bit=(C.HEADER_LEN + 6) * 8 + 5)],
        fix=False,
        reason=C.R_CRC,
        counter="rx_malformed",
        notes="04-2: the loopback corrupt hook flips a random payload bit "
        "together with csum_kind=1 to exercise the CRC path.",
    )
    mal(
        "mal-crc-flip-header-03",
        "one header bit flipped (ttl): proves the CRC covers the header",
        S1 + S10,
        base_ping,
        "udp",
        muts=[rec("flip_bit", "02-1", "header bit flip", bit=30 * 8 + 1)],
        fix=False,
        reason=C.R_CRC,
        counter="rx_malformed",
        notes="02-1 puts the CRC after the payload and says nothing about its "
        "extent; this vector pins it as covering the 32-byte header too, "
        "otherwise a corrupted ttl/flags would pass silently.",
    )
    mal(
        "mal-crc-swapped-04",
        "CRC trailer byte-swapped (big-endian CRC)",
        S1 + S10,
        base_ping,
        "udp",
        muts=[rec("set", "02-1", "CRC high byte first", offset=C.HEADER_LEN + 8, value=base_ping[C.HEADER_LEN + 9]),
              rec("set", "02-1", "CRC high byte first", offset=C.HEADER_LEN + 9, value=base_ping[C.HEADER_LEN + 8])],
        fix=False,
        reason=C.R_CRC,
        counter="rx_malformed",
    )
    mal(
        "mal-csum-kind-01",
        "csum_kind=2, which 02-1 does not define",
        S1 + S10,
        base_ping,
        "udp",
        muts=[rec("set", "02-1", "undefined csum_kind", offset=31, value=2)],
        reason=C.R_CSUM_KIND,
        counter="rx_malformed",
    )
    # -- type ------------------------------------------------------------
    mal(
        "mal-type-zero-01",
        "type=0 (no message type 0)",
        S2 + S10,
        base_ping,
        "udp",
        muts=[rec("set", "02-2", "type 0", offset=3, value=0)],
        reason=C.R_TYPE,
        counter="rx_malformed",
    )
    mal(
        "mal-type-undefined-02",
        "type=12, one past the last defined type",
        S2 + S10,
        base_ping,
        "udp",
        muts=[rec("set", "02-2", "type 12", offset=3, value=12)],
        reason=C.R_TYPE,
        counter="rx_malformed",
        notes="02-2 defines 1..11; nothing says what to do with 12..255. The "
        "conservative choice is to drop an undispatchable frame and count it.",
    )
    mal(
        "mal-type-high-03",
        "type=255",
        S2 + S10,
        base_ping,
        "udp",
        muts=[rec("set", "02-2", "type 255", offset=3, value=255)],
        reason=C.R_TYPE,
        counter="rx_malformed",
    )
    # -- frag / flags ----------------------------------------------------
    mal(
        "mal-frag-no-flag-01",
        "frag=0x0005 with FRAGMENTED clear",
        S1 + S3 + S5 + S10,
        base_ping,
        "udp",
        muts=[rec("set16", "02-3", "fragment index without the flag", offset=6, value=0x0005)],
        reason=C.R_FRAG,
        counter="rx_malformed",
    )
    mal(
        "mal-frag-last-no-flag-02",
        "frag bit15 (last) set with FRAGMENTED clear",
        S1 + S3 + S5 + S10,
        base_frag,
        "udp",
        muts=[rec("set16", "02-3", "last-fragment flag without FRAGMENTED", offset=6, value=0x8000),
              rec("set16", "02-3", "drop FRAGMENTED", offset=4, value=0x0001)],
        reason=C.R_FRAG,
        counter="rx_malformed",
    )
    mal(
        "mal-frag-single-03",
        "FRAGMENTED with frag index 0 and the last bit: a one-fragment message",
        S1 + S3 + S5 + S10,
        base_frag,
        "udp",
        muts=[rec("set", "02-5", "index 0 marked last", offset=6, value=0x00),
              rec("set", "02-5", "index 0 marked last", offset=7, value=0x80)],
        reason=C.R_FRAG,
        counter="rx_malformed",
        assumptions=["A-04"],
        notes="A-04: 02-5 makes the index start at 0 and increment, so a "
        "message that fits in one frame is sent with FRAGMENTED clear and "
        "frag=0; a FRAGMENTED frame with index 0 + last bit is contradictory.",
    )
    mal(
        "mal-seq-unreliable-01",
        "RELIABLE clear but seq=7",
        S1 + S3 + S7 + S10,
        base_ping,
        "udp",
        muts=[rec("set", "02-1", "seq on an unreliable frame", offset=24, value=7)],
        reason=C.R_SEQ,
        counter="rx_malformed",
        assumptions=["A-11"],
        notes="02-1: seq is the reliable-tier number and an unreliable frame "
        "puts 0; A-11 makes this per-frame rather than per-link.",
    )
    mal(
        "mal-send-txid-01",
        "SEND with txid=5 although 02-1 says SEND puts 0",
        S1 + S2 + S10,
        base_send,
        "udp",
        muts=[rec("set", "02-1", "txid on SEND", offset=8, value=5)],
        reason=C.R_TXID,
        counter="rx_malformed",
        assumptions=["A-15"],
        notes="A-15: 02-1 states this as a send-side rule; this vector pins the "
        "receiver-side reading (reject + rx_malformed). The alternative is to "
        "ignore it for forward compatibility.",
    )
    # -- fixed payload sizes (02-2 column) -------------------------------
    mal(
        "mal-close-payload-01",
        "CLOSE with a 4 B payload although 02-2 says the payload is empty",
        S2 + S8 + S10,
        C.encode_frame(
            type=C.TYPE_CLOSE,
            src_hash=H_A,
            dst_hash=H_B,
            dst_slot=3,
            payload=bytes(4),
            csum_kind=C.CSUM_CCITT,
        ),
        "udp",
        reason=C.R_FIXED_LEN,
        counter="rx_malformed",
        assumptions=["A-15"],
    )
    mal(
        "mal-ping-len-02",
        "PING with a 7 B payload although 02-2 fixes 8 B",
        S2 + S10,
        base_ping,
        "udp",
        raw=C.encode_frame(
            type=C.TYPE_PING,
            src_hash=H_A,
            dst_hash=H_B,
            payload=bytes(7),
            csum_kind=C.CSUM_CCITT,
        ),
        reason=C.R_FIXED_LEN,
        counter="rx_malformed",
        notes="the frame length is self-consistent (32 + 7 + 2), so this "
        "exercises the fixed payload size column of 02-2 rather than the "
        "payload_len check.",
    )
    mal(
        "mal-ack-len-03",
        "ACK with a 3 B payload (02-2 fixes 4 B)",
        S2 + S7 + S10,
        C.encode_frame(
            type=C.TYPE_ACK,
            src_hash=H_A,
            dst_hash=H_B,
            payload=bytes(3),
            csum_kind=C.CSUM_CCITT,
        ),
        "udp",
        reason=C.R_FIXED_LEN,
        counter="rx_malformed",
    )
    mal(
        "mal-error-len-04",
        "ERROR with a 7 B payload (02-2 fixes 4 B errno + 4 B orig_txid)",
        S2 + S8 + S10,
        C.encode_frame(
            type=C.TYPE_ERROR,
            src_hash=H_B,
            dst_hash=H_A,
            payload=bytes(7),
            csum_kind=C.CSUM_CCITT,
        ),
        "udp",
        reason=C.R_FIXED_LEN,
        counter="rx_malformed",
    )
    # -- layer 2: addressing / TTL / profile -----------------------------
    mal(
        "mal-dst-local-01",
        "dst_hash = 0x69691905 = fnv1a32(all-zero) = A20_NODE_ID_LOCAL",
        ABIN + S1 + S10,
        base_ping,
        "udp",
        raw=C.encode_frame(
            type=C.TYPE_PING,
            src_hash=H_A,
            dst_hash=H_LOCAL,
            payload=struct.pack("<Q", 1_234_567),
            csum_kind=C.CSUM_CCITT,
        ),
        layer="dispatch",
        reason=C.D_LOCAL_DST,
        counter="rx_drops",
        notes="01-abi: addressing (LOCAL, slot) must take the local fast path "
        "and must never be serialized, so such a frame is a protocol violation.",
    )
    mal(
        "mal-ttl-zero-01",
        "ttl=0: the hop budget is exhausted",
        S1 + S10 + ["01-abi.md errnos"],
        base_ping,
        "udp",
        raw=C.encode_frame(
            type=C.TYPE_PING,
            src_hash=H_A,
            dst_hash=H_B,
            payload=struct.pack("<Q", 1_234_567),
            ttl=0,
            csum_kind=C.CSUM_CCITT,
        ),
        layer="dispatch",
        reason=C.D_TTL,
        counter="rx_drops",
        errno="A20_ERR_NODE_UNREACHABLE",
        assumptions=["A-13"],
        notes="02-1: decrement per hop, drop at 0; 01-abi maps TTL exhaustion "
        "to A20_ERR_NODE_UNREACHABLE.",
    )
    bcast = C.encode_frame(
        type=C.TYPE_SEND,
        src_hash=H_A,
        dst_hash=H_BCAST,
        payload=app_payload(TLV_CLUSTERD),
        flags=C.FLAG_BROADCAST,
        csum_kind=C.CSUM_CCITT,
    )
    mal(
        "mal-broadcast-mcu-01",
        "BROADCAST SEND received on an MCU tier link",
        ABIN + S3 + S5,
        bcast,
        "uart",
        rx="leaf_uart",
        layer="dispatch",
        reason=C.D_BCAST_PROFILE,
        counter="rx_drops",
        notes="01-abi: an MCU tier node receiving BROADCAST must drop it; "
        "02-5 maps an MCU tier broadcast attempt to CLUSTER_UNSUPPORTED.",
    )
    mal(
        "mal-broadcast-non-send-02",
        "BROADCAST flag on a CALL (02-3 allows it on SEND only)",
        S3 + S10,
        C.encode_frame(
            type=C.TYPE_CALL,
            src_hash=H_A,
            dst_hash=H_BCAST,
            payload=reliable_payload(0, TLV_ECHO_REQ),
            flags=C.FLAG_BROADCAST | C.FLAG_RELIABLE,
            txid=1000,
            dst_slot=3,
            seq=1,
            csum_kind=C.CSUM_CCITT,
        ),
        "udp",
        layer="dispatch",
        reason=C.D_BCAST_TYPE,
        counter="rx_drops",
        assumptions=["A-05"],
    )
    mal(
        "mal-reliable-no-cap-01",
        "RELIABLE frame from a peer that never advertised CAP_RELIABLE",
        S3 + S6 + S10,
        C.encode_frame(
            type=C.TYPE_CALL,
            src_hash=H_A,
            dst_hash=H_B,
            payload=reliable_payload(0, TLV_ECHO_REQ),
            flags=C.FLAG_RELIABLE,
            txid=1000,
            dst_slot=3,
            seq=1,
            csum_kind=C.CSUM_CCITT,
        ),
        "udp",
        ctx={"peer_caps": C.CAP_LEAF},
        layer="dispatch",
        reason=C.D_RELIABLE_NO_CAP,
        counter="rx_drops",
        notes="02-3: RELIABLE may only be set when both ends negotiated "
        "CAP_RELIABLE in HELLO.",
    )
    mal(
        "mal-frag-mcu-01",
        "FRAGMENTED frame received on an MCU tier link",
        S5 + T4,
        C.encode_frame(
            type=C.TYPE_CALL,
            src_hash=H_A,
            dst_hash=H_LEAF,
            payload=bytes(200),
            flags=C.FLAG_FRAGMENTED,
            frag=C.frag_field(1, False),
            txid=1001,
            dst_slot=1,
            csum_kind=C.CSUM_CCITT,
        ),
        "uart",
        rx="leaf_uart",
        layer="dispatch",
        reason=C.D_FRAG_PROFILE,
        counter="rx_drops",
        notes="02-5: the MCU tier forbids fragmentation, FRAGMENTED frames are "
        "dropped and counted. The frame is 234 B so it fits the 256 B UART MTU "
        "and the profile rule is what rejects it, not the length bound.",
    )
    mal(
        "mal-leaf-wrong-type-02",
        "SEND received by an MCU leaf",
        T4,
        C.encode_frame(
            type=C.TYPE_SEND,
            src_hash=H_A,
            dst_hash=H_LEAF,
            payload=app_payload(TLV_ECHO_REQ)[:64],
            csum_kind=C.CSUM_CCITT,
        ),
        "uart",
        rx="leaf_uart",
        layer="dispatch",
        reason=C.D_WRONG_STATE,
        counter="rx_drops",
        notes="04-4: on receiving SEND/ACK/NACK/fragment frames -- drop and "
        "count, not treated as an error.",
    )
    mal(
        "mal-leaf-ack-03",
        "bare ACK received by an MCU leaf",
        T4 + S7,
        C.encode_frame(
            type=C.TYPE_ACK,
            src_hash=H_A,
            dst_hash=H_LEAF,
            payload=struct.pack("<I", 3),
            csum_kind=C.CSUM_CCITT,
        ),
        "uart",
        rx="leaf_uart",
        layer="dispatch",
        reason=C.D_WRONG_STATE,
        counter="rx_drops",
    )
    mal(
        "mal-leaf-ping-down-04",
        "PING while the leaf state machine is DISCONNECTED",
        T4 + S6,
        base_ping,
        "uart",
        rx="leaf_uart_down",
        layer="dispatch",
        reason=C.D_WRONG_STATE,
        counter="rx_drops",
        notes="04-4: the leaf has only DISCONNECTED/UP; after 50 s without a PING "
        "the short address is invalidated and other traffic is refused.",
    )
    # -- ERROR handling ---------------------------------------------------
    mal(
        "mal-error-unknown-errno-01",
        "ERROR carrying errno 999, which 02-8 maps to CLUSTER_UNSUPPORTED",
        S8,
        C.encode_frame(
            type=C.TYPE_ERROR,
            src_hash=H_B,
            dst_hash=H_A,
            payload=struct.pack("<II", 999, 1000),
            csum_kind=C.CSUM_CCITT,
        ),
        "udp",
        rx="head_udp",
        ctx={"inflight_txids": {1000}},
        layer="error",
        reason=C.E_UNKNOWN_ERRNO,
        counter="rx_frames",
        errno="A20_ERR_CLUSTER_UNSUPPORTED",
        accept=True,
        notes="02-8: an unknown code is treated as A20_ERR_CLUSTER_UNSUPPORTED, "
        "so the frame is delivered rather than dropped.",
    )
    mal(
        "mal-error-stale-txid-02",
        "ERROR whose orig_txid is no longer in flight (late reply)",
        S4 + S8 + S10,
        C.encode_frame(
            type=C.TYPE_ERROR,
            src_hash=H_B,
            dst_hash=H_A,
            payload=struct.pack("<II", C.ERRNO["A20_ERR_CLUSTER_TIMEOUT"], 1000),
            csum_kind=C.CSUM_CCITT,
        ),
        "udp",
        rx="head_udp",
        ctx={"inflight_txids": {1001, 1002}},
        layer="error",
        reason=C.E_NO_TRANSACTION,
        counter="rx_drops",
        notes="02-4: a late REPLY finds no transaction by txid -> drop and count; "
        "the ERROR reply follows the same rule.",
    )
    # -- HELLO validation (02-6) -----------------------------------------
    def hello_bad(
        vid: str,
        title: str,
        sections: Sequence[str],
        *,
        reason: str,
        counter: str,
        payload: Optional[bytes] = None,
        src_hash: int = H_A,
        node: bytes = NODE_A,
        ctx: Optional[Dict[str, object]] = None,
        errno: Optional[str] = None,
        reply: Optional[str] = None,
        assumptions: Sequence[str] = (),
        notes: str = "",
    ) -> Vec:
        raw = C.encode_frame(
            type=C.TYPE_HELLO,
            src_hash=src_hash,
            dst_hash=H_B,
            payload=payload
            if payload is not None
            else hello_payload(node, C.PROFILE_DEFAULT, C.CAP_RELIABLE, NONCE_A),
            csum_kind=C.CSUM_CCITT,
        )
        return mal(
            vid,
            title,
            sections,
            raw,
            "udp",
            rx="peer_udp_hello",
            layer="hello",
            reason=reason,
            counter=counter,
            errno=errno,
            reply=reply,
            ctx=ctx,
            assumptions=assumptions,
            notes=notes,
        )

    supp = "ERROR(A20_ERR_CLUSTER_UNSUPPORTED, orig_txid=0)"
    hello_bad(
        "mal-hello-hash-mismatch-01",
        "HELLO whose header src_hash is not fnv1a32(node_id) in the payload",
        ABINH + S6 + S10,
        reason=C.H_HASH_MISMATCH,
        counter="hello_rejects",
        errno="A20_ERR_CLUSTER_UNSUPPORTED",
        reply=supp,
        src_hash=H_B,
        notes="02-1 carries fnv1a32(src node id); 02-6 makes the hash the "
        "negotiation key, so a HELLO whose header hash disagrees with the full "
        "id it carries is corrupt or forged.",
    )
    hello_bad(
        "mal-hello-hash-clash-02",
        "HELLO whose node id hashes onto an already-linked hash with a different full id",
        ABINH + S6 + S10,
        reason=C.H_HASH_CLASH,
        counter="hello_rejects",
        errno="A20_ERR_CLUSTER_UNSUPPORTED",
        reply=supp,
        ctx={"known_hashes": {H_B: NODE_A}},
        payload=hello_payload(NODE_B, C.PROFILE_DEFAULT, C.CAP_RELIABLE, NONCE_B),
        src_hash=H_B,
        notes="01-abi: two ends whose full ids differ but whose hashes are "
        "equal -> the later one is rejected with an ERROR frame; v0 does not "
        "re-address online. The clash is injected through the receiver's table "
        "(hash H_B is already held for a different full id) rather than by "
        "finding a real FNV-1a32 collision, which is not constructible -- the "
        "receiver-side check under test is identical either way.",
    )
    hello_bad(
        "mal-hello-caps-unknown-03",
        "HELLO caps=0x80 (an unknown capability bit)",
        ["01-abi.md capability bits"] + S6 + S10,
        reason=C.H_BAD_CAPS,
        counter="hello_rejects",
        errno="A20_ERR_CLUSTER_UNSUPPORTED",
        reply=supp,
        payload=hello_payload(NODE_A, C.PROFILE_DEFAULT, C.CAP_RELIABLE | 0x80, NONCE_A),
    )
    hello_bad(
        "mal-hello-relay-on-leaf-04",
        "HELLO from profile_tier=1 (MCU) that sets CAP_RELAY",
        S6 + ["01-abi.md capability bits"] + S10,
        reason=C.H_RELAY_ON_LEAF,
        counter="hello_rejects",
        errno="A20_ERR_CLUSTER_UNSUPPORTED",
        reply=supp,
        payload=hello_payload(LEAF, C.PROFILE_MCU, C.CAP_LEAF | C.CAP_RELAY, NONCE_LEAF, short_addr=0xFFFF, link_addr_len=2),
        src_hash=H_LEAF,
        notes="02-6 explicitly: caps must agree with the tier, a leaf must not "
        "set RELAY.",
    )
    hello_bad(
        "mal-hello-self-loop-05",
        "HELLO carrying our own nonce",
        S6 + S10,
        reason=C.H_SELF_LOOP,
        counter="hello_rejects",
        errno=None,
        reply=None,
        ctx={"local_nonce": NONCE_A},
        assumptions=["A-14"],
        notes="02-6: a HELLO with our own nonce is a self-loop and is rejected. "
        "A-14: no ERROR frame is sent back, because the frame came from us.",
    )
    hello_bad(
        "mal-hello-node-local-06",
        "HELLO whose node_id is all zero (A20_NODE_ID_LOCAL)",
        ABIN + S6 + S10,
        reason=C.H_RESERVED_NODE_ID,
        counter="hello_rejects",
        errno="A20_ERR_CLUSTER_UNSUPPORTED",
        reply=supp,
        node=C.NODE_ID_LOCAL,
        src_hash=H_LOCAL,
    )
    hello_bad(
        "mal-hello-node-broadcast-07",
        "HELLO whose node_id is all 0xff (A20_NODE_ID_BROADCAST)",
        ABIN + S6 + S10,
        reason=C.H_RESERVED_NODE_ID,
        counter="hello_rejects",
        errno="A20_ERR_CLUSTER_UNSUPPORTED",
        reply=supp,
        node=C.NODE_ID_BROADCAST,
        src_hash=H_BCAST,
    )
    hello_bad(
        "mal-hello-proto-future-08",
        "HELLO proto_min=1 proto_max=1 while local is v0",
        S6 + ["01-abi.md ABI stability"] + S10,
        reason=C.H_NOT_NEGOTIABLE,
        counter="hello_rejects",
        errno="A20_ERR_CLUSTER_UNSUPPORTED",
        reply=supp,
        payload=hello_payload(
            NODE_A, C.PROFILE_DEFAULT, C.CAP_RELIABLE, NONCE_A, proto_min=1, proto_max=1
        ),
        notes="02-6: the proto intersection is empty (local max 0 < peer min 1) "
        "-> ERROR frame + DOWN.",
    )
    hello_bad(
        "mal-hello-proto-gap-09",
        "HELLO proto_min=2 proto_max=3 while local is v0",
        S6 + S10,
        reason=C.H_NOT_NEGOTIABLE,
        counter="hello_rejects",
        errno="A20_ERR_CLUSTER_UNSUPPORTED",
        reply=supp,
        payload=hello_payload(
            NODE_A, C.PROFILE_DEFAULT, C.CAP_RELIABLE, NONCE_A, proto_min=2, proto_max=3
        ),
    )


# ---------------------------------------------------------------------------
# 02-1  reserved bits: 02-9(2) lists them as malformed, 02-1 forbids rejecting
# ---------------------------------------------------------------------------


def gen_reserved() -> None:
    S1 = ["02-wire-protocol.md 1"]
    S2 = ["02-wire-protocol.md 2"]
    S3 = ["02-wire-protocol.md 3"]
    S5 = ["02-wire-protocol.md 5"]
    S6 = ["02-wire-protocol.md 6"]
    S8 = ["02-wire-protocol.md 8"]
    S10 = ["02-wire-protocol.md 10"]

    def res(
        vid: str,
        title: str,
        sections: Sequence[str],
        transport: str,
        raw: bytes,
        *,
        what: str,
        notes: str,
        assumptions: Sequence[str] = (),
        ctx: Optional[Dict[str, object]] = None,
    ) -> Vec:
        v = add(
            Vec(
                vid,
                "reserved",
                title,
                list(sections),
                transport=transport,
                assumptions=assumptions,
                notes=notes,
                raw=raw,
                ctx=ctx,
            )
        )
        res_ = v.evaluate()
        check(res_["accept"], f"{vid}: 02-1 requires acceptance, got {res_['reason']}")
        v.expect = {
            "frame": {"accept": True, "reason": C.R_OK},
            "dispatch": {
                "layer": "dispatch",
                "action": "accept",
                "reason": C.R_OK,
                "counters": ["rx_frames"],
                "errno": None,
                "reply": None,
                "note": f"{what} is ignored, not rejected",
            },
        }
        return v

    res(
        "res-flags-unknown-bit-01",
        "flags bit 8 set -- a flag bit 02-3 does not define",
        S1 + S3,
        "udp",
        C.encode_frame(
            type=C.TYPE_SEND,
            src_hash=H_A,
            dst_hash=H_B,
            payload=app_payload(TLV_ECHO_REQ),
            flags=0x0100,
            csum_kind=C.CSUM_CCITT,
        ),
        what="an undefined flag bit",
        notes="02-1: reserved bits must be sent as zero, but a receiver must NOT "
        "reject a non-zero reserved bit and must ignore it (forward "
        "compatibility).",
    )
    res(
        "res-flags-compressed-02",
        "flags bit3 COMPRESSED set although 02-3 says it must be 0",
        S1 + S3,
        "udp",
        C.encode_frame(
            type=C.TYPE_SEND,
            src_hash=H_A,
            dst_hash=H_B,
            payload=app_payload(TLV_ECHO_REQ),
            flags=C.FLAG_COMPRESSED,
            csum_kind=C.CSUM_CCITT,
        ),
        what="the COMPRESSED flag",
        assumptions=["A-16"],
        notes="A-16: 02-3 says 'v0 reserved, must be 0' while 02-1 says "
        "reserved bits are never rejected. This vector pins the 02-1 reading "
        "(accept + ignore); the alternative is a drop.",
    )
    res(
        "res-frag-reserved-bits-03",
        "frag bit12 set (reserved area 12-14) on a real fragment",
        S1 + S5,
        "udp",
        C.encode_frame(
            type=C.TYPE_CALL,
            src_hash=H_A,
            dst_hash=H_B,
            payload=bytes(C.UDP_FRAGMENT_PAYLOAD),
            flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED,
            frag=C.frag_field(1, True) | 0x1000,
            txid=1001,
            dst_slot=3,
            seq=3,
            csum_kind=C.CSUM_CCITT,
        ),
        what="frag reserved bits 12-14",
        assumptions=["A-04"],
        notes="02-5 marks frag bits 12-14 reserved=0; 02-1 says ignore, do not "
        "reject. Index 1 / last still decode normally.",
    )
    res(
        "res-frag-reserved-all-04",
        "frag bits 12, 13 and 14 all set",
        S1 + S5,
        "udp",
        C.encode_frame(
            type=C.TYPE_CALL,
            src_hash=H_A,
            dst_hash=H_B,
            payload=bytes(C.UDP_FRAGMENT_PAYLOAD),
            flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED,
            frag=C.frag_field(0, False) | 0x7000,
            txid=1001,
            dst_slot=3,
            seq=3,
            csum_kind=C.CSUM_CCITT,
        ),
        what="frag reserved bits 12-14",
        notes="all three reserved bits set at once; the fragment still decodes "
        "as index 0, not last.",
        assumptions=["A-04"],
    )
    res(
        "res-dst-slot-on-ping-05",
        "PING with dst_slot=7 although 02-1 says only CALL/SEND/CLOSE use it",
        S1 + S2,
        "udp",
        C.encode_frame(
            type=C.TYPE_PING,
            src_hash=H_A,
            dst_hash=H_B,
            dst_slot=7,
            payload=struct.pack("<Q", 1_234_567),
            csum_kind=C.CSUM_CCITT,
        ),
        what="dst_slot on a PING",
        notes="A-15: 02-1 says 'the rest put 0', which is a send-side rule; the "
        "receiver ignores the field instead of rejecting the frame, consistent "
        "with the reserved-field rule of 02-1.",
        assumptions=["A-15"],
    )
    res(
        "res-txid-on-close-06",
        "CLOSE with txid=99 (02-1 constrains txid only for SEND)",
        S1 + S2 + S8,
        "udp",
        C.encode_frame(
            type=C.TYPE_CLOSE,
            src_hash=H_A,
            dst_hash=H_B,
            dst_slot=3,
            txid=99,
            csum_kind=C.CSUM_CCITT,
        ),
        what="txid on a CLOSE",
        notes="A-15: 02-1 constrains txid explicitly only for SEND, so a CLOSE "
        "carrying one is ignored rather than rejected.",
        assumptions=["A-15"],
    )
    res(
        "res-hello-reserved-07",
        "HELLO payload reserved byte = 1",
        S1 + S6,
        "udp",
        C.encode_frame(
            type=C.TYPE_HELLO,
            src_hash=H_A,
            dst_hash=H_B,
            payload=hello_payload(NODE_A, C.PROFILE_DEFAULT, C.CAP_RELIABLE, NONCE_A, reserved=1),
            csum_kind=C.CSUM_CCITT,
        ),
        what="the HELLO payload reserved byte at offset 23",
        notes="02-6 marks offset 23 reserved; 02-1's forward-compatibility rule "
        "applies inside the payload too, so the link still comes UP.",
    )


# ---------------------------------------------------------------------------
# 02-9(2)  state-dependent counters
# ---------------------------------------------------------------------------


def gen_state() -> None:
    S5 = ["02-wire-protocol.md 5"]
    S7 = ["02-wire-protocol.md 7"]
    S10 = ["02-wire-protocol.md 10"]
    S4 = ["02-wire-protocol.md 4"]
    T4 = ["04-transports.md 4"]

    reply = C.encode_frame(
        type=C.TYPE_CALL_REPLY,
        src_hash=H_B,
        dst_hash=H_A,
        payload=reliable_payload(1, TLV_ECHO_RESP),
        flags=C.FLAG_RELIABLE,
        txid=1000,
        seq=1,
        csum_kind=C.CSUM_CCITT,
    )
    v = add(
        Vec(
            "state-dup-seq-01",
            "state",
            "CALL_REPLY with a seq already seen: duplicate, drop but re-ACK",
            S7 + S10,
            transport="udp",
            rx="head_udp",
            raw=reply,
            precondition="seq 1 on this link has already been received (the frame "
            "is byte-identical to the first delivery)",
            followup="dedup_drops += 1; the ACK is sent again; the caller does "
            "not see a second reply",
            notes="02-7: the receiver keeps a 256-entry seq bitmap per link, "
            "drops duplicate frames and re-sends the ACK.",
        )
    )
    res = v.evaluate()
    check(res["frame"] is not None, "state-dup-seq-01 must decode")
    dup = C.classify(reply, dict(RX_CTX["peer_udp"], seq_seen=True))
    check(dup["reason"] == C.D_DUP and dup["counter"] == "dedup_drops", "state-dup-seq-01")
    v.expect = {
        "frame": {"accept": True, "reason": C.R_OK},
        "dispatch": {
            "layer": "reliable",
            "action": "drop",
            "reason": C.D_DUP,
            "counters": ["dedup_drops"],
            "errno": None,
            "reply": "ACK re-sent for the already received cumulative seq",
        },
    }

    v = add(
        Vec(
            "state-retransmit-01",
            "state",
            "the RTO retransmission of a CALL must be byte-identical",
            S7 + S10,
            transport="udp",
            raw=C.encode_frame(
                type=C.TYPE_CALL,
                src_hash=H_A,
                dst_hash=H_B,
                payload=reliable_payload(7, TLV_ECHO_REQ),
                flags=C.FLAG_RELIABLE,
                txid=1000,
                dst_slot=3,
                seq=1,
                csum_kind=C.CSUM_CCITT,
            ),
            precondition="the first copy was lost; RTO = max(200 ms, 2 x RTT "
            "mean) has expired",
            followup="retransmits += 1 on the sender; the receiver drops the "
            "duplicate (dedup_drops) and re-ACKs; the service side must not "
            "execute twice (02-4 dedup window over (src_hash, txid))",
            notes="02-7: retransmission reuses the same seq so that the "
            "receiver's bitmap can recognise it; only the bytes are resent, no "
            "field is rewritten.",
            assumptions=["A-11"],
        )
    )
    v.evaluate()
    v.expect = {
        "frame": {"accept": True, "reason": C.R_OK},
        "dispatch": {
            "layer": "reliable",
            "action": "accept",
            "reason": C.R_OK,
            "counters": ["rx_frames"],
            "errno": None,
            "reply": None,
            "note": "byte-identical to the first copy; the receiver counts "
            "dedup_drops, the sender counts retransmits",
        },
    }

    v = add(
        Vec(
            "state-reasm-gap-01",
            "state",
            "fragment index 2 of 3 arrives, index 1 is lost: no immediate drop",
            S5 + S10,
            transport="udp",
            raw=C.encode_frame(
                type=C.TYPE_CALL,
                src_hash=H_A,
                dst_hash=H_B,
                payload=bytes(C.UDP_FRAGMENT_PAYLOAD),
                flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED,
                frag=C.frag_field(2, False),
                txid=3001,
                dst_slot=3,
                seq=7,
                csum_kind=C.CSUM_CCITT,
            ),
            precondition="fragments 0 and 2 of txid 3001 have arrived; fragment "
            "1 was lost and never arrives",
            followup="no counter at receive time; after 5 s "
            "(min(reasm timeout, CALL deadline)) reasm_timeouts += 1 and the "
            "calling thread gets A20_ERR_CLUSTER_TIMEOUT",
            notes="02-5 allows out-of-order arrival, so a fragment index gap is "
            "a reassembly-state condition, not a malformed frame -- 02-9(2)'s "
            "literal 'fragment index jump' reading is contradicted by 02-5 and "
            "is recorded here instead of as a drop.",
        )
    )
    v.evaluate()
    v.expect = {
        "frame": {"accept": True, "reason": C.R_OK},
        "dispatch": {
            "layer": "reasm",
            "action": "accept",
            "reason": C.R_OK,
            "counters": ["rx_frames"],
            "errno": None,
            "reply": None,
            "note": "no counter at receive time; reasm_timeouts after 5 s",
        },
    }

    v = add(
        Vec(
            "state-reasm-evict-01",
            "state",
            "a well-formed fragment arriving when the reassembly cache is full",
            S5 + S10 + ["01-abi.md limits"],
            transport="udp",
            raw=C.encode_frame(
                type=C.TYPE_CALL,
                src_hash=H_A,
                dst_hash=H_B,
                payload=bytes(C.UDP_FRAGMENT_PAYLOAD),
                flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED,
                frag=C.frag_field(0, False),
                txid=4001,
                dst_slot=3,
                seq=11,
                csum_kind=C.CSUM_CCITT,
            ),
            precondition="the reassembly cache already holds 256 KiB "
            "(A20_LIMIT_CLX_REASM_BYTES_DEFAULT) of half-assembled messages",
            followup="reasm_evicted += 1 for the oldest partial message; this "
            "fragment is admitted; no counter for the frame itself",
            notes="02-5: the cache is limit-driven and never blocks the "
            "sender; MCU tier has a 0 B cache (fragmentation is forbidden).",
        )
    )
    v.evaluate()
    v.expect = {
        "frame": {"accept": True, "reason": C.R_OK},
        "dispatch": {
            "layer": "reasm",
            "action": "accept",
            "reason": C.S_REASM_EVICT,
            "counters": ["reasm_evicted", "rx_frames"],
            "errno": None,
            "reply": None,
            "note": "the frame is admitted; reasm_evicted is charged to the "
            "oldest partial message, not to this frame",
        },
    }

    v = add(
        Vec(
            "state-inflight-limit-01",
            "state",
            "a 65th unanswered CALL on a DEFAULT link (limit 64)",
            S4 + S10 + ["01-abi.md limits"],
            transport="udp",
            raw=None,
            wire_frame=False,
            precondition="64 CALL transactions are already unanswered on this "
            "endpoint (A20_LIMIT_CLX_INFLIGHT_DEFAULT)",
            followup="tx_drops += 1 and the new call returns "
            "A20_ERR_NO_SPACE(13) (01-abi maps A20_ERR_RESOURCE_LIMIT to "
            "A20_ERR_NO_SPACE); no frame is ever built",
            notes="02-4: exceeding the outstanding-CALL limit returns "
            "RESOURCE_LIMIT and must not block waiting for a free slot.",
        )
    )
    v.expect = {
        "frame": {"accept": False, "reason": "not built"},
        "dispatch": {
            "layer": "local",
            "action": "drop",
            "reason": "inflight_call_limit",
            "counters": ["tx_drops"],
            "errno": "A20_ERR_NO_SPACE",
            "reply": None,
        },
    }

    oversize = C.encode_frame(
        type=C.TYPE_CALL,
        src_hash=H_A,
        dst_hash=H_LEAF,
        payload=bytes(300),
        txid=5001,
        dst_slot=1,
        csum_kind=C.CSUM_CCITT,
    )
    v = add(
        Vec(
            "state-mcu-oversize-01",
            "state",
            "MCU tier: a 300 B message exceeds the 256 B frame limit",
            S5 + T4 + S10,
            transport="uart",
            raw=oversize,
            ctx={"profile": C.PROFILE_MCU, "peer_caps": C.CAP_LEAF},
            precondition="an MCU tier link that negotiated short_addr 0x0007",
            followup="tx_drops += 1 and the caller gets "
            "A20_ERR_INVALID_ARGUMENT(12) (01-abi: A20_ERR_INVALID_ARGS); the "
            "frame below is never transmitted -- it exists so the C side can see "
            "what the refused frame would have looked like",
            notes="02-5: the MCU tier forbids fragmentation, a message larger "
            "than the MTU returns A20_ERR_INVALID_ARGS immediately. 02-5 also "
            "says a fragmenter must not build this frame at all.",
        )
    )
    res = v.evaluate()
    v.expect = {
        "frame": {"accept": True, "reason": C.R_OK},
        "dispatch": {
            "layer": "local",
            "action": "drop",
            "reason": "payload_over_mtu",
            "counters": ["tx_drops"],
            "errno": "A20_ERR_INVALID_ARGUMENT",
            "reply": None,
            "note": "the frame layer accepts it (it is well formed and the "
            "loopback/UDP MTU is larger); the MCU tier refuses to send it",
        },
    }
    check(res["accept"], "state-mcu-oversize-01 must be a well-formed frame")


# ---------------------------------------------------------------------------
# 04-4  SLIP variant
# ---------------------------------------------------------------------------


def gen_slip() -> None:
    S1 = ["02-wire-protocol.md 1"]
    S2 = ["02-wire-protocol.md 2"]
    S5 = ["02-wire-protocol.md 5"]
    S6 = ["02-wire-protocol.md 6"]
    S8 = ["02-wire-protocol.md 8"]
    T4 = ["04-transports.md 4"]

    def slip(
        vid: str,
        title: str,
        sections: Sequence[str],
        raw: bytes,
        stream: bytes,
        *,
        expect_frames: int = 1,
        notes: str = "",
        assumptions: Sequence[str] = (),
    ) -> Vec:
        v = add(
            Vec(
                vid,
                "slip",
                title,
                list(sections),
                transport="uart",
                assumptions=assumptions,
                notes=notes,
                raw=raw,
                slip_stream=stream,
            )
        )
        res = v.evaluate()
        check(
            len(res["slip_frames"]) == expect_frames,
            f"{vid}: slip_decode recovered {len(res['slip_frames'])} frames, expected {expect_frames}",
        )
        v.expect = {
            "frame": {"accept": True, "reason": C.R_OK},
            "dispatch": {
                "layer": "slip",
                "action": "decode",
                "reason": C.R_OK,
                "counters": ["rx_frames"],
                "errno": None,
                "reply": None,
                "slip_frames": expect_frames,
                "slip_notes": res["slip_notes"],
            },
        }
        return v

    def uart_frame(**kw) -> bytes:
        base = dict(
            type=C.TYPE_PING,
            src_hash=H_LEAF,
            dst_hash=H_A,
            payload=struct.pack("<Q", 1_234_567),
            csum_kind=C.CSUM_CCITT,
        )
        base.update(kw)
        return C.encode_frame(**base)

    # leaf HELLO: the nonce contains 0xC0, 0xDB, 0xDC, 0xDD so every escape
    # rule of 04-4 is exercised by a legal frame.
    hello_leaf = uart_frame(
        type=C.TYPE_HELLO,
        src_hash=H_LEAF,
        payload=hello_payload(
            LEAF, C.PROFILE_MCU, C.CAP_LEAF, NONCE_LEAF, short_addr=0xFFFF, link_addr_len=2
        ),
    )
    slip(
        "slip-hello-leaf-01",
        "leaf HELLO with 0xC0/0xDB/0xDC/0xDD in the nonce: every escape rule",
        S6 + T4,
        hello_leaf,
        C.slip_encode(hello_leaf),
        notes="04-4: DB DC carries a data 0xC0 and DB DD a data 0xDB; the frame "
        "itself is a plain CL frame with csum_kind=1 forced.",
    )
    hello_ack = uart_frame(
        type=C.TYPE_HELLO_ACK,
        src_hash=H_A,
        dst_hash=H_LEAF,
        payload=hello_payload(
            NODE_A,
            C.PROFILE_SERVER,
            C.CAP_RELAY | C.CAP_RELIABLE,
            NONCE_A,
            short_addr=0x0007,
            link_addr_len=2,
        ),
    )
    slip(
        "slip-hello-ack-01",
        "head HELLO_ACK assigning short_addr 0x0007",
        S6 + T4,
        hello_ack,
        C.slip_encode(hello_ack),
    )
    ping = uart_frame(payload=struct.pack("<Q", 0xC0C0DBDBDBDBDBDB))
    slip(
        "slip-ping-01",
        "PING whose timestamp is 0xC0C0DBDBDBDBDBDB (8 escaped bytes)",
        S2 + T4,
        ping,
        C.slip_encode(ping),
        assumptions=["A-05b"],
    )
    slip(
        "slip-pong-01",
        "PONG echoing slip-ping-01",
        S2 + T4,
        ping[:-34] + b"\x04" + ping[-34:],  # type byte 3 -> PONG, CRC recomputed
        C.slip_encode(ping[:-34] + b"\x04" + ping[-34:]),
        assumptions=["A-05b"],
    )
    call = uart_frame(
        type=C.TYPE_CALL,
        dst_hash=H_LEAF,
        payload=app_payload([(0x8001, b"dot"), (0x8002, bytes([0xC0, 0xDB, 0x00, 0x01]))]),
        txid=1000,
        dst_slot=1,
        csum_kind=C.CSUM_CCITT,
    )
    slip(
        "slip-call-01",
        "CALL with 0xC0 and 0xDB inside a TLV value",
        S2 + T4,
        call,
        C.slip_encode(call),
    )
    reply = uart_frame(
        type=C.TYPE_CALL_REPLY,
        src_hash=H_LEAF,
        dst_hash=H_A,
        payload=app_payload([(0x8003, struct.pack("<I", 2048))]),
        txid=1000,
        csum_kind=C.CSUM_CCITT,
    )
    slip(
        "slip-call-reply-01",
        "CALL_REPLY from the leaf operator",
        S2 + T4,
        reply,
        C.slip_encode(reply),
    )
    err = uart_frame(
        type=C.TYPE_ERROR,
        src_hash=H_LEAF,
        dst_hash=H_A,
        payload=struct.pack("<II", C.ERRNO["A20_ERR_NOT_FOUND"], 1000),
        csum_kind=C.CSUM_CCITT,
    )
    slip(
        "slip-error-01",
        "ERROR NOT_FOUND from the leaf",
        S8 + T4,
        err,
        C.slip_encode(err),
    )
    close = uart_frame(type=C.TYPE_CLOSE, src_hash=H_LEAF, dst_hash=H_A, dst_slot=1)
    slip(
        "slip-close-01",
        "CLOSE from the leaf (empty payload, 34 B on the wire)",
        S8 + T4,
        close,
        C.slip_encode(close),
    )
    slip(
        "slip-two-frames-01",
        "two frames back to back in one stream (one UART, no idle gap required)",
        T4,
        ping,
        C.slip_encode(ping) + C.slip_encode(close),
        expect_frames=2,
    )
    slip(
        "slip-mal-resync-01",
        "leading empty frames between ENDs: resync artifact, not a frame",
        T4,
        ping,
        bytes((C.SLIP_END, C.SLIP_END, C.SLIP_END)) + C.slip_encode(ping),
        expect_frames=1,
        assumptions=["A-10"],
        notes="04-4: the leading END doubles as the resync point, so an empty "
        "frame between two ENDs yields no frame and no counter (A-10).",
    )
    bad_esc = C.slip_encode(ping)[:-1] + bytes((C.SLIP_END, C.SLIP_ESC, 0x41, C.SLIP_END))
    slip(
        "slip-mal-badescape-01",
        "ESC followed by an undefined code byte: half frame discarded",
        T4,
        b"",
        bad_esc,
        expect_frames=1,
        notes="only the intact first frame is recovered; the damaged one is "
        "dropped (rx_drops) and the receiver resyncs on the next END.",
    )
    slip(
        "slip-mal-unescaped-02",
        "stream ends on a lone ESC (inter-byte timeout before the escape code)",
        T4,
        ping,
        C.slip_encode(ping) + bytes((C.SLIP_ESC,)),
        expect_frames=1,
        notes="04-4: silence > 50 ms drops the half frame.",
    )
    slip(
        "slip-mal-partial-03",
        "frame cut in half with no trailing END: partial frame timeout",
        T4,
        ping,
        C.slip_encode(ping)[: len(C.slip_encode(ping)) // 2],
        expect_frames=0,
        notes="04-4: partial frame timeout drops the half frame; notes record "
        "'partial_frame'.",
    )
    slip(
        "slip-mal-truncated-04",
        "END-terminated stream shorter than a 32 B header",
        T4,
        b"",
        bytes((C.SLIP_END,)) + b"\x43\x4c\x00\x03\x00\x00" + bytes((C.SLIP_END,)),
        expect_frames=0,
    )
    # Receive side: a stream with no END that never fits the 512 B buffer.
    overrun = bytearray(b"\x43\x4c\x00\x05\x00\x00\x00\x00")
    overrun += b"\x00" * 24
    overrun[28:30] = (600).to_bytes(2, "little")
    overrun[30] = C.DEFAULT_TTL
    overrun[31] = C.CSUM_CCITT
    overrun += b"\x5a" * 600  # never terminated: 602 decoded bytes > 512
    overrun = bytes(overrun)
    slip(
        "slip-mal-oversize-05",
        "unterminated stream of 602 decoded bytes exceeds the 512 B RX buffer",
        T4,
        b"",
        C.slip_encode(overrun),
        expect_frames=0,
        notes="04-4: the leaf owns a single static 512 B receive buffer. When it "
        "fills before an END the half frame must be dropped and the receiver "
        "resyncs on the next END; the note records 'oversize'. Nothing is "
        "written past the buffer -- that is the property 06-1 P-fuzz checks.",
    )

    # Send side: 04-4's 320 B transmit buffer bounds the worst-case escaped
    # frame.  The worst case is a frame whose flags/frag/txid/hashes/dst_slot/
    # seq bytes and whose whole payload are 0xDB, with a CRC that also needs
    # escaping.  With payload_len/ttl/csum_kind/magic fixed by 02-1, the
    # escapable set is 24 header bytes + payload + 2 CRC bytes, so
    # escaped = 2*(32 + p) + 2 + 2 and p=129 is exactly 320 B.
    payload = bytes([C.SLIP_ESC]) * 128
    budget = bytearray(32)
    struct.pack_into("<H", budget, 0, C.MAGIC)
    budget[2] = C.WIRE_VER
    budget[3] = C.TYPE_CALL
    struct.pack_into("<H", budget, 4, C.FLAG_RELIABLE)
    struct.pack_into("<H", budget, 6, 0)  # frag must be 0 when FRAGMENTED is clear
    for off in (8, 12, 16, 20, 24):  # txid, src, dst, dst_slot, seq: all free
        budget[off : off + 4] = b"\xdb\xdb\xdb\xdb"
    struct.pack_into("<H", budget, 28, len(payload))
    budget[30] = C.DEFAULT_TTL
    budget[31] = C.CSUM_CCITT
    frame = bytes(budget) + payload
    frame += struct.pack("<H", C.crc16_ccitt(frame))
    stream = C.slip_encode(frame)
    check(
        len(stream) <= C.UART_TX_BUF,
        f"escaped MCU frame is {len(stream)} B, over the {C.UART_TX_BUF} B "
        f"transmit buffer",
    )
    slip(
        "slip-uart-budget-06",
        "MCU operator frame at the 128 B payload budget with every payload "
        f"byte escaped ({len(stream)} B on the wire, buffer is {C.UART_TX_BUF} B)",
        T4 + S5,
        frame,
        stream,
        notes="04-4 gives the leaf a 320 B transmit buffer and recommends an "
        "operator payload of at most 128 bytes. The two agree: 24 escapable "
        "header bytes plus a payload plus the CRC give a worst-case escaped "
        "frame of 2*(32+p)+4 bytes, so p=129 is exactly 320 B and p=130 does "
        "not fit. This vector is p=128 with a payload made entirely of 0xDB, "
        "the value that escapes hardest. ASSUMPTION A-17.",
        assumptions=["A-17"],
    )


# ---------------------------------------------------------------------------
# output
# ---------------------------------------------------------------------------

MANIFEST_HEADER = {
    "schema": "a20-cluster-goldvectors/1",
    "doc": "docs/cluster/02-wire-protocol.md 9",
    "generated_by": "tools/cluster-ref/gen_vectors.py",
    "reference_codec": "tools/cluster-ref/clframe.py",
    "counter_names": list(C.COUNTERS),
    "mtu": {C.TRANSPORT_NAME[t]: C.TRANSPORT_MTU[t] for t in C.TRANSPORT_NAME},
    "max_payload_len": dict(C.MAX_PAYLOAD),
    "node_hashes": {
        "NODE_A": {"node_id": NODE_A.hex(), "hash": f"0x{H_A:08X}"},
        "NODE_B": {"node_id": NODE_B.hex(), "hash": f"0x{H_B:08X}"},
        "LEAF": {"node_id": LEAF.hex(), "hash": f"0x{H_LEAF:08X}"},
        "A20_NODE_ID_LOCAL": {"node_id": C.NODE_ID_LOCAL.hex(), "hash": f"0x{H_LOCAL:08X}"},
        "A20_NODE_ID_BROADCAST": {"node_id": C.NODE_ID_BROADCAST.hex(), "hash": f"0x{H_BCAST:08X}"},
    },
    "errno": {k: v for k, v in C.ERRNO.items()},
}


def write_outputs() -> Dict[str, object]:
    entries = []
    for v in VECTORS:
        doc = v.document()
        d = os.path.join(VECDIR, v.dir)
        os.makedirs(d, exist_ok=True)
        base = os.path.join(d, v.vid)
        if v.wire_frame:
            hex_name = os.path.relpath(base + (".slip.hex" if v.slip_stream is not None else ".hex"), VECDIR)
            with open(base + (".slip.hex" if v.slip_stream is not None else ".hex"), "w") as f:
                data = v.slip_stream if v.slip_stream is not None else v.raw
                f.write(data.hex() + "\n")
            doc["hex_file"] = hex_name
        json_name = os.path.relpath(base + ".json", VECDIR)
        with open(base + ".json", "w") as f:
            json.dump(doc, f, indent=2, sort_keys=False)
            f.write("\n")
        entries.append(
            {
                "id": v.vid,
                "category": v.category,
                "file": json_name,
                "hex_file": doc.get("hex_file"),
                "title": v.title,
                "doc_sections": v.sections,
                "assumptions": v.assumptions,
                "transport": v.transport,
                "expect": v.expect,
            }
        )

    counts: Dict[str, int] = {}
    for v in VECTORS:
        counts[v.category] = counts.get(v.category, 0) + 1
    per_type: Dict[str, int] = {}
    for v in VECTORS:
        if v.category != "valid" or not v.wire_frame:
            continue
        res = v.result or v.evaluate()
        frame = res["frame"]
        per_type[C.TYPE_NAMES[frame.type]] = per_type.get(C.TYPE_NAMES[frame.type], 0) + 1

    counters_used = sorted(
        {
            c
            for v in VECTORS
            for c in (v.expect or {}).get("dispatch", {}).get("counters", [])
        }
    )
    manifest = dict(MANIFEST_HEADER)
    manifest["counts"] = {
        "total": len(VECTORS),
        "by_category": counts,
        "valid_by_type": per_type,
        "counters_exercised": counters_used,
        "counters_never_exercised": [c for c in C.COUNTERS if c not in counters_used],
    }
    manifest["vectors"] = entries
    with open(os.path.join(VECDIR, "MANIFEST.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    # human readable index
    lines = [
        "# Gold vector index",
        "",
        "Generated by `tools/cluster-ref/gen_vectors.py` from",
        "`docs/cluster/02-wire-protocol.md` 9.  Do not edit by hand -- run",
        "`python3 tools/cluster-ref/gen_vectors.py`.",
        "",
        "| id | category | transport | expectation | doc |",
        "|---|---|---|---|---|",
    ]
    for e in entries:
        exp = e["expect"]["dispatch"]
        action = exp["action"]
        counters = ",".join(exp.get("counters") or []) or "-"
        errno = exp.get("errno") or "-"
        expect_txt = f"{action} / {exp['reason']} / {counters}" + (f" / {errno}" if errno != "-" else "")
        lines.append(
            f"| `{e['id']}` | {e['category']} | {e['transport']} | {expect_txt} | "
            + "; ".join(s.replace("02-wire-protocol.md", "02").replace("01-abi.md", "01")
                        .replace("04-transports.md", "04").replace("05-userspace.md", "05")
                        for s in e["doc_sections"])
            + " |"
        )
    lines += [
        "",
        "## Counts",
        "",
        f"- total vectors: {len(VECTORS)}",
    ]
    for k, n in sorted(counts.items()):
        lines.append(f"- {k}: {n}")
    lines.append("")
    lines.append("Valid frames per message type (02-9 requirement 1, >= 2 each):")
    lines.append("")
    for t in sorted(C.TYPE_NAMES.values()):
        lines.append(f"- {t}: {per_type.get(t, 0)}")
    lines += [
        "",
        "02-10 counters exercised: " + ", ".join(counters_used),
        "",
        "02-10 counters not exercised by a single-vector expectation: "
        + ", ".join(c for c in C.COUNTERS if c not in counters_used),
        "",
    ]
    with open(os.path.join(VECDIR, "INDEX.md"), "w") as f:
        f.write("\n".join(lines))
    return manifest


def main() -> int:
    gen_valid()
    gen_malformed()
    gen_reserved()
    gen_state()
    gen_slip()
    manifest = write_outputs()
    c = manifest["counts"]
    print(f"vectors: {c['total']} total, by category {c['by_category']}")
    print(f"valid per type: {c['valid_by_type']}")
    print(f"counters exercised: {c['counters_exercised']}")
    print(f"counters not exercised: {c['counters_never_exercised']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
