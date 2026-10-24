#!/usr/bin/env python3
"""Host regression for kernel drv_dma_alloc compact-PFN to device-PA mapping.

The generated C fixture extracts the real frame.h range conversion helpers and
real kernel drv_dma_alloc/drv_dma_free bodies. Only allocator, VA alias, and
kernel environment services are mocked. The negative control swaps in the old
compact-PFN multiplication and must fail the known-PA assertions.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
FRAME_H = ROOT / "kernel/include/mm/frame.h"
DRV_ENV_H = ROOT / "kernel/include/drivers/dual/drv_env.h"


def extract_function(source: str, signature: str) -> str:
    """Return a C function by signature, matching its body braces."""
    start = source.find(signature)
    if start < 0:
        raise ValueError(f"missing source function: {signature}")
    brace = source.find("{", start)
    if brace < 0:
        raise ValueError(f"missing body for: {signature}")

    depth = 0
    i = brace
    state = "code"
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if ch == "/" and nxt == "*":
                state = "block_comment"
                i += 2
                continue
            if ch == "/" and nxt == "/":
                state = "line_comment"
                i += 2
                continue
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return source[start : i + 1]
        elif state == "block_comment" and ch == "*" and nxt == "/":
            state = "code"
            i += 2
            continue
        elif state == "line_comment" and ch == "\n":
            state = "code"
        elif state in ("string", "char"):
            if ch == "\\":
                i += 2
                continue
            if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        i += 1
    raise ValueError(f"unterminated function body: {signature}")


def extracted_source(old_pfn_multiply: bool = False) -> str:
    frame_text = FRAME_H.read_text()
    driver_text = DRV_ENV_H.read_text()
    helper_signatures = (
        "static inline const pfa_range_t *pfa_range_for_pfn(",
        "static inline const pfa_range_t *pfa_range_for_pa(",
        "static inline paddr_t pfn_to_phys(",
        "static inline pfn_t phys_to_pfn(",
        "static inline int pfn_valid(",
    )
    helpers = "\n\n".join(
        extract_function(frame_text, signature) for signature in helper_signatures
    )
    alloc = extract_function(driver_text, "static inline int drv_dma_alloc(")
    free = extract_function(driver_text, "static inline void drv_dma_free(")
    if old_pfn_multiply:
        corrected = "uint64_t pa = (uint64_t)pfn_to_phys(pfn);"
        if corrected not in alloc:
            raise ValueError("corrected production expression not found")
        alloc = alloc.replace(
            corrected, "uint64_t pa = (uint64_t)pfn * DRV_PAGE_SIZE;", 1
        )

    prelude = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PAGE_SIZE_BITS 12
#define PAGE_SIZE (1u << PAGE_SIZE_BITS)
#define PAGE_OFFSET 0
#define PFN_NONE ((uint32_t)-1)
#define DRV_PAGE_SIZE 4096u
#define PFA_MAX_RANGES 4

typedef uint32_t pfn_t;
typedef uint64_t paddr_t;
typedef struct { paddr_t base, end; pfn_t start_pfn, end_pfn; } pfa_range_t;
static struct { size_t nr_ranges; pfa_range_t ranges[PFA_MAX_RANGES]; } pfa;

typedef struct drv_dma {
    uint64_t va0;
    uint64_t cookie;
    uint32_t npages;
    uint32_t _pad;
    uint64_t phys[64];
} drv_dma_t;

static unsigned alloc_order_seen;
static unsigned free_order_seen;
static pfn_t alloc_pfn;
static int alloc_fails;
static unsigned char alias_storage[64 * DRV_PAGE_SIZE];

static pfn_t pfa_alloc(int order) {
    alloc_order_seen = (unsigned)order;
    return alloc_fails ? PFN_NONE : alloc_pfn;
}
static void pfa_free(pfn_t pfn, int order) {
    if (pfn != alloc_pfn) {
        fprintf(stderr, "free PFN mismatch: got %u expected %u\n",
                pfn, alloc_pfn);
        exit(20);
    }
    free_order_seen = (unsigned)order;
}
static void *pfn_to_virt(pfn_t pfn) {
    (void)pfn;
    return alias_storage;
}
'''
    checks = r'''
static int check_one(uint32_t npages, pfn_t chosen, paddr_t expected_pa) {
    drv_dma_t dma;
    memset(&dma, 0xA5, sizeof(dma));
    memset(alias_storage, 0xA5, sizeof(alias_storage));
    alloc_pfn = chosen;
    alloc_fails = 0;
    alloc_order_seen = free_order_seen = 999;
    if (drv_dma_alloc(&dma, npages) != 0) {
        fprintf(stderr, "allocation unexpectedly failed for %u pages\n", npages);
        return 1;
    }
    unsigned expected_order = 0;
    while ((1u << expected_order) < npages)
        expected_order++;
    if (alloc_order_seen != expected_order || dma.cookie != chosen ||
        dma.npages != npages || dma.va0 != (uint64_t)(uintptr_t)alias_storage) {
        fprintf(stderr, "allocation metadata mismatch for %u pages\n", npages);
        return 2;
    }
    for (uint32_t i = 0; i < npages; i++) {
        if (dma.phys[i] != expected_pa + (uint64_t)i * DRV_PAGE_SIZE) {
            fprintf(stderr,
                    "wrong PA: pages=%u index=%u got=0x%llx expected=0x%llx\n",
                    npages, i, (unsigned long long)dma.phys[i],
                    (unsigned long long)(expected_pa + (uint64_t)i * DRV_PAGE_SIZE));
            return 3;
        }
    }
    for (uint32_t i = npages; i < 64; i++) {
        if (dma.phys[i] != 0) {
            fprintf(stderr, "DMA address tail not zero at %u\n", i);
            return 4;
        }
    }
    for (size_t i = 0; i < (size_t)npages * DRV_PAGE_SIZE; i++) {
        if (alias_storage[i] != 0) {
            fprintf(stderr, "allocated payload not zero at byte %zu\n", i);
            return 5;
        }
    }
    drv_dma_free(&dma);
    if (free_order_seen != expected_order || dma.va0 != 0 || dma.cookie != 0 ||
        dma.npages != 0) {
        fprintf(stderr, "free contract mismatch for %u pages\n", npages);
        return 6;
    }
    return 0;
}

int main(void) {
    /* Two discontiguous physical ranges use independent compact PFN spans. */
    pfa.nr_ranges = 2;
    pfa.ranges[0] = (pfa_range_t){ .base = 0x21000000, .end = 0x21040000,
                                    .start_pfn = 0, .end_pfn = 64 };
    pfa.ranges[1] = (pfa_range_t){ .base = 0x88000000, .end = 0x88100000,
                                    .start_pfn = 1024, .end_pfn = 1280 };
    int rc = check_one(1, 7, 0x21007000);
    if (rc) return rc;
    rc = check_one(3, 1032, 0x88008000);
    if (rc) return rc;
    rc = check_one(64, 1088, 0x88040000);
    if (rc) return rc;

    drv_dma_t untouched;
    memset(&untouched, 0x5A, sizeof(untouched));
    if (drv_dma_alloc(NULL, 1) == 0 || drv_dma_alloc(&untouched, 0) == 0 ||
        drv_dma_alloc(&untouched, 65) == 0) {
        fprintf(stderr, "invalid input accepted\n");
        return 7;
    }
    alloc_fails = 1;
    if (drv_dma_alloc(&untouched, 1) == 0) {
        fprintf(stderr, "allocator failure accepted\n");
        return 8;
    }
    puts("drv_dma_phys_host: PASS");
    return 0;
}
'''
    return prelude + "\n" + helpers + "\n" + alloc + "\n" + free + "\n" + checks


def build_and_run(source: str, directory: pathlib.Path, label: str) -> subprocess.CompletedProcess[str]:
    c_path = directory / f"{label}.c"
    exe_path = directory / label
    c_path.write_text(source)
    compile_result = subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_path), "-o", str(exe_path)],
        text=True,
        capture_output=True,
        check=False,
    )
    if compile_result.returncode != 0:
        raise RuntimeError(
            f"{label} compile failed:\n{compile_result.stdout}{compile_result.stderr}"
        )
    return subprocess.run([str(exe_path)], text=True, capture_output=True, check=False)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="drv-dma-phys-") as tmp:
        tempdir = pathlib.Path(tmp)
        fixed = build_and_run(extracted_source(), tempdir, "fixed")
        if fixed.returncode != 0:
            raise AssertionError(f"corrected source failed:\n{fixed.stdout}{fixed.stderr}")
        print(fixed.stdout, end="")

        old = build_and_run(extracted_source(old_pfn_multiply=True), tempdir, "old")
        if old.returncode == 0 or "wrong PA:" not in old.stderr:
            raise AssertionError(
                "old compact-PFN multiplication was not detected as wrong PA:\n"
                f"stdout={old.stdout}\nstderr={old.stderr}"
            )
        print("drv_dma_phys_host: old-expression negative control detected wrong PA")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
