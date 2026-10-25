#!/usr/bin/env python3
"""Check the documented Linux/RISC-V64 quickstart before a full build.

No packages are installed and no repository files are changed. Compiler probes
use temporary files; this checks prerequisites, not kernel correctness.
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = {
    "git": "git",
    "make": "build-essential",
    "cc": "build-essential",
    "riscv64-unknown-elf-gcc": "gcc-riscv64-unknown-elf",
    "riscv64-unknown-elf-objcopy": "binutils-riscv64-unknown-elf",
    "riscv64-linux-gnu-gcc": "gcc-riscv64-linux-gnu libc6-dev-riscv64-cross",
    "qemu-system-riscv64": "qemu-system-misc (Ubuntu) / qemu-system-riscv (Debian 13)",
    "mkfs.fat": "dosfstools",
    "mkfs.ext4": "e2fsprogs",
    "e2fsck": "e2fsprogs",
    "mcopy": "mtools",
    "mmd": "mtools",
    "dd": "coreutils",
    "flock": "util-linux",
    "rustc": "rustup; see docs/dev/build.md",
}
TARGETS = ("riscv64imac-unknown-none-elf", "riscv64gc-unknown-none-elf")


def find_tool(name):
    found = shutil.which(name)
    if found:
        return found
    # Match the image tools' fallback in Makefile on Debian login shells.
    if name in ("mkfs.fat", "mkfs.ext4", "e2fsck"):
        return shutil.which(name, path="/usr/sbin:/sbin")
    return None


def probe(label, argv, hint, expected_prefix=None):
    try:
        result = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL {label}: {exc}\n     {hint}")
        return False
    if result.returncode:
        print(f"FAIL {label}:\n{(result.stderr or result.stdout).strip()}\n     {hint}")
        return False
    if expected_prefix and not result.stdout.startswith(expected_prefix):
        print(f"FAIL {label}: {result.stdout.strip()}\n     {hint}")
        return False
    print(f"OK   {label}")
    return True


def main():
    if sys.version_info < (3, 11):
        print("FAIL Python 3.11+ required; use Debian 13 / Ubuntu 24.04 or newer Python")
        return 1
    import tomllib

    print("Checking Linux/RISC-V64 text-mode quickstart prerequisites")
    ok = sys.platform.startswith("linux")
    if not ok:
        print("FAIL this quickstart targets Linux; use a Linux VM or the documented container")
    available = {}
    for name, package in TOOLS.items():
        available[name] = find_tool(name)
        if available[name]:
            print(f"OK   {name}: {available[name]}")
        else:
            print(f"FAIL {name} missing; install {package}")
            ok = False

    if not (ROOT / "user/external/apps/fastfetch/src/fastfetch.c").is_file():
        print("FAIL fastfetch submodule missing; run git submodule update --init --recursive")
        ok = False

    with tempfile.TemporaryDirectory(prefix="a20-quickstart-") as tmp:
        tmp = Path(tmp)
        c = tmp / "probe.c"
        c.write_text("int probe(void) { return 0; }\n")
        cc = available["riscv64-unknown-elf-gcc"]
        if cc:
            ok = probe("RISC-V freestanding compiler", [cc, "-march=rv64imafdc_zicsr_zifencei",
                       "-mabi=lp64", "-ffreestanding", "-c", str(c), "-o", str(tmp / "probe.o")],
                       "Install gcc-riscv64-unknown-elf and its binutils") and ok
        cc = available["riscv64-linux-gnu-gcc"]
        if cc:
            c.write_text("#include <limits.h>\n#include <stdint.h>\n#include <linux/fb.h>\n")
            ok = probe("RISC-V libc and Linux headers", [cc, "-E", str(c), "-o", str(tmp / "probe.i")],
                       "Install libc6-dev-riscv64-cross, including linux-libc-dev-riscv64-cross") and ok
        if available["qemu-system-riscv64"]:
            ok = probe("QEMU executable", [available["qemu-system-riscv64"], "--version"],
                       "Install the RISC-V system emulator") and ok
        if available["rustc"]:
            version = tomllib.loads((ROOT / "rust-toolchain.toml").read_text())["toolchain"]["channel"]
            ok = probe("Rust version", [available["rustc"], "--version"],
                       "Use rustup's rustc on PATH and remove conflicting RUSTUP_TOOLCHAIN / directory overrides",
                       expected_prefix=f"rustc {version} ") and ok
            rs = tmp / "probe.rs"
            rs.write_text("#![no_std]\npub fn probe() -> u32 { 0 }\n")
            for target in TARGETS:
                ok = probe(f"Rust core for {target}", [available["rustc"], "--crate-type=lib",
                           "--emit=metadata", "--target", target, str(rs), "-o", str(tmp / f"{target}.rmeta")],
                           f"rustup target add --toolchain {version} {target}") and ok

    print("quickstart prerequisites: " + ("PASS" if ok else "FAIL; see docs/dev/build.md"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
