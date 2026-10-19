#!/usr/bin/env python3
"""C/Rust acceptance for the process module's Rust selectors on RV64 SMP2.

For each case the runner builds the same `ARCH=riscv64 ABI=both NR_CPUS=2`
kernel twice, changing only the selector under test, then boots both images
under QEMU `-smp 2` and runs the real guest fixture.  The two serial logs must
carry an identical `<PREFIX>` trace, which is the functional equivalence check
between the C policy and the Rust policy.

The runner interleaves the order (C then Rust, then Rust then C) so a warm-cache
advantage cannot masquerade as an implementation difference, and it records the
natural QEMU exit status, the poweroff marker, the kernel/image hashes, the
guest fixture hash and the per-run wall time for the end-to-end comparison.

Builds go through the repository's run-build.sh wrapper; each guest holds the
shared `activity` lease plus the exclusive `qemu-1` lease.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LOCK_DIR = Path("<repo>/.git/parallel-rewrite")
WRAPPER = LOCK_DIR / "run-build.sh"
DEFAULT_USER_BUILD_DIR = Path(
    "<build-dir>/zcode-expanded-20261017/process-build/user-riscv64")

CASES = {
    "pid": {
        "selector": "PID_IMPL",
        "guest": "pid_state_test",
        "trace_prefix": "PID_STATE_TEST:",
        "sources": ["kernel/proc/pid.c", "kernel/proc/pid_alloc.rs"],
        "fixture": "user/cmds/core/pid_state_test.c",
        "markers": [
            "PID_STATE_TEST: PASS",
            "PID_STATE_TEST: initial-pid-max PASS value=32768",
            "PID_STATE_TEST: pidmap-fields PASS",
            "PID_STATE_TEST: self-id PASS",
            "PID_STATE_TEST: simultaneous-ids PASS delta=8",
            "PID_STATE_TEST: reap-releases PASS",
            "PID_STATE_TEST: cursor-advances PASS",
            "PID_STATE_TEST: fork-reap-cycles PASS rounds=200",
            "PID_STATE_TEST: pid-max-window PASS",
            "PID_STATE_TEST: pid-max-refusals PASS",
        ],
    },
    "parkwait": {
        "selector": "PARK_WAIT_IMPL",
        "guest": "proc_stress",
        "guests": ["proc_stress", "syscall_smoke"],
        "trace_prefix": "PROC_STRESS:",
        "sources": ["kernel/proc/park.c", "kernel/proc/park_wait_state.c",
                    "kernel/proc/park_wait_state.rs",
                    "kernel/proc/park_wait_state.h"],
        "fixture": "user/cmds/stress/proc_stress.c",
        "forbid": ["SYSCALL_SMOKE: FAIL"],
        "markers": [
            "PROC_STRESS: PASS",
            "PROC_STRESS: signal-stop-exit PASS",
            "PROC_STRESS: signal-mask-park PASS",
            "PROC_STRESS: reap-churn PASS",
            "SYSCALL_SMOKE: PASS",
            "SYSCALL_SMOKE: parent-exit-reap stress PASS",
        ],
    },
    "taskobjects": {
        "selector": "TASK_OBJECTS_IMPL",
        "guest": "/bin/native-futex-rv",
        "trace_prefix": "NATIVE",
        "sources": ["kernel/proc/task_object.c", "kernel/proc/task_object.rs"],
        "fixture": "kernel/proc/task_object.c",
        "markers": ["NATIVE_THREAD: PASS", "NATIVE_FUTEX: PASS",
                    "NATIVE_ATOMIC32: PASS"],
    },
}

SMP_MARKER = "[SMP] 2/2 configured CPUs online"
POWEROFF = "System is going down for power-off NOW"


def sha256_file(path: Path) -> str | None:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return None


def dirty_diff_hash() -> str:
    diff = subprocess.run(["git", "diff", "HEAD"], cwd=REPO,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    names = subprocess.run(["git", "ls-files", "-o", "--exclude-standard"], cwd=REPO,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    h = hashlib.sha256()
    h.update(diff)
    h.update(b"\0")
    h.update(names)
    return h.hexdigest()


def build(flags: list[str], root: Path, jobs: str, label: str,
          retries: int) -> tuple[list[str], Path, int]:
    """Run one dev-build under the shared build slot; retry while it is busy."""
    command = [str(WRAPPER), "build-1", "make", f"-j{jobs}",
               f"MUSL_MAKE_JOBS=-j{jobs}", *flags, "dev-build"]
    log = root / f"{label}-build.log"
    rc = 75
    for attempt in range(retries):
        with log.open("wb") as output:
            rc = subprocess.run(command, cwd=REPO, stdout=output,
                                stderr=subprocess.STDOUT).returncode
        if rc != 75:
            break
        # The shared build slot is busy: back off and retry the same command.
        time.sleep(min(10 * (attempt + 1), 60))
    return command, log, rc


def run_case(name: str, args) -> dict:
    case = CASES[name]
    root = args.build_root.resolve() / name
    root.mkdir(parents=True, exist_ok=True)
    selectors = case.get("selectors") or [case["selector"]]
    markers = [*case["markers"], SMP_MARKER, POWEROFF]
    forbid = ["PANIC", "FATAL", "LOCKSTALL", f"{case['trace_prefix']} FAIL",
              *case.get("forbid", [])]
    # A fixture whose whole point is to crash a child says so explicitly.
    for exempt in case.get("forbid_exempt", []):
        forbid = [w for w in forbid if w != exempt]

    record: dict = {
        "case": name,
        "selectors": selectors,
        "guest_fixture": case["fixture"],
        "guest_fixture_sha256": sha256_file(REPO / case["fixture"]),
        "sources_sha256": {p: sha256_file(REPO / p) for p in case["sources"]},
        "required_markers": markers,
        "forbidden_markers": forbid,
        "builds": {},
        "images": {},
        "runs": [],
    }
    common = case.get("base", ["ARCH=riscv64", "BOARD=qemu-virt-riscv64",
                               "ABI=both", "BRINGUP=0", "NR_CPUS=2",
                               "ALLOW_UNVERIFIED_SMP=1", "KERNEL_WERROR=1"])
    common = [*common, *case.get("extra", [])]
    user_build_dir = Path(case.get("user_build_dir") or args.user_build_dir)

    for impl in ("c", "rust"):
        build_dir = root / impl
        build_dir.mkdir(parents=True, exist_ok=True)
        flags = [*common, *[f"{sel}={impl}" for sel in selectors],
                 f"BUILD_DIR={build_dir}", f"USER_BUILD_DIR={user_build_dir}"]
        command = [str(WRAPPER), "build-1", "make", f"-j{args.jobs}",
                   f"MUSL_MAKE_JOBS=-j{args.jobs}", *flags, "dev-build"]
        log = root / f"{impl}-build.log"
        if args.skip_build:
            # Re-run only the guest phase of an already-built case, which keeps
            # the shared guest lease held for as little time as possible.
            rc = 0 if (build_dir / "kernel.elf").exists() else 1
        else:
            command, log, rc = build(flags, root, args.jobs, impl, args.build_retries)
        record["builds"][impl] = {
            "command": command, "exit_status": rc,
            "log": str(log), "log_sha256": sha256_file(log),
        }
        if rc != 0:
            return {**record, "result": f"build-failed({rc})"}

    for impl in ("c", "rust"):
        build_dir = root / impl
        flags = [*common, *[f"{sel}={impl}" for sel in selectors],
                 f"BUILD_DIR={build_dir}", f"USER_BUILD_DIR={user_build_dir}"]
        argv_text = subprocess.check_output(
            ["make", "-s", *flags, "_qemu_argv"], cwd=REPO, text=True).strip()
        qemu_argv = shlex.split(argv_text)
        if "-smp" not in qemu_argv or qemu_argv[qemu_argv.index("-smp") + 1] != "2":
            return {**record, "result": f"expected -smp 2 in {qemu_argv}"}
        qemu_argv = [*qemu_argv, *case.get("qemu_extra", [])]
        record["images"][impl] = {
            "build_dir": str(build_dir),
            "qemu_argv": qemu_argv,
            "qemu_argv_sha256": hashlib.sha256("\0".join(qemu_argv).encode()).hexdigest(),
            "kernel_elf_sha256": sha256_file(build_dir / "kernel.elf"),
            "fat32_img": str(build_dir / "fat32.img"),
            "fat32_img_pre_sha256": sha256_file(build_dir / "fat32.img"),
        }

    order: list[str] = []
    for round_index in range(args.rounds):
        order.extend(["c", "rust"] if round_index % 2 == 0 else ["rust", "c"])

    def run_guest(index: int, impl: str) -> dict:
        """Run one guest under the shared lease, in a dedicated worker process.

        The worker runs as `flock -s activity.lock flock -x qemu-1.lock ...`,
        the same shape the other owners use.  A blocking flock puts this run in
        the kernel's wait queue, so a long peer batch delays it fairly instead
        of letting a fast poller starve; a busy inner lock exits 75 and is
        retried by the caller.
        """
        image = record["images"][impl]
        log_path = root / f"run{index}-{impl}.log"
        spec_path = root / f"run{index}-{impl}.spec.json"
        result_path = root / f"run{index}-{impl}.result.json"
        spec = {
            "qemu_argv": image["qemu_argv"],
            "log": str(log_path),
            "result": str(result_path),
            "guests": case.get("guests") or [case["guest"]],
            "trace_prefix": case["trace_prefix"],
            "markers": markers,
            "forbid": forbid,
            "timeout": args.timeout,
            "boot_delay": args.boot_delay,
            "fat32_img": image["fat32_img"],
        }
        spec_path.write_text(json.dumps(spec))
        if result_path.exists():
            result_path.unlink()
        command = ["timeout", str(args.lease_timeout),
                   "flock", "-s", str(LOCK_DIR / "activity.lock"),
                   "flock", "-E", "75", "-n", str(LOCK_DIR / "qemu-1.lock"),
                   sys.executable, str(Path(__file__).resolve()),
                   "--guest-worker", str(spec_path)]
        for _attempt in range(args.lease_retries):
            proc = subprocess.run(command, cwd=REPO, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
            if proc.returncode == 0 and result_path.exists():
                result = json.loads(result_path.read_text())
                result["round"] = index
                result["impl"] = impl
                return result
            if proc.returncode == 124:
                return {"round": index, "impl": impl, "lease_busy": True}
            if proc.returncode != 75:
                return {"round": index, "impl": impl,
                        "worker_failed": proc.returncode}
            time.sleep(0.2)
        return {"round": index, "impl": impl, "lease_busy": True}

    for index, impl in enumerate(order):
        run = run_guest(index, impl)
        if run.get("lease_busy"):
            record["result"] = "guest-lease-busy"
            return record
        if "worker_failed" in run:
            record["result"] = f"guest-worker-failed({run['worker_failed']})"
            return record
        record["runs"].append(run)
        if (run["missing_markers"] or run["qemu_returncode"] != 0
                or any(run["fatal_markers"].values())):
            record["result"] = "guest-failed"
            return record


    traces = {impl: sorted({line for run in record["runs"] if run["impl"] == impl
                            for line in run["trace"]}) for impl in ("c", "rust")}
    record["trace"] = traces
    if traces["c"] != traces["rust"]:
        record["result"] = "trace-mismatch"
        record["trace_only_c"] = sorted(set(traces["c"]) - set(traces["rust"]))
        record["trace_only_rust"] = sorted(set(traces["rust"]) - set(traces["c"]))
        return record

    timings = {impl: [run["wall_seconds"] for run in record["runs"]
                      if run["impl"] == impl] for impl in ("c", "rust")}
    record["wall_seconds"] = timings
    record["wall_seconds_median"] = {impl: statistics.median(v)
                                     for impl, v in timings.items()}
    record["result"] = "pass"
    return record


def guest_worker(spec_path: Path) -> int:
    """Body of one guest run; the caller already holds both leases."""
    spec = json.loads(spec_path.read_text())
    log_path = Path(spec["log"])
    markers = spec["markers"]
    forbid = spec["forbid"]
    timeout = spec["timeout"]
    boot_delay = spec["boot_delay"]
    started = time.monotonic()
    proc = subprocess.Popen(spec["qemu_argv"], cwd=REPO, stdin=subprocess.PIPE,
                            stdout=open(log_path, "wb"),
                            stderr=subprocess.STDOUT)
    sent = False
    try:
        while time.monotonic() - started < timeout:
            if not sent and time.monotonic() - started >= boot_delay:
                for line in (*spec["guests"], "poweroff"):
                    proc.stdin.write((line + "\n").encode())
                    proc.stdin.flush()
                    time.sleep(0.5)
                sent = True
            content = log_path.read_text(errors="replace")
            if all(m in content for m in markers):
                break
            if any(f in content for f in forbid):
                break
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
    elapsed = time.monotonic() - started
    content = log_path.read_text(errors="replace")
    result = {
        "qemu_argv": spec["qemu_argv"],
        "qemu_returncode": proc.returncode,
        "qemu_log": str(log_path),
        "qemu_log_sha256": sha256_file(log_path),
        "wall_seconds": round(elapsed, 3),
        "poweroff_seen": POWEROFF in content,
        "fatal_markers": {w: content.count(w) for w in forbid},
        "missing_markers": [m for m in markers if m not in content],
        "trace": sorted({line for line in content.splitlines()
                         if line.startswith(spec["trace_prefix"])}),
        "fat32_img_post_sha256": sha256_file(Path(spec["fat32_img"])),
    }
    Path(spec["result"]).write_text(json.dumps(result))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--case", action="append", choices=[*CASES, "all"], default=[])
    ap.add_argument("--guest-worker", type=Path, default=None,
                    help="internal: run one guest spec under the held lease")
    ap.add_argument("--lease-timeout", type=int, default=3600,
                    help="seconds to wait in the flock queue for one guest")
    ap.add_argument("--build-root", type=Path,
                    default=Path("<build-dir>/zcode-expanded-20261017/process-runs/process"))
    ap.add_argument("--jobs", default="2")
    ap.add_argument("--timeout", type=float, default=220.0)
    ap.add_argument("--boot-delay", type=float, default=14.0)
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--skip-build", action="store_true",
                    help="reuse an existing build root and run only the guests")
    ap.add_argument("--build-retries", type=int, default=120,
                    help="build-slot acquisition attempts before giving up")
    ap.add_argument("--lease-retries", type=int, default=60000,
                    help="guest-lease acquisition attempts before giving up")
    ap.add_argument("--user-build-dir", type=Path, default=DEFAULT_USER_BUILD_DIR)
    args = ap.parse_args()
    if args.guest_worker is not None:
        return guest_worker(args.guest_worker)

    requested = args.case or ["all"]
    names = list(CASES) if "all" in requested else requested
    root = args.build_root.resolve()
    root.mkdir(parents=True, exist_ok=True)

    header = {
        "schema": 1,
        "runner": str(Path(__file__).resolve()),
        "runner_sha256": sha256_file(Path(__file__).resolve()),
        "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"],
                                                 cwd=REPO, text=True).strip(),
        "dirty_diff_sha256": dirty_diff_hash(),
        "cases": names,
    }
    results = []
    failed = False
    for name in names:
        result = run_case(name, args)
        results.append(result)
        status = result["result"]
        print(f"{name}: {status}")
        if status == "pass":
            print(f"  kernel c={result['images']['c']['kernel_elf_sha256']}")
            print(f"  kernel rust={result['images']['rust']['kernel_elf_sha256']}")
            print(f"  wall seconds {result['wall_seconds_median']}")
            print(f"  trace lines {len(result['trace']['c'])}")
        else:
            failed = True
            print(f"  {json.dumps({k: v for k, v in result.items() if k not in ('runs', 'builds', 'images')}, indent=2)[:2000]}")
            for run in result.get("runs", []):
                print(f"  run{run['round']} {run['impl']}: rc={run['qemu_returncode']} "
                      f"missing={run['missing_markers']} fatal={run['fatal_markers']}")
    report = root / "report.json"
    report.write_text(json.dumps({**header, "results": results}, indent=2) + "\n")
    print(f"report: {report}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
