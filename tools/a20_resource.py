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
import socket
import time

from a20_error import ResourceShortage, UsageError
from dataclasses import dataclass
from pathlib import Path
from typing import Final, Sequence

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


# QEMU's hostfwd grammar: [tcp|udp]:[hostaddr]:hostport-[guestaddr]:guestport
_HOSTFWD_RE = re.compile(
    r"^(?:tcp|udp):(?P<haddr>[^:]*):(?P<hport>\d+)-(?P<gaddr>[^:]*):(?P<gport>\d+)$")


def parse_hostfwd_ports(entries: Sequence[str] | None) -> tuple[tuple[str, int], ...]:
    """Host ports an instance's [net].hostfwd will claim.

    Port 0 means "let the OS pick", which is exactly the case the preflight must
    not gate: nothing is contended in advance and QEMU reports the result only
    on its own console.  Entries that do not parse are ignored rather than
    rejected here -- `a20 check` is the layer that reports malformed hostfwd,
    and a preflight that refuses to start would be the wrong place to learn it.
    """
    out: list[tuple[str, int]] = []
    for entry in entries or ():
        m = _HOSTFWD_RE.match(entry.strip())
        if m is None:
            continue
        port = int(m.group("hport"))
        if port == 0:
            continue
        out.append((m.group("haddr") or "0.0.0.0", port))
    return tuple(out)


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
    busy_ports: tuple[tuple[str, int], ...] = ()

    @classmethod
    def snapshot(cls, disk_path: Path,
                 want_ports: Sequence[tuple[str, int]] = ()) -> HostResources:
        return cls(
            mem_available_mb=_mem_available_mb(),
            cpu_count=os.cpu_count() or 1,
            load1=os.getloadavg()[0],
            disk_free_mb=shutil.disk_usage(disk_path).free // _MIB,
            running_guests=count_running_guests(),
            busy_ports=busy_ports(want_ports),
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


def busy_ports(want: Sequence[tuple[str, int]]) -> tuple[tuple[str, int], ...]:
    """Which of the requested host ports cannot be bound right now.

    Probed by actually binding, rather than by reading /proc or ss: a port held
    by another QEMU, by a leftover process, or by a socket in TIME_WAIT all show
    up the same way here, which is the only answer that actually predicts
    whether QEMU's bind will succeed.
    """
    taken: list[tuple[str, int]] = []
    for addr, port in want:
        family = socket.AF_INET6 if ":" in addr else socket.AF_INET
        with socket.socket(family, socket.SOCK_STREAM) as s:
            # Deliberately no SO_REUSEADDR: QEMU does not set it either, so
            # this reproduces QEMU's own bind semantics rather than a laxer one.
            try:
                s.bind((addr, port))
            except OSError:
                taken.append((addr, port))
    return tuple(taken)


@dataclass(frozen=True, slots=True)
class Requirement:
    mem_mb: int
    cpus: int
    disk_mb: int
    guests: int = 1
    ports: tuple[tuple[str, int], ...] = ()

    def describe(self) -> str:
        parts = [f"mem {self.mem_mb} MiB", f"{self.cpus} vCPU", f"disk {self.disk_mb} MiB"]
        if self.ports:
            parts.append("ports " + ",".join(f"{a}:{p}" for a, p in self.ports))
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
        ports=parse_hostfwd_ports(inst.net.hostfwd),
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
    for addr, port in have.busy_ports:
        d.append(f"host port: {addr}:{port} is already in use by another process")
    return Verdict(ok=not d, deficits=tuple(d))


def format_report(need: Requirement, have: HostResources) -> str:
    return (f"mem {have.mem_available_mb}/{need.mem_mb} MiB, "
            f"cpu load {have.load1:.1f}/{have.cpu_count} want {need.cpus}, "
            f"disk {have.disk_free_mb}/{need.disk_mb} MiB, "
            f"guests {have.running_guests}")


def gate(need: Requirement, policy: Policy | None = None, disk_path: Path | None = None,
         *, wait: bool = True, echo=lambda _msg: None, label: str = "instance") -> Verdict:
    """Gate one guest launch, waiting for the host if necessary.

    Takes a ``Requirement`` rather than an ``Instance`` so that callers which
    launch QEMU by hand (the ``smoke-*`` make targets) can reuse exactly the
    same wait loop and the same policy as ``tools/a20``, instead of either
    duplicating the loop or skipping the gate.

    Returns the final verdict.  Raises SystemExit only when the wait was
    requested, the wait timed out, or waiting was declined and the host is
    short -- in every one of those cases the guest is not started, which is the
    entire point.
    """
    policy = policy or Policy.from_env()
    root = disk_path or Path.cwd()
    deadline = time.monotonic() + policy.wait_timeout_s if policy.wait_timeout_s > 0 else None
    reported: tuple[str, ...] | None = None
    last_emit = 0.0

    while True:
        have = HostResources.snapshot(root, need.ports)
        verdict = evaluate(need, have, policy)
        if verdict.ok:
            return verdict
        if not wait:
            raise ResourceShortage(
                f"insufficient host resources for {label}: {verdict.reason()}")
        # Re-announce only when the *kind* of shortfall changes, so a long wait
        # does not scroll a line per poll, plus a periodic heartbeat so the
        # process does not look hung.
        kinds = tuple(d.split(":", 1)[0] for d in verdict.deficits)
        now = time.monotonic()
        if kinds != reported or now - last_emit >= _HEARTBEAT_SECONDS:
            echo(f"a20: waiting for host resources ({need.describe()}) -- {verdict.reason()}")
            reported, last_emit = kinds, now
        if deadline is not None and now >= deadline:
            raise ResourceShortage(
                f"gave up waiting {policy.wait_timeout_s:.0f}s for host resources "
                f"to run {label}: {verdict.reason()}")
        time.sleep(_POLL_SECONDS)


def preflight(inst: Instance, policy: Policy | None = None, disk_path: Path | None = None,
              *, wait: bool = True, echo=lambda _msg: None) -> Verdict:
    """Gate one instance run; see :func:`gate` for the wait semantics."""
    policy = policy or Policy.from_env()
    return gate(requirement_for(inst, policy), policy, disk_path,
                wait=wait, echo=echo, label=inst.name)


# --------------------------------------------------------------------------
# Standalone CLI.
#
# The `smoke-*` make targets launch `qemu-system-*` directly instead of going
# through `tools/a20 test`, so they have no instance TOML to gate against. They
# used to launch with no gate at all, which is exactly the case this module
# exists for: QEMU asking for memory the host does not have turns into the host
# OOM killer choosing a victim, and the victim is rarely the thing being
# debugged. This entry point gives those targets the same wait-for-resources
# behaviour, reading the same A20_* environment policy.
#
#   python3 tools/a20_resource.py --mem-mb 1024 --cpus 2
#   python3 tools/a20_resource.py -m 1G -c 1 --hostfwd 127.0.0.1:5555
# --------------------------------------------------------------------------
def _parse_size_mb(text: str) -> int:
    """Accept plain MiB or the QEMU `-m` spelling (1G, 512M, 2G)."""
    t = text.strip()
    mult = 1
    if t and t[-1] in "kKmMgGtT":
        mult = {"k": 1 // 1024, "m": 1, "g": 1024, "t": 1024 * 1024}[t[-1].lower()]
        t = t[:-1]
    if t.endswith("b"):  # trailing "B" is legal for QEMU ("1GB")
        t = t[:-1]
    return int(float(t) * mult)


def _parse_hostfwd(values: Sequence[str]) -> tuple[tuple[str, int], ...]:
    out: list[tuple[str, int]] = []
    for v in values:
        addr, _, port = v.rpartition(":")
        if not port.isdigit():
            raise UsageError(f"--hostfwd wants addr:port, got {v!r}")
        out.append((addr or "127.0.0.1", int(port)))
    return tuple(out)


def main(argv: Sequence[str] | None = None) -> int:
    import argparse

    ap = argparse.ArgumentParser(
        prog="a20_resource",
        description="Gate a QEMU launch on host resources, waiting if necessary.")
    ap.add_argument("-m", "--mem", required=True,
                    help="guest memory the launch will ask for (e.g. 1024, 1G)")
    ap.add_argument("-c", "--cpus", type=int, default=1,
                    help="vCPUs the launch will ask for (default: 1)")
    ap.add_argument("--disk-mb", type=int, default=0,
                    help="free space the launch needs; 0 means the A20_MIN_DISK_MB floor")
    ap.add_argument("--hostfwd", action="append", default=[], metavar="ADDR:PORT",
                    help="host port the launch will bind; repeatable")
    ap.add_argument("--disk-path", default=None,
                    help="path whose filesystem free space to measure (default: cwd)")
    ap.add_argument("--no-wait", action="store_true",
                    help="fail immediately instead of waiting for resources")
    ap.add_argument("--quiet", action="store_true", help="suppress the pass line")
    args = ap.parse_args(argv)

    policy = Policy.from_env()
    need = Requirement(
        mem_mb=_parse_size_mb(args.mem) + policy.reserve_mem_mb,
        cpus=max(1, args.cpus),
        disk_mb=max(args.disk_mb, policy.min_disk_mb),
        ports=_parse_hostfwd(args.hostfwd),
    )
    root = Path(args.disk_path) if args.disk_path else Path.cwd()
    verdict = gate(need, policy, root, wait=not args.no_wait,
                   echo=lambda m: print(m, flush=True),
                   label=f"{need.describe()}")
    if not args.quiet:
        have = HostResources.snapshot(root, need.ports)
        print(f"a20: resources OK -- {format_report(need, have)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
