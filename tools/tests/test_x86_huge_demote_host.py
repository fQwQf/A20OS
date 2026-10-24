#!/usr/bin/env python3
"""Extract and host-test the real mm_demote_huge_page() implementation."""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = pathlib.Path(__file__).resolve().parents[2]
KERNEL = ROOT / "kernel"
VM_SOURCE = KERNEL / "mm/vm.c"

STUBS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#define PAGE_OFFSET 0UL
#include "page_table.h"
#define PAGE_SIZE 4096UL
#define PMD_SIZE (2UL * 1024 * 1024)
#define PMD_PAGE_COUNT (PMD_SIZE / PAGE_SIZE)
#define PFN_NONE ((pfn_t)-1)
#define MM_FAIL_STAGE_HUGE_DEMOTE 1

typedef uint64_t pfn_t;
typedef struct mm_struct { pt_root_t *pgdir; } mm_struct_t;

static unsigned char *old_block;
static unsigned char *new_pages[PMD_PAGE_COUNT + 1];
static uint64_t mapped_ptes[PMD_PAGE_COUNT];
static pte_t mapped_flags[PMD_PAGE_COUNT];
static pte_t old_pte;
static size_t map_calls;
static size_t alloc_calls;
static size_t old_frame_puts;
static const pfn_t old_pfn = (pfn_t)0x12345678;

static pte_t *pt_lookup_leaf(pt_root_t *pgdir, vaddr_t addr, int *level,
                             vaddr_t *base, size_t *size)
{
    (void)pgdir;
    (void)addr;
    if (level) *level = 1;
    if (base) *base = 0;
    if (size) *size = PMD_SIZE;
    return &old_pte;
}
static pfn_t phys_to_pfn(paddr_t pa)
{
    (void)pa;
    return old_pfn;
}
static int pfn_valid(pfn_t pfn) { return pfn == old_pfn; }
static int mm_tlb_hold_frame(mm_struct_t *mm, pfn_t pfn)
{
    (void)mm;
    return pfn == old_pfn ? 0 : -1;
}
static pfn_t pfa_alloc_page(void)
{
    pfn_t pfn = ++alloc_calls;
    if (pfn > PMD_PAGE_COUNT)
        return PFN_NONE;
    if (posix_memalign((void **)&new_pages[pfn], PAGE_SIZE, PAGE_SIZE) != 0)
        return PFN_NONE;
    return pfn;
}
static void *pfn_to_virt(pfn_t pfn)
{
    return pfn > 0 && pfn <= PMD_PAGE_COUNT ? new_pages[pfn] : NULL;
}
static paddr_t pfn_to_phys(pfn_t pfn)
{
    return (paddr_t)(uintptr_t)pfn_to_virt(pfn);
}
static void frame_put(pfn_t pfn)
{
    if (pfn == old_pfn)
        old_frame_puts++;
}
static int mm_fail_inject(int stage)
{
    (void)stage;
    return 0;
}
static int pt_unmap_leaf(mm_struct_t *mm, vaddr_t base, void *a, void *b,
                         void *c, void *d)
{
    (void)mm; (void)base; (void)a; (void)b; (void)c; (void)d;
    return 0;
}
static void mm_tlb_note_change(mm_struct_t *mm, vaddr_t base, size_t size)
{
    (void)mm; (void)base; (void)size;
}
static int pt_map(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags)
{
    (void)pgdir;
    if (va / PAGE_SIZE != map_calls || map_calls >= PMD_PAGE_COUNT)
        return -1;
    mapped_flags[map_calls] = flags;
    mapped_ptes[map_calls] = arch_pte_leaf(pa, flags);
    map_calls++;
    return 0;
}
static void arch_tlb_flush_page_local(vaddr_t base) { (void)base; }
"""

CHECKS = r"""
int main(void)
{
    if (posix_memalign((void **)&old_block, PMD_SIZE, PMD_SIZE) != 0)
        return 2;
    for (size_t i = 0; i < PMD_SIZE; i++)
        old_block[i] = (unsigned char)((i * 7U + 0x31U) & 0xffU);
    paddr_t old_pa = (paddr_t)(uintptr_t)old_block;
    pte_t input_flags = PTE_U | PTE_W | PTE_A | PTE_D | PTE_COW | PTE_G;
    old_pte = arch_pte_block(old_pa, input_flags);
    mm_struct_t mm = { .pgdir = (pt_root_t *)1 };

    int rc = mm_demote_huge_page(&mm, 0);
    if (rc != 0 || map_calls != PMD_PAGE_COUNT || old_frame_puts != 1) {
        fprintf(stderr, "demote setup failed rc=%d maps=%zu puts=%zu\n",
                rc, map_calls, old_frame_puts);
        return 1;
    }
    for (size_t i = 0; i < PMD_PAGE_COUNT; i++) {
        if (mapped_flags[i] & ARCH_PTE_BLOCK_ONLY_FLAGS) {
            fprintf(stderr, "demoted flags retained block-only bit at %zu: %llx\n",
                    i, (unsigned long long)mapped_flags[i]);
            return 1;
        }
        if ((mapped_flags[i] & (PTE_U | PTE_W | PTE_A | PTE_D | PTE_COW | PTE_G)) !=
            (input_flags & (PTE_U | PTE_W | PTE_A | PTE_D | PTE_COW | PTE_G)) ||
            !(mapped_ptes[i] & PTE_NX)) {
            fprintf(stderr, "demoted permission flags changed at %zu: %llx\n",
                    i, (unsigned long long)mapped_flags[i]);
            return 1;
        }
        if (mapped_ptes[i] & PTE_PS) {
            fprintf(stderr, "4K PTE carries PS/PAT bit at %zu: %llx\n",
                    i, (unsigned long long)mapped_ptes[i]);
            return 1;
        }
        if (memcmp(new_pages[i + 1], old_block + i * PAGE_SIZE, PAGE_SIZE) != 0) {
            fprintf(stderr, "demoted page content mismatch at %zu\n", i);
            return 1;
        }
    }
    for (size_t i = 1; i <= PMD_PAGE_COUNT; i++)
        free(new_pages[i]);
    free(old_block);
    puts("real mm_demote_huge_page extraction passed");
    return 0;
}
"""


def extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise SystemExit(f"function not found: {signature}")
    brace = source.find("{", start)
    if brace < 0:
        raise SystemExit(f"function body missing: {signature}")
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise SystemExit(f"unterminated function body: {signature}")


def compile_and_run(cc: str, function: str, negative: bool) -> subprocess.CompletedProcess[str]:
    if negative:
        function, removed = re.subn(
            r"#ifdef ARCH_PTE_BLOCK_ONLY_FLAGS\n.*?\n#endif\n",
            "",
            function,
            count=1,
            flags=re.DOTALL,
        )
        if removed != 1:
            raise SystemExit("could not remove block-only flag clear for negative control")
    source = STUBS + function + CHECKS
    with tempfile.TemporaryDirectory(prefix="x86-huge-demote-") as temp:
        cfile = pathlib.Path(temp) / "demote.c"
        binary = pathlib.Path(temp) / "demote"
        cfile.write_text(source)
        command = [
            cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-DCONFIG_64BIT", "-I", str(KERNEL / "arch/x86_64/include"),
            "-I", str(KERNEL / "include"), str(cfile), "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        return subprocess.run([str(binary)], text=True, capture_output=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    function = extract_function(VM_SOURCE.read_text(), "int mm_demote_huge_page(")

    candidate = compile_and_run(args.cc, function, negative=False)
    if candidate.returncode != 0:
        print(candidate.stdout, end="")
        print(candidate.stderr, end="")
        raise SystemExit("candidate demotion fixture failed")
    print(candidate.stdout, end="")

    old = compile_and_run(args.cc, function, negative=True)
    if old.returncode == 0 or "retained block-only bit" not in old.stderr:
        print(old.stdout, end="")
        print(old.stderr, end="")
        raise SystemExit("negative control did not detect block PS leaking to base PTE")
    print("demotion negative control detected inherited PS/PAT bit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
