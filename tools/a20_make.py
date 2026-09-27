"""Shared make execution helpers for the a20 instance runner.

The Makefile remains the build engine; these helpers are the single place
where a20 hands derived variables to it, and the single place that reads
values back out of it.
"""

from __future__ import annotations

import shlex
import subprocess
from pathlib import Path
from typing import Sequence

from a20_derive import derive_make_vars
from a20_instance import Instance

REPO_ROOT = Path(__file__).resolve().parent.parent


class MakeQueryError(RuntimeError):
    """make refused to report the variables we asked for."""


def exec_make(inst: Instance, target: str, extra: list[str], dry_run: bool) -> int:
    cmd = ["make", "-C", str(REPO_ROOT), *derive_make_vars(inst), target, *extra]
    if dry_run:
        print(shlex.join(cmd))
        return 0
    return subprocess.run(cmd, check=False).returncode


def query_make(inst: Instance | None, names: Sequence[str],
               extra: Sequence[str] = ()) -> dict[str, str]:
    """Read Makefile variable values by asking make, never by re-deriving them.

    BUILD_DIR alone encodes ARCH, BOARD, ABI, BRINGUP, NOMMU, SMP count,
    driver deployment and three more switches in its name.  A Python copy of
    that formula is a second source of truth that silently rots the first time
    the Makefile grows a variant suffix, so every consumer that needs a path
    asks make for it in one round trip.
    """
    if not names:
        return {}
    fmt = " ".join(f'"$({n})"' for n in names)
    recipe = f"a20-query-vars:;@printf '%s\\n' {fmt}"
    derived = derive_make_vars(inst) if inst is not None else []
    out = subprocess.run(
        ["make", "-s", "-C", str(REPO_ROOT), *derived, *extra,
         f"--eval={recipe}", "a20-query-vars"],
        check=False, capture_output=True, text=True,
    )
    if out.returncode != 0:
        raise MakeQueryError(out.stderr.strip() or f"make exited {out.returncode}")
    lines = out.stdout.splitlines()
    if len(lines) != len(names):
        raise MakeQueryError(
            f"expected {len(names)} value(s) from make, got {len(lines)}: {out.stdout!r}")
    return dict(zip(names, lines, strict=True))


def build_instance(inst: Instance, extra: list[str], dry_run: bool) -> int:
    """Build what running this instance needs: kernel-only for bringup, else dev-build."""
    # armv7m (MCU) has no userspace image; the Makefile forces BRINGUP=1 there.
    if inst.kernel.bringup or inst.arch == "armv7m":
        return exec_make(inst, "kernel-only", extra, dry_run)
    if inst.rootfs.world is not None:
        return exec_make(inst, "image-world", extra, dry_run)
    return exec_make(inst, "dev-build", extra, dry_run)
