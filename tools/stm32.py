#!/usr/bin/env python3
"""STM32 board actions: config gates, OpenOCD flashing, and the QEMU bring-up.

Programming a microcontroller and launching its simulator are not compilation,
so they live here.  The firmware build stays in make; this module is handed the
paths, the adapter settings and the ELF, all of which make owns.

The OpenOCD command is assembled here rather than copied out of a Makefile
because it is a 22-entry Tcl script, but the *settings* stay in make
(STM32_OPENOCD_INTERFACE, STM32_OPENOCD_TRANSPORT, ...) so a board variant is a
variable, not a code edit.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# The Tcl register-reset sequence that hands control from the bootloader to the
# image just flashed.  Order matters and the two $variables are read back from
# the vector table, so this is a fixed script, not a template.
POST_FLASH_TCL: tuple[str, ...] = (
    "init",
    "mww 0xE000EDF0 0xA05F0003",
    "sleep 50",
    "flash probe 0",
    "flash write_image erase {elf}",
    "verify_image {elf}",
    "set boot_sp [mrw 0x08000000]",
    "set boot_pc [mrw 0x08000004]",
    "reg msp $boot_sp",
    "reg psp 0",
    "reg control 0",
    "reg primask 0",
    "reg basepri 0",
    "reg faultmask 0",
    "reg pc $boot_pc",
    "resume",
    "shutdown",
)

# Assertions that the STM32 port has not grown hardcoded MMIO addresses or
# leaked the ARMv7-M config macro outside the architecture and board code.  Each
# is a regex over the tree; the gate passes when the search finds nothing.  The
# scope strings are joined onto KERNEL_DIR, which make owns.
CONFIG_ASSERTIONS: tuple[tuple[str, str, tuple[str, ...]], ...] = (
    ("hardcoded STM32 MMIO address",
     r"0x400[0-9A-Fa-f]{5}|0xE000E[0-9A-Fa-f]{3}",
     ("platform/stm32f103", "--glob", "*.[ch]",
      "--glob", "!board.c", "--glob", "!board_config.h")),
    ("CONFIG_ARMV7M outside arch/board code",
     r"CONFIG_ARMV7M",
     ("", "--glob", "!kernel/arch/**", "--glob", "!kernel/platform/**",
      "--glob", "!kernel/external/**",
      "--glob", "!kernel/include/core/arch.h")),
)


def openocd_argv(a) -> list[str]:
    argv = ["openocd", "-f", a.interface]
    # The adapter-serial option is only meaningful with a CMSIS-DAP that has a
    # serial; emitting it empty makes OpenOCD fail to find any adapter.
    if a.serial:
        argv += ["-c", f"adapter serial {a.serial}"]
    argv += ["-c", f"transport select {a.transport}",
             "-c", f"adapter speed {a.adapter_khz}",
             "-f", "target/stm32f1x.cfg"]
    argv += [arg for step in POST_FLASH_TCL
             for arg in ("-c", step.replace("{elf}", a.elf))]
    return argv


def do_flash(a) -> int:
    if shutil.which("openocd") is None:
        print("openocd not found; install OpenOCD or use STM32CubeProgrammer",
              file=sys.stderr)
        return 1
    elf = Path(a.elf)
    if not a.elf or not elf.is_file():
        print(f"error: firmware not found: {a.elf or '(empty STM32_XUANWU_ELF)'}",
              file=sys.stderr)
        return 1
    argv = openocd_argv(a)
    print("+ " + " ".join(argv), flush=True)
    return subprocess.run(argv, cwd=REPO, check=False).returncode


def check_config(a) -> int:
    """Run the source-level assertions, then let make do the real build."""
    for label, pattern, scope in CONFIG_ASSERTIONS:
        # The original searched $(KERNEL_DIR)/<scope>; an empty scope means the
        # tree root itself, which is how the second assertion searches all of
        # KERNEL_DIR while its --glob exclusions still name kernel/... paths.
        path = "/".join(p for p in (a.kernel_dir, *scope[:1]) if p)
        argv = ["rg", "-n", pattern, path, *scope[1:]]
        r = subprocess.run(argv, cwd=REPO, check=False,
                           capture_output=True, text=True)
        if r.returncode not in (0, 1):
            print(f"check-stm32f103: rg failed for {label}: {r.stderr.strip()}",
                  file=sys.stderr)
            return r.returncode
        if r.stdout.strip():
            print(f"check-stm32f103: {label} found:\n{r.stdout}", file=sys.stderr)
            return 1
    b = subprocess.run(["make", f"ARCH={a.arch}", f"BOARD={a.board}",
                        f"ABI={a.abi}", f"BRINGUP={a.bringup}",
                        a.build_target], cwd=REPO, check=False)
    if b.returncode != 0:
        return b.returncode
    print("check-stm32f103: PASS")
    return 0


def qemu_run(a) -> int:
    argv = ["qemu-system-arm", "-machine", "stm32vldiscovery", "-nographic",
            "-kernel", a.kernel_bin]
    print("+ " + " ".join(argv), flush=True)
    return subprocess.run(argv, cwd=REPO, check=False).returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    f = sub.add_parser("openocd-flash")
    f.add_argument("--interface", required=True)
    f.add_argument("--serial", default="")
    f.add_argument("--transport", required=True)
    f.add_argument("--adapter-khz", required=True)
    f.add_argument("--elf", required=True)
    f.add_argument("--print-argv", action="store_true")

    c = sub.add_parser("check-config")
    c.add_argument("--kernel-dir", required=True)
    c.add_argument("--arch", required=True)
    c.add_argument("--board", required=True)
    c.add_argument("--abi", required=True)
    c.add_argument("--bringup", default="0")
    c.add_argument("--build-target", required=True)

    q = sub.add_parser("qemu-run")
    q.add_argument("--kernel-bin", required=True)

    a = ap.parse_args()
    if a.cmd == "openocd-flash":
        if a.print_argv:
            print(" ".join(openocd_argv(a)))
            return 0
        return do_flash(a)
    if a.cmd == "check-config":
        return check_config(a)
    return qemu_run(a)


if __name__ == "__main__":
    raise SystemExit(main())
