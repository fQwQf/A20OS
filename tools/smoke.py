#!/usr/bin/env python3
"""Run a smoke/gate case described in tools/smoke_cases.py.

The case table used to live as ~1900 lines of shell embedded in the .mk files.
That shell was unreviewable, unlintable, and its failure modes were invisible:
a target that printed nothing looked exactly like a target that passed.  Each
case is now data here, and this module is the single interpreter for it.

Order of operations per case mirrors what the make recipe did:
  1. resource gate  (tools/a20_resource.py) -- wait rather than risk the OOM killer
  2. pre-build commands (some gates delete a driver so it rebuilds with a
     smoke-test define)
  3. recursive build
  4. QEMU under tools/run_with_timeout.py, log to file
  5. post commands (optional; e.g. tools/check_wav_pcm.py re-asserts the PCM
     content of an audio run's wav output -- a check the host can make but the
     guest log cannot)
  6. every pass pattern must appear in the log, else dump the tail and fail

`--print-argv` exists so the invocation can be diffed against make; it is what
makes a future edit to this file falsifiable.
"""

from __future__ import annotations

import argparse
import os
import shlex
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
devnull = subprocess.DEVNULL
sys.path.insert(0, str(Path(__file__).resolve().parent))

from smoke_cases import CASES  # noqa: E402

TIMEOUT_HELPER = "tools/run_with_timeout.py"
GATE = "tools/a20_resource.py"
# CI must fail rather than hang when the host is genuinely out of room; an
# interactive `a20 run` waits forever, so the ceiling is here instead.
GATE_WAIT_S = os.environ.get("A20_WAIT_TIMEOUT", "900")

STEP35_APPEND = ("a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 "
                 "a20.dns=10.0.2.3 a20.hostname=a20os")
STEP35_MARKERS = [
    "SCHED_STRESS: PASS", "FUTEX_STRESS: PASS",
    "FUTEX_STRESS: unrelated-wake-isolation PASS",
    "FUTEX_STRESS: stale-timeout-isolation PASS",
    "PROC_STRESS: PASS", "PROC_STRESS: vfork-auto-reap PASS",
    "PROC_STRESS: signal-stop-exit PASS", "PROC_STRESS: signal-mask-park PASS",
    "IO_EVENT_TEST: PASS", "VFS_STRESS: PASS", "SOCKET_STRESS: PASS",
    "LIFETIME_STRESS: PASS", "lifetime_errors: 0",
    "System is going down for power-off NOW.",
]
STEP35_TIMEOUT_CAPACITY = [
    "LIFETIME_STRESS: timeout-capacity-1 PASS",
    "LIFETIME_STRESS: timeout-capacity PASS entries=",
    "LIFETIME_STRESS: timeout-capacity+1 PASS",
    "LIFETIME_STRESS: timeout-capacity PASS capacity=",
]
STEP35_SMP_RUNQUEUE = ["SCHED_STRESS: smp-runqueue PASS",
                       "scheduler_violations: 0"]
STEP35_LOCK_SPLIT = [
    "SCHED_STRESS: lock-split PASS", "runqueue_local_picks:",
    "runqueue_lock_acquires:", "runqueue_parallel_pick_peak:",
    "scheduler_violations: 0",
]
STEP35_FORBID = r"PANIC|sched invariant|reference underflow|use-after-free|\[LOCK\]"
# Positional args main() handles itself instead of looking up in CASES;
# smoke_audit.py reads this so a wired subcommand is not read as a typo'd case.
SUBCOMMANDS = {"step35", "arch-mmu-matrix", "devtools", "mesa-attach"}
# arch -> (board, qemu binary, base qemu flags, needs -embedded build dir)
MATRIX_ARCHS: dict[str, tuple[str, str, list[str], bool]] = {
    "arm32": ("qemu-virt-arm32", "qemu-system-arm",
              ["-machine", "virt", "-cpu", "cortex-a15"], True),
    "aarch64": ("qemu-virt-aarch64", "qemu-system-aarch64",
                ["-machine", "virt", "-cpu", "cortex-a57",
                 "-global", "virtio-mmio.force-legacy=false"], False),
    "riscv64": ("qemu-virt-riscv64", "qemu-system-riscv64",
                ["-machine", "virt", "-bios", "default",
                 "-global", "virtio-mmio.force-legacy=false"], False),
    "riscv32": ("qemu-virt-riscv32", "qemu-system-riscv32",
                ["-machine", "virt", "-bios", "default",
                 "-global", "virtio-mmio.force-legacy=false"], True),
}
# Markers were matched with bare `grep -q` (POSIX BRE, where `+ ? ( ) | { }`
# are literals) but Python re follows ERE, so `timeout-capacity+1` -- a literal
# the log prints -- would stop matching and turn a green gate red.  Escape only
# the divergent set; `.` `*` `[` `^` `$` agree in both dialects.
_BRE_LITERAL = re.compile(r"([+?()|{}])")


def bre(marker: str) -> str:
    return _BRE_LITERAL.sub(r"\\\1", marker)


def sh(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=REPO, check=False, **kw)


def run_gate(case: dict) -> None:
    g = case.get("gate")
    if not g:
        return
    env = dict(os.environ, A20_WAIT_TIMEOUT=GATE_WAIT_S)
    r = sh([sys.executable, GATE, "-m", g["mem"], "-c", str(g["cpus"])], env=env)
    if r.returncode != 0:
        raise SystemExit(r.returncode)


def run_build(case: dict) -> None:
    for cmd in case.get("pre") or []:
        if sh(["bash", "-c", cmd]).returncode != 0:
            raise SystemExit(f"pre-build command failed: {cmd}")
    b = case.get("build")
    if not b:
        return
    sink = devnull if case.get("quiet") else None
    r = sh(["make", *b["vars"], b["target"]], stdout=sink, stderr=sink)
    if r.returncode != 0:
        raise SystemExit(f"build failed: make {' '.join(b['vars'])} {b['target']}")


def qemu_argv(case: dict) -> list[str]:
    """The full QEMU command line, as the make recipe invoked it."""
    argv = [sys.executable, TIMEOUT_HELPER]
    stdin = case.get("stdin")
    if stdin and stdin["kind"] == "sendline":
        if stdin.get("expect"):
            argv += ["--expect", stdin["expect"]]
        for ln in stdin["lines"]:
            argv += ["--send-line", ln]
    if stdin and stdin["kind"] == "sendline_seq":
        # Each line waits for its own marker, so a guest that has to consume
        # one input before it will accept the next does not lose the tail.
        for marker, line in stdin["steps"]:
            argv += ["--expect-line", marker, line]
    argv.append(case["timeout"])
    argv += case["argv"]
    return argv


def split_command(cmd: str) -> list[str]:
    """Split a make variable into argv.

    The Makefile builds QEMU as `env LD_LIBRARY_PATH=... qemu-system-<arch>`
    whenever a locally built virglrenderer is present (Makefile QEMU_VIRGL_LIB),
    so --qemu is a command line, not a program name. Handing that to Popen as one
    argv element makes it look for a binary literally named
    "env LD_LIBRARY_PATH=... qemu-system-x86_64" and fail with FileNotFoundError
    that names the whole string.

    shlex rather than str.split: the paths in it may contain spaces, and the
    quotes have to survive. Harmless when QEMU is a bare program name.
    """
    return shlex.split(cmd)


def run_qemu(case: dict) -> int:
    log = Path(case["log"])
    log.parent.mkdir(parents=True, exist_ok=True)
    argv = qemu_argv(case)
    stdin = case.get("stdin")
    with log.open("wb") as fh:
        if stdin and stdin["kind"] == "pipe":
            payload = "".join(f"{ln}\n" for ln in stdin["lines"]).encode()
            proc = subprocess.Popen(argv, cwd=REPO, stdin=subprocess.PIPE,
                                    stdout=fh, stderr=subprocess.STDOUT)
            time.sleep(stdin["delay"])
            try:
                proc.stdin.write(payload)
                proc.stdin.flush()
                proc.stdin.close()
            except (BrokenPipeError, ValueError):
                pass
            return proc.wait()
        return sh(argv, stdout=fh, stderr=subprocess.STDOUT).returncode


def run_post_build(case: dict) -> int:
    """Post-build, pre-QEMU commands (e.g. injecting mlibc binaries into the
    FAT32 image the build step just composed)."""
    for cmd in case.get("post-build") or []:
        if sh(["bash", "-c", cmd]).returncode != 0:
            print(f"post-build command failed: {cmd}")
            return 1
    return 0


def run_post(case: dict) -> int:
    """Post-run host-side checks; a nonzero exit fails the case outright."""
    for cmd in case.get("post") or []:
        if sh(["bash", "-c", cmd]).returncode != 0:
            print(f"post-run check failed: {cmd}")
            return 1
    return 0


def grep_matches(pattern: str, text: str) -> bool:
    """`grep -q PATTERN` semantics, not substring containment.

    The recipes used `grep -q`, so these are regular expressions: 57 of the 146
    patterns rely on that (escaped brackets such as \\[HDA\\], wildcards like
    FAKELD:.*ok, and line anchors like ^valid_pages:).  A substring test fails
    every one of them.  MULTILINE reproduces grep's per-line matching so ^ and
    $ anchor to line boundaries.
    """
    return re.search(pattern, text, re.MULTILINE) is not None


def report(name: str, case: dict, status: int) -> int:
    log = Path(case["log"])
    text = log.read_text(encoding="utf-8", errors="replace")
    missing = [p for p in case["expect"] if not grep_matches(p, text)]
    violated = [p for p in case.get("forbid") or [] if grep_matches(p, text)]
    if not missing and not violated:
        # pass_msg was captured from the make recipe, where $log was a shell
        # variable; render it with this case's real path.
        print(case["pass_msg"].replace("$log", str(log)))
        return 0
    # A forbidden pattern is a stronger signal than a missing one: the gate ran
    # and reported the thing it exists to catch.  Say so, and show the evidence.
    if violated:
        print(f"{name}: FAIL forbidden pattern present: {violated!r}; "
              f"matching lines from {log}:")
        for pat in violated:
            shown = [ln for ln in text.splitlines() if grep_matches(pat, ln)][:5]
            for ln in shown:
                print(f"  {ln}")
        return 1
    reason = "timeout without PASS" if (case.get("timeout_msg") and status == 124) \
        else f"failed with status {status}"
    print(f"{name}: {reason}; missing {missing!r}; tail of {log}:")
    print("\n".join(text.splitlines()[-80:]))
    return 1


def step35_fail(head: str, log: Path, evidence: list[str] | None = None) -> int:
    print(f"{head} log={log}")
    body = evidence if evidence is not None else log.read_text(
        encoding="utf-8", errors="replace").splitlines()[-120:]
    print("\n".join(body))
    return 1


def step35_check(case: dict, status: int) -> int:
    log = Path(case["log"])
    text = log.read_text(encoding="utf-8", errors="replace")
    if status != 0:
        return step35_fail(f"_step35_smoke: QEMU failed status={status}", log)
    groups = [STEP35_MARKERS]
    if case["require_timeout_capacity"]:
        groups.append(STEP35_TIMEOUT_CAPACITY)
    if case["require_smp_runqueue"]:
        groups.append(STEP35_SMP_RUNQUEUE)
    if case["require_lock_split"]:
        groups.append(STEP35_LOCK_SPLIT)
    for group in groups:
        for marker in group:
            if not grep_matches(bre(marker), text):
                return step35_fail(f"_step35_smoke: missing '{marker}'", log)
    n = case["nr_cpus"]
    if n > 1 and f"[SMP] {n}/{n} configured CPUs online" not in text:
        return step35_fail(f"_step35_smoke: not all {n} CPUs came online", log)
    if grep_matches(STEP35_FORBID, text):
        hit = [ln for ln in text.splitlines()
               if grep_matches(STEP35_FORBID, ln)][:20]
        return step35_fail("_step35_smoke: lifecycle diagnostic failure", log, hit)
    print(f"_step35_smoke: PASS ({case['label']}); log saved to {log}")
    return 0


def step35_case(a: argparse.Namespace) -> dict:
    return {
        "log": str(Path(a.log_dir) / f"step35-{a.label}-"
                  f"{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}.log"),
        "stdin": {"kind": "sendline", "expect": "# ",
                  "lines": ["lifetime_stress", "cat /proc/a20/task_lifetime",
                            "poweroff"]},
        "timeout": a.timeout,
        "argv": [a.qemu, *a.qemu_flag, "-kernel", a.kernel,
                 "-append", STEP35_APPEND],
        "label": a.label, "nr_cpus": a.nr_cpus,
        "require_timeout_capacity": a.require_timeout_capacity,
        "require_smp_runqueue": a.require_smp_runqueue,
        "require_lock_split": a.require_lock_split,
    }


def step35_main(a: argparse.Namespace) -> int:
    case = step35_case(a)
    Path(a.log_dir).mkdir(parents=True, exist_ok=True)
    if a.print_argv:
        print(" ".join(qemu_argv(case)))
        return 0
    return step35_check(case, run_qemu(case))


def matrix_case(a: argparse.Namespace, arch: str, nommu: int) -> dict:
    board, qemu, base, embedded = MATRIX_ARCHS[arch]
    variant = f"{arch}-nommu" if nommu else arch
    build = f".kernel-build/{arch}-{board}-both-dev"
    if embedded:
        build += "-embedded"
    if nommu:
        build += "-nommu"
    return {
        "log": f".kernel-build/smoke/{variant}-shell.log",
        "stdin": {"kind": "pipe", "delay": a.input_delay,
                  "lines": [f"echo A20_MATRIX_{variant}_OK",
                            "/bin/echo A20_EXTERNAL_OK", "poweroff"]},
        "timeout": a.timeout,
        "argv": [qemu, *base, "-m", "1G", "-nographic", "-smp", "1",
                 "-drive", f"file={build}/fat32.img,if=none,format=raw,id=x0",
                 "-device", "virtio-blk-device,bus=virtio-mmio-bus.0,drive=x0",
                 "-netdev", "user,id=net",
                 "-device", "virtio-net-device,bus=virtio-mmio-bus.4,netdev=net",
                 "-kernel", f"{build}/kernel.elf"],
    }


def matrix_main(a: argparse.Namespace) -> int:
    """One QEMU boot per arch, twice: with MMU and build-only without it.

    NOMMU variants stop after the build -- their runtime is platform-specific,
    so the original never launched QEMU for them.
    """
    Path(".kernel-build/smoke").mkdir(parents=True, exist_ok=True)
    for arch in MATRIX_ARCHS:
        for nommu in (0, 1):
            variant = f"{arch}-nommu" if nommu else arch
            print(f"=== smoke-arch-mmu-matrix: {variant} ===")
            run_build({"build": {"vars": [f"ARCH={arch}", "ABI=both",
                                          "BRINGUP=0", f"NOMMU={nommu}"],
                                 "target": "dev-build"}, "quiet": True})
            if nommu:
                print(f"smoke-arch-mmu-matrix: {variant} build-only PASS "
                      "(NOMMU runtime is platform-specific)")
                continue
            case = matrix_case(a, arch, nommu)
            status = run_qemu(case)
            log = Path(case["log"])
            text = log.read_text(encoding="utf-8", errors="replace")
            want = [f"A20_MATRIX_{variant}_OK", "A20_EXTERNAL_OK",
                    "System is going down for power-off NOW"]
            if any(not grep_matches(bre(m), text) for m in want):
                print(f"smoke-arch-mmu-matrix: {variant} FAIL "
                      f"(status={status or 0})")
                print("\n".join(text.splitlines()[-100:]))
                return 1
            print(f"smoke-arch-mmu-matrix: {variant} PASS")
    return 0


def devtools_main(a: argparse.Namespace) -> int:
    """Boot the devtools rootfs and look for its single PASS marker.

    The verdict is the marker's presence alone: the original branched on
    `if grep -q ...` and never consulted QEMU's exit status, so a run that still
    printed DEVTOOLS_SMOKE: PASS counted as a pass however QEMU ended.  The
    status is reported only on failure.
    """
    log = Path(a.log_dir) / f"devtools-{a.label}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    case = {
        "log": str(log),
        "stdin": {"kind": "pipe", "delay": a.input_delay,
                  "lines": ["chroot /extra /bin/sh /devtools-smoke.sh", "poweroff"]},
        "timeout": a.timeout,
        "argv": [*split_command(a.qemu), *a.qemu_flag,
                 "-drive", f"file={Path(a.img).resolve()},if=none,"
                           f"format=raw,id=xdevtools",
                 "-device", f"{a.blk_second},drive=xdevtools",
                 "-kernel", a.kernel],
    }
    status = run_qemu(case)
    text = log.read_text(encoding="utf-8", errors="replace")
    if grep_matches(bre("DEVTOOLS_SMOKE: PASS"), text):
        print(f"smoke-devtools: PASS; log saved to {log}")
        return 0
    print(f"smoke-devtools: failed (status {status}); tail of {log}:")
    print("\n".join(text.splitlines()[-80:]))
    return 1


def mesa_attach_main(a: argparse.Namespace) -> int:
    """Boot a Mesa-only rootfs and record what stock Mesa does with our DRM node.

    Two verdicts, kept separate because they have different owners.

    Our side -- that QEMU really attached a virtio-gpu device and the kernel
    brought it up with virgl -- is asserted. It is fully under our control, and
    until it is asserted the rest of this gate is vacuous: with DISPLAY_MODE left
    at text the guest boots with no GPU at all, every VIRTGPU ioctl answers
    -ENODEV, and "Mesa did not attach" looks exactly like a Mesa bug.

    Mesa's side -- whether libEGL/libgbm bind virtio_gpu_dri.so -- is asserted
    when --require-virgl-attach is given, which the make target passes by
    default. Stock Mesa does reach virgl: the renderer string names it. An
    earlier revision of this gate excused a failure as "Alpine's Mesa ships no
    virgl backend", on the evidence that libgallium imports no virgl_renderer_*
    symbols. That evidence was worthless -- a dlopen'd renderer leaves no import
    -- and the real cause was ours: VIRTGPU_GETPARAM's value field is a user
    pointer, not an in/out scalar, so Mesa read every param back as 0 and
    abandoned the device after a full round of successful ioctls.

    The marker required is the probe's own "end", which it only prints after both
    eglinfo runs, so a probe that dies early still fails here.
    """
    log = Path(a.log_dir) / f"mesa-attach-{a.label}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    case = {
        "log": str(log),
        "stdin": {"kind": "pipe", "delay": a.input_delay,
                  "lines": ["chroot /extra /bin/sh /mesa-probe.sh", "poweroff"]},
        "timeout": a.timeout,
        "argv": [*split_command(a.qemu), *a.qemu_flag,
                 "-drive", f"file={Path(a.img).resolve()},if=none,"
                           f"format=raw,id=xmesa",
                 "-device", f"{a.blk_second},drive=xmesa",
                 "-kernel", a.kernel],
    }
    status = run_qemu(case)
    text = log.read_text(encoding="utf-8", errors="replace")

    print(f"smoke-mesa-attach: transcript of Mesa's behaviour "
          f"(log saved to {log})")
    for line in text.splitlines():
        if "MESA_PROBE:" in line:
            print("  " + line.split("MESA_PROBE:", 1)[1].rstrip())

    if not grep_matches(bre("MESA_PROBE: end"), text):
        print("smoke-mesa-attach: FAIL the probe did not run to completion "
              f"(status {status})")
        return 1

    if not grep_matches(bre("virtio-gpu 3D (virgl)"), text):
        print("smoke-mesa-attach: FAIL the guest never brought up a virgl "
              "virtio-gpu device, so nothing below says anything about Mesa. "
              "Check that GPU_3D=1 and DISPLAY_MODE=gui reach this target.")
        return 1

    # eglinfo labels the line per profile ("OpenGL core profile renderer:",
    # "OpenGL ES profile renderer:"), so anchor on "renderer:" and not on the
    # single spelling "OpenGL renderer string:" -- that narrower pattern misses
    # every line eglinfo actually prints.
    attached = grep_matches(bre(r"profile renderer: *virgl"), text)
    if attached:
        print("smoke-mesa-attach: virtio_gpu_dri attached: yes")
        print("smoke-mesa-attach: PASS")
        return 0

    print("smoke-mesa-attach: virtio_gpu_dri attached: NO")
    print("smoke-mesa-attach: kernel side is healthy (virgl device up, capset "
          "negotiated); Mesa fell back to software rendering.")
    if a.require_virgl_attach:
        print("smoke-mesa-attach: FAIL Mesa did not bind virgl. Check the two "
              "gates that silently look healthy: VIRTGPU_GETPARAM must write "
              "through the value pointer (not store a scalar in the struct), "
              "and drm_version.version_major must be 0, or Mesa's "
              "virgl_drm_get_version() returns -EINVAL before it ever creates "
              "a context.")
        return 1
    print("smoke-mesa-attach: PASS (Mesa attach not enforced; see above)")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("case")
    ap.add_argument("--print-argv", action="store_true",
                    help="print the QEMU argv and exit (for diffing against make)")
    ap.add_argument("--list", action="store_true", help="list case names and exit")
    ap.add_argument("--label")
    ap.add_argument("--log-dir")
    ap.add_argument("--qemu")
    ap.add_argument("--qemu-flag", action="append", default=[])
    ap.add_argument("--kernel")
    ap.add_argument("--timeout")
    ap.add_argument("--nr-cpus", type=int)
    ap.add_argument("--input-delay", type=float, default=8.0)
    ap.add_argument("--img")
    ap.add_argument("--blk-second", dest="blk_second")
    ap.add_argument("--require-timeout-capacity", type=int, default=0)
    ap.add_argument("--require-smp-runqueue", type=int, default=0)
    ap.add_argument("--require-lock-split", type=int, default=0)
    ap.add_argument("--require-virgl-attach", action="store_true",
                    help="mesa-attach: fail unless Mesa binds virtio_gpu_dri")
    a = ap.parse_args(argv)
    if a.case == "arch-mmu-matrix":
        return matrix_main(a)
    if a.case == "devtools":
        return devtools_main(a)
    if a.case == "mesa-attach":
        return mesa_attach_main(a)
    if a.case in SUBCOMMANDS:
        missing = [n for n in ("label", "log_dir", "qemu", "kernel", "timeout")
                   if getattr(a, n) is None]
        if missing or a.nr_cpus is None:
            ap.error("step35 requires --label --log-dir --qemu --kernel "
                     "--timeout --nr-cpus")
        return step35_main(a)
    if a.list:
        print("\n".join(sorted(CASES)))
        return 0
    if a.case not in CASES:
        print(f"unknown case {a.case!r}; try --list", file=sys.stderr)
        return 2
    case = CASES[a.case]
    if a.print_argv:
        print(" ".join(qemu_argv(case)))
        return 0
    run_gate(case)
    run_build(case)
    if run_post_build(case):
        return 1
    status = run_qemu(case)
    if run_post(case):
        return 1
    return report(a.case, case, status)


if __name__ == "__main__":
    raise SystemExit(main())
