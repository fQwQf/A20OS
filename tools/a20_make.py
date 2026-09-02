"""Shared make execution helpers for the a20 instance runner.

The Makefile remains the build engine; these helpers are the single place
where a20 hands derived variables to it.
"""

from __future__ import annotations

import shlex
import subprocess
from pathlib import Path

from a20_derive import derive_make_vars
from a20_instance import Instance

REPO_ROOT = Path(__file__).resolve().parent.parent


def exec_make(inst: Instance, target: str, extra: list[str], dry_run: bool) -> int:
    cmd = ["make", "-C", str(REPO_ROOT), *derive_make_vars(inst), target, *extra]
    if dry_run:
        print(shlex.join(cmd))
        return 0
    return subprocess.run(cmd, check=False).returncode


def build_instance(inst: Instance, extra: list[str], dry_run: bool) -> int:
    """Build what running this instance needs: kernel-only for bringup, else dev-build."""
    # armv7m (MCU) has no userspace image; the Makefile forces BRINGUP=1 there.
    if inst.kernel.bringup or inst.arch == "armv7m":
        return exec_make(inst, "kernel-only", extra, dry_run)
    if not inst.gui.enabled:
        # Mirror _run_impl: text-mode boots skip the LVGL desktop.
        extra.append("USER_BUILD_DESKTOP=0")
    return exec_make(inst, "dev-build", extra, dry_run)
