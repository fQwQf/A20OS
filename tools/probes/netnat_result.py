#!/usr/bin/env python3
"""Validate one complete guest NAT smoke log and emit its comparison marker."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import sys
from typing import Any


CASE_ID = "nat-rewrite/dnat-tcp-echo-v1"
FIXTURE_PAYLOAD_HASHES = {16: 3919010405}
EXPECTED_PORTS = {"forward_port": 18081, "listen_port": 18082}
EXPECTED_NAT_ACTION = "dnat"
EXPECTED_METRICS = {
    "elapsed_ns", "ns_per_transaction", "transactions_per_second",
}
MARKER = "A20_COMPARE_JSON:"
FATAL_RE = re.compile(r"\b(?:PANIC|FATAL|LOCKSTALL)\b", re.IGNORECASE)
PASS_RE = re.compile(r"^NETNAT_TEST:\s+PASS\b", re.MULTILINE)
FAIL_RE = re.compile(r"NETNAT_TEST:\s*FAIL\b")
SHUTDOWN_AUDIT_RE = re.compile(r"\[SD\]\s*audit errors=(\d+)(?=$|[\s\[])")


class ResultError(ValueError):
    pass


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ResultError(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def _integer(value: Any) -> bool:
    return type(value) is int


def _finite_positive(value: Any) -> bool:
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value) and value > 0)


def validate_log(log_text: str, *, qemu_status: int, probe_status: int,
                 expected_transactions: int = 16) -> dict[str, Any]:
    if expected_transactions not in FIXTURE_PAYLOAD_HASHES:
        raise ResultError(
            f"no registered NAT echo fixture for {expected_transactions} transactions; "
            "the current fixture is fixed at 16"
        )
    if not _integer(qemu_status) or qemu_status != 0:
        raise ResultError(f"QEMU exited with status {qemu_status}, expected 0")
    if not _integer(probe_status) or probe_status != 0:
        raise ResultError(f"host probe exited with status {probe_status}, expected 0")
    fatal = FATAL_RE.search(log_text)
    if fatal:
        raise ResultError(f"guest log contains fatal marker {fatal.group(0)!r}")
    if FAIL_RE.search(log_text):
        raise ResultError("guest log contains NETNAT_TEST: FAIL")
    passes = list(PASS_RE.finditer(log_text))
    if len(passes) != 1:
        raise ResultError(f"expected exactly one NETNAT_TEST: PASS line, found {len(passes)}")

    if "System is going down for power-off NOW." not in log_text:
        raise ResultError("guest did not report normal poweroff")
    if len(re.findall(r"\[SD\]\s*POWER_OFF entry\b", log_text)) != 1:
        raise ResultError("expected exactly one shutdown POWER_OFF entry audit")
    audits = SHUTDOWN_AUDIT_RE.findall(log_text)
    if audits != ["0"]:
        raise ResultError(f"expected exactly one shutdown audit with errors=0, found {audits}")

    marker_count = log_text.count(MARKER)
    if marker_count != 1:
        raise ResultError(f"expected exactly one {MARKER} record, found {marker_count}")
    marker_lines = [line for line in log_text.splitlines() if MARKER in line]
    if len(marker_lines) != 1 or not marker_lines[0].startswith(MARKER):
        raise ResultError(f"{MARKER} must begin its sole log line")
    try:
        payload = json.loads(marker_lines[0][len(MARKER):].strip(),
                              object_pairs_hook=_strict_object,
                              parse_constant=lambda value: (_ for _ in ()).throw(
                                  ResultError(f"invalid JSON numeric constant {value}")))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise ResultError(f"invalid comparison marker JSON: {exc}") from exc

    if not isinstance(payload, dict) or set(payload) != {"schema", "case", "contract", "metrics"}:
        raise ResultError("marker must contain exactly schema, case, contract, and metrics")
    if not _integer(payload["schema"]) or payload["schema"] != 1:
        raise ResultError("marker schema must be integer 1")
    if payload["case"] != CASE_ID:
        raise ResultError(f"marker case must be {CASE_ID}")

    contract = payload["contract"]
    expected_contract = {
        "completed": expected_transactions,
        "errors": 0,
        "payload_hash_fnv1a32": FIXTURE_PAYLOAD_HASHES[expected_transactions],
        **EXPECTED_PORTS,
        "nat_action": EXPECTED_NAT_ACTION,
    }
    if not isinstance(contract, dict) or set(contract) != set(expected_contract):
        raise ResultError("contract keys do not match the registered NAT fixture schema")
    for key in ("completed", "errors", "payload_hash_fnv1a32", "forward_port", "listen_port"):
        if not _integer(contract[key]):
            raise ResultError(f"contract.{key} must be an integer")
    if contract != expected_contract:
        raise ResultError(f"contract differs from fixed {expected_transactions}-transaction fixture")

    metrics = payload["metrics"]
    if not isinstance(metrics, dict) or set(metrics) != EXPECTED_METRICS:
        raise ResultError(f"metrics must contain exactly {sorted(EXPECTED_METRICS)}")
    for name, value in metrics.items():
        if not _finite_positive(value):
            raise ResultError(f"metrics.{name} must be a finite positive number")

    return payload


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="complete QEMU guest console log")
    parser.add_argument("--qemu-status", type=int, required=True)
    parser.add_argument("--probe-status", type=int, required=True)
    parser.add_argument("--expected-transactions", type=int, default=16)
    args = parser.parse_args(argv)
    try:
        log_text = args.log.read_text(encoding="utf-8", errors="replace")
        payload = validate_log(log_text, qemu_status=args.qemu_status,
                               probe_status=args.probe_status,
                               expected_transactions=args.expected_transactions)
    except (OSError, ResultError) as exc:
        print(f"NETNAT_RESULT: FAIL {exc}", file=sys.stderr)
        return 1
    summary = payload["contract"]
    print("NETNAT_RESULT: PASS "
          f"completed={summary['completed']} errors={summary['errors']} "
          f"qemu_status={args.qemu_status} probe_status={args.probe_status}")
    print(MARKER + " " + json.dumps(payload, sort_keys=True, separators=(",", ":"),
                                    allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
