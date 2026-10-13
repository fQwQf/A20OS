#!/usr/bin/env python3
"""Build and run the shared NAT oracle against the fixed v0.18 C source.

The C0 adapter materializes one historical source file inside the comparison
run directory. It never checks out a revision or writes into the worktree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
SOURCE_PATH = "kernel/net/netfilter_rewrite.c"
FIXTURE_PATH = "tools/test-nat-rewrite-host.c"
HEADER_SUFFIXES = {".h", ".inc", ".def"}


class HeaderClosureError(RuntimeError):
    pass


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _git_file(revision: str, path: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(ROOT), "show", f"{revision}:{path}"],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    return result.stdout


def _parse_dependencies(output: bytes) -> list[str]:
    flattened = output.decode("utf-8").replace("\\\n", " ")
    dependencies: set[str] = set()
    for line in flattened.splitlines():
        if ":" not in line:
            continue
        _, target_deps = line.split(":", 1)
        for token in shlex.split(target_deps):
            path = Path(token)
            if path.is_absolute():
                try:
                    token = path.resolve().relative_to(ROOT).as_posix()
                except ValueError:
                    continue
            if token.startswith("kernel/include/") or Path(token).suffix in HEADER_SUFFIXES:
                if (ROOT / token).is_file():
                    dependencies.add(Path(token).as_posix())
    return sorted(dependencies)


def _verify_header_closure(revision: str, paths: list[str],
                           git_file=_git_file) -> list[dict[str, str]]:
    """Require every header actually used by the compiler to match fixed C0."""
    records = []
    for relpath in paths:
        current = (ROOT / relpath).read_bytes()
        try:
            historical = git_file(revision, relpath)
        except (subprocess.CalledProcessError, FileNotFoundError) as exc:
            raise HeaderClosureError(
                f"C0 revision {revision} has no project header {relpath}; extract its historical closure"
            ) from exc
        current_hash = _sha256(current)
        historical_hash = _sha256(historical)
        if current_hash != historical_hash:
            raise HeaderClosureError(
                f"C0 header drift for {relpath}: working tree {current_hash}, "
                f"revision {historical_hash}; extract its historical closure"
            )
        records.append({"path": relpath, "sha256": current_hash,
                        "revision_sha256": historical_hash})
    return records


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--revision", required=True,
                        help="Git revision whose C reference source is compiled")
    args = parser.parse_args()
    run_dir_text = os.environ.get("A20_COMPARE_RUN_DIR")
    if not run_dir_text:
        parser.error("A20_COMPARE_RUN_DIR must point to the runner's fresh run directory")
    run_dir = Path(run_dir_text).resolve()
    if not run_dir.is_dir():
        parser.error(f"run directory does not exist: {run_dir}")

    source_dir = run_dir / "fixed-c0-source"
    source_dir.mkdir()
    source = source_dir / "netfilter_rewrite.c"
    manifest_path = run_dir / "manifest.json"
    manifest: dict[str, object] = {
        "schema": 1, "revision": args.revision, "status": "PREPARING",
        "source_path": SOURCE_PATH, "fixture_path": FIXTURE_PATH,
    }
    try:
        source_bytes = _git_file(args.revision, SOURCE_PATH)
        source.write_bytes(source_bytes)
        binary = run_dir / "test-nat-rewrite-c0"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler:
            raise RuntimeError("CC resolved to an empty compiler command")
        common_flags = ["-std=gnu99", "-O1", "-g", "-Wall", "-Wextra",
                        "-Wno-unused-parameter", "-Ikernel/include"]
        dependency_cmd = compiler + common_flags + [
            "-MM", "-MT", "v1-nat-c0", FIXTURE_PATH, str(source),
        ]
        dependency_result = subprocess.run(dependency_cmd, cwd=ROOT,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        manifest["dependency_command"] = dependency_cmd
        manifest["dependency_returncode"] = dependency_result.returncode
        if dependency_result.returncode:
            raise RuntimeError("compiler failed to enumerate C0 header closure: " +
                               dependency_result.stderr.decode("utf-8", "replace"))
        closure_paths = _parse_dependencies(dependency_result.stdout)
        closure = _verify_header_closure(args.revision, closure_paths)
        manifest["header_closure"] = closure
        manifest["source_sha256"] = _sha256(source_bytes)
        fixture = (ROOT / FIXTURE_PATH).read_bytes()
        manifest["fixture_sha256"] = _sha256(fixture)
        manifest["input_sha256"] = _sha256(
            b"nat-host-oracle-v1\0" + source_bytes + b"\0" + fixture + b"\0" +
            b"\0".join((record["path"] + "=" + record["sha256"]).encode()
                       for record in closure)
        )
        compiler_version = subprocess.run(compiler + ["--version"], cwd=ROOT,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        manifest["compiler"] = compiler
        manifest["compiler_version"] = (
            compiler_version.stdout + compiler_version.stderr
        ).decode("utf-8", "replace").strip()
        compile_cmd = compiler + common_flags + [
            "-o", str(binary), FIXTURE_PATH, str(source),
        ]
        manifest["compile_command"] = compile_cmd
        manifest["status"] = "COMPILING"
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
        compiled = subprocess.run(compile_cmd, cwd=ROOT, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        manifest["compile_returncode"] = compiled.returncode
        manifest["compile_stdout_sha256"] = _sha256(compiled.stdout)
        manifest["compile_stderr_sha256"] = _sha256(compiled.stderr)
        if compiled.stdout:
            sys.stdout.buffer.write(compiled.stdout)
        if compiled.stderr:
            sys.stderr.buffer.write(compiled.stderr)
        if compiled.returncode:
            manifest["status"] = "COMPILE_FAILED"
            manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                     encoding="utf-8")
            return compiled.returncode
        manifest["binary_sha256"] = _sha256(binary.read_bytes())
        manifest["status"] = "RUNNING"
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")

        result = subprocess.run([str(binary)], cwd=ROOT, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        manifest["run_command"] = [str(binary)]
        manifest["run_returncode"] = result.returncode
        manifest["run_stdout_sha256"] = _sha256(result.stdout)
        manifest["run_stderr_sha256"] = _sha256(result.stderr)
        manifest["status"] = "PASS" if result.returncode == 0 else "TEST_FAILED"
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
        if result.stdout:
            sys.stdout.buffer.write(result.stdout)
        if result.stderr:
            sys.stderr.buffer.write(result.stderr)
        return result.returncode
    except (subprocess.CalledProcessError, HeaderClosureError, OSError, RuntimeError) as exc:
        manifest["status"] = "REJECTED"
        manifest["error"] = str(exc)
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
        print(f"fixed-C0 adapter rejected run: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
