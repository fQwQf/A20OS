#!/usr/bin/env python3
"""Self-test for the tools/cluster-ref wire-format reference (WB1).

Four layers, all host side, no kernel code involved:

1. primitives   -- 01-abi FNV-1a32 node-hash anchors, 02-1 CRC16-CCITT check
                   value, 02-1 field offsets, against independent inline
                   reimplementations (not against clframe's own loops).
2. roundtrip    -- every message type of 02-2 encodes, decodes and re-encodes
                   byte-identically, in both csum kinds, plus the 04-4 SLIP and
                   05-3 TLV roundtrips and their error paths.
3. vectors      -- (a) gen_vectors.py is re-run into a temporary directory and
                   every produced byte is compared against the committed
                   vectors/ tree (drift check, per gen_vectors' own docstring);
                   (b) every committed .hex is independently decoded with
                   clframe and compared against its .json frame block and
                   expect.frame verdict.
4. scenario     -- gen_scenario.py's build() output is compared byte for byte
                   against scenario_hello_call_close.txt, and every hex line in
                   the committed file is independently decoded (frames must be
                   clean; the single deliberately corrupted one must fail the
                   02-1 CRC), and the appendix SLIP streams must roundtrip.

Run:  python3 tools/cluster-ref/selftest.py
Exit 0 when everything is green, 1 with a failure list otherwise.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import struct
import sys
import tempfile
from typing import Dict, List, Optional, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import clframe as C  # noqa: E402
import gen_scenario as S  # noqa: E402
import gen_vectors as G  # noqa: E402

VECDIR = os.path.join(HERE, "vectors")
SCENARIO = os.path.join(HERE, "scenario_hello_call_close.txt")

FAILURES: List[str] = []
PASSED = 0


def check(cond: bool, msg: str) -> bool:
    global PASSED
    if cond:
        PASSED += 1
        print(f"  ok   {msg}")
    else:
        FAILURES.append(msg)
        print(f"  FAIL {msg}")
    return bool(cond)


# ---------------------------------------------------------------------------
# 1. primitives
# ---------------------------------------------------------------------------


def fnv_independent(data: bytes) -> int:
    """Inline FNV-1a32 written from the 01-abi sentence alone: offset basis
    2166136261, prime 16777619, h = (h ^ b) * 16777619 per byte."""
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def crc_independent(data: bytes) -> int:
    """Inline CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect/xor)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def t_primitives() -> None:
    print("[1] primitives (01-abi node hash, 02-1 CRC, 02-1 layout)")
    check(C.fnv1a32(bytes(16)) == 0x69691905, "fnv1a32(all-zero) == 0x69691905")
    check(C.fnv1a32(b"\xff" * 16) == 0x360779F5, "fnv1a32(all-0xff) == 0x360779f5")
    ids = [bytes(range(1, 17)), bytes([0xA1 + i for i in range(15)] + [0xB0]),
           bytes([0xDE, 0xAD, 0xBE, 0xEF] + [0] * 11 + [0x01])]
    for i, nid in enumerate(ids):
        check(
            C.fnv1a32(nid) == fnv_independent(nid),
            f"fnv1a32(node A/B/leaf {i}) matches the independent loop "
            f"(0x{fnv_independent(nid):08X})",
        )
    check(
        C.crc16_ccitt(b"123456789") == 0x29B1,
        "crc16-ccitt('123456789') == 0x29B1 (CRC-16/CCITT-FALSE check value)",
    )
    check(C.crc16_ccitt(b"") == 0xFFFF, "crc16-ccitt(empty) == init 0xFFFF")
    for blob in (b"\x43\x4c", bytes(range(64)), bytes(200)):
        check(
            C.crc16_ccitt(blob) == crc_independent(blob),
            f"crc16-ccitt({len(blob)} B blob) matches the independent loop",
        )
    check(struct.pack("<H", C.MAGIC) == b"\x43\x4c", "magic 0x4C43 LE -> wire bytes 43 4C ('CL')")
    for name, off in C.FIELD_OFFSETS.items():
        check(C.FIELD_OFFSETS[name] == off, f"02-1 offset {name} == {off}")
    check(
        (C.FIELD_OFFSETS["payload_len"], C.FIELD_OFFSETS["ttl"], C.FIELD_OFFSETS["csum_kind"])
        == (28, 30, 31),
        "02-1 offsets payload_len=28 ttl=30 csum_kind=31",
    )
    check(C.HEADER_LEN == 32 and C.WIRE_VER == 0, "02-1 header 32 B, wire v0")
    # 02-10: the ten counter names, append-only.
    check(
        C.COUNTERS
        == (
            "tx_frames", "rx_frames", "tx_drops", "rx_drops", "rx_malformed",
            "retransmits", "reasm_timeouts", "reasm_evicted", "dedup_drops", "hello_rejects",
        ),
        "02-10 counter tuple unchanged (append-only contract)",
    )
    # 01-abi errno numbers as reused per its own arbitration table.
    check(
        (C.ERRNO["A20_ERR_NODE_UNREACHABLE"], C.ERRNO["A20_ERR_CLUSTER_TIMEOUT"],
         C.ERRNO["A20_ERR_REMOTE_CLOSED"], C.ERRNO["A20_ERR_CLUSTER_UNSUPPORTED"])
        == (26, 27, 28, 29),
        "01-abi new cluster errnos land at 26..29",
    )
    check(
        (C.ERRNO["A20_ERR_INVALID_ARGUMENT"], C.ERRNO["A20_ERR_NO_SPACE"],
         C.ERRNO["A20_ERR_NOT_FOUND"], C.ERRNO["A20_ERR_EXISTS"])
        == (12, 13, 24, 10),
        "01-abi reuse table: INVALID_ARGUMENT=12 NO_SPACE=13 NOT_FOUND=24 EXISTS=10",
    )


# ---------------------------------------------------------------------------
# 2. roundtrip
# ---------------------------------------------------------------------------


def roundtrip(**kw) -> Tuple[bytes, object, Dict[str, object]]:
    raw = C.encode_frame(**kw)
    frame, diag = C.decode_frame(raw)
    again = C.encode_frame(
        type=frame.type, ver=frame.ver, magic=frame.magic, flags=frame.flags,
        frag=frame.frag, txid=frame.txid, src_hash=frame.src_hash,
        dst_hash=frame.dst_hash, dst_slot=frame.dst_slot, seq=frame.seq,
        ttl=frame.ttl, csum_kind=frame.csum_kind, payload=frame.payload,
    )
    return raw, frame, {"diag": diag, "reenc": again, "ok": again == raw}


def t_roundtrip() -> None:
    print("[2] encode -> decode -> re-encode roundtrips (02-1/02-2, both csum kinds)")
    pl = b"\x01\x02\x03\x04" * 2
    hello = C.encode_hello_payload(
        bytes(range(1, 17)), 0, 0, C.PROFILE_DEFAULT, C.CAP_RELIABLE, 0, 6,
        bytes(range(8)),
    )
    cases = [
        ("HELLO", dict(type=C.TYPE_HELLO, src_hash=0x11111111, dst_hash=0x22222222, payload=hello)),
        ("HELLO_ACK", dict(type=C.TYPE_HELLO_ACK, src_hash=0x11111111, dst_hash=0x22222222, payload=hello)),
        ("PING", dict(type=C.TYPE_PING, src_hash=1, dst_hash=2, payload=struct.pack("<Q", 7))),
        ("PONG", dict(type=C.TYPE_PONG, src_hash=2, dst_hash=1, payload=struct.pack("<Q", 7))),
        ("SEND", dict(type=C.TYPE_SEND, src_hash=1, dst_hash=2, payload=pl)),
        ("CALL", dict(type=C.TYPE_CALL, src_hash=1, dst_hash=2, payload=pl, txid=9, dst_slot=3)),
        ("CALL_REPLY", dict(type=C.TYPE_CALL_REPLY, src_hash=2, dst_hash=1, payload=pl, txid=9)),
        ("CLOSE", dict(type=C.TYPE_CLOSE, src_hash=2, dst_hash=1, dst_slot=3)),
        ("ACK", dict(type=C.TYPE_ACK, src_hash=1, dst_hash=2, payload=struct.pack("<I", 4))),
        ("NACK", dict(type=C.TYPE_NACK, src_hash=1, dst_hash=2, payload=struct.pack("<I", 4))),
        ("ERROR", dict(type=C.TYPE_ERROR, src_hash=2, dst_hash=1,
                       payload=struct.pack("<II", C.ERRNO["A20_ERR_NOT_FOUND"], 9))),
        ("CALL reliable+ack-prefix", dict(
            type=C.TYPE_CALL, src_hash=1, dst_hash=2,
            payload=struct.pack("<I", 3) + pl, flags=C.FLAG_RELIABLE,
            txid=9, dst_slot=3, seq=1)),
        ("CALL fragmented mid", dict(
            type=C.TYPE_CALL, src_hash=1, dst_hash=2, payload=bytes(C.UDP_FRAGMENT_PAYLOAD),
            flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED, frag=C.frag_field(1, False),
            txid=10, dst_slot=3, seq=2)),
        ("CALL fragmented last", dict(
            type=C.TYPE_CALL, src_hash=1, dst_hash=2, payload=bytes(64),
            flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED, frag=C.frag_field(2, True),
            txid=10, dst_slot=3, seq=2)),
        ("SEND broadcast", dict(
            type=C.TYPE_SEND, src_hash=1, dst_hash=C.fnv1a32(C.NODE_ID_BROADCAST),
            payload=pl, flags=C.FLAG_BROADCAST)),
    ]
    for csum in (C.CSUM_CCITT, C.CSUM_NONE):
        for name, kw in cases:
            kw = dict(kw, csum_kind=csum)
            raw, frame, res = roundtrip(**kw)
            tag = f"{name} csum_kind={csum}"
            ok = check(res["ok"], f"{tag}: re-encode is byte-identical ({len(raw)} B)")
            if ok:
                check(res["diag"]["reason"] == C.R_OK, f"{tag}: decodes clean")
                check(frame.payload_len == len(kw.get("payload", b"")),
                      f"{tag}: payload_len echoed")
                if kw.get("txid") is not None:
                    check(frame.txid == kw["txid"], f"{tag}: txid echoed")
                check(frame.crc_present == (csum == C.CSUM_CCITT), f"{tag}: crc_present")
    # ACK-prefix split (02-7, A-05/A-06)
    _, frame, _ = roundtrip(
        type=C.TYPE_CALL, src_hash=1, dst_hash=2, payload=struct.pack("<I", 5) + b"payload",
        flags=C.FLAG_RELIABLE, txid=1, seq=1,
    )
    check(frame.ack_prefix == 5 and frame.app_payload == b"payload" and frame.app_offset == 4,
          "RELIABLE CALL: 4 B cumulative ACK split off (02-7)")
    _, frame, _ = roundtrip(type=C.TYPE_CALL, src_hash=1, dst_hash=2, payload=b"payload", txid=1)
    check(frame.ack_prefix is None and frame.app_offset == 0,
          "unreliable CALL: no ACK prefix split (A-05/A-06)")
    # TLV (05-3)
    items = [(0x8001, b"abc"), (0x8002, b""), (0x0101, struct.pack("<Q", 88)), (0x0001, b"d")]
    enc = C.tlv_encode(items)
    check(C.tlv_decode(enc) == items, "05-3 TLV encode/decode roundtrip incl. empty + padded values")
    check(len(enc) == sum(C.tlv_element_len(len(v)) for _, v in items),
          "05-3 per-element 4-byte alignment (A-07)")
    for bad in (b"\x01\x00", b"\x01\x00\x04\x00\x01", b"\x01\x00\x01\x00\x01\x00"):
        try:
            C.tlv_decode(bad)
            check(False, f"05-3 truncated TLV {bad.hex()} must raise TlvError")
        except C.TlvError:
            check(True, f"05-3 truncated TLV {bad.hex()} raises TlvError")
    check(C.tlv_is_known_tag(0x0001) and C.tlv_is_known_tag(0x0100)
          and C.tlv_is_known_tag(0x8000) and not C.tlv_is_known_tag(0x0200),
          "05-3 tag space 0001-00FF/0100-01FF/8000+")
    # SLIP (04-4).  The blobs carry the CL magic because slip_decode() drops
    # unescaped frames that do not start with 43 4C (they never reach the CL
    # layer -- 04-4 keeps that check at the SLIP finish boundary).
    for name, blob in (
        ("plain frame", b"\x43\x4c" + bytes(30)),
        ("all C0", b"\x43\x4c" + b"\xc0" * 38),
        ("all DB", b"\x43\x4c" + b"\xdb" * 38),
        ("mixed", b"\x43\x4c" + b"\xc0\xdb\xdc\xdd\xc0" * 7 + b"\xc0"),
    ):
        stream = C.slip_encode(blob)
        frames, notes = C.slip_decode(stream)
        check(frames == [blob] and notes == [],
              f"04-4 SLIP roundtrip ({name}, {len(blob)} B -> {len(stream)} B)")
    # HELLO payload reader
    h = C.decode_hello_payload(hello)
    check(h["node_id"] == bytes(range(1, 17)).hex()
          and h["node_id_hash"] == f"0x{C.fnv1a32(bytes(range(1, 17))):08X}"
          and h["caps"] == C.CAP_RELIABLE and h["profile_tier"] == C.PROFILE_DEFAULT,
          "02-6 HELLO payload field-by-field readback")


# ---------------------------------------------------------------------------
# 3. vectors
# ---------------------------------------------------------------------------


def t_vectors_regen() -> None:
    print("[3a] vector regeneration is byte-identical to the committed tree")
    tmp = tempfile.mkdtemp(prefix="a20-vec-")
    try:
        old_dir = G.VECDIR
        G.VECDIR = tmp
        try:
            G.gen_valid()
            G.gen_malformed()
            G.gen_reserved()
            G.gen_state()
            G.gen_slip()
            G.write_outputs()
        finally:
            G.VECDIR = old_dir
        drift: List[str] = []
        for root, _dirs, files in os.walk(tmp):
            for fn in files:
                new = os.path.join(root, fn)
                rel = os.path.relpath(new, tmp)
                old = os.path.join(VECDIR, rel)
                if not os.path.exists(old):
                    drift.append(f"{rel}: missing from the committed tree")
                    continue
                with open(new, "rb") as f1, open(old, "rb") as f2:
                    if f1.read() != f2.read():
                        drift.append(f"{rel}: bytes differ")
        committed = set()
        for root, _dirs, files in os.walk(VECDIR):
            for fn in files:
                committed.add(os.path.relpath(os.path.join(root, fn), VECDIR))
        produced = set()
        for root, _dirs, files in os.walk(tmp):
            for fn in files:
                produced.add(os.path.relpath(os.path.join(root, fn), tmp))
        for rel in sorted(committed - produced):
            drift.append(f"{rel}: not regenerated (stale in the committed tree)")
        check(not drift, "regenerated vectors/ == committed vectors/ "
              + ("" if not drift else f"-- drift: {drift[:6]}"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def t_vectors_decode() -> None:
    print("[3b] every committed .hex decodes to exactly its .json verdict")
    hex_files: List[str] = []
    for cat in ("valid", "malformed", "reserved", "state", "slip"):
        d = os.path.join(VECDIR, cat)
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".hex"):
                hex_files.append(os.path.join(cat, fn))
    n_frame = n_slip = 0
    for rel in hex_files:
        with open(os.path.join(VECDIR, rel), "r") as f:
            raw = bytes.fromhex(f.read().strip())
        base = rel[:-4]  # strip .hex
        if base.endswith(".slip"):
            base = base[:-5]  # slip vectors: <id>.slip.hex -> <id>.json
        with open(os.path.join(VECDIR, base + ".json"), "r") as f:
            doc = json.load(f)
        if rel.startswith("slip/"):
            frames, notes = C.slip_decode(raw)
            want_f = [bytes.fromhex(h) for h in doc["slip"]["slip_frames"]]
            want_n = doc["slip"]["slip_notes"]
            check(frames == want_f and notes == want_n,
                  f"{doc['id']}: slip decode == json ({len(want_f)} frames, notes {notes})")
            for fr in frames:
                rt, _rt_notes = C.slip_decode(C.slip_encode(fr))
                check(rt == [fr], f"{doc['id']}: slip roundtrip of the decoded frame")
            if doc.get("frame_hex") and notes == [] and len(frames) == 1 \
                    and frames[0].hex() == doc["frame_hex"]:
                check(C.slip_encode(frames[0]) == raw,
                      f"{doc['id']}: stream is exactly slip_encode(frame)")
            n_slip += 1
            continue
        mtu = G.RX_CTX[doc["receiver"]["role"]]["mtu"]
        frame, diag = C.decode_frame(raw, mtu=mtu)
        exp = doc["expect"]["frame"]
        check((frame is not None) == bool(exp["accept"]) and diag["reason"] == exp["reason"],
              f"{doc['id']}: verdict {diag['reason']} == json {exp['reason']}")
        if frame is None:
            check(raw[:C.HEADER_LEN].hex() == doc.get("raw_header", ""),
                  f"{doc['id']}: raw_header echoed")
            n_frame += 1
            continue
        j = doc["frame"]
        same = (
            j["magic"] == f"0x{frame.magic:04X}"
            and j["ver"] == frame.ver
            and j["type"] == frame.type
            and j["type_name"] == frame.type_name
            and j["flags"] == f"0x{frame.flags:04X}"
            and j["frag"] == f"0x{frame.frag:04X}"
            and j["frag_seq"] == frame.frag_seq
            and j["frag_last"] == frame.frag_last
            and j["frag_reserved_nonzero"] == frame.frag_reserved_nonzero
            and j["txid"] == frame.txid
            and j["src_hash"] == f"0x{frame.src_hash:08X}"
            and j["dst_hash"] == f"0x{frame.dst_hash:08X}"
            and j["dst_slot"] == frame.dst_slot
            and j["seq"] == frame.seq
            and j["payload_len"] == frame.payload_len
            and j["ttl"] == frame.ttl
            and j["csum_kind"] == frame.csum_kind
            and j["wire_len"] == frame.wire_len == len(raw)
            and j["crc_present"] == frame.crc_present
        )
        check(same, f"{doc['id']}: every header field matches the json frame block")
        p = doc["payload"]
        check(p["hex"] == frame.payload.hex() and p["len"] == len(frame.payload)
              and p["app_offset"] == frame.app_offset
              and p["app_hex"] == frame.app_payload.hex()
              and p["ack_prefix"] == frame.ack_prefix,
              f"{doc['id']}: payload block matches (app_offset {frame.app_offset})")
        if "hello" in p:
            check(p["hello"] == C.decode_hello_payload(frame.payload),
                  f"{doc['id']}: 02-6 HELLO fields match")
        if "view" in p:
            check(
                [(int(t, 16), bytes.fromhex(v)) for t, _l, v in
                 ((i["tag"], i["len"], i["hex"]) for i in p["view"]["items"])]
                == C.tlv_decode(frame.app_payload),
                f"{doc['id']}: 05-3 TLV view matches",
            )
        n_frame += 1
    print(f"  .. {n_frame} frame vectors + {n_slip} slip vectors checked")
    check(n_frame >= 90, "vector count sanity (>= 90 frame vectors)")
    check(n_slip >= 16, "vector count sanity (>= 16 slip vectors)")


def t_manifest() -> None:
    print("[3c] MANIFEST.json / INDEX.md agree with the files on disk")
    with open(os.path.join(VECDIR, "MANIFEST.json")) as f:
        man = json.load(f)
    ids = [e["id"] for e in man["vectors"]]
    check(len(ids) == len(set(ids)) == man["counts"]["total"],
          f"manifest ids unique and total == {man['counts']['total']}")
    missing = [e["id"] for e in man["vectors"]
               if not os.path.exists(os.path.join(VECDIR, e["file"]))
               or (e.get("hex_file") and not os.path.exists(os.path.join(VECDIR, e["hex_file"])))]
    check(not missing, f"every manifest entry's file exists{'' if not missing else missing}")
    by_cat: Dict[str, int] = {}
    for e in man["vectors"]:
        by_cat[e["category"]] = by_cat.get(e["category"], 0) + 1
    check(by_cat == man["counts"]["by_category"], "counts.by_category matches the entries")
    per_type = man["counts"]["valid_by_type"]
    check(all(n >= 2 for n in per_type.values()) and set(per_type) == set(C.TYPE_NAMES.values()),
          "02-9(1): every message type has >= 2 valid frames")
    check(man["counts"]["by_category"].get("malformed", 0)
          + man["counts"]["by_category"].get("state", 0) >= 20,
          "02-9(2): >= 20 malformed/state vectors with expected behaviour")
    with open(os.path.join(VECDIR, "INDEX.md")) as f:
        idx = f.read()
    absent = [i for i in ids if f"`{i}`" not in idx]
    check(not absent, f"INDEX.md lists every vector id{'' if not absent else absent}")


# ---------------------------------------------------------------------------
# 4. scenario
# ---------------------------------------------------------------------------


def t_scenario() -> None:
    print("[4] scenario_hello_call_close.txt")
    with open(SCENARIO) as f:
        committed = f.read()
    check(S.build() == committed, "gen_scenario.build() reproduces the file byte for byte")
    steps = re.findall(r"^\[(\d\d)\]", committed, re.M)
    check(len(steps) >= 15 and steps == [f"{i:02d}" for i in range(1, len(steps) + 1)],
          f"02-9(3): step numbers 01..{len(steps)} contiguous (HELLO -> CALL -> frags -> REPLY -> CLOSE)")
    labels = re.findall(r"^\[\d\d\] (.+?) +\d\d-[^\n]*$", committed, re.M)
    types_in_order = [re.sub(r".+->\s+\S+\s+", "", lbl, count=1) for lbl in labels]
    check(bool(types_in_order) and types_in_order[0] == "HELLO"
          and "CALL" in types_in_order and "CALL_REPLY" in types_in_order
          and "CLOSE" in types_in_order and "ERROR" in types_in_order,
          f"02-9(3): transaction covers HELLO..CALL..REPLY..ERROR..CLOSE ({types_in_order[:2]}..)")
    n_call_frag = sum(1 for t in types_in_order if t.startswith("CALL frag"))
    n_reply_frag = sum(1 for t in types_in_order if t.startswith("CALL_REPLY frag"))
    check(n_call_frag >= 3 and n_reply_frag >= 2,
          f"02-9(3): fragmented CALL ({n_call_frag} frags) and CALL_REPLY ({n_reply_frag}) legs present")
    # every raw hex line decodes; only the deliberate corruption fails the CRC
    hex_lines = re.findall(r"^      hex: ([0-9a-f]+)$", committed, re.M)
    check(len(hex_lines) >= 3, f"{len(hex_lines)} raw frame hex lines found")
    bad = 0
    for hx in hex_lines:
        raw = bytes.fromhex(hx)
        frame, diag = C.decode_frame(raw, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
        if diag["reason"] != C.R_OK:
            bad += 1
            check(diag["reason"] == C.R_CRC,
                  f"non-clean scenario frame is exactly the CRC-corruption demo "
                  f"({diag['reason']}, {len(raw)} B)")
    check(bad <= 1, f"at most one deliberately corrupt frame in the script (got {bad})")
    # appendix SLIP pairs: frame(N B): / slip(M B): must roundtrip
    appendix = committed.split("APPENDIX", 1)[1]
    pairs = re.findall(
        r"frame \((\d+) B\): ([0-9a-f]+)\n    slip +\((\d+) B\): ([0-9a-f]+)", appendix)
    check(len(pairs) == 2, f"appendix carries the 2 UART/SLIP handshake frames (got {len(pairs)})")
    for blen, fh, slen, sh in pairs:
        frame = bytes.fromhex(fh)
        stream = bytes.fromhex(sh)
        frames, notes = C.slip_decode(stream)
        check(int(blen) == len(frame) and int(slen) == len(stream),
              f"appendix lengths consistent ({blen} B frame, {slen} B stream)")
        check(frames == [frame] and notes == [],
              "appendix SLIP stream decodes to exactly the printed frame")
        check(C.slip_encode(frame) == stream,
              "appendix stream is exactly slip_encode(frame) (04-4)")
        f2, diag2 = C.decode_frame(frame, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UART])
        check(f2 is not None and diag2["reason"] == C.R_OK and f2.type in (C.TYPE_HELLO, C.TYPE_HELLO_ACK),
              "appendix frame decodes as a HELLO/HELLO_ACK at the UART MTU")


# ---------------------------------------------------------------------------


def main() -> int:
    t_primitives()
    t_roundtrip()
    t_vectors_regen()
    t_vectors_decode()
    t_manifest()
    t_scenario()
    print()
    if FAILURES:
        print(f"selftest: {len(FAILURES)} FAILURE(S), {PASSED} checks passed")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print(f"selftest: all green ({PASSED} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
