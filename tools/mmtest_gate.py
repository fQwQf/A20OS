#!/usr/bin/env python3
"""Real-software gate for the single-level memory model.

Why this exists
---------------
Every other MM gate in this tree (smoke-mm-stress, smoke-mm-pt-race,
smoke-mm-fork-exec-race) exercises syscalls the kernel itself wrote, on
allocations the kernel handed out.  They can all pass while the model is wrong
in the places real programs hit: a compiler that mmaps a large arena, a JIT
that mprotects code pages, a version control system that mmaps a 3 GiB index
and then remaps it, an interpreter that forks 5000 objects.

This gate boots a distro world image and runs git, vim, gcc, python and
nodejs in it.  Each stage verifies *content*, not exit codes -- a `git clone`
that produced an empty repository exits 0.

Two verdicts, both required
---------------------------
1. ``MMTEST_RESULT: PASS`` -- all five stages ran and checked out.
2. The ``[MM-ASM]`` audit line must be all zeros -- the hardware page tables
   and the per-PTE metadata agreed in every live address space at shutdown.

Either one alone is worthless.  (1) alone passes on a kernel whose two
representations have drifted apart, because nothing in a workload notices
until the drift becomes a wrong page.  (2) alone passes on a kernel that
mmaps nothing at all.  The gate is the conjunction, which is the only thing
that says the model is load-bearing and correct at the same time.

The audit line only prints on the shutdown path, so the in-guest script powers
itself off; otherwise the guest is killed by the timeout, the audit never runs,
and the audit half of this gate silently degrades into "no output, skip it" --
the exact failure mode a green bar must not have.

Usage:
    make smoke-mm-software
    python3 tools/mmtest_gate.py --image IMG --kernel KERNEL [--timeout N]
                                 [--log PATH] [--cmdline STR] [--no-build]
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The world carries the five tools plus the kernel's own corpus tools.
DEFAULT_WORLD = "mmtest"
DEFAULT_SIZE_MB = "2048"
QEMU_MEM = "2G"
# Unpacking /extra (ext4 + ~400 MiB of apk world) under TCG is not fast; the
# in-guest script waits for the distro init to hand over the shell before it
# receives the command, so this is dead time from the feeder's point of view.
FEED_DELAY = 30

AUDIT_RE = re.compile(
    r"\[MM-ASM\] pt_pages=(\d+) entries=(\d+) missing_meta=(\d+) present=(\d+) "
    r"absent=(\d+) prot=(\d+) cow=(\d+) vma=(\d+) vmai=(\d+) cls=(\d+) "
    r"safe=(\d+) anon_virt=(\d+) seg_slots=(\d+) seg_bad=(\d+) "
    r"seg_kind=(\d+) seg_ok=(\d+) seg_diff=(\d+) seg_miss=(\d+) "
    r"seg_dispatch=(\d+) seg_fallback=(\d+)"
)
AUDIT_FIELDS = [
    "pt_pages", "entries", "missing_meta", "present", "absent", "prot",
    "cow", "vma", "vmai", "cls", "safe", "anon_virt",
    "seg_slots", "seg_bad", "seg_kind", "seg_ok", "seg_diff", "seg_miss",
    "seg_dispatch", "seg_fallback",
]
# Fields that are counts of pages and must be 0.  pt_pages and entries are
# totals, and anon_virt is the on-demand-paging state the model is built on --
# none of them is a mismatch.
# seg_slots is an observation, not a verdict: it counts PT-node entries that
# name a segment, which is > 0 for any process with a file or VMO mapping.
# seg_bad and seg_kind are verdicts and must be 0.
NOT_A_MISMATCH = ("pt_pages", "entries", "anon_virt", "seg_slots",
                  "seg_ok", "seg_miss", "seg_dispatch", "seg_fallback")

# The mapping-record list invariant, printed as its own line.  It replaced the
# `seg extent` / `idx_agree` / `idx_diff` counters, whose subject -- "do the
# mapping list and the segment list agree" -- stopped existing once the two
# representations were merged into one record (roadmap 13.18).
#
# Parsed for the same reason AUDIT_RE is: a counter the kernel computes and
# prints but the gate never reads is decoration.  `overlap` is what the binary
# search in mm_seg_find() silently depends on -- an unsorted list answers
# "which mapping is this?" with a neighbour's -- and `dead` is the
# use-after-free that the lock-model gate once caught as magic=0x0.
MAPLIST_RE = re.compile(
    r"\[MM-ASM\]\s+map list: entries=(\d+) overlap=(\d+) dead=(\d+) ok=(\d+)"
)


def log(msg: str) -> None:
    print(f"[mmtest] {msg}", flush=True)


def run(cmd, **kw) -> int:
    log("$ " + " ".join(cmd))
    return subprocess.call(cmd, cwd=REPO, **kw)


def build_image(world: str, size_mb: str) -> str:
    rc = run(["make", "ARCH=riscv64", f"PKG_WORLD={world}",
              f"PKG_SIZE_MB={size_mb}", "image-world"])
    if rc != 0:
        sys.exit(rc)
    return os.path.join(REPO, "build", "images", f"{world}-riscv64.img")


def build_kernel() -> str:
    rc = run(["make", "ARCH=riscv64", "BRINGUP=0", "dev-build"])
    if rc != 0:
        sys.exit(rc)
    d = os.path.join(REPO, ".kernel-build",
                     "riscv64-qemu-virt-riscv64-both-dev")
    return os.path.join(d, "kernel.elf")


def boot(img: str, kernel: str, log_path: str, timeout: int,
         cmdline: str, guest_cmd: str) -> int:
    """Boot the world image, feed one command to the shell, collect the log.

    The image is copied first.  QEMU takes a write lock on a raw disk, so two
    concurrent runs -- or a run started before the previous qemu has exited --
    fail with "Failed to get write lock", which reads like a disk bug and is
    not one.
    """
    snap = tempfile.mkdtemp(prefix="mmtest-img-")
    snap_img = os.path.join(snap, "world.img")
    try:
        shutil.copyfile(img, snap_img)
        fifo = os.path.join(snap, "in")
        os.mkfifo(fifo)
        feed_log = open(os.path.join(snap, "feed.log"), "wb")
        feeder = subprocess.Popen(
            [sys.executable, "-c", (
                "import sys,time\n"
                "f=open(sys.argv[1],'wb',buffering=0)\n"
                f"time.sleep({FEED_DELAY})\n"
                f"f.write(({guest_cmd!r}+'\\n').encode())\n"
                f"time.sleep({timeout - FEED_DELAY - 10})\n"),
             fifo],
            stdout=feed_log, stderr=subprocess.STDOUT)

        cmd = [
            "qemu-system-riscv64",
            "-machine", "virt", "-m", QEMU_MEM, "-nographic", "-smp", "4",
            "-bios", "default",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={snap_img},if=none,format=raw,id=xworld",
            "-device",
            "virtio-blk-device,drive=xworld,bus=virtio-mmio-bus.0",
            "-kernel", kernel,
        ]
        if cmdline:
            cmd += ["-append", cmdline]

        log(f"booting (timeout {timeout}s, log {log_path})")
        t0 = time.time()
        with open(fifo, "rb") as fin, open(log_path, "wb") as fout:
            rc = subprocess.call(cmd, stdin=fin, stdout=fout,
                                 stderr=subprocess.STDOUT, timeout=timeout + 60)
        log(f"qemu exit={rc} after {time.time() - t0:.0f}s")
        feeder.kill()
        feed_log.close()
        return rc
    except subprocess.TimeoutExpired:
        log(f"TIMEOUT after {timeout}s -- the guest never finished")
        return 124
    finally:
        shutil.rmtree(snap, ignore_errors=True)


def verdict(log_path: str) -> int:
    try:
        with open(log_path, "r", errors="replace") as f:
            text = f.read()
    except OSError as e:
        log(f"cannot read log: {e}")
        return 1

    ok = True

    if "MMTEST_RESULT: PASS" in text:
        log("stages: PASS (git vim gcc python nodejs)")
    else:
        ok = False
        log("stages: FAIL")
        for line in text.splitlines():
            if "MMTEST: FAIL" in line:
                log("  " + line.strip())

    m = AUDIT_RE.search(text)
    if not m:
        ok = False
        log("audit: NO [MM-ASM] LINE -- the guest never reached the shutdown "
            "path, so the metadata/page-table comparison never ran. A gate "
            "that cannot see its own invariant has not passed it.")
    else:
        counts = dict(zip(AUDIT_FIELDS, (int(g) for g in m.groups())))
        bad = {k: v for k, v in counts.items()
               if k not in NOT_A_MISMATCH and v != 0}
        if bad:
            ok = False
            log(f"audit: DIRTY {bad}")
            for line in text.splitlines():
                if "first " in line and "mismatch" in line:
                    log("  " + line.strip())
        else:
            log(f"audit: clean ({counts['pt_pages']} PT pages, "
                f"{counts['entries']} entries, "
                f"{counts['anon_virt']} anon_virt)")
            # The shadow counters are the P6 measurement, not a correctness
            # verdict.  seg_diff must be 0; seg_ok and seg_miss are numbers to
            # read, and printing only the verdicts would hide the one thing
            # this gate was extended to produce.
            if counts["seg_ok"] or counts["seg_miss"]:
                log(f"  seg shadow: {counts['seg_ok']} agreed, "
                    f"{counts['seg_miss']} uncovered, "
                    f"{counts['seg_diff']} disagreed "
                    f"({counts['seg_slots']} annotated node entries); "
                    f"dispatch took the page-table name "
                    f"{counts['seg_dispatch']} "
                    f"times and fell back to the mapping list "
                    f"{counts['seg_fallback']}")

    ml = MAPLIST_RE.search(text)
    if not ml:
        # Same reasoning as the missing [MM-ASM] line above: the invariant this
        # check exists for did not run, so it has not been passed.
        ok = False
        log("map list: NO LINE -- the mapping-record list was never walked, so "
            "the single-lookup precondition was not verified.")
    else:
        entries, overlap, dead, passed = (int(g) for g in ml.groups())
        if overlap or dead or passed != entries:
            ok = False
            log(f"map list: BROKEN entries={entries} overlap={overlap} "
                f"dead={dead} ok={passed}")
            if overlap:
                log("  the list is not sorted by start, so mm_seg_find()'s "
                    "binary search can answer with a neighbouring mapping")
            if dead:
                log("  a record lost its magic or has an inverted extent "
                    "(use-after-free)")
            if passed != entries:
                log(f"  {entries - passed} record(s) failed the list "
                    "invariant check")
        else:
            log(f"map list: clean ({entries} mapping records, sorted, live, "
                "no overlap)")

    log("PASS" if ok else "FAIL")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--image")
    ap.add_argument("--kernel")
    ap.add_argument("--log", default="/tmp/mmtest-gate.log")
    ap.add_argument("--timeout", type=int, default=2400)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--world", default=DEFAULT_WORLD)
    ap.add_argument("--size-mb", default=DEFAULT_SIZE_MB)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--guest-cmd", default="mmtest")
    a = ap.parse_args()

    # Invoked bare (no leading slash) on purpose: the distro init strips the
    # first character of the leading "/" on the first stdin line, so
    # "/usr/local/bin/mmtest.sh" arrives as "sr/local/bin/mmtest.sh". The
    # non-slash symlink /usr/bin/mmtest is what makes this work.
    if not a.no_build:
        if not a.kernel:
            a.kernel = build_kernel()
        if not a.image:
            a.image = build_image(a.world, a.size_mb)
    if not a.kernel or not a.image:
        sys.exit("--image/--kernel required with --no-build")

    rc = boot(a.image, a.kernel, a.log, a.timeout, a.cmdline, a.guest_cmd)
    log(f"guest log: {a.log}")
    v = verdict(a.log)
    # A qemu that had to be killed is not itself a model failure; the log
    # verdict is what decides.
    return v


if __name__ == "__main__":
    sys.exit(main())