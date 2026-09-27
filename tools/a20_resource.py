"""Host resource preflight for a20 instance runs.

Starting a guest is the only a20 action whose failure mode is a dead machine
rather than a nonzero exit: QEMU asking for 4 GiB on a host with 1 GiB
available gets the host OOM killer to choose which process dies, and that is
usually not the one you were debugging.  The build-and-boot actions therefore
wait for the host to have room instead of proceeding and hoping.

Two measurements are load-bearing and easy to get wrong:

- Availability must come from ``MemAvailable``, not ``free``.  ``free`` excludes
  reclaimable page cache, so on an otherwise idle machine it routinely reads a
  few hundred MiB while tens of gigabytes are genuinely available; a gate built
  on ``free`` would block forever.
- Idle vCPUs do not appear in the load average, so load alone cannot tell you
  whether another guest is already holding the CPUs this one wants.  That is why
  concurrency is counted separately.

Policy is overridable through the environment so a CI runner with different
headroom does not need a code change:

  A20_RESERVE_MEM_MB    MiB held back for the build and the host itself
  A20_MAX_CONCURRENT    concurrent guests allowed at once
  A20_WAIT_TIMEOUT      seconds to wait before giving up (0 = wait forever)
  A20_MIN_DISK_MB       floor for the free-space requirement
"""

from __future__ import annotations

import os
import re
import shutil
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Final

from a20_instance import Instance

_MIB: Final = 1024 * 1024
_GIB: Final = 1024 * 1024 * 1024

# QEMU's -m accepts K/M/G/T with an optional trailing B, binary-scaled, and the
# instances use that same spelling.
_MEMORY_RE: Final = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*([kmgt]?)(?:i?b?)?\s*$", re.IGNORECASE)
_MEMORY_SCALE: Final = {"": _MIB, "k": _MIB // 1024, "m": _MIB, "g": _GIB, "t": _GIB * 1024}

DEFAULT_RESERVE_MEM_MB: Final = 1024
DEFAULT_MIN_DISK_MB: Final = 2048
DEFAULT_WAIT_TIMEOUT: Final = 0.0
_POLL_SECONDS: Final = 5.0
_HEARTBEAT_SECONDS: Final = 30.0


class MemorySpecError(ValueError):
    """A -m style size string that cannot be parsed."""


def parse_memory_mb(text: str) -> int:
    """Parse a QEMU -m size into whole MiB, rounded up.

    Rounds up because a 1.5G guest is 1536 MiB of address space to reserve; a
    fractional MiB request has no useful meaning at this granularity.
    """
    m = _MEMORY_RE.match(text)
    if m is None:
        raise MemorySpecError(f"cannot parse memory size {text!r}")
    value, unit = float(m.group(1)), m.group(2).lower()
    mb = value * _MEMORY_SCALE[unit] / _MIB
    return max(1, int(mb + 0.999))


def _env_int(name: str, default: int) -> int:
    raw = os.environ.get(name)
    if raw is None or not raw.strip():
        return default
    try:
        return int(raw)
    except ValueError:
        return default


def _env_float(name: str, default: float) -> float:
    raw = os.environ.get(name)
    if raw is None or not raw.strip():
        return default
    try:
        return float(raw)
    except ValueError:
        return default


@dataclass(frozen=True, slots=True)
class Policy:
    reserve_mem_mb: int = DEFAULT_RESERVE_MEM_MB
    min_disk_mb: int = DEFAULT_MIN_DISK_MB
    max_concurrent: int = 0  # 0 -> derive from cpu_count
    wait_timeout_s: float = DEFAULT_WAIT_TIMEOUT

    @classmethod
    def from_env(cls) -> Policy:
        return cls(
            reserve_mem_mb=_env_int("A20_RESERVE_MEM_MB", DEFAULT_RESERVE_MEM_MB),
            min_disk_mb=_env_int("A20_MIN_DISK_MB", DEFAULT_MIN_DISK_MB),
            max_concurrent=_env_int("A20_MAX_CONCURRENT", 0),
            wait_timeout_s=_env_float("A20_WAIT_TIMEOUT", DEFAULT_WAIT_TIMEOUT),
        )


@dataclass(frozen=True, slots=True)
class HostResources:
    mem_available_mb: int
    cpu_count: int
    load1: float
    disk_free_mb: int
    running_guests: int

    @classmethod
    def snapshot(cls, disk_path: Path) -> HostResources:
        return cls(
            mem_available_mb=_mem_available_mb(),
            cpu_count=os.cpu_count() or 1,
            load1=os.getloadavg()[0],
            disk_free_mb=shutil.disk_usage(disk_path).free // _MIB,
            running_guests=count_running_guests(),
        )


def _mem_available_mb() -> int:
    """MemAvailable from /proc, in MiB.

    MemAvailable is the kernel's own estimate of what a new allocation can get
    without swapping, and has existed since Linux 3.14.  The fallback sums the
    reclaimable pools instead, which is the same idea computed by hand; if even
    that cannot be read the answer is 0, so an unreadable host blocks rather
    than proceeding.
    """
    wanted = ("MemAvailable", "MemFree", "Buffers", "Cached", "SReclaimable")
    try:
        fields: dict[str, int] = {}
        with open("/proc/meminfo", encoding="ascii") as f:
            for line in f:
                key, _, rest = line.partition(":")
                if key in wanted:
                    fields[key] = int(rest.split()[0]) // 1024
    except (OSError, ValueError, IndexError):
        return 0
    if "MemAvailable" in fields:
        return fields["MemAvailable"]
    reclaimable = sum(fields.get(k, 0) for k in ("MemFree", "Buffers", "Cached", "SReclaimable"))
    return reclaimable


def count_running_guests() -> int:
    """Number of live QEMU processes, counted from /proc rather than pgrep."""
    count = 0
    try:
        entries = os.listdir("/proc")
    except OSError:
        return 0
    for entry in entries:
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/comm", encoding="utf-8", errors="replace") as f:
                if f.readline().startswith("qemu-system-"):
                    count += 1
        except OSError:
            continue
    return count


@dataclass(frozen=True, slots=True)
class Requirement:
    mem_mb: int
    cpus: int
    disk_mb: int
    guests: int = 1

    def describe(self) -> str:
        parts = [f"mem {self.mem_mb} MiB", f"{self.cpus} vCPU", f"disk {self.disk_mb} MiB"]
        return ", ".join(parts)


def requirement_for(inst: Instance, policy: Policy) -> Requirement:
    """What this instance needs from the host, derived from its own fields."""
    mem_mb = parse_memory_mb(inst.machine.memory) if inst.machine.memory else 1024
    cpus = inst.machine.smp if inst.machine.smp is not None else 1
    r = inst.rootfs
    disk_mb = sum(v for v in (r.size_mb, r.ext4_size_mb, r.extra_size_mb,
                              r.world_size_mb) if v is not None)
    return Requirement(
        mem_mb=mem_mb + policy.reserve_mem_mb,
        cpus=max(1, cpus),
        disk_mb=max(disk_mb, policy.min_disk_mb),
    )


@dataclass(frozen=True, slots=True)
class Verdict:
    ok: bool
    deficits: tuple[str, ...]

    def reason(self) -> str:
        return "; ".join(self.deficits)


def evaluate(need: Requirement, have: HostResources, policy: Policy) -> Verdict:
    cap = policy.max_concurrent or max(1, have.cpu_count // 4)
    d: list[str] = []
    if have.mem_available_mb < need.mem_mb:
        d.append(f"memory: need {need.mem_mb} MiB, available {have.mem_available_mb} MiB")
    if have.cpu_count - have.load1 < need.cpus:
        d.append(f"cpu: need {need.cpus} idle, {have.cpu_count} total at load {have.load1:.1f}")
    if have.disk_free_mb < need.disk_mb:
        d.append(f"disk: need {need.disk_mb} MiB, free {have.disk_free_mb} MiB")
    if have.running_guests + need.guests > cap:
        d.append(f"guest slots: {have.running_guests} running, cap {cap}")
    return Verdict(ok=not d, deficits=tuple(d))


def format_report(need: Requirement, have: HostResources) -> str:
    return (f"mem {have.mem_available_mb}/{need.mem_mb} MiB, "
            f"cpu load {have.load1:.1f}/{have.cpu_count} want {need.cpus}, "
            f"disk {have.disk_free_mb}/{need.disk_mb} MiB, "
            f"guests {have.running_guests}")


def preflight(inst: Instance, policy: Policy | None = None, disk_path: Path | None = None,
              *, wait: bool = True, echo=lambda _msg: None) -> Verdict:
    """Gate one instance run, waiting for the host if necessary.

    Returns the final verdict.  Raises SystemExit only when the wait was
    requested, the wait timed out, or waiting was declined and the host is
    short -- in every one of those cases the guest is not started, which is the
    entire point.
    """
    policy = policy or Policy.from_env()
    root = disk_path or Path.cwd()
    need = requirement_for(inst, policy)
    deadline = time.monotonic() + policy.wait_timeout_s if policy.wait_timeout_s > 0 else None
    reported: tuple[str, ...] | None = None
    last_emit = 0.0

    while True:
        have = HostResources.snapshot(root)
        verdict = evaluate(need, have, policy)
        if verdict.ok:
            return verdict
        if not wait:
            raise SystemExit(f"error: insufficient host resources for {inst.name}: {verdict.reason()}")
        # Re-announce only when the *kind* of shortfall changes, so a long wait
        # does not scroll a line per poll, plus a periodic heartbeat so the
        # process does not look hung.
        kinds = tuple(d.split(":", 1)[0] for d in verdict.deficits)
        now = time.monotonic()
        if kinds != reported or now - last_emit >= _HEARTBEAT_SECONDS:
            echo(f"a20: waiting for host resources ({need.describe()}) -- {verdict.reason()}")
            reported, last_emit = kinds, now
        if deadline is not None and now >= deadline:
            raise SystemExit(
                f"error: gave up waiting {policy.wait_timeout_s:.0f}s for host resources "
                f"to run {inst.name}: {verdict.reason()}")
        time.sleep(_POLL_SECONDS)
