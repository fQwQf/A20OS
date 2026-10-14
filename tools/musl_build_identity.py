#!/usr/bin/env python3
"""Print a stable identity for a musl source/configuration build.

Generated build trees are excluded from the source walk. The result is used
as a private output-directory component, so two ABI/compiler configurations
cannot clean or reuse each other's musl objects.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


EXCLUDED_DIRS = {".git", ".hg", ".svn", "__pycache__"}


def compiler_identity(command: str) -> dict[str, str]:
    executable = shutil.which(command)
    if executable is None:
        raise ValueError(f"compiler not found: {command}")
    real = str(Path(executable).resolve())

    def capture(*args: str) -> str:
        try:
            result = subprocess.run(
                [real, *args], check=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, timeout=10,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise ValueError(f"cannot identify compiler {real}: {exc}") from exc
        return result.stdout.strip()

    return {
        "path": real,
        "target": capture("-dumpmachine"),
        "version": capture("-dumpfullversion", "-dumpversion"),
    }


def source_digest(root: Path) -> tuple[str, int]:
    if not root.is_dir():
        raise ValueError(f"musl source directory does not exist: {root}")
    digest = hashlib.sha256()
    count = 0
    for current, dirs, files in os.walk(root, topdown=True, followlinks=False):
        dirs[:] = sorted(
            name for name in dirs
            if name not in EXCLUDED_DIRS and not name.startswith("build-")
        )
        files.sort()
        base = Path(current)
        for name in files:
            path = base / name
            relative = path.relative_to(root).as_posix()
            if path.is_symlink():
                payload = os.readlink(path).encode("utf-8", "surrogateescape")
                kind = b"L"
            elif path.is_file():
                payload = path.read_bytes()
                kind = b"F"
            else:
                continue
            digest.update(kind)
            digest.update(relative.encode("utf-8", "surrogateescape"))
            digest.update(b"\0")
            digest.update(len(payload).to_bytes(8, "big"))
            digest.update(payload)
            count += 1
    return digest.hexdigest(), count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--target", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cflags", default="")
    parser.add_argument("--ldflags", default="")
    args = parser.parse_args()

    try:
        source = args.source.resolve(strict=True)
        source_hash, file_count = source_digest(source)
        payload = {
            "schema": 1,
            "source": str(source),
            "source_sha256": source_hash,
            "source_files": file_count,
            "target": args.target,
            "compiler": compiler_identity(args.compiler),
            "cflags": args.cflags,
            "ldflags": args.ldflags,
        }
        canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
        print(hashlib.sha256(canonical.encode()).hexdigest())
    except (OSError, ValueError) as exc:
        print(f"musl-build-identity: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
