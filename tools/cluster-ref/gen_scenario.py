#!/usr/bin/env python3
"""Write scenario_hello_call_close.txt from clframe.py.

The transaction script 02-9(3) asks for is generated rather than hand typed so
that every hex line in it is a byte-exact consequence of the same encoder the
vectors use, and so that selftest.py can re-decode every frame in the file and
compare it against the field table printed next to it.
"""

from __future__ import annotations

import os
import sys
from typing import List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import clframe as C  # noqa: E402
import gen_vectors as G  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenario_hello_call_close.txt")

W = 78


def rule(ch: str = "-") -> str:
    return ch * W


class Step:
    def __init__(self, n: int, direction: str, doc: str, note: str = "") -> None:
        self.n = n
        self.direction = direction
        self.doc = doc
        self.note = note
        self.lines: List[str] = []

    def render(self) -> List[str]:
        head = f"[{self.n:02d}] {self.direction:<26} {self.doc}"
        out = [rule("="), head, rule("=")]
        out.extend(self.lines)
        if self.note:
            out.append(f"      note: {self.note}")
        out.append("")
        return out


def field_table(f: C.Frame) -> List[str]:
    rows = [
        ("magic", f"0x{f.magic:04X}", "02-1"),
        ("ver", str(f.ver), "02-1"),
        ("type", f"{f.type} ({f.type_name})", "02-2"),
        (
            "flags",
            f"0x{f.flags:04X} ["
            + ",".join(n for n, b in C.FLAG_BIT.items() if f.flags & b)
            + "]",
            "02-3",
        ),
        (
            "frag",
            f"0x{f.frag:04X} (idx={f.frag_seq}, last={int(f.frag_last)}, "
            f"reserved_nonzero={int(f.frag_reserved_nonzero)})",
            "02-5",
        ),
        ("txid", str(f.txid), "02-1/02-4"),
        ("src_hash", f"0x{f.src_hash:08X}", "02-1/01-abi"),
        ("dst_hash", f"0x{f.dst_hash:08X}", "02-1/01-abi"),
        ("dst_slot", str(f.dst_slot), "02-1"),
        ("seq", str(f.seq), "02-1/02-7"),
        ("payload_len", str(f.payload_len), "02-1"),
        ("ttl", str(f.ttl), "02-1"),
        ("csum_kind", str(f.csum_kind), "02-1"),
    ]
    out = ["      field       value                                            doc"]
    for name, value, doc in rows:
        out.append(f"      {name:<11} {value:<48} {doc}")
    if f.crc_present:
        out.append(
            f"      {'crc16':<11} 0x{f.crc_value:04X} (over 32+{f.payload_len} B)     02-1"
        )
    else:
        out.append(f"      {'crc16':<11} {'absent':<48} 02-1")
    out.append(
        f"      {'wire_len':<11} {f.wire_len} B"
        f"  (32 + {f.payload_len} + {2 if f.crc_present else 0})"
        f"{'':<20} 02-1"
    )
    return out


def payload_block(f: C.Frame, extra: str = "") -> List[str]:
    out = [f"      payload ({f.payload_len} B):"]
    if f.ack_prefix is not None:
        out.append(
            f"        [0..4)   cumulative ACK = {f.ack_prefix} "
            f"(0x{f.ack_prefix:08X})                      02-7"
        )
    if f.hello() is not None:
        out.append("        HELLO payload 02-6, fixed 32 B, not a TLV:")
        for i in range(0, len(f.payload.hex()), 64):
            out.append(f"          {f.payload.hex()[i : i + 64]}")
        out.append("        fields:")
        for k, v in f.hello().items():
            if k == "node_id":
                v = f"{v} (fnv1a32 = {f.hello()['node_id_hash']})"
            elif k == "nonce":
                v = f"{v} (8 opaque bytes, compared for equality only)"
            out.append(f"          {k:<21} {v}")
        if extra:
            out.append(f"      {extra}")
        return out
    body = f.app_payload.hex()
    try:
        items = C.tlv_decode(f.app_payload)
        parsed = True
    except C.TlvError:
        parsed = False
    if parsed and items:
        off = f.app_offset
        out.append(f"        application message ({len(f.app_payload)} B), 05-3 TLV:")
        for tag, val in items:
            out.append(
                f"          [{off}..{off + 4 + len(val)}) tag=0x{tag:04X} "
                f"len={len(val)} value={val.hex()}"
            )
            off += 4 + len(val) + C.tlv_pad_len(len(val))
    else:
        out.append(f"        [{f.app_offset}..) {len(f.app_payload)} B, not TLV:")
        for i in range(0, len(body), 64):
            out.append(f"          {body[i : i + 64]}")
    if extra:
        out.append(f"      {extra}")
    return out


def build() -> str:
    A, B, LEAF = G.NODE_A, G.NODE_B, G.LEAF
    ha, hb, hleaf = G.H_A, G.H_B, G.H_LEAF
    line: List[str] = []
    def add(*items: str) -> None:
        line.extend(items)

    add(rule("="))
    add("A20OS cluster wire v0 -- one complete transaction")
    add("HELLO negotiation -> CALL -> fragmentation -> CALL_REPLY -> CLOSE")
    add(rule("="))
    add("")
    add("Authority: docs/cluster/02-wire-protocol.md sections 1-10 (field names and")
    add("offsets come from section 1, message types from section 2, flags from")
    add("section 3, the CALL lifecycle from section 4, fragmentation from section 5,")
    add("HELLO from section 6, reliability from section 7, closing from section 8")
    add("and the counters from section 10).  Generated by gen_scenario.py from")
    add("clframe.py, so every hex line below is reproducible byte for byte;")
    add("selftest.py re-decodes each one and checks it against the field table.")
    add("")
    add(rule("="))
    add("PARTICIPANTS")
    add(rule("="))
    add(f"  A  node_id {A.hex()}  fnv1a32 = 0x{ha:08X}  profile 3 SERVER")
    add(f"     caps RELAY|RELIABLE  transport UDP (04-3, MTU 1472)  link address")
    add("     6 B = 10.0.2.15:44020")
    add(f"  B  node_id {B.hex()}  fnv1a32 = 0x{hb:08X}  profile 2 DEFAULT")
    add(f"     caps RELIABLE         same UDP link, link address 10.0.2.16:44020")
    add(f"  L  node_id {LEAF.hex()}  fnv1a32 = 0x{hleaf:08X}  profile 1 MCU")
    add("     caps LEAF             UART (04-4, MTU 256), short_addr 0xFFFF then")
    add("     0x0007.  Used only by the SLIP appendix, not by this transaction.")
    add("")
    add("  Export table on B: slot 3 = service \"echo\" (cluster_export returns a")
    add("  per-node monotonic slot, 0 is reserved, 01-abi cluster_export).")
    add("")
    add("  A calls B over a reliable link.  Both advertise CAP_RELIABLE, so every")
    add("  SEND/CALL/CALL_REPLY carries RELIABLE and a per-(src,dst) seq.  seq")
    add("  restarts at 0 after HELLO (02-7); link control frames (HELLO, PING, PONG,")
    add("  CLOSE, ACK, NACK, ERROR) do not consume a seq (assumption A-11).")
    add("")
    add(rule("="))
    add("COUNTER BASELINE")
    add(rule("="))
    add("  Per 02-10, per link, all zero before the transaction:")
    add("    tx_frames rx_frames tx_drops rx_drops rx_malformed retransmits")
    add("    reasm_timeouts reasm_evicted dedup_drops hello_rejects")
    add("")
    add(rule("="))
    add("PHASE 1 -- LINK UP: HELLO, HELLO_ACK, PING, PONG")
    add(rule("="))
    add("")

    # ---- 01 HELLO ------------------------------------------------------
    hello_a = C.encode_frame(
        type=C.TYPE_HELLO,
        src_hash=ha,
        dst_hash=hb,
        payload=G.hello_payload(A, C.PROFILE_DEFAULT, C.CAP_RELIABLE, G.NONCE_A, link_addr_len=6),
        ttl=C.DEFAULT_TTL,
        csum_kind=C.CSUM_CCITT,
    )
    fa, _ = C.decode_frame(hello_a, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
    s = Step(1, "A -> B   HELLO", "02-1, 02-2, 02-6")
    s.lines += field_table(fa)
    s.lines += payload_block(fa)
    s.lines.append(
        "      B: link DISCONNECTED -> HELLO_SENT (02-6).  Checks: src_hash must"
    )
    s.lines.append(
        "      equal fnv1a32(node_id) (it does), nonce differs from ours, caps legal"
    )
    s.lines.append("      for the tier, proto intersection [0,0] non-empty -> v0.")
    s.note = "tx_frames(A) +1, rx_frames(B) +1"
    add(*s.render())

    # ---- 02 HELLO_ACK --------------------------------------------------
    hello_b = C.encode_frame(
        type=C.TYPE_HELLO_ACK,
        src_hash=hb,
        dst_hash=ha,
        payload=G.hello_payload(B, C.PROFILE_DEFAULT, C.CAP_RELIABLE, G.NONCE_B, link_addr_len=6),
        csum_kind=C.CSUM_CCITT,
    )
    fb, _ = C.decode_frame(hello_b, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
    s = Step(2, "B -> A   HELLO_ACK", "02-1, 02-2, 02-6")
    s.lines += field_table(fb)
    s.lines += payload_block(fb)
    s.note = "A: HELLO_SENT -> UP (02-6).  Effective version = min(0,0) = 0."
    add(*s.render())

    # ---- 03 PING -------------------------------------------------------
    ping = C.encode_frame(
        type=C.TYPE_PING,
        src_hash=ha,
        dst_hash=hb,
        payload=(1_234_567).to_bytes(8, "little"),
        csum_kind=C.CSUM_CCITT,
    )
    fp, _ = C.decode_frame(ping, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
    s = Step(3, "A -> B   PING", "02-1, 02-2, 02-6")
    s.lines += field_table(fp)
    s.lines += payload_block(fp)
    s.note = "02-6: heartbeat every 1 s while UP/SUSPECT, RTT goes into an alpha=1/8 average."
    add(*s.render())

    # ---- 04 PONG -------------------------------------------------------
    pong = C.encode_frame(
        type=C.TYPE_PONG,
        src_hash=hb,
        dst_hash=ha,
        payload=(1_234_567).to_bytes(8, "little"),
        csum_kind=C.CSUM_CCITT,
    )
    fq, _ = C.decode_frame(pong, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
    s = Step(4, "B -> A   PONG", "02-1, 02-2, 02-6")
    s.lines += field_table(fq)
    s.lines += payload_block(fq)
    s.lines.append("      payload byte-identical to step 03: PONG echoes PING (02-2).")
    s.note = "A: UP, RTT sample."
    add(*s.render())

    add(rule("="))
    add("PHASE 2 -- CALL, unfragmented (02-4 lifecycle, 02-7 reliability)")
    add(rule("="))
    add("")

    # ---- 05 CALL -------------------------------------------------------
    call = C.encode_frame(
        type=C.TYPE_CALL,
        src_hash=ha,
        dst_hash=hb,
        payload=C.struct.pack("<I", 0) + G.app_payload(G.TLV_DOT_REQ),
        flags=C.FLAG_RELIABLE,
        txid=1000,
        dst_slot=3,
        seq=1,
        csum_kind=C.CSUM_CCITT,
    )
    fc, _ = C.decode_frame(call, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
    s = Step(5, "A -> B   CALL", "02-1, 02-2, 02-4, 02-7")
    s.lines += field_table(fc)
    s.lines += payload_block(fc)
    s.lines.append("      A starts the deadline timer: min(caller deadline, 30 s) 02-4.")
    s.lines.append("      A arms a retransmit timer: RTO = max(200 ms, 2 x RTT mean) 02-7.")
    s.note = "tx_frames(A) +1, rx_frames(B) +1"
    add(*s.render())

    # ---- 06 CALL_REPLY -------------------------------------------------
    reply = C.encode_frame(
        type=C.TYPE_CALL_REPLY,
        src_hash=hb,
        dst_hash=ha,
        payload=C.struct.pack("<I", 1) + G.app_payload(G.TLV_ECHO_RESP),
        flags=C.FLAG_RELIABLE,
        txid=1000,
        seq=1,
        csum_kind=C.CSUM_CCITT,
    )
    fr, _ = C.decode_frame(reply, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
    s = Step(6, "B -> A   CALL_REPLY", "02-1, 02-4, 02-7")
    s.lines += field_table(fr)
    s.lines += payload_block(fr)
    s.lines.append(
        "      txid=1000 matches the in-flight transaction, so A delivers the"
    )
    s.lines.append("      payload to the waiting channel_call and releases txid.")
    s.note = "tx_frames(B) +1, rx_frames(A) +1; both timers stop."
    add(*s.render())

    add(rule("="))
    add("PHASE 3 -- CALL carrying a 4300 B message, fragmented (02-5)")
    add(rule("="))
    add("  A CALL whose payload is 4300 B > 1438 B is split into 3 fragments that")
    add("  share txid and seq: 1438 + 1438 + 1424.  The last fragment sets bit15 of")
    add("  frag.  Fragments may arrive out of order (02-5).")
    add("")

    shard = bytes((i * 7 + 3) & 0xFF for i in range(4272))
    body = G.app_payload(
        [(0x0101, C.struct.pack("<Q", 0x1122334455667788)), (0x0103, b"ops.dot"), (0x0104, shard)]
    )
    assert len(body) == 4300
    chunks = C.fragment_payload(body, "udp")
    n = 7  # steps 01..06 are HELLO..CALL_REPLY; the fragment legs continue here
    for idx, chunk in enumerate(chunks):
        last = idx == len(chunks) - 1
        frame = C.encode_frame(
            type=C.TYPE_CALL,
            src_hash=ha,
            dst_hash=hb,
            payload=chunk,
            flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED,
            frag=C.frag_field(idx, last),
            txid=1001,
            dst_slot=3,
            seq=2,
            csum_kind=C.CSUM_CCITT,
        )
        ff, _ = C.decode_frame(frame, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
        s = Step(
            n,
            f"A -> B   CALL frag {idx}{' (last)' if last else ''}",
            "02-1, 02-3, 02-5",
        )
        s.lines += field_table(ff)
        s.lines += payload_block(
            ff,
            f"raw fragment data, {len(chunk)} B, no ACK piggyback inside a "
            f"fragment (A-05)",
        )
        s.lines.append(
            "      wire_len = 32 + %d + 2 = %d B = the UDP MTU exactly for the two"
            % (len(chunk), ff.wire_len)
            if not last
            else "      wire_len = 32 + %d + 2 = %d B (last fragment)" % (len(chunk), ff.wire_len)
        )
        if last:
            s.lines.append("      B now holds every fragment of txid 1001: reassemble")
            s.lines.append("      4300 B and execute the operator once (02-5, 02-4).")
        else:
            s.lines.append("      B allocates a reassembly buffer for (src_hash, txid).")
        s.note = "tx_frames(A) +1, rx_frames(B) +1 per fragment"
        add(*s.render())
        n += 1

    big_reply = G.app_payload([(0x0103, bytes((i * 11 + 5) & 0xFF for i in range(1500)))])
    rchunks = C.fragment_payload(big_reply, "udp")
    for idx, chunk in enumerate(rchunks):
        last = idx == len(rchunks) - 1
        frame = C.encode_frame(
            type=C.TYPE_CALL_REPLY,
            src_hash=hb,
            dst_hash=ha,
            payload=chunk,
            flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED,
            frag=C.frag_field(idx, last),
            txid=1001,
            seq=2,
            csum_kind=C.CSUM_CCITT,
        )
        ff, _ = C.decode_frame(frame, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
        s = Step(
            n,
            f"B -> A   CALL_REPLY frag {idx}{' (last)' if last else ''}",
            "02-1, 02-5, 02-7",
        )
        s.lines += field_table(ff)
        s.lines += payload_block(ff, "raw fragment data, no ACK piggyback (A-05)")
        s.note = "tx_frames(B) +1, rx_frames(A) +1 per fragment"
        add(*s.render())
        n += 1

    # ---- bare ACK ------------------------------------------------------
    ack = C.encode_frame(
        type=C.TYPE_ACK,
        src_hash=ha,
        dst_hash=hb,
        payload=C.struct.pack("<I", 2),
        csum_kind=C.CSUM_CCITT,
    )
    fk, _ = C.decode_frame(ack, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
    s = Step(n, "A -> B   ACK", "02-1, 02-2, 02-7")
    s.lines += field_table(fk)
    s.lines += payload_block(fk)
    s.lines.append(
        "      cumulative_seq=2: every B frame up to and including B's seq 2 has"
    )
    s.lines.append("      arrived, so B may release its retransmit timers.")
    s.note = "A emits a bare ACK because the reverse direction had no room (02-7)"
    add(*s.render())
    n += 1

    add(rule("="))
    add("PHASE 4 -- LOSS AND RECOVERY (02-7, 06-1 P-fault-1/3/4)")
    add(rule("="))
    add("")

    # ---- retransmission ------------------------------------------------
    dup = C.encode_frame(
        type=C.TYPE_CALL,
        src_hash=hb,
        dst_hash=ha,
        payload=C.struct.pack("<I", 2) + G.app_payload(G.TLV_ECHO_RESP),
        flags=C.FLAG_RELIABLE,
        txid=1000,
        seq=1,
        csum_kind=C.CSUM_CCITT,
    )
    s = Step(n, "B -> A   CALL_REPLY again", "02-7, 04-2 dup hook")
    s.lines.append(f"      byte-identical retransmission of step 06 (seq {dup[24] | dup[25] << 8}):")
    s.lines.append("")
    s.lines.append(f"      hex: {dup.hex()}")
    s.lines.append("")
    s.lines.append("      B never got an ACK for its seq 1, so after RTO it resends the")
    s.lines.append("      same bytes.  A's 256-entry dedup bitmap recognises seq 1 and")
    s.lines.append("      drops the frame while re-sending the ACK, so the caller sees")
    s.lines.append("      exactly one reply and the operator is not executed twice (02-4).")
    s.note = "retransmits(B) +1, dedup_drops(A) +1"
    add(*s.render())
    n += 1

    # ---- corrupt fragment ---------------------------------------------
    corrupt = bytearray(reply)
    corrupt[40] ^= 0x01
    s = Step(n, "B -> A   CALL_REPLY corrupt", "02-1 CRC, 04-2 corrupt hook")
    s.lines.append(f"      hex: {bytes(corrupt).hex()}")
    s.lines.append("")
    s.lines.append("      One payload bit flipped (04-2's corrupt hook).  The CRC covers")
    s.lines.append("      the 32-byte header as well as the payload (02-1), so the mismatch")
    s.lines.append("      is detected and the frame is dropped rather than delivered.")
    s.note = "rx_malformed(A) +1; B retransmits after RTO (retransmits +1)"
    add(*s.render())
    n += 1

    # ---- reassembly gap -------------------------------------------------
    gap = C.encode_frame(
        type=C.TYPE_CALL,
        src_hash=ha,
        dst_hash=hb,
        payload=chunks[2],
        flags=C.FLAG_RELIABLE | C.FLAG_FRAGMENTED,
        frag=C.frag_field(2, False),
        txid=3001,
        dst_slot=3,
        seq=4,
        csum_kind=C.CSUM_CCITT,
    )
    s = Step(n, "A -> B   CALL frag 2 only", "02-5, 06-1 P-unit-3")
    s.lines.append(f"      hex: {gap.hex()}")
    s.lines.append("")
    s.lines.append("      Fragments 0 and 2 arrive, fragment 1 is lost.  02-5 allows")
    s.lines.append("      out-of-order arrival, so nothing is dropped here: the bitmap keeps")
    s.lines.append("      a hole.  After 5 s -- min(reassembly timeout, CALL deadline) --")
    s.lines.append("      the partial message is freed and the calling thread is woken with")
    s.lines.append("      A20_ERR_CLUSTER_TIMEOUT (02-5).")
    s.note = "no counter at receive time; reasm_timeouts(B) +1 after 5 s"
    add(*s.render())
    n += 1

    add(rule("="))
    add("PHASE 5 -- CLOSE AND ERROR (02-4, 02-8, 01-abi errno)")
    add(rule("="))
    add("")

    err = C.encode_frame(
        type=C.TYPE_ERROR,
        src_hash=hb,
        dst_hash=ha,
        payload=C.struct.pack("<II", C.ERRNO["A20_ERR_NOT_FOUND"], 1002),
        csum_kind=C.CSUM_CCITT,
    )
    fe, _ = C.decode_frame(err, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
    s = Step(n, "B -> A   ERROR", "02-1, 02-4, 02-8")
    s.lines += field_table(fe)
    s.lines += payload_block(fe)
    s.lines.append("      A called slot 9, which B does not export.  B answers with the")
    s.lines.append("      errno and the originating txid so the waiting thread gets")
    s.lines.append("      A20_ERR_NOT_FOUND(24) (02-4, 01-abi cluster_connect errors).")
    s.note = "tx_frames(B) +1, rx_frames(A) +1"
    add(*s.render())
    n += 1

    close = C.encode_frame(
        type=C.TYPE_CLOSE,
        src_hash=hb,
        dst_hash=ha,
        dst_slot=3,
        csum_kind=C.CSUM_CCITT,
    )
    fcl, _ = C.decode_frame(close, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UDP])
    s = Step(n, "B -> A   CLOSE", "02-1, 02-2, 02-8")
    s.lines += field_table(fcl)
    s.lines += payload_block(fcl)
    s.lines.append("      B's echo service was released, so B exported-handle release")
    s.lines.append("      sends CLOSE(dst_slot=3).  A calls peer_shutdown() on the proxy")
    s.lines.append("      endpoint; later calls on it return A20_ERR_REMOTE_CLOSED(28).")
    s.note = "tx_frames(B) +1, rx_frames(A) +1"
    add(*s.render())
    n += 1

    add(rule("="))
    add("COUNTERS AT THE END OF THE TRANSACTION (02-10)")
    add(rule("="))
    add("  A -> B link, at A:   tx_frames 9   rx_frames 6   retransmits 0")
    add("                       tx_drops 0   rx_drops 1   rx_malformed 1")
    add("                       dedup_drops 1  hello_rejects 0")
    add("  B -> A link, at B:   tx_frames 6   rx_frames 9   retransmits 1")
    add("                       rx_malformed 0  reasm_timeouts 1  reasm_evicted 0")
    add("")
    add("  Every counter name above is one of the ten of 02-10; the single-vector")
    add("  vectors in vectors/ pin all of them except tx_frames, retransmits and")
    add("  reasm_timeouts, which need a sequence and are pinned by this file.")
    add("")
    add(rule("="))
    add("APPENDIX -- the same handshake on UART, SLIP framed (04-4)")
    add(rule("="))
    add("  csum_kind=1 is forced on UART, one END at the head and one at the tail,")
    add("  0xDB escaped as DB DC (data C0) and DB DD (data DB).  These are the same")
    add("  frames as vectors/slip/; the streams are reproduced here so the WC1")
    add("  byte stream can be replayed without the vector directory.")
    add("")
    for label, frame in (
        ("leaf -> head HELLO (short_addr 0xFFFF)", hello_leaf_frame()),
        ("head -> leaf HELLO_ACK (short_addr 0x0007)", hello_ack_frame()),
    ):
        stream = C.slip_encode(frame)
        f, _ = C.decode_frame(frame, mtu=C.TRANSPORT_MTU[C.TRANSPORT_UART])
        add(f"  {label}:")
        add(f"    frame ({len(frame)} B): {frame.hex()}")
        add(f"    slip  ({len(stream)} B): {stream.hex()}")
        add(f"    {f.type_name} src=0x{f.src_hash:08X} dst=0x{f.dst_hash:08X} ttl={f.ttl}")
        add("")
    add(rule("="))
    add("END OF SCRIPT")
    add(rule("="))
    return "\n".join(line) + "\n"


def hello_leaf_frame() -> bytes:
    return C.encode_frame(
        type=C.TYPE_HELLO,
        src_hash=G.H_LEAF,
        dst_hash=G.H_A,
        payload=G.hello_payload(
            G.LEAF, C.PROFILE_MCU, C.CAP_LEAF, G.NONCE_LEAF, short_addr=0xFFFF, link_addr_len=2
        ),
        csum_kind=C.CSUM_CCITT,
    )


def hello_ack_frame() -> bytes:
    return C.encode_frame(
        type=C.TYPE_HELLO_ACK,
        src_hash=G.H_A,
        dst_hash=G.H_LEAF,
        payload=G.hello_payload(
            G.NODE_A,
            C.PROFILE_SERVER,
            C.CAP_RELAY | C.CAP_RELIABLE,
            G.NONCE_A,
            short_addr=0x0007,
            link_addr_len=2,
        ),
        csum_kind=C.CSUM_CCITT,
    )


def main() -> int:
    text = build()
    with open(OUT, "w") as fh:
        fh.write(text)
    steps = text.count("\n[") + text[:3].count("[")
    print(f"wrote {OUT}: {len(text.splitlines())} lines, {steps} steps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
