"""Exercise the real C COW child-map bridge and its backing-class arguments."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef uintptr_t pte_t;
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;
typedef struct { void *pgdir; } mm_struct_t;
enum {
    MM_ST_ANON_MAPPED = 1,
    MM_ST_FILE_PRIVATE = 2,
    MM_ST_FILE_SHARED = 3,
    MM_ST_VMO = 4,
};

static int map_cls_calls, map_huge_calls;
static int map_cls_result, map_huge_result;
static void *seen_pgdir, *seen_mm;
static uintptr_t seen_va, seen_pa, seen_flags;
static uint8_t seen_cls;

static uintptr_t pfn_to_phys(uint32_t pfn) {
    return 0x100000UL + (uintptr_t)pfn * 4096;
}
static int pt_map_cache_cls(void *pgdir, uintptr_t va, uintptr_t pa,
                      uintptr_t flags, uint8_t cls, void *pcp);
static int pt_map_huge(mm_struct_t *mm, uintptr_t va, uintptr_t pa,
                       uintptr_t flags, uint8_t cls);

/* PRODUCTION_FUNCTION */

static int pt_map_cache_cls(void *pgdir, uintptr_t va, uintptr_t pa,
                      uintptr_t flags, uint8_t cls, void *pcp) {
    (void)pcp;
    map_cls_calls++;
    seen_pgdir = pgdir;
    seen_va = va;
    seen_pa = pa;
    seen_flags = flags;
    seen_cls = cls;
    return map_cls_result;
}
static int pt_map_huge(mm_struct_t *mm, uintptr_t va, uintptr_t pa,
                       uintptr_t flags, uint8_t cls) {
    map_huge_calls++;
    seen_mm = mm;
    seen_va = va;
    seen_pa = pa;
    seen_flags = flags;
    seen_cls = cls;
    return map_huge_result;
}
static void reset(void) {
    map_cls_calls = map_huge_calls = 0;
    map_cls_result = map_huge_result = 0;
    seen_pgdir = seen_mm = NULL;
    seen_va = seen_pa = seen_flags = 0;
    seen_cls = 0;
}

int main(void) {
    mm_struct_t child, other;
    child.pgdir = &other;
    const uint8_t classes[] = {
        MM_ST_ANON_MAPPED, MM_ST_FILE_PRIVATE, MM_ST_FILE_SHARED, MM_ST_VMO,
    };
    for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        reset();
        int rc = a20_cow_map_child(&child, 0x4000, 19, 0x55, classes[i], 0, NULL);
        assert(rc == 0);
        assert(map_cls_calls == 1 && map_huge_calls == 0);
        assert(seen_pgdir == child.pgdir && seen_va == 0x4000);
        assert(seen_pa == pfn_to_phys(19) && seen_flags == 0x55);
        assert(seen_cls == classes[i]);
    }
    for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        reset();
        int rc = a20_cow_map_child(&child, 0x200000, 23, 0xA5, classes[i], 1, NULL);
        assert(rc == 0);
        assert(map_cls_calls == 0 && map_huge_calls == 1);
        assert(seen_mm == &child && seen_va == 0x200000);
        assert(seen_pa == pfn_to_phys(23) && seen_flags == 0xA5);
        assert(seen_cls == classes[i]);
    }
    reset();
    map_cls_result = -12;
    assert(a20_cow_map_child(&child, 0x8000, 7, 0x11,
                             MM_ST_FILE_PRIVATE, 0, NULL) == -12);
    assert(map_cls_calls == 1 && map_huge_calls == 0);
    reset();
    map_huge_result = -95;
    assert(a20_cow_map_child(&child, 0x400000, 31, 0x22,
                             MM_ST_VMO, 1, NULL) == -95);
    assert(map_cls_calls == 0 && map_huge_calls == 1);
    puts("COW_MAP_CLASS: real C bridge PASS 4K/huge backing classes and errors");
    return 0;
}
'''


def extract_function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"expected one definition of {signature!r}")
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
    raise AssertionError("unterminated C function")


class CowMapClass(unittest.TestCase):
    def test_source_class_reaches_small_and_huge_mappers(self):
        source = (ROOT / "kernel/mm/cow.c").read_text()
        function = extract_function(source, "int a20_cow_map_child(")
        with tempfile.TemporaryDirectory(prefix="a20-cow-map-class-") as tmp:
            c_file = Path(tmp) / "map_class.c"
            exe = Path(tmp) / "map_class"
            c_file.write_text(HARNESS.replace("/* PRODUCTION_FUNCTION */", function))
            subprocess.run(
                ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 str(c_file), "-o", str(exe)],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(exe)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
