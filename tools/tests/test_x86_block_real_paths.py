#!/usr/bin/env python3
"""Extract real x86 huge-leaf helper paths and exercise them on the host."""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
KERNEL = ROOT / "kernel"

PRELUDE = r"""
#include <stdio.h>
#include <stdint.h>
#define PAGE_OFFSET 0UL
#include "page_table.h"
#define PAGE_SIZE 4096UL
#define PMD_SIZE (2UL * 1024 * 1024)

static pte_t *test_slot;
static size_t test_leaf_size;
static int flush_count;
static pte_t *pt_lookup_leaf(pt_root_t *pgdir, vaddr_t va, int *level,
                             vaddr_t *base, size_t *size)
{
    (void)pgdir; (void)va;
    if (level) *level = test_leaf_size == PMD_SIZE ? 1 : 0;
    if (base) *base = 0;
    if (size) *size = test_leaf_size;
    return test_slot;
}
static void arch_tlb_flush_page(vaddr_t va)
{
    (void)va;
    flush_count++;
}
"""

CHECKS = r"""
int main(void)
{
    paddr_t pa = (paddr_t)0x40000000UL;
    pte_t block = cow_replacement_leaf(pa, PTE_U | PTE_W | PTE_X, PMD_SIZE);
    if (!(block & PTE_PS) || (block & PTE_NX) || !(block & PTE_W)) {
        fprintf(stderr, "COW PMD replacement lost PS/RWX: %llx\n",
                (unsigned long long)block);
        return 1;
    }
    pte_t base = cow_replacement_leaf(pa, PTE_U | PTE_W | PTE_X, PAGE_SIZE);
    if ((base & PTE_PS) || !(base & PTE_LEAF) || (base & PTE_NX)) {
        fprintf(stderr, "COW base replacement has invalid encoding: %llx\n",
                (unsigned long long)base);
        return 1;
    }

    pte_t slot = arch_pte_block(pa, PTE_U | PTE_W | PTE_X);
    test_slot = &slot;
    test_leaf_size = PMD_SIZE;
    if (mm_mark_leaf_dirty_if_writable((pt_root_t *)1, 0) != 0 ||
        !(slot & PTE_PS) || !(slot & PTE_D) || (slot & PTE_NX) ||
        flush_count != 1) {
        fprintf(stderr, "dirty-mark PMD path lost block flags: %llx\n",
                (unsigned long long)slot);
        return 1;
    }

    slot = arch_pte_leaf(pa, PTE_U | PTE_W | PTE_X);
    test_leaf_size = PAGE_SIZE;
    if (mm_mark_leaf_dirty_if_writable((pt_root_t *)1, PAGE_SIZE) != 0 ||
        (slot & PTE_PS) || !(slot & PTE_D) || (slot & PTE_NX) ||
        flush_count != 2) {
        fprintf(stderr, "dirty-mark base path invalid: %llx\n",
                (unsigned long long)slot);
        return 1;
    }

    puts("extracted COW replacement and dirty paths passed");
    return 0;
}
"""


def extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise SystemExit(f"function not found: {signature}")
    brace = source.find("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise SystemExit(f"unterminated function: {signature}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    cow = extract_function((KERNEL / "mm/fault.c").read_text(),
                           "static pte_t cow_replacement_leaf(")
    dirty = extract_function((KERNEL / "mm/mm.c").read_text(),
                             "int mm_mark_leaf_dirty_if_writable(")
    generated = PRELUDE + cow + "\n" + dirty + "\n" + CHECKS

    with tempfile.TemporaryDirectory(prefix="x86-block-paths-") as temp:
        cfile = pathlib.Path(temp) / "paths.c"
        binary = pathlib.Path(temp) / "paths"
        cfile.write_text(generated)
        subprocess.run([
            args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-DCONFIG_64BIT", "-I", str(KERNEL / "arch/x86_64/include"),
            "-I", str(KERNEL / "include"), str(cfile), "-o", str(binary),
        ], check=True)
        result = subprocess.run([str(binary)], text=True, capture_output=True)
    if result.returncode != 0:
        print(result.stdout, end="")
        print(result.stderr, end="")
        raise SystemExit("extracted real-path fixture failed")
    print(result.stdout, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
