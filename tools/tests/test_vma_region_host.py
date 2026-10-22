import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

HARNESS = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define VM_FILE (1ULL << 13)
#define VM_VMO (1ULL << 14)
#define VM_SYSV_SHM (1ULL << 16)
typedef struct mm_seg {
    uintptr_t start, end;
    uint64_t backing_offset, vm_flags;
    uintptr_t pte_flags;
    uint32_t vmar_cap;
    void *vmar_owner, *file, *vmo;
    struct mm_seg *prev, *next;
} mm_seg_t;
struct merge_snapshot {
    uintptr_t start, end;
    uint64_t backing_offset, vm_flags;
    uintptr_t pte_flags;
    uint32_t vmar_cap, reserved;
    const void *vmar_owner, *file, *vmo, *prev, *next;
};
int a20_vma_merge_snapshot(const void *node, struct merge_snapshot *out) {
    const struct mm_seg *v = node;
    if (!v || !out) return -1;
    *out = (struct merge_snapshot){
        .start=v->start, .end=v->end, .backing_offset=v->backing_offset,
        .vm_flags=v->vm_flags, .pte_flags=v->pte_flags,
        .vmar_cap=v->vmar_cap, .vmar_owner=v->vmar_owner,
        .file=v->file, .vmo=v->vmo, .prev=v->prev, .next=v->next,
    };
    return 0;
}
struct region_snapshot { uintptr_t start, end; const void *next; };
int a20_vma_region_read(const void *node, struct region_snapshot *out) {
    const struct mm_seg *v = node;
    if (!v || !out) return -1;
    out->start=v->start; out->end=v->end; out->next=v->next; return 0;
}
void *a20_vma_split_alloc(uint32_t test_split) { (void)test_split; return NULL; }
int a20_vma_split_clone(void *a, void *b, uintptr_t c) { (void)a;(void)b;(void)c;return -12; }
int a20_vma_split_ref(void *a, uint32_t b) { (void)a;(void)b;return 0; }
void a20_vma_split_unref(void *a, uint32_t b) { (void)a;(void)b; }
void a20_vma_split_free(void *a) { (void)a; }
void a20_vma_split_retire(void *a, void *b) { (void)a;(void)b; }
void a20_vma_split_publish(void *a, void *b, void *c, uintptr_t d) { (void)a;(void)b;(void)c;(void)d; }
void a20_vma_split_annotate(void *a, void *b, void *c) { (void)a;(void)b;(void)c; }
void a20_vma_merge_commit(void *a, void *b, void *c) { (void)a;(void)b;(void)c; }
int a20_vma_can_merge_rust(const void *, const void *);
static int check(struct mm_seg a, struct mm_seg b, int expected, const char *name) {
    int r = a20_vma_can_merge_rust(&a, &b);
    if (r != expected) {
        fprintf(stderr, "VMA_HOST: FAIL %s Rust=%d expected=%d\n", name, r, expected);
        return 1;
    }
    return 0;
}
int main(void) {
    int failures = 0;
    struct mm_seg a = {.start=0x1000,.end=0x2000,.vm_flags=0,.backing_offset=UINT64_MAX};
    struct mm_seg b = {.start=0x2000,.end=0x3000,.vm_flags=0};
    failures += check(a,b,1,"anonymous-offset-ignored");
    a.vm_flags=VM_FILE; a.backing_offset=0x4000; a.file=(void *)7;
    b.vm_flags=VM_FILE; b.backing_offset=0x5000; b.file=(void *)7;
    failures += check(a,b,1,"file-contiguous");
    b.backing_offset=0x6000;
    failures += check(a,b,0,"file-offset-gap");
    b.backing_offset=0x5000; b.file=(void *)8;
    failures += check(a,b,0,"file-identity");
    a.vm_flags=VM_VMO; a.backing_offset=0x100; a.vmo=(void *)9;
    b.vm_flags=VM_VMO; b.backing_offset=0x1100; b.vmo=(void *)9;
    failures += check(a,b,1,"vmo-contiguous");
    b.vmo=(void *)10;
    failures += check(a,b,0,"vmo-identity");
    a.vm_flags=0; b.vm_flags=0; a.pte_flags=1; b.pte_flags=2;
    failures += check(a,b,0,"pte-policy");
    a.pte_flags=b.pte_flags=0; a.vmar_cap=1; b.vmar_cap=2;
    failures += check(a,b,0,"capability-policy");
    a.vmar_cap=b.vmar_cap=0; a.vm_flags=VM_SYSV_SHM; b.vm_flags=VM_SYSV_SHM;
    failures += check(a,b,0,"sysv-never-merges");
    b.start=0x2100;
    failures += check(a,b,0,"nonadjacent");
    if (failures) return 1;
    puts("VMA_HOST: production Rust merge policy matches documented contract");
    return 0;
}
"""


class VmaRegionHostTest(unittest.TestCase):
    def test_rust_merge_policy_against_documented_contract(self):
        with tempfile.TemporaryDirectory(prefix="a20-vma-region-") as directory:
            tmp = pathlib.Path(directory)
            rust_obj = tmp / "vma_region.o"
            c_file = tmp / "host.c"
            exe = tmp / "host"
            subprocess.run([
                "rustc", "--edition=2021", "--crate-type=lib", "--emit=obj",
                "-C", "panic=abort", "-C", "opt-level=2",
                str(ROOT / "kernel/mm/vma_region.rs"), "-o", str(rust_obj),
            ], check=True, cwd=ROOT)
            c_file.write_text(HARNESS)
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                str(c_file), str(rust_obj), "-o", str(exe),
            ], check=True, cwd=ROOT)
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("matches documented contract", result.stdout)


if __name__ == "__main__":
    unittest.main()
