"""Host tests for the production VMA index lookup and recent-slot hint."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def extract_function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"expected exactly one definition of {signature!r}")
    start = source.index(signature)
    brace = source.find("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(f"unterminated function {signature!r}")


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define MM_SEG_INDEX_CAPACITY 4
#define A20_PERF_VMA_LOOKUPS 1
#define A20_PERF_VMA_LOOKUP_STEPS 2
typedef uintptr_t vaddr_t;
typedef struct mm_seg mm_seg_t;
struct mm_seg { uintptr_t start, end; mm_seg_t *next, *prev; int refs; };
typedef struct {
    mm_seg_t *mmap;
    mm_seg_t *seg_index[MM_SEG_INDEX_CAPACITY];
    mm_seg_t **seg_index_large;
    size_t seg_index_capacity;
    size_t seg_index_count;
    uint8_t seg_index_state;
    size_t seg_index_hint;
} mm_struct_t;
static uint64_t steps_value, lookups_value;
static unsigned alloc_calls;
static int fail_next_alloc;
uint64_t mm_seg_index_overflow;
static void a20_perf_count(int counter) { (void)counter; lookups_value++; }
static void a20_perf_add(int counter, uint64_t n) { (void)counter; steps_value += n; }
static mm_seg_t *mm_seg_get(mm_seg_t *v) { if (v) v->refs++; return v; }
static void vma_put(mm_struct_t *mm, mm_seg_t *v) { (void)mm; assert(v && v->refs > 0); v->refs--; }
static void *kcalloc_atomic(size_t n, size_t size) {
    alloc_calls++;
    if (fail_next_alloc) { fail_next_alloc = 0; return NULL; }
    if (size && n > (size_t)-1 / size) return NULL;
    return calloc(n, size);
}
static void kfree(void *ptr) { free(ptr); }
static void kerr(const char *fmt, ...) { (void)fmt; }
static void panic(const char *message) { fprintf(stderr, "%s\\n", message); __builtin_trap(); }
'''

MAIN = r'''
static void link(mm_seg_t *items, size_t n) {
    for (size_t i = 0; i < n; i++) {
        items[i].next = i + 1 < n ? &items[i + 1] : NULL;
        items[i].prev = i ? &items[i - 1] : NULL;
    }
}
static mm_seg_t *linear(mm_seg_t *head, uintptr_t addr) {
    for (mm_seg_t *v = head; v; v = v->next)
        if (v->start <= addr && addr < v->end) return v;
    return NULL;
}
static void test_hot_hits_and_boundaries(void) {
    mm_seg_t v[3] = {{100, 200, 0, 0, 0}, {300, 400, 0, 0, 0}, {500, 600, 0, 0, 0}};
    link(v, 3); mm_struct_t mm = {.mmap = v};
    assert(mm_seg_find(&mm, 150) == &v[0]);
    assert(mm.seg_index_hint == 0);
    uint64_t before = steps_value;
    assert(mm_seg_find(&mm, 175) == &v[0]);
    assert(steps_value == before + 1);
    assert(mm_seg_find(&mm, 350) == &v[1]);
    assert(mm.seg_index_hint == 1);
    assert(mm_seg_find(&mm, 300) == &v[1]); /* inclusive start */
    assert(mm_seg_find(&mm, 400) == NULL);  /* exclusive end */
    assert(mm_seg_find(&mm, 250) == NULL);  /* gap */
    assert(mm_seg_find(&mm, 500) == &v[2]);
    assert(mm_seg_find(&mm, 600) == NULL);
    mm_seg_index_clear(&mm);
    for (int i = 0; i < 3; i++) assert(v[i].refs == 0);
}
static void test_bad_hint_and_rebuild(void) {
    mm_seg_t v[2] = {{10, 20, 0, 0, 0}, {30, 40, 0, 0, 0}};
    link(v, 2); mm_struct_t mm = {.mmap = v};
    assert(mm_seg_find(&mm, 35) == &v[1]);
    mm.seg_index_hint = (size_t)-1;
    assert(mm_seg_find(&mm, 15) == &v[0]);
    /* Mutation invalidates the old index and its remembered slot. */
    mm_seg_index_invalidate(&mm);
    assert(mm.seg_index_hint == 0);
    mm_seg_t added = {50, 60, 0, 0, 0};
    v[1].next = &added; added.prev = &v[1];
    assert(mm_seg_find(&mm, 55) == &added);
    assert(mm.seg_index_count == 3 && mm.seg_index_state == 1);
    assert(v[0].refs == 1 && v[1].refs == 1 && added.refs == 1);
    mm_seg_index_clear(&mm);
    assert(v[0].refs == 0 && v[1].refs == 0 && added.refs == 0);
}
static void test_dynamic_growth(void) {
    mm_seg_t v[5];
    for (int i = 0; i < 5; i++) { v[i] = (mm_seg_t){i * 20, i * 20 + 10, 0, 0, 0}; }
    link(v, 5); mm_struct_t mm = {.mmap = v};
    assert(mm_seg_find(&mm, 85) == &v[4]);
    assert(mm.seg_index_state == 1 && mm.seg_index_count == 5);
    assert(mm.seg_index_large != NULL && mm.seg_index_capacity >= 5);
    assert(mm.seg_index_hint == 4 && mm_seg_index_overflow == 1);
    for (int i = 0; i < 5; i++) assert(v[i].refs == 1);
    uint64_t before = steps_value;
    assert(mm_seg_find(&mm, 85) == &v[4]);
    assert(steps_value - before < 5); /* grown index uses hint/binary lookup */
    mm_seg_index_clear(&mm);
    assert(mm.seg_index_large == NULL && mm.seg_index_capacity == 0);
    for (int i = 0; i < 5; i++) assert(v[i].refs == 0);
}
static void test_oom_memo_fallback_and_retry(void) {
    mm_seg_t v[5];
    for (int i = 0; i < 5; i++) { v[i] = (mm_seg_t){i * 20, i * 20 + 10, 0, 0, 0}; }
    link(v, 5); mm_struct_t mm = {.mmap = v};
    fail_next_alloc = 1;
    unsigned before_allocs = alloc_calls;
    assert(mm_seg_find(&mm, 85) == &v[4]);
    assert(mm.seg_index_state == 2 && mm.seg_index_count == 0);
    assert(mm.seg_index_large == NULL && mm.seg_index_hint == 0);
    assert(alloc_calls == before_allocs + 1);
    for (int i = 0; i < 5; i++) assert(v[i].refs == 0);
    uint64_t before_steps = steps_value;
    assert(mm_seg_find(&mm, 85) == &v[4]);
    assert(steps_value - before_steps == 5);
    assert(alloc_calls == before_allocs + 1); /* state 2 memoizes the OOM */
    assert(mm_seg_index_overflow == 2);

    mm_seg_index_invalidate(&mm);
    assert(mm_seg_find(&mm, 85) == &v[4]);
    assert(mm.seg_index_state == 1 && mm.seg_index_count == 5);
    assert(mm.seg_index_large != NULL && mm.seg_index_capacity >= 5);
    for (int i = 0; i < 5; i++) assert(v[i].refs == 1);
    mm_seg_index_clear(&mm);
    for (int i = 0; i < 5; i++) assert(v[i].refs == 0);
}
static uint32_t rng = 0x31415926u;
static uint32_t next_random(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void test_randomized_mutation_oracle(void) {
    mm_seg_t v[6] = {{0}};
    size_t n = 5;
    for (size_t i = 0; i < n; i++) v[i] = (mm_seg_t){100 + i * 100, 150 + i * 100, 0, 0, 0};
    link(v, n); mm_struct_t mm = {.mmap = v};
    for (unsigned round = 0; round < 300; round++) {
        uintptr_t addr = next_random() % 800;
        assert(mm_seg_find(&mm, addr) == linear(mm.mmap, addr));
        if (round % 31 == 0) {
            mm_seg_index_invalidate(&mm);
            assert(mm.seg_index_hint == 0);
        }
        if (round == 150) {
            /* Replace one interval while invalid, then let lookup rebuild. */
            v[2].start = 355; v[2].end = 390;
            for (size_t i = 0; i < n; i++) { v[i].start = 100 + i * 100; v[i].end = 150 + i * 100; }
            v[2].start = 355; v[2].end = 390;
            mm_seg_index_invalidate(&mm);
        }
    }
    mm_seg_index_clear(&mm);
    for (size_t i = 0; i < n; i++) assert(v[i].refs == 0);
}
int main(void) {
    test_hot_hits_and_boundaries();
    test_bad_hint_and_rebuild();
    test_dynamic_growth();
    test_oom_memo_fallback_and_retry();
    test_randomized_mutation_oracle();
    puts("vma-lookup-hint: production lookup differential host tests PASS");
    return 0;
}
'''


class VmaLookupHintTests(unittest.TestCase):
    def test_production_lookup_hint(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("cc is required for this host oracle")
        source = (ROOT / "kernel/mm/vma.c").read_text()
        functions = "\n\n".join(
            extract_function(source, signature)
            for signature in (
                "static mm_seg_t **mm_seg_index_slots(",
                "static size_t mm_seg_index_active_capacity(",
                "static size_t mm_seg_list_count(",
                "static void mm_seg_index_rebuild(",
                "void mm_seg_index_invalidate(",
                "void mm_seg_index_clear(",
                "mm_seg_t *mm_seg_find(",
            )
        )
        with tempfile.TemporaryDirectory(prefix="a20-vma-hint-") as tmp:
            c_file, exe = Path(tmp) / "vma_hint.c", Path(tmp) / "vma_hint"
            c_file.write_text(PRELUDE + functions + MAIN)
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", str(c_file), "-o", str(exe)],
                check=True,
            )
            result = subprocess.run([str(exe)], check=True, text=True, capture_output=True)
            self.assertIn("production lookup differential", result.stdout)


if __name__ == "__main__":
    unittest.main()
