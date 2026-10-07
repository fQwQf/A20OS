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
import sys
import time

from dataclasses import dataclass
from pathlib import Path
from typing import Final, Sequence

from a20_error import ResourceShortage, UsageError
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


# QEMU's hostfwd grammar: hostfwd=[tcp|udp]:[hostaddr]:hostport-[guestaddr]:guestport[=on|off]
# The `hostfwd=` key and the boolean are optional here because an instance
# manifest may write either the readable short form or the fully spelled one.
_HOSTFWD_RE = re.compile(
    r"^(?:hostfwd=)?(?P<proto>tcp|udp):(?P<haddr>[^:]*):(?P<hport>\d+)"
    r"-(?P<gaddr>[^:]*):(?P<gport>\d+)(?:=(?:on|off))?$")


def parse_hostfwd_ports(entries: Sequence[str] | None) -> tuple[tuple[str, int, str], ...]:
    """Host ports an instance's [net].hostfwd will claim.

    Port 0 means "let the OS pick", which is exactly the case the preflight must
    not gate: nothing is contended in advance and QEMU reports the result only
    on its own console.  Entries that do not parse are ignored rather than
    rejected here -- `a20 check` is the layer that reports malformed hostfwd,
    and a preflight that refuses to start would be the wrong place to learn it.
    """
    out: list[tuple[str, int, str]] = []
    for entry in entries or ():
        m = _HOSTFWD_RE.match(entry.strip())
        if m is None:
            continue
        port = int(m.group("hport"))
        if port == 0:
            continue
        out.append((m.group("haddr") or "0.0.0.0", port, m.group("proto")))
    return tuple(out)


# A [net].backend spec claims host ports too: `listen=addr:port` binds a TCP
# socket, `localaddr=addr:port` binds a UDP one.  `connect=` is a client and
# claims nothing.  Only the key/value pairs the instance wrote are probed here;
# anything malformed is `a20 check`'s business, not the preflight's.
_NETDEV_PORT_RE = re.compile(r"(?:^|,)(?P<key>listen|localaddr)=(?P<value>[^,]*)")


def parse_netdev_ports(backend: str | None) -> tuple[tuple[str, int, str], ...]:
    """Host ports an instance's [net].backend will bind, with their protocol."""
    out: list[tuple[str, int, str]] = []
    for m in _NETDEV_PORT_RE.finditer(backend or ""):
        addr, sep, port = m.group("value").rpartition(":")
        if not sep or not port.isdigit():
            continue
        if int(port) == 0:
            continue
        proto = "udp" if m.group("key") == "localaddr" else "tcp"
        out.append((addr or "0.0.0.0", int(port), proto))
    return tuple(out)


def instance_net_ports(inst: Instance) -> tuple[tuple[str, int, str], ...]:
    """Every host port an instance's [net] section will bind.

    Covers both the default user backend ([net].hostfwd) and a backend
    override ([net].backend, e.g. the cluster demo's UDP tunnel), so the
    preflight and `a20 ports` see one merged view instead of silently missing
    whatever was declared through the newer field.
    """
    return tuple(dict.fromkeys(
        parse_hostfwd_ports(inst.net.hostfwd) + parse_netdev_ports(inst.net.backend)))


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


def _env_num(name, default, cast):
    """Read a numeric env var, complaining if it is set but unreadable.

    Silently falling back made a typo look like it had taken effect: with
    A20_MAX_CONCURRENT=many the budget was quietly the default, and the only
    symptom was a run that blocked for a reason nobody could name.
    """
    raw = os.environ.get(name)
    if raw is None or not raw.strip():
        return default
    try:
        return cast(raw)
    except ValueError:
        print(f"a20: warning: {name}={raw!r} is not a number; using {default}",
              file=sys.stderr)
        return default


def _env_int(name: str, default: int) -> int:
    return _env_num(name, default, int)


def _env_float(name: str, default: float) -> float:
    return _env_num(name, default, float)


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
    busy_ports: tuple[tuple[str, int, str], ...] = ()

    @classmethod
    def snapshot(cls, disk_path: Path,
                 want_ports: Sequence[tuple[str, int, str]] = ()) -> HostResources:
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


def busy_ports(want: Sequence[tuple[str, int, str]]) -> tuple[tuple[str, int, str], ...]:
    """Which of the requested host ports cannot be bound right now.

    Probed by actually binding, rather than by reading /proc or ss: a port held
    by another QEMU, by a leftover process, or by a socket in TIME_WAIT all show
    up the same way here, which is the only answer that actually predicts
    whether QEMU's bind will succeed.  The protocol is probed as declared --
    a UDP tunnel port is free even when a TCP listener holds the number, and
    probing TCP for it would deadlock the gate on a conflict that does not
    exist for the guest.
    """
    taken: list[tuple[str, int, str]] = []
    for addr, port, proto in want:
        family = socket.AF_INET6 if ":" in addr else socket.AF_INET
        kind = socket.SOCK_DGRAM if proto == "udp" else socket.SOCK_STREAM
        with socket.socket(family, kind) as s:
            # Deliberately no SO_REUSEADDR: QEMU does not set it either, so
            # this reproduces QEMU's own bind semantics rather than a laxer one.
            try:
                s.bind((addr, port))
            except OSError:
                taken.append((addr, port, proto))
    return tuple(taken)


@dataclass(frozen=True, slots=True)
class Requirement:
    mem_mb: int
    cpus: int
    disk_mb: int
    guests: int = 1
    ports: tuple[tuple[str, int, str], ...] = ()

    def describe(self) -> str:
        parts = [f"mem {self.mem_mb} MiB", f"{self.cpus} vCPU", f"disk {self.disk_mb} MiB"]
        if self.ports:
            parts.append("ports " + ",".join(format_net_port(p) for p in self.ports))
        return ", ".join(parts)


def format_net_port(p: tuple[str, int, str]) -> str:
    """One claimed host port as `addr:port`, with a /udp suffix when UDP."""
    addr, port, proto = p
    return f"{addr}:{port}" if proto == "tcp" else f"{addr}:{port}/udp"


def requirement_for(inst: Instance, policy: Policy) -> Requirement:
    """What this instance needs from the host, derived from its own fields."""
    mem_mb = parse_memory_mb(inst.machine.memory) if inst.machine.memory else 1024
    cpus = inst.machine.smp if inst.machine.smp is not None else 1
    r = inst.rootfs
    disk_mb = sum(v for v in (r.size_mb, r.ext4_size_mb, r.world_size_mb)
                  if v is not None)
    return Requirement(
        mem_mb=mem_mb + policy.reserve_mem_mb,
        cpus=max(1, cpus),
        disk_mb=max(disk_mb, policy.min_disk_mb),
        ports=instance_net_ports(inst),
    )


@dataclass(frozen=True, slots=True)
class Verdict:
    ok: bool
    deficits: tuple[str, ...]
    remedies: tuple[str, ...] = ()

    def reason(self) -> str:
        return "; ".join(self.deficits)

    def remedy(self) -> str:
        return "; ".join(self.remedies)


def evaluate(need: Requirement, have: HostResources, policy: Policy,
             *, guest: bool = True) -> Verdict:
    """Check a requirement against the host.

    `guest=False` narrows the check to what a build-only step consumes.
    Asking `a20 package` to wait for 1 GiB of guest memory would be wrong:
    it never starts a guest, and refusing it because a guest is running
    would just make the two commands interfere.
    """
    cap = policy.max_concurrent or max(1, have.cpu_count // 4)
    d: list[str] = []
    r: list[str] = []
    if guest and have.mem_available_mb < need.mem_mb:
        d.append(f"memory: need {need.mem_mb} MiB, available {have.mem_available_mb} MiB")
        r.append("free memory (stop a guest or a build) and retry")
    if guest and have.cpu_count - have.load1 < need.cpus:
        d.append(f"cpu: need {need.cpus} idle, {have.cpu_count} total at load {have.load1:.1f}")
        r.append("wait for the current load to fall below the runqueue")
    if have.disk_free_mb < need.disk_mb:
        d.append(f"disk: need {need.disk_mb} MiB, free {have.disk_free_mb} MiB")
        r.append("free disk space, or lower A20_MIN_DISK_MB")
    if guest and have.running_guests + need.guests > cap:
        d.append(f"guest slots: {have.running_guests} running, cap {cap}")
        r.append("wait for a running guest to exit, or raise A20_MAX_CONCURRENT "
                 "(the default cap is one guest per 4 CPUs)")
    for addr, port, proto in (have.busy_ports if guest else ()):
        d.append(f"host port: {addr}:{port}/{proto} is already in use by another process")
        r.append(f"stop whatever holds {addr}:{port}, or give this instance a "
                 f"different hostfwd")
    return Verdict(ok=not d, deficits=tuple(d), remedies=tuple(r))


def format_report(need: Requirement, have: HostResources) -> str:
    return (f"mem {have.mem_available_mb}/{need.mem_mb} MiB, "
            f"cpu load {have.load1:.1f}/{have.cpu_count} want {need.cpus}, "
            f"disk {have.disk_free_mb}/{need.disk_mb} MiB, "
            f"guests {have.running_guests}")


def gate(need: Requirement, policy: Policy | None = None, disk_path: Path | None = None,
         *, wait: bool = True, echo=lambda _msg: None, label: str = "instance",
         guest: bool = True) -> Verdict:
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
        verdict = evaluate(need, have, policy, guest=guest)
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
            echo(f"a20: waiting to {label} -- host resources are short")
            echo(f"      have/want: {need.describe()}")
            echo(f"      blocked:   {verdict.reason()}")
            echo(f"      to proceed:{(' ' + verdict.remedy()) if verdict.remedy() else ''}")
            echo(f"      press Ctrl-C to give up")
            reported, last_emit = kinds, now
        if deadline is not None and now >= deadline:
            raise ResourceShortage(
                f"gave up waiting {policy.wait_timeout_s:.0f}s to {label}: "
                f"{verdict.reason()}",
                hint=verdict.remedy() or None)
        time.sleep(_POLL_SECONDS)


def preflight(inst: Instance, policy: Policy | None = None, disk_path: Path | None = None,
              *, wait: bool = True, echo=lambda _msg: None,
              guest: bool = True) -> Verdict:
    """Gate one instance run; see :func:`gate` for the wait semantics."""
    policy = policy or Policy.from_env()
    return gate(requirement_for(inst, policy), policy, disk_path,
                wait=wait, echo=echo, label=inst.name, guest=guest)


# --------------------------------------------------------------------------
# QEMU-launch preflight (consolidated from tools/a20_preflight.py, deleted).
#
# Same semantics the old module had, now on this module's single wait loop:
#   * guest image health (e2fsck -fn) is checked first and is not covered by
#     A20_PREFLIGHT=0 -- booting a rootfs the last run left dirty is never
#     what anyone wants;
#   * memory is required with headroom (1.5x declared + 1 GiB reserve) and
#     disk with an 8 GiB floor;
#   * A20_PREFLIGHT=0 skips the wait, A20_PREFLIGHT_TIMEOUT bounds it
#     (default 900s; CI passes a bounded A20_WAIT_TIMEOUT instead).
# --------------------------------------------------------------------------
MEMORY_HEADROOM_RATIO = 1.5
MEMORY_RESERVE_BYTES = 1 << 30  # 1 GiB
QEMU_DISK_MIN_MB = 8 << 10      # 8 GiB
QEMU_WAIT_DEFAULT_S = 900.0

# Mirrors of the Makefile defaults, so the gate sizes an instance that sets
# nothing the same way the launcher will.  self_check() keeps these honest.
DEFAULT_MEMORY = "1G"      # Makefile QEMU_MEMORY ?= 1G
DEFAULT_MEMORY_GUI = "4G"  # tools/targets-pkg.mk QEMU_MEMORY_GUI ?= 4G
DEFAULT_VCPUS = 1          # Makefile NR_CPUS ?= 1

_SIZE_SUFFIXES = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30, "T": 1 << 40}


def parse_size(text: str) -> int:
    """Parse a QEMU size string like "4G", "512M" or a bare byte count."""
    s = text.strip()
    if not s:
        raise ValueError("empty size")
    suffix = s[-1].upper()
    if suffix in _SIZE_SUFFIXES:
        return int(float(s[:-1]) * _SIZE_SUFFIXES[suffix])
    return int(s)


def self_check(makefile_text: str, pkg_mk_text: str) -> list[str]:
    """Report drift between the mirrored Makefile defaults and this module.

    Duplicated constants rot silently: bump QEMU_MEMORY_GUI in the Makefile and
    the gate keeps admitting instances the launcher cannot actually satisfy.
    `tools/a20 check` calls this so the drift fails a gate instead of turning
    into a mysterious OOM later.
    """
    import re
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
                f"{where}: default is {found.group(1)} but a20_resource mirrors "
                f"{expected} -- update both"
            )
    return problems


def check_guest_image(path: Path) -> list[str]:
    """Report ext4 errors in a guest rootfs image that is about to be booted.

    The guest images are created without a journal, so a run that does not shut
    down cleanly -- which is the normal case, since these are booted with a
    timeout and killed -- leaves on-disk metadata dirty.  The failure then
    presents as the kernel being broken (udev timeouts, fonts, apps refusing to
    start), so the launch is refused instead.  Read-only: e2fsck -fn.
    """
    import shutil
    import subprocess
    if os.environ.get("A20_PREFLIGHT_SKIP_FSCK", "0").strip().lower() in {"1", "yes", "on"}:
        return []
    if not path.exists() or shutil.which("e2fsck") is None:
        return []
    proc = subprocess.run(["e2fsck", "-fn", str(path)],
                          capture_output=True, text=True, check=False)
    if proc.returncode == 0:
        return []
    interesting = [
        line.strip()
        for line in (proc.stdout + proc.stderr).splitlines()
        if any(marker in line for marker in
               ("checksum", "orphaned", "unused inode", "cleared", "Fix?", "UNEXPECTED"))
    ]
    if not interesting:
        return []
    return [
        f"guest image {path.name} has filesystem errors:",
        *(f"  {line}" for line in interesting[:4]),
        "  it was left dirty by a previous run: these images have no journal and the"
        " guest is normally killed at timeout rather than shut down.",
        "  rebuild it with 'tools/a20 build <instance>' before booting"
        " (or A20_PREFLIGHT_SKIP_FSCK=1 to boot it anyway)",
    ]


def gate_qemu(*, memory: str, vcpus: int, disk_path: str, image: Path | None,
              label: str, echo=lambda _msg: None) -> None:
    """Preflight one QEMU launch; raises ResourceShortage when it must not run."""
    if image is not None:
        dirty = check_guest_image(image)
        if dirty:
            raise ResourceShortage("\n".join(dirty))
    if os.environ.get("A20_PREFLIGHT", "1").strip().lower() in {"0", "no", "off"}:
        return
    raw = os.environ.get("A20_PREFLIGHT_TIMEOUT")
    if raw is None:
        budget = QEMU_WAIT_DEFAULT_S
    else:
        try:
            budget = max(0.0, float(raw))
        except ValueError:
            budget = QEMU_WAIT_DEFAULT_S
    policy = Policy(wait_timeout_s=budget)
    mem_mb = (int(parse_size(memory) * MEMORY_HEADROOM_RATIO) >> 20) \
        + MEMORY_RESERVE_BYTES // (1 << 20)
    need = Requirement(mem_mb=mem_mb, cpus=vcpus, disk_mb=QEMU_DISK_MIN_MB)
    gate(need, policy, Path(disk_path), wait=True, echo=echo, label=label)


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
