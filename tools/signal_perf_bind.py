#!/usr/bin/env python3
"""Commit-bound provenance records for Signal guest evidence.

Historical guest/perf logs recorded numbers but not the source tree they ran
against; a report without at least one binding (source commit, kernel/image
hashes) can neither be rerun nor challenged, and mixing records from two
different trees inside one pairing silently invalidates the comparison.  Both
failure modes are the "collection looks successful but means nothing" trap:
the report says PASS while the evidence qualifies only as historical
observation.

`perf_bind` solves that for one finished guest round: it records
  - the exact source commit and a hash over the dirty worktree diff,
  - sha256 of the kernel and the guest image (the image carries the userland),
  - the bench argv and every runner-established configuration fact,
and it refuses mismatched input artifacts: a new record whose kernel/image
hashes disagree with an existing record for the same log name means the
directory mixes rounds built from different trees, which must be a hard error,
not a plausible-looking number.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess

RUNNER_TOOL = "signal_guest_runner.py"
PAIR_TOOL = "signal_perf_pair.py"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def user_program_sha256(image: Path, program: str = "signal_bench") -> str | None:
    """Hash the exact benchmark executable carried by the boot image."""
    copied = subprocess.run(
        ["mcopy", "-i", str(image), f"::/{program}", "-"],
        capture_output=True, check=False,
    )
    if copied.returncode != 0:
        return None
    return hashlib.sha256(copied.stdout).hexdigest()


def dirty_diff_sha256(repo: Path) -> str | None:
    """Hash of the current dirty worktree diff, or None when the tree is clean.

    Hash the actual tracked patch plus contents of untracked, non-ignored
    files. Build/evidence files below `out/` are excluded so writing a record
    does not change the identity it records. Git-ignored build outputs are
    excluded by `ls-files --others --exclude-standard`.
    """
    tracked = subprocess.run(
        ["git", "-C", str(repo), "diff", "--binary", "HEAD", "--", ".", ":!out/**"],
        capture_output=True, check=False,
    )
    untracked = subprocess.run(
        ["git", "-C", str(repo), "ls-files", "--others", "--exclude-standard", "-z"],
        capture_output=True, check=False,
    )
    if tracked.returncode != 0 or untracked.returncode != 0:
        return None
    digest = hashlib.sha256()
    digest.update(b"tracked-diff\0")
    digest.update(tracked.stdout)
    paths = sorted(path for path in untracked.stdout.split(b"\0")
                   if path and not path.startswith(b"out/"))
    for raw_path in paths:
        path = repo / raw_path.decode("utf-8", errors="surrogateescape")
        try:
            if path.is_symlink():
                payload = b"symlink\0" + path.readlink().as_posix().encode("utf-8", errors="surrogateescape")
            elif path.is_file():
                payload = b"file\0" + path.read_bytes()
            else:
                continue
        except OSError:
            return None
        digest.update(raw_path + b"\0" + payload + b"\0")
    if not tracked.stdout and not paths:
        return None
    return digest.hexdigest()


def commit_of(repo: Path) -> str | None:
    rev = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=False,
    )
    return rev.stdout.strip() or None if rev.returncode == 0 else None


def perf_bind(
    *,
    repo: Path,
    kernel: Path,
    image: Path,
    impl: str,
    round_no: int,
    summary_lines: list[str],
    bench_args: str,
    timeout: float,
    arch: str = "riscv64",
    cpus: int = 2,
    mem: str = "1G",
    user_program: str = "signal_bench",
) -> dict:
    """The full binding record for one guest round; no estimation, only facts."""
    return {
        "round": round_no,
        "impl": impl,
        "summary": list(summary_lines),
        "bench_args": bench_args,
        "config": {
            "arch": arch,
            "cpus": cpus,
            "mem": mem,
            "qemu_timeout_s": timeout,
            "runner": RUNNER_TOOL,
            "pairing": PAIR_TOOL,
        },
        "source": {
            "commit": commit_of(repo),
            "dirty_diff_sha256": dirty_diff_sha256(repo),
        },
        "inputs": {
            "kernel_sha256": sha256_file(kernel),
            "image_sha256": sha256_file(image),
            "user_program": user_program,
            "user_program_sha256": user_program_sha256(image, user_program),
        },
    }


def load_bind(path: Path) -> dict | None:
    try:
        return json.loads(path.read_text("utf-8"))
    except (OSError, ValueError):
        return None


def check_log_identity(previous: dict, next_record: dict) -> str | None:
    """A mismatched import on one field is fatal: the evidence mixes trees.

    Returns the offending field name when `next_record` would silently replace
    a record whose kernel/image hash came out differently; None when the log
    name is repurposed consistently (rerun of the same artifacts).
    """
    for section in ("source", "config"):
        if previous.get(section) != next_record.get(section):
            return section
    for field in ("impl", "round", "bench_args", "round_label"):
        if previous.get(field) != next_record.get(field):
            return field
    field = "inputs.kernel_sha256"
    if previous["inputs"]["kernel_sha256"] != next_record["inputs"]["kernel_sha256"]:
        return field
    field = "inputs.image_sha256"
    if previous["inputs"]["image_sha256"] != next_record["inputs"]["image_sha256"]:
        return field
    field = "inputs.user_program_sha256"
    if previous["inputs"].get("user_program_sha256") != next_record["inputs"].get("user_program_sha256"):
        return field
    return None
