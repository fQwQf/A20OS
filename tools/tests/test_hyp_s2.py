#!/usr/bin/env python3
"""Execute the production native stage-2 radix engine with mock frame/metadata.

The real hyp_s2.c and public hypervisor headers are compiled unchanged. The
fixture models a compact PFN map, frame references, metadata and locks; it does
not emulate hardware TLBs or prove guest execution on either architecture.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SHIMS = {
    "core/lock.h": r'''#pragma once
#include "core/types.h"
#include <assert.h>
typedef struct { unsigned held; } spinlock_t;
static inline void spin_init(spinlock_t *l) { l->held = 0; }
static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!l->held); l->held = 1; return 0;
}
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) {
    (void)f; assert(l->held); l->held = 0;
}
''',
    "core/cpu.h": r'''#pragma once
static inline unsigned cpu_current_id(void) { return 0; }
''',
    "core/string.h": '#include <string.h>\n',
    "mm/pt.h": r'''#pragma once
#include "core/types.h"
#include "core/errno.h"
#define PAGE_SIZE 4096u
#define MM_PT_META_ENTRIES 512
#define PTE_R 1u
#define PTE_W 2u
#define PTE_X 4u
#define PTE_U 8u
#define MM_ST_PT_NODE 1u
#define MM_ST_GUEST_MEM 9u
#define MM_ST_CLS_BYTE(c) ((uint8_t)((c) << 4))
#define MM_ST_GET_CLASS(c) ((uint8_t)((c) >> 4))
#define MM_ST_PROT_R 1u
#define MM_ST_PROT_W 2u
#define MM_ST_PROT_X 4u
typedef uint32_t pfn_t;
typedef struct { uint8_t cls[512]; unsigned level; } pt_meta_t;
typedef struct {
    uint64_t pt_pages, entries, missing_meta, present_mismatch;
    uint64_t absent_mismatch, prot_mismatch, seg_bad_slot, seg_extent_mismatch;
} mm_pt_audit_report_t;
static inline uint64_t mm_pt_audit_errors(const mm_pt_audit_report_t *r) {
    return r->missing_meta + r->present_mismatch + r->absent_mismatch +
           r->prot_mismatch + r->seg_bad_slot + r->seg_extent_mismatch;
}
/* Preserve the production host-helper's user-access gate. Stage-2 must
 * derive its own access metadata: native guest mappings intentionally lack U. */
static inline uint8_t mm_pt_prot_bits(pte_t p) { return (p & PTE_U) ? p & 7 : 0; }
int mm_pt_node_init_atomic(pte_t *p, unsigned level);
void mm_pt_node_fini(pte_t *p);
pt_meta_t *mm_pt_meta(pte_t *p);
void mm_pt_note_present(pte_t *p, unsigned level, unsigned i, uint8_t c);
void mm_pt_note_absent(pte_t *p, unsigned level, unsigned i);
void mm_pt_frame_return(pfn_t p);
int frame_is_lent_to_guest(pfn_t p);
''',
    "mm/frame.h": r'''#pragma once
#include "mm/pt.h"
#include "core/lock.h"
#define PFN_NONE ((pfn_t)~0u)
#define FRAME_F_ALLOC 1u
#define FRAME_F_PT 4u
#define FRAME_F_GUEST 8u
#define TEST_FRAMES 128u
typedef struct { unsigned flags, refcount; } frame_meta_t;
typedef struct { frame_meta_t meta[TEST_FRAMES]; spinlock_t lock; } pfa_t;
extern pfa_t pfa;
pfn_t pfa_alloc_flags(int order, int reclaim);
void pfa_free(pfn_t p, int order);
void *pfn_to_virt(pfn_t p);
paddr_t pfn_to_phys(pfn_t p);
pfn_t phys_to_pfn(paddr_t pa);
pfn_t virt_to_pfn(const void *p);
int pfn_valid(pfn_t p);
void frame_get(pfn_t p);
void frame_put(pfn_t p);
''',
}


class NativeStage2Test(unittest.TestCase):
    def test_production_radix_ownership_and_failure_paths(self):
        with tempfile.TemporaryDirectory(prefix="a20-hyp-s2-") as tmp:
            tmp = Path(tmp)
            for name, content in SHIMS.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = tmp / "hyp_s2_host"
            result = subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fno-builtin", "-DARCH_HAS_PGTABLE_OPS=1", "-DCONFIG_64BIT=1",
                "-I", str(tmp), "-I", str(ROOT / "kernel/include"),
                str(ROOT / "kernel/hyp/hyp_s2.c"),
                str(ROOT / "tools/tests/hyp_s2_host.c"), "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("HYP_S2_HOST: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
