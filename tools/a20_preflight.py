"""Host resource gate for QEMU launches.

A `tools/a20 run|debug|test` on a GUI or 4G instance can starve the host:
QEMU grabs a few gigabytes of guest RAM, vCPUs, and the build writes a 4 GiB
image.  When the machine is already busy the result is not a clean failure --
it is an OOM kill of the *host*, a build that gets swapped, or a guest whose
timing-sensitive gates fail for reasons that have nothing to do with the code
under test.  Every "the test was flaky" report on a busy host should be
suspected before the test itself is.

So the launch path samples the host first and, when the instance cannot fit,
*waits* for the resources to come back rather than proceeding.  Waiting is the
default because the common cause is another agent or a co-tenant finishing,
which resolves on its own; giving up immediately only moves the retry to the
human.

Set A20_PREFLIGHT=0 to skip, A20_PREFLIGHT_TIMEOUT=<seconds> to bound the
wait (0 means do not wait, just report), and A20_PREFLIGHT_VERBOSE=1 to see
the sample even when it passes.
"""

from __future__ import annotations

import os
import re
import shutil
import sys
import time
from dataclasses import dataclass

# A QEMU guest is not a memory hog in the steady state -- it only commits what
# it touches -- but a desktop image under a KVM host will happily take most of
# its declared RAM during boot.  Require the declared size plus a reserve so
# the build, the toolchain and the compositor all still fit beside it.
MEMORY_HEADROOM_RATIO = 1.5
MEMORY_RESERVE_BYTES = 1 << 30  # 1 GiB

# Loading above one runnable task per core means a TCG guest gets time-sliced
# badly enough to trip watchdog timeouts that look like kernel hangs.
CPU_LOAD_PER_CORE = 1.0

# Desktop images are 4 GiB and the build writes several more artifacts.
DISK_MIN_FREE_BYTES = 8 << 30  # 8 GiB

POLL_INTERVAL_SECONDS = 15.0
DEFAULT_WAIT_SECONDS = 900.0  # 15 min

# Mirrors of the Makefile defaults, so the gate sizes an instance that sets
# nothing the same way the launcher will.  Deliberately not parsed out of the
# Makefile at runtime: `self_check` is what keeps these honest (see below).
DEFAULT_MEMORY = "1G"  # Makefile:296 QEMU_MEMORY ?= 1G
DEFAULT_MEMORY_GUI = "4G"  # tools/targets-pkg.mk:46 QEMU_MEMORY_GUI ?= 4G
DEFAULT_VCPUS = 1  # Makefile:47 NR_CPUS ?= 1

_SIZE_SUFFIXES = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30, "T": 1 << 40}


class PreflightError(Exception):
    """The host cannot run this instance and the wait budget was exhausted."""


def parse_size(text: str) -> int:
    """Parse a QEMU size string like "4G", "512M" or a bare byte count."""
    s = text.strip()
    if not s:
        raise ValueError("empty size")
    suffix = s[-1].upper()
    if suffix in _SIZE_SUFFIXES:
        return int(float(s[:-1]) * _SIZE_SUFFIXES[suffix])
    return int(s)


@dataclass(frozen=True, slots=True)
class Need:
    """What the instance wants from the host."""

    label: str
    memory_bytes: int
    vcpus: int
    disk_path: str


@dataclass(frozen=True, slots=True)
class Host:
    """One sample of the host's capacity."""

    mem_available: int
    mem_total: int
    cpus: int
    load1: float
    disk_free: int

    def shortfalls(self, need: Need) -> list[str]:
        out: list[str] = []
        want_mem = int(need.memory_bytes * MEMORY_HEADROOM_RATIO) + MEMORY_RESERVE_BYTES
        if self.mem_available < want_mem:
            out.append(
                f"RAM: need {want_mem >> 20} MiB available "
                f"(guest {need.memory_bytes >> 20} MiB + headroom), "
                f"have {self.mem_available >> 20} MiB of {self.mem_total >> 20} MiB"
            )
        if self.cpus < need.vcpus:
            out.append(f"CPU: need {need.vcpus} cores, have {self.cpus}")
        elif self.load1 > self.cpus * CPU_LOAD_PER_CORE:
            out.append(
                f"CPU: load1 {self.load1:.1f} exceeds {self.cpus} core(s) "
                "(guest would be time-sliced into spurious timeouts)"
            )
        if self.disk_free < DISK_MIN_FREE_BYTES:
            out.append(
                f"disk: need {DISK_MIN_FREE_BYTES >> 30} GiB free on "
                f"{need.disk_path}, have {self.disk_free >> 30} GiB"
            )
        return out


def self_check(makefile_text: str, pkg_mk_text: str) -> list[str]:
    """Report drift between the mirrored Makefile defaults and this module.

    Duplicated constants rot silently: bump QEMU_MEMORY_GUI in the Makefile and
    the gate keeps admitting instances the launcher cannot actually satisfy.
    `tools/a20 check` calls this so the drift fails a gate instead of turning
    into a mysterious OOM later.
    """
    problems: list[str] = []
    targets = (
        (makefile_text, r"^QEMU_MEMORY\s*\?=\s*(\S+)", DEFAULT_MEMORY, "Makefile"),
        (makefile_text, r"^NR_CPUS\s*\?=\s*(\d+)", str(DEFAULT_VCPUS), "Makefile"),
        (pkg_mk_text, r"^QEMU_MEMORY_GUI\s*\?=\s*(\S+)", DEFAULT_MEMORY_GUI, "targets-pkg.mk"),
    )
    for text, pattern, expected, where in targets:
        found = re.search(pattern, text, re.MULTILINE)
        if found is None:
            problems.append(f"{where}: no assignment matching {pattern}")
        elif found.group(1) != expected:
            problems.append(
                f"{where}: default is {found.group(1)} but a20_preflight mirrors "
                f"{expected} -- update both"
            )
    return problems


def _meminfo() -> tuple[int, int]:
    """(MemAvailable, MemTotal) in bytes."""
    avail = total = 0
    with open("/proc/meminfo", encoding="ascii") as fh:
        for line in fh:
            if line.startswith("MemAvailable:"):
                avail = int(line.split()[1]) << 10
            elif line.startswith("MemTotal:"):
                total = int(line.split()[1]) << 10
            if avail and total:
                break
    return avail, total


def sample(disk_path: str) -> Host:
    avail, total = _meminfo()
    try:
        load1 = os.getloadavg()[0]
    except OSError:
        load1 = 0.0
    usage = shutil.disk_usage(disk_path)
    return Host(
        mem_available=avail,
        mem_total=total,
        cpus=os.cpu_count() or 1,
        load1=load1,
        disk_free=usage.free,
    )


def _enabled() -> bool:
    return os.environ.get("A20_PREFLIGHT", "1").strip().lower() not in {"0", "no", "off"}


def _verbose() -> bool:
    return os.environ.get("A20_PREFLIGHT_VERBOSE", "0").strip().lower() in {"1", "yes", "on"}


def _wait_budget() -> float:
    raw = os.environ.get("A20_PREFLIGHT_TIMEOUT")
    if raw is None:
        return DEFAULT_WAIT_SECONDS
    try:
        return max(0.0, float(raw))
    except ValueError:
        return DEFAULT_WAIT_SECONDS


def gate(need: Need, *, stream=sys.stderr) -> Host:
    """Block until the host can host `need`, or raise PreflightError.

    Returns the sample that passed so callers can log it.  No-op (returns the
    current sample) when A20_PREFLIGHT=0.
    """
    if not _enabled():
        return sample(need.disk_path)

    budget = _wait_budget()
    deadline = time.monotonic() + budget
    first = True
    reported: list[str] = []

    while True:
        host = sample(need.disk_path)
        problems = host.shortfalls(need)
        if not problems:
            if _verbose() or not first:
                print(
                    f"[preflight] {need.label}: ok "
                    f"({host.mem_available >> 20} MiB RAM free, "
                    f"load1 {host.load1:.1f} on {host.cpus} cores, "
                    f"{host.disk_free >> 30} GiB disk free)",
                    file=stream,
                )
            return host

        if problems != reported:
            reported = problems
            print(f"[preflight] {need.label}: waiting for host resources", file=stream)
            for line in problems:
                print(f"[preflight]   - {line}", file=stream)

        first = False
        if time.monotonic() >= deadline:
            raise PreflightError(
                f"host still cannot run {need.label} after {budget:.0f}s:\n"
                + "\n".join(f"  - {line}" for line in problems)
                + "\n  free the resources, raise A20_PREFLIGHT_TIMEOUT, "
                "or set A20_PREFLIGHT=0 to launch anyway"
            )
        time.sleep(POLL_INTERVAL_SECONDS)
