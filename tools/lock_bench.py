#!/usr/bin/env python3
"""Lock-contention benchmark harness for the lock-serialization-split roadmap.

Runs one workload per boot on a real multi-vCPU QEMU guest and records
/proc/a20/lock_contention (per-lock totals plus the per-call-site sample
table) and /proc/a20/perf, then reduces repeated runs to median / min / max.

Why the window is reset rather than subtracted: the lock counters are
cumulative since boot with no way to narrow the window, and
docs/server-readiness.md records a wrong conclusion drawn from exactly that
(a stress spike read as boot traffic and vice versa).  The kernel exposes
`echo reset > /proc/a20/lock_contention` and the same for perf
(kernel/fs/procfs/procfs.c), so each run brackets its own workload with a
reset and a read, and the measured number belongs to the workload alone.

Every knob is an environment variable or a flag so the re-measurement round
after the proc/net/slab splits can re-run this file with zero edits:

  RUNS=5          repetitions per workload (median/min/max over these)
  SMP=8           -smp value passed to QEMU (host fallback is automatic)
  WORKLOADS=...   comma separated subset, default all four
  OUT_DIR=...     JSON + markdown destination, default docs/measured
  PHASE=baseline  output stem; baseline | after
  KERNEL_ELF, FAT32_IMG, BUILD_DIR, SKIP_BUILD=1
                  point the benchmark at an already-built image
  QEMU=..., QEMU_ACCEL=...
                  which emulator and accelerator string
  TIMEOUT=600     per-boot wall-clock budget

Usage:
  tools/lock_bench.py --phase baseline
  RUNS=3 SMP=4 tools/lock_bench.py --phase after
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Workload shape follows the existing smoke cases: the same user programs,
# same console interaction, and for net the same stress binary the
# smoke-smp-lock-contention gate uses.
WORKLOADS = {
    "mm_stress": {
        "cmd": "mm_stress",
        "pass_marker": "MM_STRESS: PASS",
        "desc": "mm_stress, full default case set",
    },
    "net_stress": {
        "cmd": "net_stress_test",
        "pass_marker": "NET_STRESS_TEST: PASS",
        "desc": "net_stress_test, 4 concurrent 4 MiB loopback TCP transfers "
                "(same binary as smoke-smp-lock-contention)",
    },
    "sched_stress": {
        "cmd": "sched_stress",
        "pass_marker": "SCHED_STRESS: PASS",
        "desc": "sched_stress, runqueue migration + lock-split phases",
    },
    "futex_stress": {
        "cmd": "futex_stress",
        "pass_marker": "FUTEX_STRESS: PASS",
        "desc": "futex_stress, PI / requeue / timeout cleanup cases",
    },
}
DEFAULT_WORKLOADS = ["mm_stress", "net_stress", "sched_stress", "futex_stress"]

# Locks the roadmap cares about; the raw JSON keeps every registered lock.
TRACKED_LOCKS = ["tasklist", "lwip", "slab", "runq"]  # proc_lock eliminated; its global residue is tasklist_lock

# Sentinels delimit the console regions so a dropped or interleaved command
# is visible in the JSON instead of silently shifting every later block.
# They are emitted with `echo`; a bare token is not a shell command.
SENT_PRE = "@@LOCKBENCH-PRE@@"
SENT_POST = "@@LOCKBENCH-POST@@"


def log(msg: str) -> None:
    print(f"[lock_bench] {msg}", flush=True)


def env_int(name: str, default: int) -> int:
    v = os.environ.get(name)
    if not v:
        return default
    try:
        return int(v)
    except ValueError:
        log(f"warning: {name}={v!r} is not an integer, using {default}")
        return default


def sha256_file(path: Path) -> str | None:
    if not path.is_file():
        return None
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def host_metadata() -> dict:
    def read(path):
        try:
            return Path(path).read_text().strip()
        except OSError:
            return None

    cpu_model = None
    try:
        for line in read("/proc/cpuinfo").splitlines():
            if line.startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip()
                break
    except Exception:
        pass
    return {
        "uname": " ".join(platform.uname()),
        "nproc": os.cpu_count(),
        "cpu_model": cpu_model,
        "mem_total_kb": next(
            (l.split()[1] for l in (read("/proc/meminfo") or "").splitlines()
             if l.startswith("MemTotal")), None),
        "loadavg": read("/proc/loadavg"),
        "python": sys.version.split()[0],
    }


def git_metadata() -> dict:
    def run(*args):
        try:
            return subprocess.run(["git", "-C", str(REPO), *args],
                                  capture_output=True, text=True,
                                  timeout=30).stdout.strip()
        except Exception:
            return None
    return {
        "head": run("rev-parse", "HEAD"),
        "describe": run("describe", "--always", "--dirty"),
        "status_porcelain": run("status", "--porcelain"),
    }


def qemu_version() -> str | None:
    exe = os.environ.get("QEMU", "qemu-system-riscv64")
    try:
        out = subprocess.run([exe, "--version"], capture_output=True,
                             text=True, timeout=30).stdout
        return out.splitlines()[0].strip()
    except Exception:
        return None


# ---------------------------------------------------------------- QEMU probing

def probe_smp(requested: int) -> tuple[int, list[str], dict]:
    """Ask QEMU whether it accepts -smp N with tcg,thread=multi.

    The probe loads /dev/null as the kernel, so QEMU gets past every option
    it would reject and then fails on the kernel image itself.  That failure
    is the success condition: reaching it means -smp N and the accelerator
    string were both parsed.  Anything else on stderr is a real rejection and
    is not read as acceptance.
    """
    exe = os.environ.get("QEMU", "qemu-system-riscv64")
    notes: list[str] = []
    attempts = []
    for n in (requested, 4):
        cmd = [exe, "-machine", "virt", "-m", "256M", "-nographic", "-smp",
               str(n), "-accel", "tcg,thread=multi", "-bios", "none",
               "-kernel", "/dev/null", "-no-reboot"]
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=60,
                               stdin=subprocess.DEVNULL)
            err = (p.stderr or "").strip().splitlines()
            # Option parsing happens before the kernel is loaded, so a
            # kernel-image complaint proves the options were accepted.
            ok = any("could not load kernel" in l or "invalid ELF" in l
                     for l in err) or p.returncode == 0
            attempts.append({"smp": n, "returncode": p.returncode,
                             "accepted": ok, "stderr_tail": err[-3:]})
        except subprocess.TimeoutExpired:
            # QEMU booted the empty -kernel and sat there: that means the
            # -smp / -accel combination was accepted.
            ok = True
            attempts.append({"smp": n, "returncode": "timeout-accepted",
                             "accepted": True, "stderr_tail": []})
        except FileNotFoundError as e:
            notes.append(f"QEMU binary {exe} not usable: {e}")
            return 0, notes, {"error": str(e)}
        if ok:
            if n != requested:
                notes.append(
                    f"host/QEMU refused -smp {requested} -accel "
                    f"tcg,thread=multi; fell back to -smp {n} as the roadmap "
                    f"§5 permits. Attempts: {attempts}")
            return n, notes, {"attempts": attempts, "accepted_smp": n}
        notes.append(f"-smp {n} rejected by QEMU: {attempts[-1]}")
    notes.append(
        f"neither -smp {requested} nor -smp 4 was accepted; cannot measure "
        f"lock contention on a single CPU. Attempts: {attempts}")
    return 0, notes, {"attempts": attempts, "accepted_smp": None}


# ------------------------------------------------------------------- build

def build_variant_dir(smp: int) -> Path:
    """Where Makefile's BUILD_VARIANT puts an ABI=linux NR_CPUS=smp dev build.

    Mirrors Makefile:396 (BUILD_DIR = .kernel-build/$(ARCH)-$(BOARD)-
    $(BUILD_VARIANT)); the same path smoke-smp-lock-contention boots at
    NR_CPUS=4 and docs/roadmap/perf-overhaul.md §5 references at 8.
    """
    return REPO / ".kernel-build" / \
        f"riscv64-qemu-virt-riscv64-linux-dev-smp{smp}"


def build_image(log_dir: Path, smp: int) -> dict:
    """Build the kernel + bootable FAT image for the benchmark's variant.

    NR_CPUS is pinned to the vCPU count actually handed to QEMU.  Makefile
    defaults NR_CPUS ?= 1, which would silently produce a single-CPU guest
    in which a spinlock cannot be contended at all -- the exact trap
    documented above smoke-smp-lock-contention in tools/targets-smoke.mk.
    """
    build_dir = Path(os.environ.get("BUILD_DIR", build_variant_dir(smp)))
    kernel_elf = Path(os.environ.get("KERNEL_ELF",
                                     str(build_dir / "kernel.elf")))
    fat32 = Path(os.environ.get("FAT32_IMG", str(build_dir / "fat32.img")))

    if os.environ.get("SKIP_BUILD") == "1":
        log(f"SKIP_BUILD=1: using existing {kernel_elf}")
        if not kernel_elf.is_file() or not fat32.is_file():
            raise SystemExit(
                f"SKIP_BUILD=1 but {kernel_elf} or {fat32} is missing")
        return {"build_dir": str(build_dir), "kernel_elf": kernel_elf,
                "fat32_img": fat32, "built": False, "log": None,
                "build_cmd": None}

    logf = log_dir / "build.log"
    cmd = [os.environ.get("MAKE", "make"), "-C", str(REPO), "-s",
           "ARCH=riscv64", "BOARD=qemu-virt-riscv64", "ABI=linux",
           "BRINGUP=0", f"NR_CPUS={smp}", "dev-build"]
    log(f"building: {' '.join(cmd)}  (output -> {logf})")
    started = time.time()
    with logf.open("w") as fh:
        rc = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL).returncode
    secs = round(time.time() - started, 1)
    if rc != 0:
        tail = "\n".join(logf.read_text(errors="replace").splitlines()[-40:])
        raise SystemExit(f"build failed (rc={rc}); tail of {logf}:\n{tail}")
    log(f"build ok in {secs}s -> {build_dir}")
    return {"build_dir": str(build_dir), "kernel_elf": kernel_elf,
            "fat32_img": fat32, "built": True, "log": str(logf),
            "build_seconds": secs, "build_cmd": cmd}


def verify_nr_cpus(build_dir: str, smp: int) -> dict:
    """Read back the CONFIG_NR_CPUS the kernel was actually compiled with.

    Makefile:894 passes it as -DCONFIG_NR_CPUS=$(NR_CPUS); the flag set is
    stamped into <build-dir>/.build-flags, which is what gets read here, so a
    stale directory from an earlier build cannot masquerade as this one.
    """
    out = {"expected_smp": smp, "checked": []}
    found = None
    stamp = Path(build_dir) / ".build-flags"
    for c in (stamp,):
        if not c.is_file():
            continue
        m = re.search(r"-DCONFIG_NR_CPUS=(\d+)",
                      c.read_text(errors="replace"))
        val = int(m.group(1)) if m else None
        out["checked"].append({"path": str(c), "config_nr_cpus": val})
        if val is not None and found is None:
            found = val
    out["config_nr_cpus"] = found
    out["ok"] = found is not None and found >= smp
    out["stamp_mtime"] = stamp.stat().st_mtime if stamp.is_file() else None
    return out


# ------------------------------------------------------------- console driving

def region(text: str, pre: str, post: str) -> str | None:
    """Return the console text between two sentinels, or None if a sentinel
    is missing.  A missing region is reported, never silently ignored."""
    i = text.find(pre)
    if i < 0:
        return None
    j = text.find(post, i + len(pre))
    if j < 0:
        return None
    return text[i + len(pre):j]


def _drain(stream, sink: list, lock: threading.Lock) -> None:
    """Read QEMU's console into `sink` until EOF.

    The guest has to keep printing while the driver decides what to type
    next, so the console cannot be read only after the process exits.
    """
    try:
        while True:
            chunk = stream.read(1)
            if not chunk:
                break
            with lock:
                sink.append(chunk.decode(errors="replace"))
    except (ValueError, OSError):
        pass


def run_one_boot(workload: dict, smp: int, kernel_elf: Path, fat32: Path,
                 scratch: Path, timeout_s: int, input_delay: float) -> dict:
    """Boot QEMU, run one workload between counter resets, collect counters."""
    # Work on a per-run copy so the built image stays byte-identical and the
    # re-measurement round starts from the same state with zero setup.
    scratch.mkdir(parents=True, exist_ok=True)
    img = scratch / "fat32.img"
    shutil.copyfile(fat32, img)

    exe = os.environ.get("QEMU", "qemu-system-riscv64")
    accel = os.environ.get("QEMU_ACCEL", "tcg,thread=multi")
    cmd = [
        exe,
        "-machine", "virt", "-m", "1G", "-nographic",
        "-smp", str(smp), "-accel", accel, "-bios", "default",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={img},if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        "-netdev", "user,id=net",
        "-device", "virtio-net-device,netdev=net,bus=virtio-mmio-bus.4",
        "-kernel", str(kernel_elf),
        "-append", ("a20.ip=10.0.2.15 a20.netmask=255.255.255.0 "
                    "a20.gateway=10.0.2.2 a20.dns=10.0.2.3 "
                    "a20.hostname=a20os"),
    ]

    # reset -> workload -> read.  The pre-load is a second read so the boot
    # window is measured on its own; the delta is the workload window.
    # Sentinels go through `echo`, not as bare words: a bare token is not a
    # command, and mksh's "inaccessible or not found" path wedges the console
    # -- the command after it never runs.
    pre_block = [
        f"echo {SENT_PRE}",
        "echo reset > /proc/a20/lock_contention",
        "echo reset > /proc/a20/perf",
        "cat /proc/a20/lock_contention",
        "cat /proc/a20/perf",
        f"echo {SENT_POST}",
    ]
    post_block = [
        f"echo {SENT_PRE}-2",
        "cat /proc/a20/lock_contention",
        "cat /proc/a20/perf",
        f"echo {SENT_POST}-2",
        "poweroff",
    ]

    started = time.time()
    # The console is driven by a state machine, not one bulk write.  Piping
    # every line at once wedges the guest: a `cat /proc/a20/perf` dump is
    # ~250 UART lines, and the shell is not reading its input while it prints,
    # so the queued tail never gets consumed and the run dies before poweroff.
    # Waiting for the workload's own PASS marker before sending the closing
    # block removes the guess about how long the workload takes.
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    buf: list[str] = []
    lock = threading.Lock()
    reader = threading.Thread(target=_drain, args=(p.stdout, buf, lock),
                              daemon=True)
    reader.start()

    def send(lines: list[str]) -> None:
        try:
            p.stdin.write(("".join(x + "\n" for x in lines)).encode())
            p.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass

    def wait_for(needle: str, budget: float) -> bool:
        end = time.time() + budget
        while time.time() < end:
            with lock:
                if needle in "".join(buf):
                    return True
            if p.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    timed_out = False
    booted = wait_for("# ", input_delay)
    send(pre_block)
    got_post = wait_for(SENT_POST, timeout_s)
    send([workload["cmd"]])
    finished = wait_for(workload["pass_marker"], timeout_s)
    send(post_block)
    try:
        rc = p.wait(timeout=60 if finished else 20)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = p.wait()
    secs = time.time() - started
    timed_out = not finished
    with lock:
        console = "".join(buf)
    (scratch / "console.log").write_text(console)
    if not booted:
        console += "\n--- note: shell prompt '#' never appeared before the "\
                   "input delay elapsed ---\n"
    rec = {
        "qemu_cmd": cmd,
        "qemu_returncode": rc,
        "timed_out": timed_out,
        "shell_prompt_seen": booted,
        "workload_marker_seen": finished,
        "pre_block_acknowledged": got_post,
        "wall_seconds": round(secs, 1),
        "console_log": str(scratch / "console.log"),
    }

    rec["passed"] = workload["pass_marker"] in console
    rec["panicked"] = "panic" in console.lower()
    pre = region(console, SENT_PRE, SENT_POST)
    post = region(console, SENT_PRE + "-2", SENT_POST + "-2")
    rec["boot_window_present"] = pre is not None
    rec["workload_window_present"] = post is not None
    if pre is None or post is None:
        rec["error"] = ("missing counter region: "
                        f"pre={pre is not None} post={post is not None}")
        return rec

    rec["locks_pre"] = parse_locks(pre)
    rec["locks_post"] = parse_locks(post)
    rec["perf_pre"] = parse_perf(pre)
    rec["perf_post"] = parse_perf(post)
    rec["workload_lock_delta"] = {
        lock: {field: rec["locks_post"].get(lock, {}).get(field, 0) - pre_v.get(field, 0)
               for field in ("acquires", "spins", "max_spins")}
        for lock, pre_v in rec["locks_pre"].items()}
    rec["workload_perf_delta"] = {
        k: rec["perf_post"].get(k, 0) - v
        for k, v in rec["perf_pre"].items()}
    rec["callsites"] = parse_callsites(post)
    # Control window: traffic between the reset and the first read, i.e. the
    # two `cat`s and the shell's own work.  It is the floor the workload
    # numbers sit on, not boot-time contention -- the counters were zeroed
    # before it, so anything boot-time is deliberately excluded.
    rec["control_window"] = rec["locks_pre"]
    return rec


LOCK_RE = re.compile(r"^(\w+): (\d+) (\d+) max=(\d+)\s*$")
SITE_RE = re.compile(r"^\s*\[(\w+)\] (\S+)\+0x([0-9a-f]+): "
                     r"(\d+) (\d+) max=(\d+)\s*$")


def parse_locks(text: str) -> dict:
    """lock name -> {acquires, spins, max_spins} for the lock total line."""
    out: dict[str, dict] = {}
    for line in text.splitlines():
        m = LOCK_RE.match(line)
        if m:
            out[m.group(1)] = {"acquires": int(m.group(2)),
                               "spins": int(m.group(3)),
                               "max_spins": int(m.group(4))}
    return out


def parse_callsites(text: str) -> dict:
    """lock -> list of {symbol, offset, acquires, spins, max_spins}.

    The kernel hashes the return address into a fixed 32-slot table
    (LOCK_CALLSITE_SAMPLES), so the site list is a sample, not a complete
    attribution; lock total spins can legitimately exceed the summed sites.
    """
    out: dict[str, list] = {}
    for line in text.splitlines():
        m = SITE_RE.match(line)
        if m:
            out.setdefault(m.group(1), []).append({
                "symbol": m.group(2),
                "offset": m.group(3),
                "acquires": int(m.group(4)),
                "spins": int(m.group(5)),
                "max_spins": int(m.group(6)),
            })
    for v in out.values():
        v.sort(key=lambda d: -d["spins"])
    return out


def parse_perf(text: str) -> dict:
    """Counter name -> value.  syscall_N_count/_ticks pairs are folded into
    syscall_N_count / syscall_N_ticks keys so the two lines cannot collide."""
    out: dict[str, int] = {}
    for line in text.splitlines():
        line = line.strip()
        m = re.match(r"^(\w+): (\d+)$", line)
        if m:
            out[m.group(1)] = int(m.group(2))
            continue
        m = re.match(r"^syscall_(\d+)_(count|ticks): (\d+)$", line)
        if m:
            out[f"syscall_{m.group(1)}_{m.group(2)}"] = int(m.group(3))
    return out


# --------------------------------------------------------------- aggregation

def series(runs: list[dict], getter) -> list[int]:
    return [getter(r) for r in runs if getter(r) is not None]


def summarize(values: list[int]) -> dict:
    if not values:
        return {"n": 0}
    return {
        "n": len(values),
        "values": values,
        "median": int(statistics.median(values)),
        "min": min(values),
        "max": max(values),
        "mean": round(statistics.fmean(values), 1),
        "stdev": round(statistics.pstdev(values), 1),
    }


def aggregate(runs: list[dict]) -> dict:
    agg: dict = {"per_run_ok": [bool(r.get("passed")) for r in runs],
                 "locks": {}, "perf": {}, "callsites": {}}
    for lock in sorted({k for r in runs for k in r.get("locks_post", {})}):
        deltas = [r.get("workload_lock_delta", {}).get(lock, {}) for r in runs]
        boot = [r.get("control_window", {}).get(lock, {}) for r in runs]
        entry = {}
        for field in ("acquires", "spins", "max_spins"):
            entry[field] = summarize(
                [d[field] for d in deltas if field in d])
            entry[f"control_{field}"] = summarize(
                [b[field] for b in boot if field in b])
        agg["locks"][lock] = entry

    for name in sorted({k for r in runs for k in r.get("workload_perf_delta", {})}):
        agg["perf"][name] = summarize(
            [r["workload_perf_delta"][name] for r in runs
             if name in r.get("workload_perf_delta", {})])

    # Call sites: keyed by lock+symbol; reported per run as a list so a site
    # that only shows up in some runs does not get averaged with zeros.
    for lock in sorted({k for r in runs for k in r.get("callsites", {})}):
        per_run = []
        syms = set()
        for r in runs:
            sites = r.get("callsites", {}).get(lock, [])
            per_run.append(sites)
            syms.update(s["symbol"] for s in sites)
        agg["callsites"][lock] = {
            "symbols": sorted(syms),
            "spins_by_run": {
                s: [next((x["spins"] for x in run if x["symbol"] == s), 0)
                    for run in per_run] for s in sorted(syms)},
            "acquires_by_run": {
                s: [next((x["acquires"] for x in run if x["symbol"] == s), 0)
                    for run in per_run] for s in sorted(syms)},
        }
    return agg


# ------------------------------------------------------------------- render

def render_markdown(result: dict, path: Path) -> None:
    phase = result["phase"]
    env = result["env"]
    meta = result["metadata"]
    lines = []
    A = lines.append
    A(f"# Lock contention baseline ({phase})")
    A("")
    A(f"Generated by `tools/lock_bench.py` (roadmap "
      f"`docs/roadmap/lock-serialization-split.md` §5). "
      f"Protocol: reset counters → run one workload → read counters; "
      f"{result['runs']} repetitions per workload, median plus min/max.")
    A("")
    A(f"- Phase: `{phase}`")
    A(f"- Worktree: `{meta['repo']}`")
    A(f"- Git HEAD: `{meta['git']['head']}` (`{meta['git']['describe']}`)")
    A(f"- Kernel commit: `{meta['kernel_commit']}`")
    A(f"- vCPUs: {result['smp']} (`-smp {result['smp']} "
      f"-accel {result['accel']}`), built with `NR_CPUS={env['smp']}`, "
      f"compiled-in `CONFIG_NR_CPUS="
      f"{result['nr_cpus_check'].get('config_nr_cpus')}`")
    A(f"- kernel.elf sha256: `{meta['kernel_elf_sha256']}`")
    A(f"- fat32.img sha256 (as built): `{meta['fat32_img_sha256']}`")
    A(f"- QEMU: `{meta['qemu_version']}`")
    A(f"- Host: {meta['host']['uname']}, {meta['host']['nproc']} logical "
      f"CPUs, {meta['host']['cpu_model']}")
    A(f"- Build command: `{' '.join(result['build']['build_cmd'])}`"
      if result['build'].get('build_cmd') else "- Build: reused existing image")
    if meta["repo_dirty_files"]:
        A(f"- **Untracked/modified files present**: "
          f"`{'`, `'.join(meta['repo_dirty_files'])}`")
    A("")
    for n in result["notes"]:
        A(f"> {n}")
        A("")
    if meta["repo_dirty_files"]:
        A("> The tree was not clean when this was produced; the kernel "
          "objects hashed above are what actually ran.")
        A("")

    A("## Per-workload result")
    A("")
    A("`acquires` / `spins` / `max_spins` are the workload window only: the "
      "counters are zeroed with `echo reset > /proc/a20/lock_contention` "
      "immediately before the workload command and read immediately after, so "
      "boot-time traffic is excluded by construction rather than subtracted "
      "after the fact. `Control-window` is the same counters between the "
      "reset and the first read -- the floor the workload numbers sit on.")
    A("")
    A("**Noise discipline.** `docs/server-readiness.md` records repeated runs "
      "of one 4-core workload spanning 0 to ~920024 spins, and this table's "
      "own max-min spread is the honest resolution limit for every cell "
      "below. A post-change median that differs from the baseline median by "
      "less than that cell's max-min spread is **not resolvable** at this "
      "sample size and must be written as such rather than claimed.")
    A("")
    A("| Workload | Lock | Acquires (median) | Spins (median) | Spins (min) "
      "| Spins (max) | Spins stdev | Spins spread (max-min) "
      "| Worst acquire (median) | Control-window spins (median) |")
    A("|---|---|---|---|---|---|---|---|---|---|")
    for wl in result["workload_order"]:
        agg = result["results"][wl]["aggregate"]
        any_row = False
        for lock in TRACKED_LOCKS + [l for l in agg["locks"]
                                     if l not in TRACKED_LOCKS]:
            e = agg["locks"].get(lock)
            if not e or e["spins"]["n"] == 0 and e["acquires"]["n"] == 0:
                continue
            A(f"| {wl if not any_row else ''} | `{lock}` | "
              f"{e['acquires']['median']} | {e['spins']['median']} | "
              f"{e['spins']['min']} | {e['spins']['max']} | "
              f"{e['spins']['stdev']} "
              f"| {e['spins']['max'] - e['spins']['min']} "
              f"| {e['max_spins']['median']} | "
              f"{e['control_spins']['median']} |")
            any_row = True
        if not any_row:
            A(f"| {wl} | (no lock counters registered) | - | - | - | - | - "
              f"| - | - | - |")
    A("")

    seen = sorted({lock for wl in result["workload_order"]
                   for lock in result["results"][wl]["aggregate"]["locks"]})
    missing = [l for l in TRACKED_LOCKS if l not in seen]
    A(f"Locks that reported at least one counter: "
      f"{', '.join('`' + l + '`' for l in seen) or 'none'}.")
    if missing:
        A("")
        A(f"Tracked locks that never appeared: "
          f"{', '.join('`' + l + '`' for l in missing)}. "
          f"`/proc/a20/lock_contention` only reports locks passed to "
          f"`lock_counters_register()` (kernel/core/lock_counters.c), so an "
          f"absent name means this kernel does not register that lock for "
          f"accounting -- not that it was uncontended. Any claim about such a "
          f"lock needs a registration change first.")
    A("")

    A("## /proc/a20/perf deltas (workload window)")
    A("")
    A("Only counters that moved in at least one run are listed; the rest are "
      "structurally zero for these workloads. `syscall_<N>_count` / "
      "`syscall_<N>_ticks` are raw kernel syscall numbers, not names; N is "
      "resolved by tools/syscall-coverage or docs/native-abi.")
    A("")
    names = sorted({n for wl in result["workload_order"]
                    for n, s in result["results"][wl]["aggregate"]["perf"].items()
                    if s["n"] and (s["median"] or s["max"])})
    if names:
        A("| Counter | " + " | ".join(result["workload_order"]) + " |")
        A("|---" * (len(result["workload_order"]) + 1) + "|")
        for n in names:
            cells = []
            for wl in result["workload_order"]:
                s = result["results"][wl]["aggregate"]["perf"].get(n)
                cells.append(f"{s['median']} [{s['min']}-{s['max']}]"
                             if s and s["n"] else "-")
            A(f"| `{n}` | " + " | ".join(cells) + " |")
    else:
        A("_No perf counter moved in any run._")
    A("")

    A("## /proc/a20/lock_contention call-site sampling (workload window)")
    A("")
    A("Per-run spin counts per symbol. The kernel hashes the return address "
      "into a fixed 32-slot table (`LOCK_CALLSITE_SAMPLES`), so this is a "
      "sample: summed site spins can be below the lock's total, and a run may "
      "legitimately attribute nothing.")
    A("")
    for wl in result["workload_order"]:
        agg = result["results"][wl]["aggregate"]["callsites"]
        if not agg:
            continue
        A(f"### {wl}")
        A("")
        A("| Lock | Symbol | Spins per run | Acquires per run |")
        A("|---|---|---|---|")
        for lock, sites in sorted(agg.items()):
            for sym in sites["symbols"]:
                A(f"| `{lock}` | `{sym}` | "
                  f"{sites['spins_by_run'][sym]} | "
                  f"{sites['acquires_by_run'][sym]} |")
        A("")

    A("## Run log")
    A("")
    A("| Workload | Run | Result | Wall (s) | QEMU rc | Console |")
    A("|---|---|---|---|---|---|")
    for wl in result["workload_order"]:
        for i, r in enumerate(result["results"][wl]["runs"], 1):
            A(f"| {wl if i == 1 else ''} | {i} | "
              f"{'PASS' if r.get('passed') else 'FAIL'}"
              f"{' (timeout)' if r.get('timed_out') else ''}"
              f"{' (panic)' if r.get('panicked') else ''} | "
              f"{r.get('wall_seconds')} | {r.get('qemu_returncode')} | "
              f"`{r.get('console_log')}` |")
    A("")
    A("## Raw data")
    A("")
    A(f"Per-run JSON, full `/proc/a20/lock_contention` and `/proc/a20/perf` "
      f"snapshots, the control window and the workload window, every "
      f"registered lock, and the exact QEMU argv of each boot: "
      f"`{result['json_name']}`.")
    A("")
    A("## Reproducing")
    A("")
    A("The measured guest kernel is the ABI=linux NR_CPUS=8 dev variant, the "
      "same build directory docs/roadmap/perf-overhaul.md \u00a75 names and "
      "the one smoke-smp-lock-contention boots at NR_CPUS=4. The plain "
      "release build is the unmodified-kernel build check; its "
      "`instances/release-riscv64.toml` also pins smp=8.")
    A("")
    A("```sh")
    A("# 1. unmodified kernel builds clean (both arches); -> kernel-rv, disk.img")
    A("make -s ARCH=riscv64 BOARD=qemu-virt-riscv64")
    A("")
    A("# 2. the measured image: 8 vCPUs, ABI=linux dev variant")
    A("make -s ARCH=riscv64 BOARD=qemu-virt-riscv64 ABI=linux BRINGUP=0 \\")
    A(f"     NR_CPUS={env['smp']} dev-build")
    A("")
    A("# 3. the measurement itself")
    A(f"RUNS={result['runs']} SMP={env['smp']} "
      f"WORKLOADS={','.join(result['workload_order'])} \\")
    A(f"  OUT_DIR={env['out_dir']} tools/lock_bench.py --phase {phase}")
    A("")
    A("# or, equivalently, through make (same harness, same defaults):")
    A(f"make bench-locks RUNS={result['runs']} SMP={env['smp']} "
      f"PHASE={phase}")
    A("```")
    A("")
    A("The post-modification round is the same command with "
      "`--phase after` and `OUT_DIR=docs/measured`, so no file in this "
      "directory needs editing to re-measure. Per-run console logs land in "
      "`.kernel-build/bench-logs/<phase>/<workload>/run<N>/console.log` and "
      "are not committed; the JSON carries the parsed snapshots.")
    path.write_text("\n".join(lines) + "\n")


# ---------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--phase", default=os.environ.get("PHASE", "baseline"),
                    help="baseline | after  (selects the output stem)")
    ap.add_argument("--runs", type=int, default=env_int("RUNS", 5))
    ap.add_argument("--smp", type=int, default=env_int("SMP", 8))
    ap.add_argument("--workloads", default=os.environ.get(
        "WORKLOADS", ",".join(DEFAULT_WORKLOADS)))
    ap.add_argument("--out-dir", default=os.environ.get(
        "OUT_DIR", str(REPO / "docs" / "measured")))
    ap.add_argument("--timeout", type=int, default=env_int("TIMEOUT", 600))
    ap.add_argument("--input-delay", type=float,
                    default=float(os.environ.get("INPUT_DELAY", "10")))
    args = ap.parse_args()

    unknown = [w for w in args.workloads.split(",") if w and w not in WORKLOADS]
    if unknown:
        raise SystemExit(f"unknown workload(s): {', '.join(unknown)}; "
                         f"known: {', '.join(WORKLOADS)}")
    order = [w for w in DEFAULT_WORKLOADS
             if w in [x for x in args.workloads.split(",") if x]]

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = os.environ.get("TAG", f"lock-{args.phase}")
    log_dir = Path(os.environ.get("LOG_DIR",
                                  str(REPO / ".kernel-build" / "bench-logs")))
    log_dir.mkdir(parents=True, exist_ok=True)

    smp, notes, probe = probe_smp(args.smp)
    for n in notes:
        log(f"probe: {n}")
    if smp == 0:
        result = {"phase": args.phase, "status": "blocked",
                  "notes": notes, "probe": probe, "env": vars(args),
                  "workload_order": order, "runs": args.runs, "smp": None,
                  "accel": None}
        print(json.dumps(result, indent=2, default=str))
        return 2

    build = build_image(log_dir, smp)
    nr_check = verify_nr_cpus(build["build_dir"], smp)
    log(f"CONFIG_NR_CPUS={nr_check.get('config_nr_cpus')} (need >= {smp}): "
        f"{'OK' if nr_check['ok'] else 'MISMATCH'}")
    if not nr_check["ok"]:
        notes.append(
            f"compiled-in CONFIG_NR_CPUS="
            f"{nr_check.get('config_nr_cpus')} is below the -smp {smp} the "
            f"guest is given; the extra vCPUs would stay offline and no "
            f"cross-core contention could be measured.")

    dirty = [ln[3:] for ln in (subprocess.run(
        ["git", "-C", str(REPO), "status", "--porcelain"],
        capture_output=True, text=True).stdout or "").splitlines()
        if ln.strip()]

    result = {
        "phase": args.phase,
        "status": "ok",
        "runs": args.runs,
        "smp": smp,
        "accel": os.environ.get("QEMU_ACCEL", "tcg,thread=multi"),
        "workload_order": order,
        "notes": notes,
        "probe": probe,
        "build": {k: str(v) if isinstance(v, Path) else v
                  for k, v in build.items()},
        "nr_cpus_check": nr_check,
        "env": {"runs": args.runs, "smp": args.smp, "timeout": args.timeout,
                "input_delay": args.input_delay, "out_dir": str(out_dir),
                "tag": stem},
        "metadata": {
            "repo": str(REPO),
            "git": git_metadata(),
            "repo_dirty_files": dirty,
            "kernel_commit": subprocess.run(
                ["git", "-C", str(REPO / "kernel"), "rev-parse", "HEAD"],
                capture_output=True, text=True).stdout.strip() or None,
            "host": host_metadata(),
            "qemu_version": qemu_version(),
            "kernel_elf": str(build["kernel_elf"]),
            "kernel_elf_sha256": sha256_file(build["kernel_elf"]),
            "fat32_img": str(build["fat32_img"]),
            "fat32_img_sha256": sha256_file(build["fat32_img"]),
        },
        "results": {},
    }

    for wl in order:
        log(f"=== {wl}: {args.runs} run(s) on -smp {smp} ===")
        runs = []
        for i in range(1, args.runs + 1):
            scratch = log_dir / args.phase / wl / f"run{i}"
            log(f"  {wl} run {i}/{args.runs}")
            rec = run_one_boot(WORKLOADS[wl], smp, build["kernel_elf"],
                               build["fat32_img"], scratch,
                               args.timeout, args.input_delay)
            rec["run"] = i
            if not rec.get("passed") or rec.get("panicked") or \
                    rec.get("timed_out"):
                log(f"    run {i}: pass={rec.get('passed')} "
                    f"panic={rec.get('panicked')} "
                    f"timeout={rec.get('timed_out')} "
                    f"(console: {rec['console_log']})")
            runs.append(rec)
        result["results"][wl] = {
            "description": WORKLOADS[wl]["desc"],
            "runs": runs,
            "aggregate": aggregate(runs),
        }

    json_path = out_dir / f"{stem}-data.json"
    result["json_name"] = json_path.name
    json_path.write_text(json.dumps(result, indent=2, default=str))
    md_path = out_dir / f"{stem}.md"
    render_markdown(result, md_path)
    log(f"wrote {json_path}")
    log(f"wrote {md_path}")

    # JSON must land before the markdown is claimed, and both names go into
    # the result so the caller can publish them.
    print(json.dumps({"json": str(json_path), "md": str(md_path),
                      "smp": smp, "runs": args.runs}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())