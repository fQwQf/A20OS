"""Exercise production fault-window ownership with a racing installed page."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(signature)


class FaultWindowTest(unittest.TestCase):
    def test_racing_middle_page_keeps_offsets_and_ownership(self):
        body = function((ROOT / "kernel/mm/fault.c").read_text(),
                        "static int fault_map_window")
        harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
typedef unsigned long vaddr_t;
typedef unsigned long pfn_t;
typedef unsigned long pte_t;
typedef struct { int unused; } mm_struct_t;
typedef struct { int unused; } mm_cursor_t;
#define PAGE_SIZE 4096
#define PFN_NONE (~0UL)
#define MM_ST_ANON_MAPPED 1
static unsigned long leaves[4];
static int stop_at = -1;
static unsigned long pfn_to_phys(pfn_t pfn) { return pfn * PAGE_SIZE; }
static int mm_addrspace_lock(mm_struct_t *m, vaddr_t a, vaddr_t b,
                             mm_cursor_t *c) {
    (void)m; (void)a; (void)b; (void)c; return 0;
}
static void mm_cursor_unlock(mm_cursor_t *c) { (void)c; }
static int mm_cursor_map_if_absent(mm_cursor_t *c, vaddr_t va,
                                    unsigned long pa, pte_t f, int cls) {
    (void)c; (void)f; (void)cls;
    unsigned slot = va / PAGE_SIZE;
    if ((int)slot == stop_at) return -ENOMEM;
    if (leaves[slot]) return -EEXIST;
    leaves[slot] = pa;
    return 0;
}
'''
        checks = r'''
int main(void) {
    mm_struct_t mm;
    pfn_t pfns[4] = {10, 11, 12, 13};
    /* A sibling faults the middle page while frame preparation is unlocked. */
    leaves[1] = 99 * PAGE_SIZE;
    assert(fault_map_window(&mm, 0, 4 * PAGE_SIZE, pfns, 4, 0) == 3);
    assert(leaves[0] == 10 * PAGE_SIZE);
    assert(leaves[1] == 99 * PAGE_SIZE);
    assert(leaves[2] == 12 * PAGE_SIZE);
    assert(leaves[3] == 13 * PAGE_SIZE);
    assert(pfns[0] == PFN_NONE && pfns[1] == 11);
    assert(pfns[2] == PFN_NONE && pfns[3] == PFN_NONE);
    /* An allocation failure after partial success must not release a live
     * mapping or leak either of the unconsumed candidates. */
    for (int i = 0; i < 4; i++) { leaves[i] = 0; pfns[i] = 10 + i; }
    stop_at = 2;
    assert(fault_map_window(&mm, 0, 4 * PAGE_SIZE, pfns, 4, 0) == 2);
    assert(pfns[0] == PFN_NONE && pfns[1] == PFN_NONE);
    assert(pfns[2] == 12 && pfns[3] == 13);
    assert(leaves[2] == 0 && leaves[3] == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp) / "test.c"
            exe = Path(temp) / "test"
            src.write_text(harness + body + checks)
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(src),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
