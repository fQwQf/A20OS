#!/usr/bin/env python3
"""Check production x86 block-PTE encoding and generic huge-leaf call paths.

The negative control reconstructs the prior generic leaf encoder.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = pathlib.Path(__file__).resolve().parents[2]
KERNEL = ROOT / "kernel"
C_SOURCE = HERE / "x86_pte_block_encoder_host.c"


def compile_and_run(cc: str, header_dir: pathlib.Path, old: bool) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="x86-pte-block-") as temp:
        binary = pathlib.Path(temp) / "x86-pte-block-host"
        command = [
            cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-DCONFIG_64BIT", "-DPAGE_OFFSET=0xffff800000000000UL",
            "-I", str(header_dir), "-I", str(KERNEL / "include"),
            str(C_SOURCE), "-o", str(binary),
        ]
        if old:
            command.insert(1, "-DOLD_NEGATIVE_CONTROL")
        subprocess.run(command, check=True)
        return subprocess.run([str(binary)], text=True, capture_output=True)


def require_source_path(source: str, needles: tuple[str, ...], name: str) -> None:
    missing = [needle for needle in needles if needle not in source]
    if missing:
        raise SystemExit(f"{name}: expected real source path missing {missing!r}")


def audit_real_generic_paths() -> None:
    mm = (KERNEL / "mm/mm.c").read_text()
    fault = (KERNEL / "mm/fault.c").read_text()
    cow = (KERNEL / "mm/cow.c").read_text()
    mprotect = (KERNEL / "mm/mprotect.c").read_text()
    mprotect_txn = (KERNEL / "mm/mprotect_txn.rs").read_text()

    require_source_path(mm, (
        "*pte = arch_pte_block(arch_pte_addr(*pte), flags);",
        "*pte = arch_pte_block(pa, flags);",
        "? arch_pte_block(pfn_to_phys(nf), arch_pte_flags(pte))",
    ), "dirty marking, huge install, clone/COW reuse")
    require_source_path(fault, (
        "if (leaf_size >= PMD_SIZE)",
        "return arch_pte_block(pa, flags);",
    ), "fault COW replacement")
    require_source_path(cow, (
        "(leaf_size >= PMD_SIZE)",
        "? arch_pte_block((paddr_t)pa, (pte_t)flags)",
    ), "fork parent downgrade")
    # The protection transaction demotes huge leaves in its pre-pass, then
    # installs a base leaf. Keep the demotion and PS=0 policy explicit.
    require_source_path(mprotect_txn, (
        "// Finish every fallible demotion before changing any access bit.",
        "mm_demote_huge_page(mm, va)",
    ), "mprotect huge-leaf demotion pre-pass")
    require_source_path(mprotect, (
        "pte_t replacement = arch_pte_leaf((paddr_t)pa, (pte_t)flags);",
    ), "mprotect's demoted base-leaf install path")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()

    audit_real_generic_paths()
    candidate_header = KERNEL / "arch/x86_64/include"
    candidate = compile_and_run(args.cc, candidate_header, old=False)
    if candidate.returncode != 0:
        print(candidate.stdout, end="")
        print(candidate.stderr, end="")
        raise SystemExit("candidate header host checks failed")
    print(candidate.stdout, end="")

    # This negative control reconstructs the old generic fallback by replacing
    # arch_pte_block with arch_pte_leaf in the real production header.
    old = compile_and_run(args.cc, candidate_header, old=True)
    if old.returncode == 0 or "hardware PS" not in old.stderr:
        print(old.stdout, end="")
        print(old.stderr, end="")
        raise SystemExit("old-header negative control did not detect missing PS")
    print("old-header negative control detected missing hardware PS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
