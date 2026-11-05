#!/usr/bin/env python3
"""Exercise deferred final-mm teardown eligibility and recursive page-table release.

The fixture extracts the production eligibility helper, mm_destroy_impl, and
pt_destroy_user walk from ROOT/kernel, with stateful allocator, TLB, VMA, and
callback service stubs.
"""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VM_SOURCE = ROOT / "kernel/mm/vm.c"
MM_SOURCE = ROOT / "kernel/mm/mm.c"


def extract_function(source: str, signature: str) -> str:
    """Return one complete C function, ignoring braces inside comments/strings."""
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    state = "code"
    escaped = False
    i = opening
    while i < len(source):
        ch = source[i]
        following = source[i + 1] if i + 1 < len(source) else ""
        if state == "line-comment":
            if ch == "\n":
                state = "code"
        elif state == "block-comment":
            if ch == "*" and following == "/":
                state = "code"
                i += 1
        elif state == "string" or state == "char":
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif (state == "string" and ch == '"') or (
                state == "char" and ch == "'"
            ):
                state = "code"
        elif ch == "/" and following == "/":
            state = "line-comment"
            i += 1
        elif ch == "/" and following == "*":
            state = "block-comment"
            i += 1
        elif ch == '"':
            state = "string"
        elif ch == "'":
            state = "char"
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
        i += 1
    raise ValueError(f"unterminated function: {signature}")


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARCH_HAS_PGTABLE_OPS 1
#define CONFIG_SWAP 1
#define PTE_D 1
#define ARCH_PT_ROOT_LEVEL 2
#define ARCH_PT_BITS 2
#define MM_ST_FILE_PRIVATE 2
#define MM_ST_GET_CLASS(x) (x)
#define ARCH_PT_ROOT_ORDER 2
#define ARCH_PT_USER_END 3
#define PAGE_SIZE 4096u
#define PTE_V UINT64_C(1)
#define PTE_LEAF UINT64_C(2)
#define PTE_U UINT64_C(4)
#define PTE_SWAP UINT64_C(0x8000000000000000)
#define VM_SHARED (1ULL << 3)
#define VM_ANON (1ULL << 4)
#define VM_FILE (1ULL << 13)
#define VM_VMO (1ULL << 14)
#define VM_SYSV_SHM (1ULL << 16)
#define VM_PFNMAP (1ULL << 17)

typedef uint64_t vaddr_t;
typedef uint64_t pte_t;
typedef uint64_t pfn_t;
typedef uint64_t paddr_t;
typedef pte_t pt_root_t;
typedef struct mm_seg {
    uint64_t vm_flags;
    vaddr_t start, end;
    void *file;
    void *file_vnode;
    void *vmo;
    void *vmar_owner;
    struct mm_seg *next;
} mm_seg_t;
typedef struct mm_struct {
    int refcount;
    int arch_asid;
    pt_root_t *pgdir;
    mm_seg_t *mmap;
} mm_struct_t;

static int refcount_dec_and_test(int *refcount) { return --*refcount == 0; }
static int pte_is_swap(pte_t pte) { return (pte & PTE_SWAP) != 0; }
static uint64_t pte_to_swp_entry(pte_t pte) { return pte & ~PTE_SWAP; }
static int arch_pte_is_leaf(pte_t pte) { return (pte & PTE_LEAF) != 0; }
static uint64_t arch_pte_addr(pte_t pte) {
    return pte & UINT64_C(0x000ffffffffff000);
}
static pfn_t phys_to_pfn(paddr_t pa) { return pa >> 12; }
static void kerr(const char *format, ...) { (void)format; }
static pte_t *arch_pte_to_ptr(pte_t pte);
static int pfn_valid(pfn_t pfn);
static int arch_pt_level_entries(int level) { (void)level; return 4; }
static size_t pt_level_size(int level) { return (size_t)PAGE_SIZE << (ARCH_PT_BITS * level); }

static unsigned frame_put_calls[64];
static unsigned frame_refs[64];
static unsigned invalid_frame_put_calls;
static unsigned swap_free_calls;
static unsigned free_vma_pages_calls;
static unsigned node_fini_calls;
static unsigned pt_free_calls;
static unsigned root_free_order_calls;
static unsigned nonroot_free_order_calls;
static unsigned pt_free_pfn_calls[64];
static unsigned flush_calls;
static unsigned deferred_callbacks;
static unsigned segment_records_freed;
static unsigned deferred_pending;
static unsigned release_saw_null_pgdir;
static unsigned mm_kfree_calls;
static unsigned pt_destroy_finished;
static unsigned tlb_finish_calls;
static unsigned tlb_finished;
static unsigned pt_destroy_expected;
static unsigned queue_from_retire_drain;
static pte_t root_table[4];
static pte_t user_table_a[4];
static pte_t user_table_b[4];
static pte_t *table_by_pfn[64];
static int file_fixture;
static mm_seg_t *expected_file_vma;

static void reset_state(void) {
    memset(frame_put_calls, 0, sizeof(frame_put_calls));
    memset(frame_refs, 0, sizeof(frame_refs));
    memset(pt_free_pfn_calls, 0, sizeof(pt_free_pfn_calls));
    memset(root_table, 0, sizeof(root_table));
    memset(user_table_a, 0, sizeof(user_table_a));
    memset(user_table_b, 0, sizeof(user_table_b));
    memset(table_by_pfn, 0, sizeof(table_by_pfn));
    invalid_frame_put_calls = 0;
    swap_free_calls = free_vma_pages_calls = node_fini_calls = 0;
    pt_free_calls = root_free_order_calls = nonroot_free_order_calls = 0;
    flush_calls = deferred_callbacks = segment_records_freed = 0;
    deferred_pending = release_saw_null_pgdir = mm_kfree_calls = 0;
    pt_destroy_finished = tlb_finish_calls = tlb_finished = 0;
    pt_destroy_expected = queue_from_retire_drain = 0;
    file_fixture = 0; expected_file_vma = NULL;
}

static pte_t leaf(pfn_t pfn, int user) {
    return (pfn << 12) | PTE_V | PTE_LEAF | (user ? PTE_U : 0);
}
static pte_t table_entry(pfn_t pfn) { return (pfn << 12) | PTE_V; }
static pte_t *arch_pte_to_ptr(pte_t pte) {
    return table_by_pfn[(arch_pte_addr(pte) >> 12)];
}
static int pfn_valid(pfn_t pfn) {
    return pfn < 64 && table_by_pfn[pfn] != NULL;
}

static void frame_put(pfn_t pfn) {
    if (pfn >= 64) {
        fprintf(stderr, "invalid or duplicate frame_put for pfn=%llu\n",
                (unsigned long long)pfn);
        invalid_frame_put_calls++;
        return;
    }
    if (!tlb_finished) invalid_frame_put_calls++;
    frame_put_calls[pfn]++;
    if (frame_refs[pfn] == 0) {
        fprintf(stderr, "duplicate frame_put for pfn=%llu\n",
                (unsigned long long)pfn);
        invalid_frame_put_calls++;
        return;
    }
    frame_refs[pfn]--;
}
static void frame_put_many(const pfn_t *pfns, size_t count) {
    for (size_t i = 0; i < count; ++i) frame_put(pfns[i]);
}
static void swap_free(uint64_t entry) { (void)entry; swap_free_calls++; }
typedef struct { int refs; } page_cache_page_t;
typedef struct { unsigned char cls[4]; page_cache_page_t **cache_owner; } pt_meta_t;
static page_cache_page_t file_page;
static pt_meta_t *mm_pt_meta(pte_t *table) {
    static pt_meta_t file_meta = { .cls = {MM_ST_FILE_PRIVATE, 0, 0, 0} };
    return file_fixture && table == user_table_b ? &file_meta : NULL;
}
static page_cache_page_t *mm_file_cache_mapping_get(mm_seg_t *vma, vaddr_t va, pfn_t pfn) {
    if (file_fixture && vma == expected_file_vma && va == 65536 && pfn == 12) {
        file_page.refs++; return &file_page;
    }
    return NULL;
}
static void page_cache_put(page_cache_page_t *page) { page->refs--; }
static void mm_pt_note_absent(pte_t *table, int level, int index) {
    (void)table; (void)level; (void)index;
}
static pfn_t table_pfn(pte_t *table) {
    for (pfn_t pfn = 1; pfn < 64; ++pfn)
        if (table_by_pfn[pfn] == table) return pfn;
    return 0;
}
static void mm_pt_node_fini(pte_t *table) {
    node_fini_calls++;
    if (table_pfn(table) == 2) deferred_pending++;
}
static void pfa_free(pfn_t pfn, int order) {
    pt_free_calls++;
    if (pfn < 64) pt_free_pfn_calls[pfn]++;
    if (pfn == 1 && order == ARCH_PT_ROOT_ORDER) root_free_order_calls++;
    else if (pfn != 1 && order == 0) nonroot_free_order_calls++;
}
static void pt_free_table(pte_t *table, int level) {
    pfn_t pfn = table_pfn(table);
    if (!tlb_finished) invalid_frame_put_calls++;
    mm_pt_node_fini(table);
    pfa_free(pfn, level == ARCH_PT_ROOT_LEVEL ? ARCH_PT_ROOT_ORDER : 0);
    if (pfn == 1) pt_destroy_finished = 1;
}

static void mm_vma_flush_deferred(mm_struct_t *mm);
static void mm_tlb_invalidate_begin(mm_struct_t *mm) { (void)mm; }
static void mm_tlb_invalidate_finish(mm_struct_t *mm) {
    (void)mm;
    tlb_finish_calls++;
    tlb_finished = 1;
    mm_vma_flush_deferred(mm);
}
static void arch_tlb_flush(void) {}
static void aio_context_reap_mm(mm_struct_t *mm) { (void)mm; }
static void mm_vma_flush_deferred(mm_struct_t *mm) {
    flush_calls++;
    if (!deferred_pending) return;
    if (!tlb_finished || mm->pgdir != NULL ||
        (pt_destroy_expected && !pt_destroy_finished)) {
        fprintf(stderr, "deferred VMA callback ran before page-table teardown\n");
        return;
    }
    release_saw_null_pgdir++;
    deferred_callbacks += deferred_pending;
    segment_records_freed += deferred_pending;
    deferred_pending = 0;
}
static void mm_seg_index_clear(mm_struct_t *mm) { (void)mm; }
static void free_vma_pages(mm_struct_t *mm, mm_seg_t *vma) {
    (void)mm; (void)vma; free_vma_pages_calls++;
}
static void vma_get(mm_seg_t *vma) { (void)vma; }
static void vma_put(mm_struct_t *mm, mm_seg_t *vma) { (void)mm; (void)vma; }
static void mm_pt_retire_drain(mm_struct_t *mm) {
    if (queue_from_retire_drain && !mm->pgdir) deferred_pending++;
}
static void arch_mm_context_release(int asid) { (void)asid; }
static void kfree(void *ptr) { (void)ptr; mm_kfree_calls++; }

void pt_destroy_user_mm(mm_struct_t *mm);

/* Production eligibility and mm_destroy_impl functions are inserted here. */
/* INSERT_VM_FUNCTIONS */

/* The real pt_destroy_user_recursive and pt_destroy_user are inserted here. */
/* INSERT_PT_FUNCTIONS */

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } \
} while (0)

static void setup_anon_tree(void) {
    pt_destroy_expected = 1;
    table_by_pfn[1] = root_table;
    table_by_pfn[2] = user_table_a;
    table_by_pfn[3] = user_table_b;
    root_table[0] = table_entry(2);
    root_table[1] = table_entry(3);
    root_table[3] = leaf(13, 0); /* shared kernel half must remain untouched */
    user_table_a[0] = leaf(10, 1);
    user_table_a[1] = leaf(11, 0); /* protected leaf still belongs to user tree */
    user_table_a[2] = PTE_SWAP | UINT64_C(7);
    user_table_b[0] = leaf(12, 1); /* huge leaf head is released once */
    frame_refs[10] = frame_refs[11] = frame_refs[12] = frame_refs[13] = 1;
}

static int test_eligible_final_destroy(int shared) {
    mm_struct_t mm = { .refcount = 1, .arch_asid = 1, .pgdir = root_table };
    mm_seg_t vma = { .vm_flags = VM_ANON };
    reset_state();
    setup_anon_tree();
    if (shared) {
        vma.vm_flags |= VM_SHARED;
        frame_refs[10]++; frame_refs[11]++; frame_refs[12]++;
    }
    mm.mmap = &vma;
    mm_destroy_impl(&mm);

    CHECK(free_vma_pages_calls == 0, "eligible anon VMA skipped range walk");
    CHECK(frame_put_calls[10] == 1 && frame_refs[10] == (unsigned)shared,
          "base anonymous leaf released exactly once");
    CHECK(frame_put_calls[11] == 1 && frame_refs[11] == (unsigned)shared,
          "protected anonymous leaf released exactly once");
    CHECK(frame_put_calls[12] == 1 && frame_refs[12] == (unsigned)shared,
          "huge anonymous head released exactly once");
    CHECK(frame_put_calls[13] == 0 && frame_refs[13] == 1,
          "shared kernel-half frame was not released");
    CHECK(invalid_frame_put_calls == 0, "no invalid, duplicate, or early release");
    CHECK(tlb_finish_calls == 1 && tlb_finished,
          "TLB finish precedes remaining leaf and PT release");
    CHECK(swap_free_calls == 1, "anonymous swap entry released once");
    CHECK(pt_free_calls == 3 && node_fini_calls == 3,
          "each user PT node and root finalized once");
    CHECK(root_free_order_calls == 1 && nonroot_free_order_calls == 2,
          "root order and nonroot order preserved");
    CHECK(pt_free_pfn_calls[1] == 1 && pt_free_pfn_calls[2] == 1 &&
          pt_free_pfn_calls[3] == 1,
          "each page-table frame returned exactly once");
    CHECK(flush_calls == 3 && deferred_callbacks == 1 &&
          segment_records_freed == 1 && deferred_pending == 0,
          "post-PT deferred VMA release drained exactly once");
    CHECK(release_saw_null_pgdir == 1 && mm.pgdir == NULL,
          "deferred release saw cleared root pointer");
    CHECK(mm_kfree_calls == 1, "final mm released once");
    return 0;
}

static int test_ineligible(uint64_t flags, int pointer_kind) {
    mm_struct_t mm = { .refcount = 1, .arch_asid = 1, .pgdir = root_table };
    mm_seg_t vma = { .vm_flags = VM_ANON | flags };
    int marker = 0;
    reset_state();
    table_by_pfn[1] = root_table;
    pt_destroy_expected = 1;
    mm.mmap = &vma;
    if (pointer_kind == 1) vma.file = &marker;
    if (pointer_kind == 2) vma.file_vnode = &marker;
    if (pointer_kind == 3) vma.vmo = &marker;
    if (pointer_kind == 4) vma.vmar_owner = &marker;
    mm_destroy_impl(&mm);
    CHECK(free_vma_pages_calls == 1,
          "ineligible/special VMA used original page-release walk");
    CHECK(root_free_order_calls == 1 && pt_free_calls == 1,
          "ineligible empty address space still destroyed its root once");
    return 0;
}

static int test_file_owner_fallback_address(void) {
    mm_struct_t mm = { .refcount = 1, .arch_asid = 1, .pgdir = root_table };
    mm_seg_t file = { .vm_flags = VM_FILE, .start = 65536, .end = 81920,
                     .file_vnode = &mm };
    mm_seg_t anon = { .vm_flags = VM_ANON, .start = 0, .end = 65536, .next = &file };
    reset_state(); setup_anon_tree(); mm.mmap = &anon;
    file_fixture = 1; expected_file_vma = &file; file_page.refs = 1;
    mm_destroy_impl(&mm);
    CHECK(file_page.refs == 0, "terminal cache fallback uses byte-addressed VMA offset");
    CHECK(frame_put_calls[12] == 0, "cache-owned leaf never releases an ordinary frame ref");
    CHECK(invalid_frame_put_calls == 0, "cache fallback keeps terminal release ordering");
    return 0;
}

static int test_nonfinal_reference(void) {
    mm_struct_t mm = { .refcount = 2, .arch_asid = 1, .pgdir = root_table };
    mm_seg_t vma = { .vm_flags = VM_ANON };
    reset_state();
    mm.mmap = &vma;
    mm_destroy_impl(&mm);
    CHECK(mm.refcount == 1 && flush_calls == 0 &&
          free_vma_pages_calls == 0 && pt_free_calls == 0,
          "nonfinal reference performs no teardown");
    return 0;
}

static int test_final_flush_without_root(void) {
    mm_struct_t mm = { .refcount = 1, .arch_asid = 1, .pgdir = NULL };
    mm_seg_t vma = { .vm_flags = VM_ANON };
    reset_state();
    mm.mmap = &vma;
    queue_from_retire_drain = 1;
    mm_destroy_impl(&mm);
    CHECK(free_vma_pages_calls == 0 && pt_free_calls == 0,
          "empty final anon mm has no range walk or page-table frees");
    CHECK(flush_calls == 3 && tlb_finish_calls == 1 && tlb_finished,
          "no-root path still performs the final deferred flush");
    CHECK(deferred_callbacks == 1 && segment_records_freed == 1 &&
          deferred_pending == 0,
          "retire-drain deferred callback is released exactly once");
    CHECK(mm_kfree_calls == 1, "no-root final mm released once");
    return 0;
}

int main(void) {
    if (test_eligible_final_destroy(0) || test_eligible_final_destroy(1)) return 1;
    if (test_ineligible(VM_FILE, 0)) return 1;
    if (test_ineligible(VM_SHARED | VM_FILE, 2)) return 1;
    if (test_ineligible(VM_PFNMAP, 0)) return 1;
    if (test_ineligible(VM_VMO, 0)) return 1;
    if (test_ineligible(VM_SYSV_SHM, 0)) return 1;
    if (test_ineligible(0, 1) || test_ineligible(0, 2) ||
        test_ineligible(0, 3) || test_ineligible(0, 4)) return 1;
    if (test_nonfinal_reference()) return 1;
    if (test_final_flush_without_root()) return 1;
    if (test_file_owner_fallback_address()) return 1;
    puts("anon-final-destroy-host: PASS");
    return 0;
}
"""


def main() -> int:
    vm = VM_SOURCE.read_text()
    mm = MM_SOURCE.read_text()
    vm_functions = "\n\n".join(
        extract_function(vm, signature)
        for signature in (
            "static int mm_destroy_can_defer_leaf_release(",
            "static void mm_destroy_impl(",
        )
    )
    pt_functions = "\n\n".join(
        extract_function(mm, signature)
        for signature in (
            "static void pt_destroy_user_recursive(",
            "void pt_destroy_user_mm(",
        )
    )
    generated = HARNESS.replace("/* INSERT_VM_FUNCTIONS */", vm_functions)
    generated = generated.replace("/* INSERT_PT_FUNCTIONS */", pt_functions)

    with tempfile.TemporaryDirectory(prefix="anon-final-destroy-host-") as temp:
        source = Path(temp) / "fixture.c"
        binary = Path(temp) / "fixture"
        source.write_text(generated)
        subprocess.run(
            ["cc", "-std=gnu17", "-O0", "-Wall", "-Wextra", "-Werror",
             str(source), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
