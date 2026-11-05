#!/usr/bin/env python3
"""Summarize paired benchmark samples.json files.

The input is a list of sample objects containing variant, pair, case, output,
exit_code, counter_audit_output, and collection_enabled_after_case. Metrics are
read only from guest output; host wall_seconds is deliberately ignored. Every
pair must contain one successful control and candidate row for each case, with
a valid disabled-counter audit.

For fileio cases, only output between FILEIO_RUN_BEGIN and FILEIO_RUN_END is
measured. The FILEIO PASS line, verified byte count, and cleanup status are
validated separately; verification reads are never used as throughput.

--quality is a required caller-supplied label (noisy, clean, or unclassified).
It records the caller's assessment and does not prove environmental quality.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import statistics
from pathlib import Path


CASES = {
    "cpu-1": ("events_per_second", "events/s", "higher"),
    "cpu-2": ("events_per_second", "events/s", "higher"),
    "threads-2": ("events_per_second", "events/s", "higher"),
    "memory-write": ("written_mib_per_second", "MiB/s", "higher"),
    "sha256": ("sha256_bytes_per_second", "bytes/s", "higher"),
    "fork": ("bogo_ops_per_second_real_time", "ops/s", "higher"),
    "dense-fork": ("ns_per_op", "ns/op", "lower"),
    "getpid": ("ns_per_op", "ns/op", "lower"),
    "yield": ("ns_per_op", "ns/op", "lower"),
    "read": ("ns_per_op", "ns/op", "lower"),
    "write": ("ns_per_op", "ns/op", "lower"),
    "futex-mismatch": ("ns_per_op", "ns/op", "lower"),
    "fileio-100": ("written_mib_per_second", "MiB/s", "higher"),
    "fileio-16384": ("written_mib_per_second", "MiB/s", "higher"),
}

MICRO_MODES = {
    "dense-fork": "fork-touched",
    "getpid": "getpid",
    "yield": "yield",
    "read": "read",
    "write": "write",
    "futex-mismatch": "futex-mismatch",
}

COUNTER_BEGIN_RE = re.compile(r"^APK_COUNTER_STATE_(\d+)_(\d+)$")
COUNTER_END_RE = re.compile(r"^APK_COUNTER_STATE_END_(\d+)_(\d+)$")
ENABLED_RE = re.compile(r"^collection_enabled_before_snapshot:\s*([01])$")
FILEIO_PASS_RE = re.compile(
    r"^FILEIO: PASS verified_bytes=(\d+) cleaned=1$"
)


def fail(message: str) -> None:
    raise ValueError(message)


def _positive_finite(value: float, label: str) -> float:
    if not math.isfinite(value) or value <= 0:
        fail(f"{label}: metric must be finite and positive, got {value}")
    return value


def _parse_fileio(case: str, output: str) -> tuple[float, int]:
    begin = [match.start() for match in re.finditer(
        r"(?m)^FILEIO_RUN_BEGIN\s*$", output
    )]
    end = [match.start() for match in re.finditer(
        r"(?m)^FILEIO_RUN_END\s*$", output
    )]
    if len(begin) != 1 or len(end) != 1 or begin[0] >= end[0]:
        fail(f"{case}: expected one ordered FILEIO_RUN_BEGIN/END section")

    run_end = output.index("FILEIO_RUN_END", begin[0])
    measured = output[begin[0]:run_end]
    rate_lines = re.findall(
        r"(?m)^\s*written, MiB/s:\s*([0-9]+(?:\.[0-9]+)?)\s*$",
        measured,
    )
    if len(rate_lines) != 1:
        fail(f"{case}: expected one written MiB/s metric in run section")

    pass_rows = []
    for line in output.splitlines():
        match = FILEIO_PASS_RE.fullmatch(line)
        if match:
            pass_rows.append(int(match.group(1)))
    if len(pass_rows) != 1 or pass_rows[0] <= 0:
        fail(f"{case}: expected one FILEIO PASS with positive verified_bytes")

    return _positive_finite(float(rate_lines[0]), case), pass_rows[0]


def parse_guest_metric(case: str, output: str) -> tuple[float, int | None]:
    if case not in CASES:
        fail(f"unsupported benchmark case: {case}")

    if case.startswith("fileio-"):
        metric, verified_bytes = _parse_fileio(case, output)
        return metric, verified_bytes

    if case in ("cpu-1", "cpu-2"):
        found = re.findall(
            r"(?m)^\s*events per second:\s*([0-9]+(?:\.[0-9]+)?)\s*$",
            output,
        )
        if len(found) != 1:
            fail(f"{case}: expected one events-per-second metric")
        return _positive_finite(float(found[0]), case), None

    if case == "threads-2":
        times = re.findall(
            r"(?m)^\s*total time:\s*([0-9]+(?:\.[0-9]+)?)s\s*$",
            output,
        )
        events = re.findall(
            r"(?m)^\s*total number of events:\s*([0-9]+)\s*$",
            output,
        )
        if len(times) != 1 or len(events) != 1:
            fail("threads-2: expected one total-events count and total time")
        total_time = float(times[0])
        if not math.isfinite(total_time) or total_time <= 0:
            fail("threads-2: total time must be finite and positive")
        rate = int(events[0]) / total_time
        return _positive_finite(rate, "threads-2 events/s"), None

    if case == "memory-write":
        found = re.findall(
            r"(?m)^\s*([0-9]+(?:\.[0-9]+)?) MiB transferred "
            r"\(([0-9]+(?:\.[0-9]+)?) MiB/sec\)\s*$",
            output,
        )
        if len(found) != 1:
            fail("memory-write: expected one transferred MiB/s metric")
        return _positive_finite(float(found[0][1]), case), None

    if case == "sha256":
        data_rows = re.findall(
            r"(?m)^\+DT:sha256:(\d+):(\d+)\s*$",
            output,
        )
        speed_rows = re.findall(
            r"(?m)^\+F:\d+:sha256:([0-9]+(?:\.[0-9]+)?)\s*$",
            output,
        )
        if len(data_rows) != 1 or len(speed_rows) != 1:
            fail("sha256: expected one sha256 size and bytes/s row")
        duration, size = map(int, data_rows[0])
        if duration != 5 or size != 16384:
            fail("sha256: expected the configured 5s, 16384-byte workload")
        return _positive_finite(float(speed_rows[0]), "sha256 bytes/s"), None

    if case == "fork":
        rows = []
        fork_line = re.compile(
            r"^stress-ng:\s+metrc:\s+\[\d+\]\s+fork\s+\d+\s+"
            r"[0-9]+(?:\.[0-9]+)?\s+[0-9]+(?:\.[0-9]+)?\s+"
            r"[0-9]+(?:\.[0-9]+)?\s+([0-9]+(?:\.[0-9]+)?)\s+"
            r"([0-9]+(?:\.[0-9]+)?)\s*$"
        )
        for line in output.splitlines():
            match = fork_line.fullmatch(line)
            if match:
                rows.append(match.groups())
        if len(rows) != 1:
            fail("fork: expected one stress-ng fork metrics row")
        # stress-ng prints real-time ops/s before the usr+sys ops/s column.
        return _positive_finite(float(rows[0][0]), "fork real-time ops/s"), None

    micro_line = re.compile(
        r"^MICRO\s+mode=([^\s]+)\s+n=\d+\s+mb=\d+\s+"
        r"elapsed_ns=\d+\s+ns_per_op=([0-9]+(?:\.[0-9]+)?)"
        r"(?=\s|$)(?:\s+[A-Za-z_][A-Za-z0-9_]*=[^\s]+)*\s*$"
    )
    expected_mode = MICRO_MODES[case]
    found = [
        match.group(2)
        for line in output.splitlines()
        if (match := micro_line.fullmatch(line))
        and match.group(1) == expected_mode
    ]
    if len(found) != 1:
        fail(f"{case}: expected one matching MICRO ns_per_op metric")
    return _positive_finite(float(found[0]), case), None


def audit_disabled(row: dict) -> None:
    text = row.get("counter_audit_output")
    if not isinstance(text, str):
        fail("missing counter audit output")

    lines = text.splitlines()
    begins = []
    ends = []
    for index, line in enumerate(lines):
        begin = COUNTER_BEGIN_RE.fullmatch(line)
        end = COUNTER_END_RE.fullmatch(line)
        if begin:
            begins.append((index, begin.groups()))
        if end:
            ends.append((index, end.groups()))

    if len(begins) != 1 or len(ends) != 1:
        fail("counter audit must contain exactly one begin/end marker pair")
    if begins[0][1] != ends[0][1] or begins[0][0] >= ends[0][0]:
        fail("counter audit begin/end markers are mismatched or out of order")

    enabled = [
        match.group(1)
        for line in lines[begins[0][0] + 1:ends[0][0]]
        if (match := ENABLED_RE.fullmatch(line))
    ]
    if enabled != ["0"]:
        fail("counter audit must contain exactly one disabled state")
    after = row.get("collection_enabled_after_case")
    if isinstance(after, bool) or after != 0:
        fail("counter collection remained enabled after case")


def summarize(samples: list[dict], quality: str) -> dict:
    if quality not in ("noisy", "clean", "unclassified"):
        fail("quality must be noisy, clean, or unclassified")

    grouped: dict[tuple[str, int], dict[str, dict]] = {}
    for index, row in enumerate(samples):
        if not isinstance(row, dict):
            fail(f"sample {index}: expected object")

        variant = row.get("variant")
        case = row.get("case")
        pair = row.get("pair")
        if variant not in ("control", "candidate"):
            fail(f"sample {index}: invalid variant")
        if case not in CASES:
            fail(f"sample {index}: unsupported case {case!r}")
        if not isinstance(pair, int) or isinstance(pair, bool) or pair <= 0:
            fail(f"sample {index}: pair must be a positive integer")

        exit_code = row.get("exit_code")
        if not isinstance(exit_code, int) or isinstance(exit_code, bool) or exit_code != 0:
            fail(f"{case} pair {pair} {variant}: invalid or nonzero exit code")
        if case == "sha256":
            expected_argv = [
                "openssl", "speed", "-elapsed", "-seconds", "5",
                "-bytes", "16384", "-evp", "sha256", "-mr",
            ]
            if row.get("argv") != expected_argv:
                fail("sha256: argv does not match the configured OpenSSL workload")
            environment = row.get("environment")
            if environment is not None:
                if not isinstance(environment, dict) or environment.get("OPENSSL_ia32cap") != "0":
                    fail("sha256: recorded environment must set OPENSSL_ia32cap=0")
        audit_disabled(row)

        sides = grouped.setdefault((case, pair), {})
        if variant in sides:
            fail(f"{case} pair {pair}: duplicate {variant}")
        sides[variant] = row

    if not grouped:
        fail("no samples")

    pair_sets: dict[str, set[int]] = {}
    pair_rows = []
    for (case, pair), sides in sorted(grouped.items()):
        if set(sides) != {"control", "candidate"}:
            fail(f"{case} pair {pair}: missing paired side")

        pair_sets.setdefault(case, set()).add(pair)
        metric_name, unit, direction = CASES[case]
        control, control_verified = parse_guest_metric(
            case, sides["control"].get("output", "")
        )
        candidate, candidate_verified = parse_guest_metric(
            case, sides["candidate"].get("output", "")
        )
        if control <= 0:
            fail(f"{case} pair {pair}: zero control denominator")
        if control_verified != candidate_verified:
            fail(f"{case} pair {pair}: fileio verified byte counts differ")

        ratio = candidate / control
        result = {
            "case": case,
            "pair": pair,
            "metric": metric_name,
            "unit": unit,
            "direction": direction,
            "control": control,
            "candidate": candidate,
            "candidate_over_control": ratio,
        }
        if control_verified is not None:
            result["verified_bytes"] = control_verified
        if direction == "lower":
            result["latency_decrease_percent"] = 100.0 * (1.0 - ratio)
        else:
            result["throughput_increase_percent"] = 100.0 * (ratio - 1.0)
        pair_rows.append(result)

    expected_pairs = next(iter(pair_sets.values()))
    for case, case_pairs in pair_sets.items():
        if case_pairs != expected_pairs:
            fail(f"{case}: pair set differs from other cases")

    case_results = []
    for case in sorted(pair_sets):
        rows = [row for row in pair_rows if row["case"] == case]
        case_results.append({
            "case": case,
            "metric": rows[0]["metric"],
            "unit": rows[0]["unit"],
            "direction": rows[0]["direction"],
            "pairs": rows,
            "median_candidate_over_control": statistics.median(
                row["candidate_over_control"] for row in rows
            ),
        })

    return {
        "quality": quality,
        "quality_semantics": "caller-supplied label; not automatically verified",
        "metric_source": "guest output only; host wall_seconds ignored",
        "sha256_cap_note": (
            "samples.json does not record per-sample environment; "
            "verify OPENSSL_ia32cap=0 in the runner configuration"
        ),
        "case_results": case_results,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples", type=Path, help="run-pairs samples.json")
    parser.add_argument(
        "--quality",
        required=True,
        choices=("noisy", "clean", "unclassified"),
    )
    parser.add_argument(
        "--out",
        required=True,
        type=Path,
        help="new JSON output path; must not exist",
    )
    args = parser.parse_args()
    if args.out.exists():
        parser.error(f"refusing to overwrite existing output: {args.out}")

    try:
        samples = json.loads(args.samples.read_text())
        if not isinstance(samples, list):
            fail("input JSON must be a list")
        result = summarize(samples, args.quality)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        parser.error(str(exc))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
