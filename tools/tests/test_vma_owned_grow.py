"""Host fixture for the production VMA owned-index growth path.

This fixture extracts the real static/index functions from vma.c and supplies
only allocation, refcount, panic, and perf-counter stubs.
"""
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
                return source[start : pos + 1]
    raise AssertionError(f"unterminated function {signature!r}")


PRELUDE = r'''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <unistd.h>
#define MM_SEG_INDEX_CAPACITY 1024
#define A20_PERF_VMA_LOOKUPS 1
#define A20_PERF_VMA_LOOKUP_STEPS 2
typedef uintptr_t vaddr_t;
typedef struct mm_seg mm_seg_t;
struct mm_seg { uintptr_t start, end; mm_seg_t *next, *prev; int refs; };
typedef struct {
    mm_seg_t *mmap;
    mm_seg_t *seg_index[MM_SEG_INDEX_CAPACITY];
    mm_seg_t **seg_index_large;
    size_t seg_index_capacity, seg_index_count;
    uint8_t seg_index_state;
    size_t seg_index_hint;
} mm_struct_t;
static uint64_t steps_value, lookups_value;
static int fail_next_index_alloc;
uint64_t mm_seg_index_overflow;
static void a20_perf_count(int counter) { (void)counter; lookups_value++; }
static void a20_perf_add(int counter, uint64_t n) { (void)counter; steps_value += n; }
static mm_seg_t *mm_seg_get(mm_seg_t *v) { if (v) v->refs++; return v; }
static void vma_put(mm_struct_t *mm, mm_seg_t *v) {
    (void)mm; assert(v && v->refs > 0); v->refs--;
}
static void *kcalloc_atomic(size_t n, size_t size) {
    if (fail_next_index_alloc) { fail_next_index_alloc = 0; return NULL; }
    if (size && n > (size_t)-1 / size) return NULL;
    return calloc(n, size);
}
static void kfree(void *p) { free(p); }
static void kerr(const char *fmt, ...) { (void)fmt; }
static void panic(const char *message) {
    fprintf(stderr, "%s\n", message); abort();
}
'''

MAIN = r'''
static void link_items(mm_seg_t *items, size_t n) {
    for (size_t i = 0; i < n; i++) {
        items[i].start = 0x1000 + i * 0x2000;
        items[i].end = items[i].start + 0x1000;
        items[i].prev = i ? &items[i - 1] : NULL;
        items[i].next = i + 1 < n ? &items[i + 1] : NULL;
    }
}
static mm_seg_t *linear(mm_seg_t *v, uintptr_t addr) {
    for (; v; v = v->next)
        if (v->start <= addr && addr < v->end) return v;
    return NULL;
}
static void check_size(size_t n) {
    mm_seg_t *items = calloc(n ? n : 1, sizeof(*items));
    assert(items);
    link_items(items, n);
    mm_struct_t mm = {.mmap = n ? items : NULL};
    if (n == 0) assert(mm_seg_find(&mm, 0) == NULL);
    for (size_t i = 0; i < n; i++) {
        uintptr_t hit = items[i].start + 17;
        assert(mm_seg_find(&mm, hit) == &items[i]);
        assert(mm_seg_find(&mm, items[i].start + 0x1000) == NULL);
    }
    if (!(mm.seg_index_state == 1 && mm.seg_index_count == n))
        fprintf(stderr, "size=%zu state=%u count=%zu capacity=%zu large=%p\\n",
                n, mm.seg_index_state, mm.seg_index_count,
                mm.seg_index_capacity, (void *)mm.seg_index_large);
    assert(mm.seg_index_state == 1 && mm.seg_index_count == n);
    assert((mm.seg_index_large != NULL) == (n > MM_SEG_INDEX_CAPACITY));
    if (mm.seg_index_large)
        assert(mm.seg_index_capacity >= n);
    for (size_t i = 0; i < n; i++) assert(items[i].refs == 1);
    mm_seg_index_clear(&mm);
    assert(mm.seg_index_count == 0 && !mm.seg_index_large && !mm.seg_index_capacity);
    for (size_t i = 0; i < n; i++) assert(items[i].refs == 0);
    free(items);
}
static void test_boundaries_and_growth(void) {
    check_size(0); check_size(1); check_size(1024); check_size(1025);
    check_size(2048); check_size(8192);
}
static void test_hints_gaps_and_oracle(void) {
    const size_t n = 2048;
    mm_seg_t *items = calloc(n, sizeof(*items)); assert(items);
    link_items(items, n);
    mm_struct_t mm = {.mmap = items};
    assert(mm_seg_find(&mm, items[1700].start + 5) == &items[1700]);
    assert(mm.seg_index_hint == 1700);
    assert(mm_seg_find(&mm, items[4].start + 5) == &items[4]);
    for (uintptr_t addr = 0; addr < n * 0x2000 + 0x4000; addr += 997) {
        assert(mm_seg_find(&mm, addr) == linear(items, addr));
    }
    mm_seg_index_invalidate(&mm);
    assert(mm.seg_index_count == n); /* stale entries remain owned until rebuild */
    assert(mm_seg_find(&mm, items[100].start + 1) == &items[100]);
    assert(mm.seg_index_count == n);
    mm_seg_index_clear(&mm);
    for (size_t i = 0; i < n; i++) assert(items[i].refs == 0);
    free(items);
}
static void test_repeated_growth_and_clear(void) {
    mm_seg_t *items = calloc(3000, sizeof(*items)); assert(items);
    link_items(items, 1100);
    mm_struct_t mm = {.mmap = items};
    assert(mm_seg_find(&mm, items[1099].start) == &items[1099]);
    assert(mm.seg_index_capacity >= 1100);
    mm_seg_index_invalidate(&mm);
    link_items(items, 2500);
    assert(mm_seg_find(&mm, items[2499].start) == &items[2499]);
    assert(mm.seg_index_capacity >= 2500 && mm.seg_index_count == 2500);
    mm_seg_index_clear(&mm);
    for (size_t i = 0; i < 3000; i++) assert(items[i].refs == 0);
    free(items);
}
static void test_allocation_failure_and_retry(void) {
    mm_seg_t *items = calloc(1025, sizeof(*items)); assert(items);
    link_items(items, 1025);
    mm_struct_t mm = {.mmap = items};
    fail_next_index_alloc = 1;
    assert(mm_seg_find(&mm, items[1024].start) == &items[1024]);
    assert(mm.seg_index_state == 2 && mm.seg_index_count == 0);
    for (size_t i = 0; i < 1025; i++) assert(items[i].refs == 0);
    assert(mm_seg_find(&mm, items[1024].start) == &items[1024]);
    assert(mm.seg_index_state == 2); /* no repeated allocation before mutation */
    mm_seg_index_invalidate(&mm);
    assert(mm_seg_find(&mm, items[1024].start) == &items[1024]);
    assert(mm.seg_index_state == 1 && mm.seg_index_count == 1025);
    mm_seg_index_clear(&mm);
    for (size_t i = 0; i < 1025; i++) assert(items[i].refs == 0);
    free(items);
}
static void test_fork_buffer_detach_contract(void) {
    mm_seg_t *items = calloc(1100, sizeof(*items)); assert(items);
    link_items(items, 1100);
    mm_struct_t parent = {.mmap = items};
    assert(mm_seg_find(&parent, items[1099].start) == &items[1099]);
    mm_struct_t child = parent;
    child.seg_index_large = NULL;
    child.seg_index_capacity = 0;
    child.seg_index_count = 0;
    child.seg_index_hint = 0;
    child.seg_index_state = 0;
    memset(child.seg_index, 0, sizeof(child.seg_index));
    assert(child.seg_index_large == NULL && child.seg_index_count == 0 &&
           child.seg_index_hint == 0);
    assert(parent.seg_index_large != NULL && parent.seg_index_count == 1100);
    assert(mm_seg_find(&child, items[1000].start) == &items[1000]);
    mm_seg_index_clear(&child);
    assert(parent.seg_index_large != NULL && parent.seg_index_count == 1100);
    mm_seg_index_clear(&parent);
    for (size_t i = 0; i < 1100; i++) assert(items[i].refs == 0);
    free(items);
}
static void test_cycle_panics(void) {
    pid_t pid = fork(); assert(pid >= 0);
    if (pid == 0) {
        mm_seg_t items[4] = {{0}};
        link_items(items, 4);
        items[3].next = &items[1];
        mm_struct_t mm = {.mmap = items};
        (void)mm_seg_find(&mm, 0);
        _exit(0);
    }
    int status = 0; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0));
}
int main(void) {
    test_boundaries_and_growth();
    test_hints_gaps_and_oracle();
    test_repeated_growth_and_clear();
    test_allocation_failure_and_retry();
    test_fork_buffer_detach_contract();
    test_cycle_panics();
    puts("vma-owned-grow: production index host fixture PASS");
    return 0;
}
'''

OLD_FALLBACK_FUNCTIONS = r'''
static void mm_seg_index_rebuild(mm_struct_t *mm)
{
    /* Release the previous pass's references BEFORE writing any new entry.
     * The rebuild writes in place, so putting afterwards would put slots the
     * new pass had already overwritten -- dropping references the new entries
     * now hold, and leaking the old ones.
     *
     * old_count is seg_index_count unconditionally, NOT `state == 1 ?
     * seg_index_count : 0`.  This function is only ever reached from state 0,
     * so that guard was dead: invalidate() had already zeroed the count while
     * leaving the owned pointers in the array, and the loop below therefore
     * never ran.  Every reference the index took was orphaned at the next
     * mutation and never put, so a VMA dropped from the list could never reach
     * refcount zero and neither it nor its backing vnode/VMO was ever
     * released.  Invalidate keeps the count so this pass can give the
     * references back; state 2 has already NULLed its slots and zeroed the
     * count, so it correctly contributes nothing here. */
    uint16_t old_count = mm->seg_index_count;
    __atomic_store_n(&mm->seg_index_hint, 0, __ATOMIC_RELAXED);
    for (uint16_t i = 0; i < old_count; i++) {
        vma_put(mm, mm->seg_index[i]);
        mm->seg_index[i] = NULL;
    }

    uint16_t count = 0;
    /* The list is already sorted by start, so walking it in order produces a
     * sorted index -- no sort step. */
    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        if (count == MM_SEG_INDEX_CAPACITY) {
            /* Over capacity.  Release what this pass took and fall back to the
             * list, which is always correct.
             *
             * Counted, because "correct" is all this path has always claimed
             * and nothing ever checked it: the software gates run processes
             * with 14 mappings against a capacity of 1024, so this branch had
             * never executed anywhere.  A zero here now means the fallback is
             * still unexercised rather than silently unexercised -- the
             * difference is whether a reader has to take the word for it. */
            mm_seg_index_overflow++;
            for (uint16_t i = 0; i < count; i++) {
                vma_put(mm, mm->seg_index[i]);
                mm->seg_index[i] = NULL;
            }
            mm->seg_index_count = 0;
            mm->seg_index_state = 2;
            __atomic_store_n(&mm->seg_index_hint, 0, __ATOMIC_RELAXED);
            return;
        }
        mm->seg_index[count++] = mm_seg_get(v);
    }
    mm->seg_index_count = count;
    __atomic_store_n(&mm->seg_index_hint, 0, __ATOMIC_RELAXED);
    mm->seg_index_state = 1;
}

void mm_seg_index_invalidate(mm_struct_t *mm)
{
    if (!mm)
        return;
    /* seg_index_count deliberately survives: the slots below still hold owned
     * references, and the rebuild is the pass that puts them.  Zeroing the
     * count here is what orphaned them -- see mm_seg_index_rebuild(). */
    __atomic_store_n(&mm->seg_index_hint, 0, __ATOMIC_RELAXED);
    mm->seg_index_state = 0;
}

void mm_seg_index_clear(mm_struct_t *mm)
{
    if (!mm)
        return;
    /* Keyed on the count, not on state == 1: a dirty index (state 0) still
     * owns every reference the last valid pass took. */
    for (uint16_t i = 0; i < mm->seg_index_count; i++)
        vma_put(mm, mm->seg_index[i]);
    memset(mm->seg_index, 0, sizeof(mm->seg_index));
    mm->seg_index_count = 0;
    __atomic_store_n(&mm->seg_index_hint, 0, __ATOMIC_RELAXED);
    mm->seg_index_state = 0;
}

mm_seg_t *mm_seg_find(mm_struct_t *mm, vaddr_t addr)
{
    if (!mm)
        return NULL;
    size_t steps = 0;
    a20_perf_count(A20_PERF_VMA_LOOKUPS);

    /* Rebuild only from state 0 (dirty).  The test used to be `!= 1`, which
     * made state 2 -- capacity overflow -- re-run the whole rebuild on EVERY
     * lookup: walk the list, take MM_SEG_INDEX_CAPACITY references, discover
     * the overflow, put them all back, and only then fall through to the list
     * walk the rebuild was a shortcut for.  So the over-capacity path paid a
     * full rebuild plus the walk it was meant to replace, per fault.
     *
     * It is also the case that most needs the memo: an address space over
     * MM_SEG_INDEX_CAPACITY mappings stays over it, so nothing about the retry
     * can change the answer.  mm_seg_index_invalidate() still forces the retry
     * (state 0), because a mutation is exactly what could bring the count back
     * under the cap -- one rebuild attempt per mapping change instead of one
     * per lookup.
     *
     * Measured on smoke-mm-seg-index-overflow, one line changed: the same
     * workload read 10407 overflow rebuilds with `!= 1` and 2791 with `== 0`.
     * The residue is the invalidate-per-mutation cost and is meant to be there
     * -- those are the rebuilds that can still change the answer. */
    if (mm->seg_index_state == 0)
        mm_seg_index_rebuild(mm);

    if (mm->seg_index_state == 1) {
        /* The owned index already pins the record. A hint adds no lifetime
         * dependency: every hit still checks the complete covering interval. */
        uint16_t hint = __atomic_load_n(&mm->seg_index_hint, __ATOMIC_RELAXED);
        if (hint < mm->seg_index_count) {
            steps++;
            mm_seg_t *v = mm->seg_index[hint];
            if (v->start <= addr && addr < v->end) {
                a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
                return v;
            }
        }
        /* The last entry starting at or below `addr`; it covers `addr` only if
         * it has not ended. */
        size_t lo = 0;
        size_t hi = mm->seg_index_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            steps++;
            if (mm->seg_index[mid]->start <= addr)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo > 0) {
            mm_seg_t *v = mm->seg_index[lo - 1];
            steps++;
            if (addr < v->end) {
                __atomic_store_n(&mm->seg_index_hint, (uint16_t)(lo - 1),
                                 __ATOMIC_RELAXED);
                a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
                return v;
            }
        }
        a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
        return NULL;
    }

    /* Very unusual address spaces with more than MM_SEG_INDEX_CAPACITY mappings
     * retain the proven linked-list behavior instead of allocating while
     * mm->lock is held. */
    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        steps++;
        if (steps > 100000u) {
            kerr("[VMAWALK] CYCLE pid? mm=%p addr=0x%lx steps=%u head=%p\n",
                 (void *)mm, (unsigned long)addr, (unsigned)steps,
                 (void *)mm->mmap);
            panic("mm_seg_find: mapping list cycle");
        }
        if (addr < v->end && addr >= v->start) {
            a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
            return v;
        }
        if (v->start > addr)
            break;
    }
    a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
    return NULL;
}
'''

OLD_PRELUDE = r'''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define MM_SEG_INDEX_CAPACITY 1024
#define A20_PERF_VMA_LOOKUPS 1
#define A20_PERF_VMA_LOOKUP_STEPS 2
typedef uintptr_t vaddr_t;
typedef struct mm_seg mm_seg_t;
struct mm_seg { uintptr_t start, end; mm_seg_t *next, *prev; int refs; };
typedef struct {
    mm_seg_t *mmap;
    mm_seg_t *seg_index[MM_SEG_INDEX_CAPACITY];
    uint16_t seg_index_count;
    uint8_t seg_index_state;
    uint16_t seg_index_hint;
} mm_struct_t;
static uint64_t steps_value;
uint64_t mm_seg_index_overflow;
static void a20_perf_count(int counter) { (void)counter; }
static void a20_perf_add(int counter, uint64_t n) { (void)counter; steps_value += n; }
static mm_seg_t *mm_seg_get(mm_seg_t *v) { if (v) v->refs++; return v; }
static void vma_put(mm_struct_t *mm, mm_seg_t *v) { (void)mm; assert(v && v->refs); v->refs--; }
static void kerr(const char *fmt, ...) { (void)fmt; }
static void panic(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
'''
OLD_MAIN = r'''
int main(void) {
    const size_t n = 8192;
    mm_seg_t *items = calloc(n, sizeof(*items)); assert(items);
    for (size_t i = 0; i < n; i++) {
        items[i].start = 0x1000 + i * 0x2000;
        items[i].end = items[i].start + 0x1000;
        items[i].next = i + 1 < n ? &items[i + 1] : NULL;
    }
    mm_struct_t mm = {.mmap = items};
    uint64_t before = steps_value;
    assert(mm_seg_find(&mm, items[n - 1].start + 1) == &items[n - 1]);
    assert(steps_value - before == n);
    printf("old actual overflow fallback: %lu VMA steps (expected linear negative control)\n",
           (unsigned long)(steps_value - before));
    mm_seg_index_clear(&mm);
    free(items);
    return 0;
}
'''


class OwnedGrowTests(unittest.TestCase):
    def test_production_owned_grow_functions(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("cc is required for the C host fixture")
        source = (ROOT / "kernel/mm/vma.c").read_text()
        signatures = (
            "static mm_seg_t **mm_seg_index_slots(",
            "static size_t mm_seg_index_active_capacity(",
            "static size_t mm_seg_list_count(",
            "static void mm_seg_index_rebuild(",
            "void mm_seg_index_invalidate(",
            "void mm_seg_index_clear(",
            "mm_seg_t *mm_seg_find(",
        )
        functions = "\n\n".join(extract_function(source, sig) for sig in signatures)
        with tempfile.TemporaryDirectory(prefix="a20-vma-owned-grow-") as tmp:
            c_file = Path(tmp) / "vma_owned_grow.c"
            exe = Path(tmp) / "vma_owned_grow"
            c_file.write_text(PRELUDE + functions + MAIN)
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", str(c_file), "-o", str(exe)],
                check=True,
            )
            result = subprocess.run([str(exe)], check=False, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("production index host fixture PASS", result.stdout)

    def test_old_actual_overflow_fallback_negative_control(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("cc is required for the C host fixture")
        functions = OLD_FALLBACK_FUNCTIONS
        with tempfile.TemporaryDirectory(prefix="a20-vma-old-overflow-") as tmp:
            c_file = Path(tmp) / "vma_old_fallback.c"
            exe = Path(tmp) / "vma_old_fallback"
            c_file.write_text(OLD_PRELUDE + functions + OLD_MAIN)
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", str(c_file), "-o", str(exe)],
                check=True,
            )
            result = subprocess.run([str(exe)], check=True, text=True, capture_output=True)
            self.assertIn("8192 VMA steps", result.stdout)


if __name__ == "__main__":
    unittest.main()
