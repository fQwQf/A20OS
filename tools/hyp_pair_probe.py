#!/usr/bin/env python3
"""Live P1/P2 gate. Run only after runtime validation is authorized.

Requires an already-built, running named H host with a20-vmd. It creates two
real RAMFS guests, checks shell execution and console separation, reconnects,
pauses/resumes, waits for clean shutdown and destroys both guest identities.
It never builds artifacts and never accepts a printed PASS in lieu of checks.
"""
from __future__ import annotations
import argparse
import contextlib
import json
import secrets
import struct
import time
from pathlib import Path
from a20_error import A20Error
from a20_vm import START, READ, WRITE, PAUSE, RESUME, STOP, DESTROY, STATS
from a20_vm import _name, _profile, _rpc, list_guests


def probe(host: str, profile_name: str, timeout: float, report: Path) -> None:
    profile = _profile(profile_name)
    nonce = secrets.token_hex(5)
    names = [f"probe-a-{nonce}", f"probe-b-{nonce}"]
    identities: dict[str, tuple[int, int]] = {}
    cursors = dict.fromkeys(names, 0)
    output = {name: bytearray() for name in names}
    evidence = dict(host=host, names=names, profile=profile_name, passed=[], status="running")

    def save():
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n")
        for name, data in output.items():
            report.with_name(f"{report.stem}-{name}.log").write_bytes(data)

    def action(name: str, op: int):
        epoch, generation = identities[name]
        return _rpc(host, op, _name(name) + struct.pack("<Q", generation), expected_epoch=epoch)

    def pump():
        for name in names:
            epoch, generation = identities[name]
            data, _ = _rpc(host, READ, _name(name) + struct.pack("<QQI", generation, cursors[name], 2048),
                           expected_epoch=epoch)
            if len(data) < 20:
                raise A20Error("truncated console reply")
            start, end, gap = struct.unpack_from("<QQI", data)
            if gap or start != cursors[name] or end - start != len(data) - 20:
                raise A20Error(f"{name}: history gap or invalid cursor")
            output[name].extend(data[20:]); cursors[name] = end

    def wait_for(predicate, label: str):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            pump()
            records, epoch = list_guests(host)
            if any(item[0] != epoch for item in identities.values()):
                raise A20Error("manager restarted during probe")
            by_name = {g["name"]: g for g in records}
            if predicate(by_name):
                evidence["passed"].append(label); save(); return
            time.sleep(0.03)
        save()
        raise A20Error(f"timeout: {label}; logs retained beside {report}")

    def send(name: str, data: bytes):
        epoch, generation = identities[name]
        deadline = time.monotonic() + timeout
        while data and time.monotonic() < deadline:
            reply, _ = _rpc(host, WRITE, _name(name) + struct.pack("<Q", generation) + data[:256],
                            expected_epoch=epoch)
            if len(reply) != 4: raise A20Error("truncated input acknowledgement")
            count, = struct.unpack("<I", reply)
            if count > min(256, len(data)): raise A20Error("invalid input acknowledgement")
            data = data[count:]
            if not count: pump(); time.sleep(0.03)
        if data: raise A20Error(f"{name}: input remained backpressured")

    try:
        baseline, stats_epoch = _rpc(host, STATS)
        if len(baseline) != 40: raise A20Error("stats layout mismatch")
        evidence["baseline_stats"] = list(struct.unpack("<5Q", baseline))
        for name in names:
            kernel = profile["boot"]["kernel"].encode().ljust(256, b"\0")
            result, epoch = _rpc(host, START, _name(name) + struct.pack("<II", profile["memory_mb"], profile["vcpus"]) + kernel)
            if len(result) != 8: raise A20Error("truncated start acknowledgement")
            generation, = struct.unpack("<Q", result)
            identities[name] = epoch, generation
        wait_for(lambda _: all(b"A20OS Kernel" in output[n] and b"\n# " in output[n] for n in names),
                 "two independent guest shells booted")
        if profile["vcpus"] > 1:
            marker = f"[SMP] {profile['vcpus']}/{profile['vcpus']} configured CPUs online".encode()
            wait_for(lambda _: all(marker in output[n] for n in names),
                     "both guest kernels report all requested virtual CPUs online")
        tokens = [f"A20_{n}".encode() for n in names]
        for name in names:
            # Adjacent quoted pieces mean the expected token is absent from
            # terminal input echo; seeing it proves the guest shell executed.
            send(name, f'echo "A20_"\'{name}\'\n'.encode())
        wait_for(lambda _: all(token in output[name] for name, token in zip(names, tokens)),
                 "both shells executed distinct commands")
        if tokens[0] in output[names[1]] or tokens[1] in output[names[0]]:
            raise A20Error("guest console output crossed identities")
        evidence["passed"].append("console separation")
        # Every RPC reconnects to the control socket. Retain the prior cursor
        # and require a fresh command result after many disconnects.
        fresh = f"RECONNECTED_{nonce}".encode()
        send(names[0], f'echo "RECONNECTED_"\'{nonce}\'\n'.encode())
        wait_for(lambda _: fresh in output[names[0]], "console reconnect retained guest state")
        action(names[0], PAUSE)
        wait_for(lambda state: state[names[0]]["state"] == 2, "first VMM observed stopped")
        alive = f"OTHER_ALIVE_{nonce}".encode()
        send(names[1], f'echo "OTHER_ALIVE_"\'{nonce}\'\n'.encode())
        wait_for(lambda _: alive in output[names[1]], "second guest executes while first paused")
        action(names[0], RESUME)
        wait_for(lambda state: state[names[0]]["state"] == 1, "first VMM resumed")
        for name in names: send(name, b"exit\n")
        wait_for(lambda state: all(state[n]["state"] == 4 and state[n]["code"] == 0 and
                                  cursors[n] >= state[n]["output_end"] for n in names),
                 "both guests shut down cleanly and output drained")
        for name in names: action(name, DESTROY)
        records, _ = list_guests(host)
        if any(g["name"] in names for g in records): raise A20Error("destroy left guest records")
        evidence["passed"].append("guest identities destroyed")
        after, after_epoch = _rpc(host, STATS)
        if after_epoch != stats_epoch or len(after) != 40:
            raise A20Error("stats identity/layout changed")
        evidence["final_stats"] = list(struct.unpack("<5Q", after))
        if evidence["baseline_stats"][:4] != evidence["final_stats"][:4]:
            raise A20Error("VM/vCPU/mapping/table counters did not return to baseline")
        evidence["passed"].append("VM resource counters returned to baseline")
        evidence["status"] = "pass"
    except BaseException as exc:
        evidence["status"] = "fail"; evidence["error"] = str(exc)
        raise
    finally:
        for name in identities:
            with contextlib.suppress(A20Error): action(name, STOP)
        save()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--profile", default="a20-ramfs-riscv64")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.timeout <= 0: parser.error("--timeout must be positive")
    try:
        probe(args.host, args.profile, args.timeout, args.report)
    except (A20Error, OSError, ValueError) as exc:
        print(f"HYP_PAIR: FAIL {exc}"); return 1
    print(f"HYP_PAIR: PASS; evidence in {args.report}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
