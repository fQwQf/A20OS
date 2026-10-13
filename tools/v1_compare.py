#!/usr/bin/env python3
"""Run a paired, artifact-backed A20OS v1 comparison from a TOML protocol.

Commands are argv arrays (never shell strings). Every invocation gets a fresh
run directory and complete stdout/stderr/exit/timeout record. A workload emits
one ``A20_COMPARE_JSON: {...}`` line describing its contract result and numeric
metrics; the runner compares exact contract objects and evaluates paired
performance samples against repeated self-controls.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import signal
import subprocess
import sys
import time
import tomllib
from urllib.parse import quote
from typing import Any


SCHEMA = 1
MARKER = "A20_COMPARE_JSON:"
VERSIONS = ("c0", "cn", "candidate")
DEFAULT_FATAL_PATTERNS = (
    r"\bPANIC\b", r"\bFATAL\b", r"\bLOCKSTALL\b",
    r"\bKERNEL BUG\b", r"\bBUG:\s", r"\bOops:\b", r"\bDEADLOCK DETECTED\b",
)
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
MIN_PERF_REPETITIONS = 6
CONFIDENCE = 0.95


class SpecError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise SpecError(message)


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def _canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False)


def load_spec(path: Path) -> tuple[dict[str, Any], bytes]:
    raw = path.read_bytes()
    try:
        spec = tomllib.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        raise SpecError(f"cannot parse TOML protocol: {exc}") from exc
    _require(type(spec.get("schema_version")) is int and spec["schema_version"] == SCHEMA,
             f"schema_version must be {SCHEMA}")
    comparison = spec.get("comparison")
    _require(isinstance(comparison, dict), "[comparison] table is required")
    for key in ("id", "requirement_id", "protocol_id"):
        _require(isinstance(comparison.get(key), str) and comparison[key].strip(),
                 f"comparison.{key} must be a non-empty string")
    _require(bool(SHA256_RE.fullmatch(str(comparison.get("inputs_sha256", "")))),
             "comparison.inputs_sha256 must be a 64-character SHA-256")
    _require(isinstance(comparison.get("order_seed"), int) and
             not isinstance(comparison.get("order_seed"), bool),
             "comparison.order_seed must be pre-registered as an integer")
    _require(isinstance(comparison.get("environment"), dict) and comparison["environment"],
             "comparison.environment must record the compared environment")

    versions = spec.get("versions")
    _require(isinstance(versions, dict), "[versions.c0], [versions.cn], and [versions.candidate] are required")
    for name in VERSIONS:
        record = versions.get(name)
        _require(isinstance(record, dict), f"[versions.{name}] table is required")
        for key in ("id", "revision"):
            _require(isinstance(record.get(key), str) and record[key].strip(),
                     f"versions.{name}.{key} must be a non-empty string")
        _require(isinstance(record.get("validated"), bool),
                 f"versions.{name}.validated must explicitly be true or false")
        _require(isinstance(record.get("evidence"), str) and record["evidence"].strip(),
                 f"versions.{name}.evidence must identify evidence or say pending")

    cases = spec.get("cases")
    _require(isinstance(cases, list) and cases, "at least one [[cases]] entry is required")
    seen: set[str] = set()
    for case in cases:
        _require(isinstance(case, dict), "each [[cases]] entry must be a table")
        case_id = case.get("id")
        _require(isinstance(case_id, str) and re.fullmatch(r"[A-Za-z0-9_.\-/]+", case_id),
                 "each case needs a safe id using letters, digits, _, ., /, or -")
        _require(case_id not in seen, f"duplicate case id: {case_id}")
        seen.add(case_id)
        fatal_patterns = case.get("fatal_patterns", [])
        _require(isinstance(fatal_patterns, list) and
                 all(isinstance(pattern, str) for pattern in fatal_patterns),
                 f"{case_id}: fatal_patterns must be an array of regular-expression strings")
        try:
            for pattern in fatal_patterns:
                re.compile(pattern, re.IGNORECASE)
        except re.error as exc:
            raise SpecError(f"{case_id}: invalid fatal pattern: {exc}") from exc
        _require(case.get("kind") in ("functional", "performance", "functional_performance"),
                 f"{case_id}: kind must be functional, performance, or functional_performance")
        repetitions = case.get("repetitions")
        _require(isinstance(repetitions, int) and not isinstance(repetitions, bool) and repetitions > 0,
                 f"{case_id}: repetitions must be a positive integer")
        if case["kind"] in ("performance", "functional_performance"):
            _require(repetitions >= MIN_PERF_REPETITIONS,
                     f"{case_id}: performance needs at least {MIN_PERF_REPETITIONS} paired repetitions")
        timeout = case.get("timeout_seconds")
        _require(_is_number(timeout) and timeout > 0,
                 f"{case_id}: timeout_seconds must be positive")
        _require(isinstance(case.get("expected_contract"), dict),
                 f"{case_id}: expected_contract must be an independently specified object")
        commands = case.get("commands")
        _require(isinstance(commands, dict), f"{case_id}: [cases.commands] tables are required")
        for version in VERSIONS:
            command = commands.get(version)
            _require(isinstance(command, dict), f"{case_id}: command table for {version} is required")
            argv = command.get("argv")
            _require(isinstance(argv, list) and argv and all(isinstance(arg, str) for arg in argv),
                     f"{case_id}: commands.{version}.argv must be a non-empty string array")
            artifact_paths = command.get("artifact_paths")
            _require(isinstance(artifact_paths, list) and artifact_paths and
                     all(isinstance(item, str) and item for item in artifact_paths),
                     f"{case_id}: commands.{version}.artifact_paths must list built/tested artifacts to hash")
            cwd = command.get("cwd", ".")
            _require(isinstance(cwd, str) and cwd, f"{case_id}: commands.{version}.cwd must be a path")
            env = command.get("env", {})
            _require(isinstance(env, dict) and all(isinstance(k, str) and isinstance(v, str)
                                                   for k, v in env.items()),
                     f"{case_id}: commands.{version}.env must map strings to strings")
        metrics = case.get("metrics", {})
        _require(isinstance(metrics, dict), f"{case_id}: metrics must be a table")
        if case["kind"] in ("performance", "functional_performance"):
            _require(bool(metrics), f"{case_id}: performance cases must declare metrics")
        for name, metric in metrics.items():
            _require(isinstance(metric, dict), f"{case_id}: metric {name} must be a table")
            _require(metric.get("direction") in ("lower", "higher"),
                     f"{case_id}: metric {name}.direction must be lower or higher")
            _require(isinstance(metric.get("unit"), str) and metric["unit"].strip(),
                     f"{case_id}: metric {name}.unit is required")
            budget_key = "budget_max" if metric["direction"] == "lower" else "budget_min"
            other_budget_key = "budget_min" if metric["direction"] == "lower" else "budget_max"
            _require(other_budget_key not in metric,
                     f"{case_id}: metric {name} uses {other_budget_key} with the wrong direction")
            if budget_key in metric:
                _require(_is_number(metric[budget_key]) and metric[budget_key] > 0,
                         f"{case_id}: metric {name}.{budget_key} must be a positive number")
            for key in ("calibration_sha256", "calibration_evidence"):
                _require(isinstance(metric.get(key), str) and metric[key].strip(),
                         f"{case_id}: metric {name}.{key} must identify fixed baseline calibration")
            _require(bool(SHA256_RE.fullmatch(metric["calibration_sha256"])),
                     f"{case_id}: metric {name}.calibration_sha256 must be a 64-character SHA-256")
            resolution = metric.get("resolution_ratio")
            bound = metric.get("noninferiority_bound_ratio")
            _require(_is_number(resolution) and resolution >= 1.0,
                     f"{case_id}: metric {name}.resolution_ratio must be at least 1")
            _require(_is_number(bound) and bound >= 1.0,
                     f"{case_id}: metric {name}.noninferiority_bound_ratio must be at least 1")
            _require(bound == resolution,
                     f"{case_id}: metric {name} noninferiority bound must equal the pre-registered resolution")
    return spec, raw


def _safe_token(value: str) -> str:
    # Percent encoding is injective for the allowed case IDs (unlike replacing
    # punctuation with underscores, which can alias e.g. a/b and a_b).
    return quote(value, safe="-_.")


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _atomic_json(path: Path, value: Any) -> None:
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
                     encoding="utf-8")
    temp.replace(path)


def _kill_process_group(process: subprocess.Popen[bytes]) -> None:
    grace_deadline = time.monotonic() + 2
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    # Give every member of the group, including descendants, the full grace
    # interval. The direct child exiting must not shorten it for its children.
    time.sleep(max(0.0, grace_deadline - time.monotonic()))
    # The direct child may have exited while a grandchild still owns the
    # process group. Always send SIGKILL to the group after the grace period.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def _parse_marker(output: str, expected_case: str) -> tuple[dict[str, Any] | None, str | None]:
    records = [line[len(MARKER):].strip() for line in output.splitlines()
               if line.startswith(MARKER)]
    if len(records) != 1:
        return None, f"expected exactly one {MARKER} record, found {len(records)}"
    try:
        payload = json.loads(records[0])
    except json.JSONDecodeError as exc:
        return None, f"invalid marker JSON: {exc}"
    if (not isinstance(payload, dict) or type(payload.get("schema")) is not int or
            payload["schema"] != SCHEMA):
        return None, f"marker schema must be {SCHEMA}"
    if payload.get("case") != expected_case:
        return None, f"marker case must be {expected_case!r}"
    if not isinstance(payload.get("contract"), dict):
        return None, "marker contract must be a JSON object"
    metrics = payload.get("metrics", {})
    if not isinstance(metrics, dict):
        return None, "marker metrics must be a JSON object"
    for name, value in metrics.items():
        if not isinstance(name, str) or not _is_number(value):
            return None, "marker metrics must contain finite numeric values"
    payload["metrics"] = metrics
    return payload, None


def _run_one(*, root: Path, out: Path, case: dict[str, Any], version: str,
             replica: str, iteration: int, command_spec: dict[str, Any],
             fatal_regexes: list[re.Pattern[str]]) -> dict[str, Any]:
    case_dir = out / "runs" / _safe_token(case["id"]) / f"round-{iteration:03d}" / replica
    case_dir.mkdir(parents=True, exist_ok=False)
    cwd_text = command_spec.get("cwd", ".")
    cwd = (root / cwd_text).resolve()
    substitutions = {"run_dir": str(case_dir.resolve()), "case_id": case["id"],
                    "iteration": str(iteration), "version": version}
    argv = [part.replace("{run_dir}", substitutions["run_dir"])
            .replace("{case_id}", substitutions["case_id"])
            .replace("{iteration}", substitutions["iteration"])
            .replace("{version}", substitutions["version"])
            for part in command_spec["argv"]]
    artifact_paths = [part.replace("{run_dir}", substitutions["run_dir"])
                      .replace("{case_id}", substitutions["case_id"])
                      .replace("{iteration}", substitutions["iteration"])
                      .replace("{version}", substitutions["version"])
                      for part in command_spec["artifact_paths"]]
    env = os.environ.copy()
    env.update(command_spec.get("env", {}))
    env.update({"A20_COMPARE_RUN_DIR": str(case_dir.resolve()),
                "A20_COMPARE_CASE": case["id"], "A20_COMPARE_ITERATION": str(iteration),
                "A20_COMPARE_VERSION": version})
    result: dict[str, Any] = {
        "version": version, "replica": replica, "iteration": iteration,
        "argv": argv, "cwd": str(cwd), "started_monotonic_ns": time.monotonic_ns(),
        "status": "FAIL", "marker": None, "errors": [], "fatal_matches": [],
        "argv_sha256": hashlib.sha256(json.dumps(argv, separators=(",", ":")).encode()).hexdigest(),
        "configured_environment_sha256": hashlib.sha256(json.dumps(
            command_spec.get("env", {}), sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
    }
    stdout_path = case_dir / "stdout.log"
    stderr_path = case_dir / "stderr.log"
    start = time.monotonic_ns()
    timed_out = False
    returncode: int | None = None
    try:
        with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
            process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=stdout_file,
                                       stderr=stderr_file, start_new_session=True)
            try:
                returncode = process.wait(timeout=float(case["timeout_seconds"]))
            except subprocess.TimeoutExpired:
                timed_out = True
                _kill_process_group(process)
                returncode = process.returncode
    except OSError as exc:
        result["errors"].append(f"cannot start command: {exc}")
    elapsed = time.monotonic_ns() - start
    result.update({"elapsed_ns": elapsed, "returncode": returncode, "timed_out": timed_out,
                   "stdout_path": str(stdout_path.relative_to(out)),
                   "stderr_path": str(stderr_path.relative_to(out))})
    artifact_hashes = {}
    for artifact_name in artifact_paths:
        artifact = Path(artifact_name)
        if not artifact.is_absolute():
            artifact = cwd / artifact
        if artifact.is_file():
            artifact_hashes[str(artifact)] = _hash_file(artifact)
        else:
            result["errors"].append(f"declared artifact is missing: {artifact}")
    result["artifact_sha256"] = artifact_hashes
    stdout = stdout_path.read_text(encoding="utf-8", errors="replace") if stdout_path.exists() else ""
    stderr = stderr_path.read_text(encoding="utf-8", errors="replace") if stderr_path.exists() else ""
    combined = stdout + "\n" + stderr
    result["stdout_sha256"] = _hash_file(stdout_path) if stdout_path.exists() else None
    result["stderr_sha256"] = _hash_file(stderr_path) if stderr_path.exists() else None
    for regex in fatal_regexes:
        for match in regex.finditer(combined):
            result["fatal_matches"].append({"pattern": regex.pattern,
                                            "text": match.group(0)[:200]})
    if timed_out:
        result["errors"].append("command timed out; entire process group was terminated")
    if returncode != 0 and not result["errors"]:
        result["errors"].append(f"command exited with status {returncode}")
    if result["fatal_matches"]:
        result["errors"].append("fatal/panic audit matched the complete command output")
    if returncode == 0 and not timed_out and not result["fatal_matches"] and not result["errors"]:
        marker, marker_error = _parse_marker(stdout + "\n" + stderr, case["id"])
        if marker_error:
            result["errors"].append(marker_error)
        else:
            result["marker"] = marker
            declared_metrics = set(case.get("metrics", {}))
            emitted_metrics = set(marker["metrics"])
            if declared_metrics != emitted_metrics:
                result["errors"].append(
                    f"metric keys differ: expected {sorted(declared_metrics)}, got {sorted(emitted_metrics)}")
            elif any(value <= 0 for value in marker["metrics"].values()):
                result["errors"].append("performance metric values must be positive")
            else:
                result["status"] = "OK"
    result["finished_monotonic_ns"] = time.monotonic_ns()
    return result


def _order_stat_ci(values: list[float]) -> dict[str, Any] | None:
    """Exact distribution-free confidence interval for the population median."""
    n = len(values)
    if not n:
        return None
    ordered = sorted(values)
    selected: tuple[int, float] | None = None
    for r in range(1, (n + 1) // 2 + 1):
        tail = sum(math.comb(n, k) for k in range(r)) / (2 ** n)
        coverage = 1.0 - 2.0 * tail
        if coverage >= CONFIDENCE:
            selected = (r, coverage)
        else:
            break
    if selected is None:
        return None
    r, coverage = selected
    return {"n": n, "method": "exact distribution-free order-statistic interval for median",
            "confidence": coverage, "lower": ordered[r - 1], "upper": ordered[n - r]}


def _degradation(candidate: float, reference: float, direction: str) -> float:
    return candidate / reference if direction == "lower" else reference / candidate


def _evaluate_performance(case: dict[str, Any], rounds: list[dict[str, Any]]) -> dict[str, Any]:
    if any(not round_["complete"] for round_ in rounds):
        return {"status": "FAIL", "reason": "one or more repetitions failed and remain in the sample record"}
    if len(rounds) < MIN_PERF_REPETITIONS:
        return {"status": "EVIDENCE_INSUFFICIENT", "reason": "too few valid paired repetitions"}
    output: dict[str, Any] = {"status": "PASS", "comparisons": {}}
    for reference in ("c0", "cn"):
        metrics_result: dict[str, Any] = {}
        for name, metric in case["metrics"].items():
            controls, candidate_ratios = [], []
            candidate_values = []
            raw_samples = []
            for round_ in rounds:
                a = round_["markers"][f"{reference}-a"]["metrics"][name]
                b = round_["markers"][f"{reference}-b"]["metrics"][name]
                candidate = round_["markers"]["candidate"]["metrics"][name]
                candidate_values.append(candidate)
                controls.append(max(_degradation(a, b, metric["direction"]),
                                    _degradation(b, a, metric["direction"])))
                ref_center = (a + b) / 2.0
                candidate_ratios.append(_degradation(candidate, ref_center, metric["direction"]))
                raw_samples.append({"iteration": round_["iteration"], "reference_a": a,
                                    "reference_b": b, "candidate": candidate,
                                    "reference_center": ref_center,
                                    "degradation_ratio": candidate_ratios[-1]})
            noise_ci = _order_stat_ci(controls)
            candidate_ci = _order_stat_ci(candidate_ratios)
            bound = metric["noninferiority_bound_ratio"]
            if noise_ci is None or candidate_ci is None:
                status = "EVIDENCE_INSUFFICIENT"
                reason = "repetitions do not support a 95% distribution-free median interval"
            elif noise_ci["upper"] > metric["resolution_ratio"]:
                status = "EVIDENCE_INSUFFICIENT"
                reason = "current self-control noise exceeds the pre-registered baseline resolution"
            elif candidate_ci["upper"] <= bound:
                status, reason = "PASS", "candidate upper interval is within the fixed pre-registered resolution"
            elif candidate_ci["lower"] > bound:
                status, reason = "FAIL", "candidate degradation exceeds the fixed pre-registered resolution"
            else:
                status = "EVIDENCE_INSUFFICIENT"
                reason = "interval overlaps measured resolution; add repetitions or reduce measurement noise"
            budget_key = "budget_max" if metric["direction"] == "lower" else "budget_min"
            if budget_key in metric:
                budget = metric[budget_key]
                over_budget = (any(value > budget for value in candidate_values)
                               if metric["direction"] == "lower"
                               else any(value < budget for value in candidate_values))
                if over_budget:
                    status = "FAIL"
                    reason = f"one or more candidate samples violate pre-registered {budget_key}={budget} {metric['unit']}"
            metrics_result[name] = {"unit": metric["unit"], "direction": metric["direction"],
                                    "status": status, "reason": reason,
                                    "calibration_sha256": metric["calibration_sha256"],
                                    "calibration_evidence": metric["calibration_evidence"],
                                    "pre_registered_resolution_ratio": metric["resolution_ratio"],
                                    "pre_registered_noninferiority_bound_ratio": bound,
                                    "hard_budget": metric.get(budget_key),
                                    "reference_self_control_degradation_ratio_ci": noise_ci,
                                    "candidate_degradation_ratio_ci": candidate_ci,
                                    "samples": raw_samples}
            if status == "FAIL":
                output["status"] = "FAIL"
            elif status == "EVIDENCE_INSUFFICIENT" and output["status"] != "FAIL":
                output["status"] = "EVIDENCE_INSUFFICIENT"
        output["comparisons"][reference] = metrics_result
    return output


def run(spec_path: Path, out: Path) -> dict[str, Any]:
    spec, raw = load_spec(spec_path)
    root = Path(__file__).resolve().parent.parent
    if out.exists():
        raise SpecError(f"output path already exists; preserve prior evidence: {out}")
    out.mkdir(parents=True)
    comparison = spec["comparison"]
    versions = spec["versions"]
    report: dict[str, Any] = {
        "schema_version": SCHEMA, "comparison": comparison,
        "protocol_sha256": hashlib.sha256(raw).hexdigest(),
        "spec_path": str(spec_path.resolve()), "runner_sha256": _hash_file(Path(__file__).resolve()),
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": {"platform": platform.platform(), "python": platform.python_version()},
        "order_seed": comparison["order_seed"],
        "versions": versions, "cases": [], "status": "RUNNING",
        "interpretation": "A tag or successful command does not certify a baseline; validation flags and evidence references are user supplied.",
    }
    (out / "protocol.toml").write_bytes(raw)
    _atomic_json(out / "report.json", report)
    order_random = random.Random(comparison["order_seed"])
    for case in spec["cases"]:
        fatal_patterns = list(DEFAULT_FATAL_PATTERNS) + case.get("fatal_patterns", [])
        try:
            fatal_regexes = [re.compile(pattern, re.IGNORECASE) for pattern in fatal_patterns]
        except re.error as exc:
            raise SpecError(f"{case['id']}: invalid fatal pattern: {exc}") from exc
        case_result: dict[str, Any] = {"id": case["id"], "kind": case["kind"],
                                      "repetitions": case["repetitions"], "runs": [],
                                      "status": "RUNNING"}
        report["cases"].append(case_result)
        _atomic_json(out / "report.json", report)
        rounds: list[dict[str, Any]] = []
        for iteration in range(1, case["repetitions"] + 1):
            invocations = [("c0", "c0-a"), ("c0", "c0-b"),
                           ("cn", "cn-a"), ("cn", "cn-b"),
                           ("candidate", "candidate")]
            order_random.shuffle(invocations)
            round_result: dict[str, Any] = {"iteration": iteration, "schedule": [],
                                            "runs": [], "markers": {}, "complete": True}
            for version, replica in invocations:
                round_result["schedule"].append(replica)
                run_result = _run_one(root=root, out=out, case=case, version=version,
                                      replica=replica, iteration=iteration,
                                      command_spec=case["commands"][version],
                                      fatal_regexes=fatal_regexes)
                round_result["runs"].append(run_result)
                case_result["runs"].append(run_result)
                if run_result["status"] != "OK":
                    round_result["complete"] = False
                elif run_result["marker"] is not None:
                    round_result["markers"][replica] = run_result["marker"]
                _atomic_json(out / "report.json", report)
            if round_result["complete"]:
                contracts = [round_result["markers"][slot]["contract"]
                             for slot in ("c0-a", "c0-b", "cn-a", "cn-b", "candidate")]
                expected_json = _canonical_json(case["expected_contract"])
                round_result["contract_equal_expected"] = [
                    _canonical_json(contract) == expected_json for contract in contracts]
                round_result["contract_mismatches"] = [slot for slot, passed in zip(
                    ("c0-a", "c0-b", "cn-a", "cn-b", "candidate"),
                    round_result["contract_equal_expected"]) if not passed]
                if round_result["contract_mismatches"]:
                    round_result["complete"] = False
                    round_result["errors"] = ["contract differed from independent expected_contract: " +
                                               ", ".join(round_result["contract_mismatches"])]
            rounds.append(round_result)
            case_result.setdefault("rounds", []).append(round_result)
            _atomic_json(out / "report.json", report)

        any_failures = any(not round_["complete"] for round_ in rounds)
        if case["kind"] in ("functional", "functional_performance"):
            case_result["functional"] = {
                "status": "FAIL" if any_failures else "PASS",
                "expected_contract": case["expected_contract"],
                "all_repetitions_matched": not any_failures,
            }
        if case["kind"] in ("performance", "functional_performance"):
            case_result["performance"] = _evaluate_performance(case, rounds)
        statuses = []
        for section in ("functional", "performance"):
            if section in case_result:
                statuses.append(case_result[section]["status"])
        status = "FAIL" if "FAIL" in statuses else (
            "EVIDENCE_INSUFFICIENT" if "EVIDENCE_INSUFFICIENT" in statuses else "PASS")
        if not all(versions[v]["validated"] for v in ("c0", "cn")) and status == "PASS":
            status = "EVIDENCE_INSUFFICIENT"
            case_result["baseline_note"] = "C0 and Cn both require validated=true with traceable evidence before PASS"
        case_result["status"] = status
        _atomic_json(out / "report.json", report)

    all_statuses = [case["status"] for case in report["cases"]]
    report["status"] = "FAIL" if "FAIL" in all_statuses else (
        "EVIDENCE_INSUFFICIENT" if "EVIDENCE_INSUFFICIENT" in all_statuses else "PASS")
    report["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    _atomic_json(out / "report.json", report)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spec", type=Path, help="TOML comparison protocol")
    parser.add_argument("--out", type=Path, required=True,
                        help="new evidence directory; existing paths are never overwritten")
    args = parser.parse_args(argv)
    try:
        result = run(args.spec.resolve(), args.out.resolve())
    except (OSError, SpecError) as exc:
        print(f"v1_compare: error: {exc}", file=sys.stderr)
        return 2
    print(f"v1_compare: {result['status']} ({args.out.resolve() / 'report.json'})")
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
