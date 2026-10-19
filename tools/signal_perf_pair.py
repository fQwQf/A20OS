#!/usr/bin/env python3
"""Run alternating C/R end-to-end Signal benchmarks with strict pass checks."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time

from signal_guest_runner import validate_bench_output
from signal_perf_bind import check_log_identity, load_bind, perf_bind


DEFAULT_ROUNDS = "C,R,R,C,C,R"
BIND_STALE_EXIT = 3


def bench_summary(output: str) -> tuple[str, str, str]:
    good, reason = validate_bench_output(output, 0)
    if not good:
        raise ValueError(reason)
    labels = ("killwake", "pingpong", "signalfd")
    results = []
    for label in labels:
        line = next(line for line in output.splitlines() if f"SIGNAL_BENCH: {label} " in line)
        results.append(line)
    return tuple(results)  # type: ignore[return-value]


_RESUME_RESULTS = re.compile(
    r"^SIGNAL_BENCH: (killwake|pingpong|signalfd) .*=([1-9][0-9]*)$")


def re_summary_valid(lines: list) -> list | None:
    """A stored summary is reusable only when it is exactly the three
    validated result lines (positive measurements, in the canonical order).
    These lines are the extract bench_summary() produced for the original
    run after the full log had already passed validate_bench_output(); the
    bind file is what carries that proof across process restarts."""
    if not isinstance(lines, list) or len(lines) != 3:
        return None
    labels = ["killwake", "pingpong", "signalfd"]
    for expected_label, line in zip(labels, lines):
        if not isinstance(line, str):
            return None
        match = _RESUME_RESULTS.fullmatch(line.strip())
        if match is None or match.group(1) != expected_label:
            return None
    return list(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output", type=Path, default=Path("out/perf"))
    parser.add_argument("--rounds", default=DEFAULT_ROUNDS,
                        help="comma-separated C/R sequence (default: %(default)s)")
    parser.add_argument("--bench-args", default="5000 2000 100000")
    parser.add_argument("--timeout", type=float, default=400)
    parser.add_argument("--attempts", type=int, default=3,
                        help="maximum attempts for lock-busy status 75 only")
    parser.add_argument("--busy-backoff", type=float, default=45)
    parser.add_argument("--round-label", default="pairing",
                        help="a label written into every bind record, e.g. order variant")
    parser.add_argument("--c-repo", type=Path, default=None,
                        help="worktree the C rounds were built from (C round "
                             "binds record this tree's commit/rdirty facts; "
                             "default: the pairing repo, i.e. the Rust tree)")
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    outdir = args.output if args.output.is_absolute() else repo / args.output
    outdir.mkdir(parents=True, exist_ok=True)
    sequence = [item.strip().upper() for item in args.rounds.split(",") if item.strip()]
    if not sequence or any(item not in ("C", "R") for item in sequence):
        parser.error("--rounds must contain C and/or R entries")
    if args.attempts <= 0 or args.timeout <= 0 or args.busy_backoff < 0:
        parser.error("attempts and timeout must be positive; busy-backoff cannot be negative")

    runner = Path(__file__).with_name("signal_guest_runner.py")
    bench_command = "signal_bench " + args.bench_args
    if args.c_repo and not args.c_repo.is_dir():
        print(f"--c-repo is not a directory: {args.c_repo}", file=sys.stderr)
        return 2
    c_repo = args.c_repo.resolve() if args.c_repo else repo
    for round_no, impl in enumerate(sequence, 1):
        build = repo / "out" / ("build-c-rv64" if impl == "C" else "build-r-rv64")
        kernel, image = build / "kernel.elf", build / "fat32.img"
        if not kernel.is_file() or not image.is_file():
            print(f"missing {impl} guest inputs under {build}", file=sys.stderr)
            return 2
        source_repo = c_repo if impl == "C" else repo
        before = perf_bind(
            repo=source_repo, kernel=kernel, image=image, impl=impl, round_no=round_no,
            summary_lines=[], bench_args=bench_command, timeout=args.timeout,
            arch="riscv64", cpus=2, mem="1G",
        )
        if not before["inputs"].get("user_program_sha256"):
            print(f"cannot extract {before['inputs']['user_program']} from {image}; refusing an unbound user input",
                  file=sys.stderr)
            return 2
        log = outdir / f"round-{round_no}-{impl}.log"
        bind_path = log.with_suffix(".bind.json")
        # Resume semantics: a round whose existing bind record still matches
        # the current tree's inputs, configuration and round identity is a
        # finished measurement, not stale data -- rerunning it would spend a
        # limited QEMU slot re-measuring an already-bound round with no new
        # information, so it is skipped and its recorded summary is reused.
        # Any mismatch is rejected before starting another guest, so stale
        # measurements cannot be silently replaced or mixed into the pairing.
        previous = load_bind(bind_path)
        if previous is not None:
            stored = previous.get("summary") or []
            resumed_summary = re_summary_valid(stored)
            probe = perf_bind(
                repo=source_repo, kernel=kernel, image=image, impl=impl,
                round_no=round_no, summary_lines=list(resumed_summary or []),
                bench_args=bench_command, timeout=args.timeout,
                arch="riscv64", cpus=2, mem="1G",
            )
            probe["round_label"] = args.round_label
            if resumed_summary is not None and check_log_identity(previous, probe) is None:
                # The measured summary is reused verbatim; the identity probe
                # re-hashes the source and inputs, so a round stays "already bound"
                # when the current tree's kernel/image are byte-identical to
                # the ones the bind was written for.  Anything else falls
                # through to a fresh run and then to the hard mixed-tree
                # rejection below, so a resume can never silently paste a
                # different tree's measurement onto a finished round label.
                print(f"round {round_no} {impl} already bound at "
                      f"{bind_path}; skipping (resume)")
                for line in resumed_summary:
                    print(line)
                continue
            mismatch = check_log_identity(previous, probe)
            print(f"round {round_no} {impl}: refusing stale or incomplete binding at "
                  f"{bind_path} ({mismatch or 'invalid summary'}); choose a new --output",
                  file=sys.stderr)
            return BIND_STALE_EXIT
        rc = 1
        for attempt in range(1, args.attempts + 1):
            rc = subprocess.run(
                [sys.executable, str(runner), "--perf", "--require-bench",
                 str(log), str(args.timeout), str(kernel), str(image),
                 bench_command, "POWEROFF"],
                cwd=repo, check=False,
            ).returncode
            if rc == 75 and attempt < args.attempts:
                print(f"round {round_no} {impl}: QEMU lock busy; retry {attempt + 1}/{args.attempts}")
                time.sleep(args.busy_backoff)
                continue
            break
        if rc != 0:
            print(f"round {round_no} {impl} failed with runner status {rc}; see {log}", file=sys.stderr)
            return rc if rc > 0 else 1
        output = log.read_text(errors="replace")
        try:
            summary = bench_summary(output)
        except ValueError as exc:
            print(f"round {round_no} {impl} has invalid results: {exc}; see {log}", file=sys.stderr)
            return 1
        bind_path = log.with_suffix(".bind.json")
        after = perf_bind(
            repo=source_repo, kernel=kernel, image=image, impl=impl, round_no=round_no,
            summary_lines=list(summary), bench_args=bench_command, timeout=args.timeout,
            # The runner hardcodes the riscv64 machine, 1 GiB and -smp 2; the
            # record must not quote a configuration the guest never ran.
            arch="riscv64", cpus=2, mem="1G",
        )
        for identity_field in ("source",):
            if before[identity_field] != after[identity_field]:
                print(f"round {round_no} {impl}: source tree changed while guest ran; refusing evidence",
                      file=sys.stderr)
                return BIND_STALE_EXIT
        if before["inputs"]["kernel_sha256"] != after["inputs"]["kernel_sha256"]:
            print(f"round {round_no} {impl}: kernel changed while guest ran; refusing evidence", file=sys.stderr)
            return BIND_STALE_EXIT
        record = before
        record["summary"] = list(summary)
        record["inputs"]["image_sha256_after_run"] = after["inputs"]["image_sha256"]
        record["inputs"]["user_program_sha256_after_run"] = after["inputs"].get("user_program_sha256")
        record["round_label"] = args.round_label
        guest_status = re.search(r"__A20_SIGNAL_RUNNER_EXIT_1__:(-?\d+)", output)
        if guest_status is None or int(guest_status.group(1)) != 0:
            print(f"round {round_no} {impl}: missing successful guest command status; refusing evidence",
                  file=sys.stderr)
            return 1
        record["run"] = {"runner_exit_status": rc, "guest_command_exit_status": int(guest_status.group(1))}
        previous = load_bind(bind_path)
        if previous is not None:
            mismatch = check_log_identity(previous, record)
            if mismatch is not None:
                print(
                    f"round {round_no} {impl}: {bind_path} already binds a different "
                    f"{mismatch}; this evidence directory mixes artifacts built from "
                    f"different trees and must not be appraised as one pairing "
                    f"(remove the stale logs+binds or point --output elsewhere)",
                    file=sys.stderr,
                )
                return BIND_STALE_EXIT
        bind_path.write_text(json.dumps(record, indent=2) + "\n")
        print(f"== round {round_no} impl={impl} bind={bind_path}")
        for line in summary:
            print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
