#!/usr/bin/env python3
"""Copy a buildable source snapshot and record hashes for every copied input.

The snapshot is independent of later edits in the working tree and uses copied
files, never hardlinks. Build outputs, Git metadata, and local worktrees are
omitted; symlinks among source inputs are preserved rather than followed. The
destination must not already exist.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from datetime import datetime, timezone


EXCLUDED = {
    ".git",
    "__pycache__",
    ".codegraph",
}
ROOT_EXCLUDED = {
    ".kernel-build", ".worktrees", ".omo", ".diag", ".pytest_cache",
    ".zcode", "build", "packages", "site", "hardware", "instances",
    "docs", ".github",
}


def ignore_for(source: Path):
    def ignore(directory: str, names: list[str]) -> set[str]:
        current = Path(directory)
        ignored = {name for name in names if name in EXCLUDED}
        try:
            relative = current.relative_to(source)
        except ValueError:
            relative = Path(".")
        if relative == Path("."):
            ignored.update(ROOT_EXCLUDED)
        if relative == Path("user"):
            ignored.add("build")
        if relative == Path("user/external/musl"):
            ignored.update(name for name in names if name.startswith("build-"))
        return ignored

    return ignore


def inventory(snapshot: Path) -> list[dict[str, str]]:
    records: list[dict[str, str]] = []
    for path in sorted(snapshot.rglob("*")):
        if path == snapshot / "INPUT-MANIFEST.json":
            continue
        rel = path.relative_to(snapshot).as_posix()
        if path.is_symlink():
            target = os.readlink(path)
            resolved = path.resolve(strict=False)
            try:
                resolved.relative_to(snapshot)
            except ValueError as exc:
                raise RuntimeError(f"snapshot symlink escapes snapshot: {rel} -> {target}") from exc
            record = {"path": rel, "kind": "symlink", "target": target}
            record["target_exists"] = str(resolved.exists()).lower()
            if resolved.is_file():
                record["target_sha256"] = hashlib.sha256(resolved.read_bytes()).hexdigest()
            records.append(record)
        elif path.is_file():
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            records.append({"path": rel, "kind": "file", "sha256": digest})
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path,
                        help="new snapshot directory, preferably on the root filesystem")
    parser.add_argument("--source", type=Path, default=Path.cwd(),
                        help="repository working tree (default: current directory)")
    args = parser.parse_args()
    source = args.source.resolve()
    destination = args.destination.expanduser().resolve()
    if not (source / "Makefile").is_file() or not (source / "kernel").is_dir():
        parser.error(f"{source} does not look like the A20OS source root")
    if destination.exists() or destination == source or source in destination.parents:
        parser.error("destination must be a new directory outside the source tree")
    destination.parent.mkdir(parents=True, exist_ok=True)

    try:
        head = subprocess.check_output(
            ["git", "-C", str(source), "rev-parse", "HEAD"], text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        head = "unavailable"
    invocation_cwd = str(Path.cwd().resolve())

    shutil.copytree(source, destination, symlinks=True, ignore=ignore_for(source))
    records = inventory(destination)
    manifest = {
        "source_root": str(source),
        "invocation_cwd": invocation_cwd,
        "head_at_snapshot": head,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "copy_mode": "independent copy; no hardlinks; copied inputs may be read by the build",
        "excluded_names": sorted(EXCLUDED | ROOT_EXCLUDED),
        "excluded_generated_paths": ["user/external/musl/build-*"],
        "inputs": records,
    }
    (destination / "INPUT-MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"snapshot={destination}")
    print(f"head={head}")
    print(f"input_entries={len(records)}")
    print(f"manifest={destination / 'INPUT-MANIFEST.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
