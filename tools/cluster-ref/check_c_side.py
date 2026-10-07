#!/usr/bin/env python3
"""C-side diff runner: run a C decoder over the gold vectors and compare.

This is the harness 02-wire-protocol.md 9 points at when it says the kernel
decoder (WA) and the MCU leaf (WC) must pass every gold vector *offline* before
interoperability work starts.  The C side does not link against Python; it is a
standalone program that reads one .hex file per invocation and prints its decode
result as "key=value" lines (the format is specified in README.md and
implemented by the shipped reference decoder refdec.c).  This runner executes
that program once per vector and diffs the output against the expectation
recorded in the vector's .json.

Usage:
    python3 tools/cluster-ref/check_c_side.py                    # build refdec and run all
    python3 tools/cluster-ref/check_c_side.py --refdec ./refdec  # use an existing binary
    python3 tools/cluster-ref/check_c_side.py --decoder ./mydec  # your kernel/leaf decoder
    python3 tools/cluster-ref/check_c_side.py --mode slip        # only the SLIP vectors
    python3 tools/cluster-ref/check_c_side.py --filter valid-call # substring on the vector id
    python3 tools/cluster-ref/check_c_side.py --print valid-call-01  # show one diff

The output format your decoder must produce (README.md is normative):
  - one "key=value" line per field, no spaces, LF terminated;
  - mandatory keys: wire_len, accept, reason;
  - when accept=1 and the frame is readable: the header keys (magic, ver, type,
    type_name, flags, frag, txid, src_hash, dst_hash, dst_slot, seq,
    payload_len, ttl, csum_kind) and payload_hex;
  - exit status 0 when accepted, 1 when rejected with a verdict, 2 on usage
    errors;
  - SLIP mode (`--slip <file.slip.hex>`): keys stream_len, frames,
    frame<N>_hex=..., notes=<letters or '-'>.

Verdict strings are the ones clframe.py defines (OK, short_frame, bad_magic,
...); notes letters map to the slip note words as documented in README.md.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from typing import Dict, List, Optional, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
VECDIR = os.path.join(HERE, "vectors")

# receiver role -> MTU of the receiving transport (02-1 bounds payload_len by
# it; the table mirrors gen_vectors.RX_CTX and is documented in README.md).
ROLE_MTU = {
    "peer_udp": 1472,
    "head_udp": 1472,
    "peer_udp_hello": 1472,
    "head_udp_hello": 1472,
    "head_uart": 256,
    "head_uart_hello": 256,
    "leaf_uart": 256,
    "leaf_uart_down": 256,
    "loopback": 65536,
}

# note letters (refdec) <-> note words (clframe / vector JSON)
NOTE_MAP = {
    "e": "empty",
    "u": "unterminated_escape",
    "x": "bad_escape",
    "o": "oversize",
    "p": "partial_frame",
    "t": "truncated",
    "m": "bad_magic",
}

FAILURES: List[str] = []
PASSED = 0
SKIPPED = 0


def check(cond: bool, vid: str, what: str, detail: str = "") -> bool:
    global PASSED
    if cond:
        PASSED += 1
        return True
    msg = f"{vid}: {what}" + (f" -- {detail}" if detail else "")
    FAILURES.append(msg)
    return False


def parse_kv(stdout: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for line in stdout.splitlines():
        if not line or "=" not in line:
            continue
        k, _, v = line.partition("=")
        out[k] = v
    return out


def run_decoder(decoder: str, args: List[str]) -> Tuple[int, str, str]:
    p = subprocess.run([decoder] + args, capture_output=True, text=True, timeout=30)
    return p.returncode, p.stdout, p.stderr


def cmp_frame_vector(decoder: str, rel_hex: str, doc: dict, verbose: bool) -> None:
    vid = doc["id"]
    with open(os.path.join(VECDIR, rel_hex)) as f:
        raw = bytes.fromhex(f.read().strip())
    mtu = ROLE_MTU[doc["receiver"]["role"]]
    rc, out, err = run_decoder(decoder, [os.path.join(VECDIR, rel_hex), str(mtu)])
    if not check(rc in (0, 1), vid, "decoder exit status", f"rc={rc} stderr={err.strip()[:200]}"):
        return
    kv = parse_kv(out)
    exp = doc["expect"]["frame"]
    if not check("accept" in kv and "reason" in kv, vid, "mandatory keys accept/reason present",
                 f"keys={sorted(kv)[:12]}"):
        return
    check(kv["accept"] == ("1" if exp["accept"] else "0"), vid, "accept flag",
          f"c={kv['accept']} want={exp['accept']}")
    check(kv["reason"] == exp["reason"], vid, "reason", f"c={kv['reason']} want={exp['reason']}")
    check(kv.get("wire_len") == str(len(raw)), vid, "wire_len",
          f"c={kv.get('wire_len')} want={len(raw)}")
    if doc.get("frame") is None:
        return  # rejected before the frame was readable: nothing else to diff
    j = doc["frame"]
    want = {
        "magic": f"0x{int(j['magic'], 16):04X}",
        "ver": str(j["ver"]),
        "type": str(j["type"]),
        "type_name": j["type_name"],
        "flags": f"0x{int(j['flags'], 16):04X}",
        "frag": f"0x{int(j['frag'], 16):04X}",
        "frag_seq": str(j["frag_seq"]),
        "frag_last": "1" if j["frag_last"] else "0",
        "frag_reserved_nonzero": "1" if j["frag_reserved_nonzero"] else "0",
        "txid": str(j["txid"]),
        "src_hash": j["src_hash"],
        "dst_hash": j["dst_hash"],
        "dst_slot": str(j["dst_slot"]),
        "seq": str(j["seq"]),
        "payload_len": str(j["payload_len"]),
        "ttl": str(j["ttl"]),
        "csum_kind": str(j["csum_kind"]),
        "crc_present": "1" if j["crc_present"] else "0",
    }
    for k, w in want.items():
        check(kv.get(k) == w, vid, f"header {k}", f"c={kv.get(k)} want={w}")
    check(kv.get("payload_hex") == doc["payload"]["hex"], vid, "payload_hex",
          f"len_c={len(kv.get('payload_hex') or '')} len_w={len(doc['payload']['hex'])}")
    # ACK prefix: refdec prints the raw first-4-bytes reading for any
    # SEND/CALL/CALL_REPLY with >= 4 payload bytes; the normative split (only
    # when RELIABLE and not fragmented, A-05/A-06) lives in the JSON.
    if doc["payload"]["ack_prefix"] is not None:
        check(kv.get("ack_prefix") == f"0x{doc['payload']['ack_prefix']:08X}",
              vid, "ack_prefix", f"c={kv.get('ack_prefix')}")
        check(kv.get("app_payload_hex") == doc["payload"]["app_hex"], vid, "app_payload_hex")
    # HELLO payload fields (02-6), compared when both sides rendered them.
    if "hello" in doc["payload"] and kv.get("accept") == "1":
        h = doc["payload"]["hello"]
        hwant = {
            "hello_node_id": h["node_id"],
            "hello_node_id_hash": h["node_id_hash"],
            "hello_proto_min": str(h["proto_min"]),
            "hello_proto_max": str(h["proto_max"]),
            "hello_profile_tier": str(h["profile_tier"]),
            "hello_caps": f"0x{h['caps']:02X}",
            "hello_short_addr": f"0x{h['short_addr']:04X}",
            "hello_link_addr_len": str(h["link_addr_len"]),
            "hello_reserved": str(h["reserved"]),
            "hello_nonce": h["nonce"],
        }
        for k, w in hwant.items():
            check(kv.get(k) == w, vid, f"hello {k}", f"c={kv.get(k)} want={w}")
    _ = verbose


def cmp_slip_vector(decoder: str, rel_hex: str, doc: dict) -> None:
    vid = doc["id"]
    rc, out, err = run_decoder(decoder, ["--slip", os.path.join(VECDIR, rel_hex)])
    if not check(rc in (0, 1), vid, "decoder exit status", f"rc={rc} stderr={err.strip()[:200]}"):
        return
    kv = parse_kv(out)
    slip = doc["slip"]
    want_frames = slip["slip_frames"]
    want_notes = [NOTE_MAP_INV[w] for w in slip["slip_notes"]]
    if not check("frames" in kv and "notes" in kv, vid, "slip keys frames/notes present",
                 f"keys={sorted(kv)[:8]}"):
        return
    check(kv["frames"] == str(len(want_frames)), vid, "slip frame count",
          f"c={kv['frames']} want={len(want_frames)}")
    for i, fh in enumerate(want_frames):
        check(kv.get(f"frame{i}_hex") == fh, vid, f"slip frame{i}_hex",
              f"c={(kv.get(f'frame{i}_hex') or '')[:32]}.. want={fh[:32]}..")
    got_notes = [] if kv["notes"] == "-" else list(kv["notes"])
    check(got_notes == want_notes, vid, "slip notes", f"c={got_notes} want={want_notes}")


NOTE_MAP_INV = {v: k for k, v in NOTE_MAP.items()}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--decoder", help="C decoder binary to test (default: build refdec.c)")
    ap.add_argument("--refdec", dest="decoder", help="alias of --decoder")
    ap.add_argument("--mode", choices=("all", "frame", "slip"), default="all")
    ap.add_argument("--filter", default="", help="substring filter on vector ids")
    ap.add_argument("--print", dest="show", metavar="VID",
                    help="run one vector and print the decoder output + diff")
    args = ap.parse_args()

    decoder = args.decoder
    tmpdir = None
    if decoder is None:
        tmpdir = tempfile.mkdtemp(prefix="a20-refdec-")
        decoder = os.path.join(tmpdir, "refdec")
        cc = os.environ.get("CC", "cc")
        r = subprocess.run([cc, "-O2", "-Wall", "-Wextra", "-o", decoder,
                            os.path.join(HERE, "refdec.c")], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"check_c_side: cannot build refdec.c with {cc}:\n{r.stderr}", file=sys.stderr)
            return 2
    try:
        # the decoder's own self-test first (anchors, slip roundtrip)
        rc, out, err = run_decoder(decoder, ["--selftest"])
        if rc != 0 or "FAIL" in out:
            print(f"check_c_side: decoder --selftest failed (rc={rc}):\n{out}{err}",
                  file=sys.stderr)
            return 2
        print(f"decoder selftest: ok ({out.count(' ok')} anchors)")
        if args.show:
            for cat in ("valid", "malformed", "reserved", "state", "slip"):
                jf = os.path.join(VECDIR, cat, args.show + ".json")
                if os.path.exists(jf):
                    with open(jf) as f:
                        doc = json.load(f)
                    hexf = doc.get("hex_file")
                    if not hexf:
                        print(f"{args.show}: no hex file (abstract vector)")
                        return 0
                    with open(os.path.join(VECDIR, hexf)) as f:
                        print("hex:", f.read().strip())
                    if hexf.endswith(".slip.hex"):
                        rc, out, _ = run_decoder(decoder, ["--slip", os.path.join(VECDIR, hexf)])
                    else:
                        mtu = ROLE_MTU[doc["receiver"]["role"]]
                        rc, out, _ = run_decoder(decoder, [os.path.join(VECDIR, hexf), str(mtu)])
                    print(out)
                    return 0
            print(f"check_c_side: no vector named {args.show}", file=sys.stderr)
            return 2

        with open(os.path.join(VECDIR, "MANIFEST.json")) as f:
            manifest = json.load(f)
        n_frame = n_slip = 0
        for entry in manifest["vectors"]:
            vid = entry["id"]
            if args.filter and args.filter not in vid:
                continue
            hexf = entry.get("hex_file")
            if not hexf:
                continue  # abstract vectors (e.g. state-inflight-limit-01)
            with open(os.path.join(VECDIR, entry["file"])) as f:
                doc = json.load(f)
            if hexf.endswith(".slip.hex"):
                if args.mode in ("all", "slip"):
                    cmp_slip_vector(decoder, hexf, doc)
                    n_slip += 1
            else:
                if args.mode in ("all", "frame"):
                    cmp_frame_vector(decoder, hexf, doc, verbose=bool(args.show))
                    n_frame += 1
        print()
        if FAILURES:
            print(f"check_c_side: {len(FAILURES)} FAILURE(S) "
                  f"({PASSED} checks passed, {n_frame} frame + {n_slip} slip vectors)")
            for f in FAILURES:
                print(f"  - {f}")
            return 1
        print(f"check_c_side: all green ({PASSED} checks over "
              f"{n_frame} frame + {n_slip} slip vectors)")
        return 0
    finally:
        if tmpdir:
            import shutil
            shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
