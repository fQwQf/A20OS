#!/usr/bin/env python3
"""Fetch optional libc regression fixtures; never a production dependency."""
import argparse
import fcntl
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = {
    "mksh": ("https://github.com/mirabilos/mksh-cvs2git.git", "341752823184238dc9b2b0a22b48da984f25442d"),
    "sbase": ("https://git.suckless.org/sbase", "c1341583c96307cb0e6152c963ed23c4d56a4278"),
}


def fetch(name, destination):
    url, revision = FIXTURES[name]
    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.with_suffix(".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not (destination / ".git").exists():
            subprocess.run(["git", "init", str(destination)], check=True)
        def git(*args, **kwargs):
            return subprocess.run(["git", "-C", str(destination), *args], check=True, **kwargs)
        if git("status", "--porcelain", capture_output=True, text=True).stdout:
            raise SystemExit(f"test fixture has local modifications: {destination}")
        present = subprocess.run(["git", "-C", str(destination), "cat-file", "-e", revision], capture_output=True)
        if present.returncode:
            git("fetch", "--depth=1", url, revision)
        git("checkout", "--detach", revision)
        head = git("rev-parse", "HEAD", capture_output=True, text=True).stdout.strip()
        if head != revision:
            raise SystemExit("test fixture revision mismatch")
        (destination / ".a20-fixture-ready").write_text(revision + "\n")
        # The marker is metadata, not a modification to upstream test sources.
        exclude = destination / ".git/info/exclude"
        exclude.parent.mkdir(parents=True, exist_ok=True)
        if not exclude.exists() or ".a20-fixture-ready" not in exclude.read_text():
            with exclude.open("a") as stream:
                stream.write("\n.a20-fixture-ready\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name", choices=FIXTURES)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    fetch(args.name, args.output)
