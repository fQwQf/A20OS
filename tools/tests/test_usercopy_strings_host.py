import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


def extract_c_function(source: str, signature: str) -> str:
    """Return one exact C function definition, failing closed if ambiguous."""
    if source.count(signature) != 1:
        raise AssertionError(f"expected exactly one C definition of {signature!r}")
    start = source.index(signature)
    brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing function body for {signature!r}")
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(f"unterminated C function {signature!r}")


HOST_PRELUDE = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define PAGE_SIZE 4096u
typedef uintptr_t vaddr_t;
typedef struct mm_struct { int unused; } mm_struct_t;
typedef struct task_t { mm_struct_t *mm; } task_t;
static task_t test_task;
static mm_struct_t test_mm;
static uint64_t test_user_limit;
#define USER_VA_LIMIT test_user_limit
static inline int user_range_ok(uint64_t va, size_t n);
static unsigned char *test_arena;
static unsigned char test_mapped[4];
static unsigned test_accesses;
static unsigned test_grants;
static unsigned test_unlocks;
static int test_held;
static int test_lock_error;
static int test_fault_page = -1;
static int test_fault_fail;
static unsigned test_fault_calls;

static task_t *proc_current(void) { return &test_task; }
static int user_access_begin(task_t *t, uint64_t va, int write,
                             void **kaddr, size_t *chunk, uint64_t *flags) {
    test_accesses++;
    if (!t || !t->mm || write || !user_range_ok(va, 1)) return -EFAULT;
    uintptr_t base = (uintptr_t)test_arena;
    if (va < base || va - base >= 4u * PAGE_SIZE) return -EFAULT;
    unsigned page = (unsigned)((va - base) / PAGE_SIZE);
    if (!test_mapped[page]) {
        if ((int)page != test_fault_page) return -EFAULT;
        test_fault_calls++;
        if (test_held != 0) test_lock_error = 1;
        if (test_fault_fail) return -EFAULT;
        test_mapped[page] = 1;
    }
    test_grants++;
    if (test_held != 0) test_lock_error = 1;
    test_held++;
    *kaddr = (void *)(uintptr_t)va;
    size_t page_left = PAGE_SIZE - (va & (PAGE_SIZE - 1));
    size_t user_left = (size_t)(USER_VA_LIMIT - va);
    *chunk = page_left < user_left ? page_left : user_left;
    *flags = 0;
    return 0;
}
static void user_access_end(task_t *t, uint64_t flags) {
    (void)t; (void)flags;
    test_unlocks++;
    if (test_held != 1) test_lock_error = 1;
    else test_held--;
}

int a20_usercopy_page_access(task_t *t, mm_struct_t *mm, uintptr_t address,
                             int write, void **kaddr, size_t *chunk,
                             uint64_t *irq, unsigned *preempt) {
    test_accesses++;
    if (!t || !mm || t->mm != mm || write || !user_range_ok(address, 1))
        return -EFAULT;
    uintptr_t base = (uintptr_t)test_arena;
    if (address < base || address - base >= 4u * PAGE_SIZE) return -EFAULT;
    unsigned page = (unsigned)((address - base) / PAGE_SIZE);
    if (!test_mapped[page])
        return (int)page == test_fault_page ? 1 : -EFAULT;
    test_grants++;
    if (test_held != 0) test_lock_error = 1;
    test_held++;
    *kaddr = (void *)address;
    size_t page_left = PAGE_SIZE - (address & (PAGE_SIZE - 1));
    size_t user_left = (size_t)(USER_VA_LIMIT - address);
    *chunk = page_left < user_left ? page_left : user_left;
    *irq = 0; *preempt = 0;
    return 0;
}
int a20_usercopy_resolve_fault(task_t *t, mm_struct_t *mm, uintptr_t address,
                               int write, uint64_t irq, unsigned preempt) {
    (void)t; (void)mm; (void)write; (void)irq; (void)preempt;
    if (test_held != 0) test_lock_error = 1;
    test_fault_calls++;
    uintptr_t base = (uintptr_t)test_arena;
    if (address < base || address - base >= 4u * PAGE_SIZE) return -EFAULT;
    unsigned page = (unsigned)((address - base) / PAGE_SIZE);
    if ((int)page != test_fault_page || test_fault_fail) return -EFAULT;
    test_mapped[page] = 1;
    return 0;
}
void a20_usercopy_page_unlock(mm_struct_t *mm, uint64_t irq, unsigned preempt) {
    (void)mm; (void)irq; (void)preempt;
    test_unlocks++;
    if (test_held != 1) test_lock_error = 1;
    else test_held--;
}

static int same_bytes(const unsigned char *a, const unsigned char *b, size_t n) {
    return memcmp(a, b, n) == 0;
}
static int fail(const char *name, const char *detail) {
    fprintf(stderr, "FAIL %s: %s\n", name, detail);
    return 1;
}
"""


HOST_MAIN = r"""
long user_strncpy(char *dst, const char *src, size_t max);
long user_strnlen(const char *src, size_t max);
long a20_user_strncpy_rust(task_t *, mm_struct_t *, char *, const char *, size_t);
long a20_user_strnlen_rust(task_t *, mm_struct_t *, const char *, size_t);

static void reset(unsigned mapped_pages, uintptr_t limit) {
    memset(test_mapped, 0, sizeof(test_mapped));
    for (unsigned i = 0; i < mapped_pages; i++) test_mapped[i] = 1;
    test_accesses = 0;
    test_grants = 0;
    test_unlocks = 0;
    test_held = 0;
    test_lock_error = 0;
    test_fault_page = -1;
    test_fault_fail = 0;
    test_fault_calls = 0;
    test_user_limit = limit;
}

static int pair_len(const char *name, const char *src, size_t max,
                    uintptr_t limit, unsigned mapped, long expected,
                    unsigned expected_accesses) {
    reset(mapped, limit);
    long c = user_strnlen(src, max);
    unsigned c_accesses = test_accesses;
    unsigned c_grants = test_grants, c_unlocks = test_unlocks;
    int c_lock_error = test_lock_error || test_held != 0;
    reset(mapped, limit);
    long r = a20_user_strnlen_rust(&test_task, &test_mm, src, max);
    unsigned r_accesses = test_accesses;
    unsigned r_grants = test_grants, r_unlocks = test_unlocks;
    int r_lock_error = test_lock_error || test_held != 0;
    if (c != expected || r != expected || c != r)
        return fail(name, "C/Rust/expected result mismatch");
    if (c_accesses != expected_accesses || r_accesses != expected_accesses)
        return fail(name, "unexpected page-access probe count");
    if (c_lock_error || r_lock_error)
        return fail(name, "lock guard leaked or resolver ran under access lock");
    if (c_grants != c_unlocks || r_grants != r_unlocks)
        return fail(name, "page grant/unlock counts differ");
    return 0;
}

static int pair_copy(const char *name, const char *src, size_t max,
                     uintptr_t limit, unsigned mapped, long expected,
                     size_t expected_prefix, int expect_terminator,
                     unsigned expected_accesses,
                     unsigned expected_rust_accesses) {
    unsigned char *c_dst = malloc(max ? max : 1);
    unsigned char *r_dst = malloc(max ? max : 1);
    if (!c_dst || !r_dst) return fail(name, "host allocation failed");
    memset(c_dst, 0xa5, max ? max : 1);
    memset(r_dst, 0xa5, max ? max : 1);
    reset(mapped, limit);
    long c = user_strncpy((char *)c_dst, src, max);
    unsigned c_accesses = test_accesses;
    unsigned c_grants = test_grants, c_unlocks = test_unlocks;
    int c_lock_error = test_lock_error || test_held != 0;
    reset(mapped, limit);
    long r = a20_user_strncpy_rust(&test_task, &test_mm, (char *)r_dst,
                                   src, max);
    unsigned r_accesses = test_accesses;
    unsigned r_grants = test_grants, r_unlocks = test_unlocks;
    int r_lock_error = test_lock_error || test_held != 0;
    if (c != expected || r != expected || c != r) {
        free(c_dst); free(r_dst);
        return fail(name, "C/Rust/expected result mismatch");
    }
    if (c_accesses != expected_accesses || r_accesses != expected_rust_accesses) {
        free(c_dst); free(r_dst);
        return fail(name, "unexpected page-access probe count");
    }
    if (c_lock_error || r_lock_error) {
        free(c_dst); free(r_dst);
        return fail(name, "lock guard leaked");
    }
    if (c_grants != c_unlocks || r_grants != r_unlocks) {
        free(c_dst); free(r_dst);
        return fail(name, "page grant/unlock counts differ");
    }
    if (!same_bytes(c_dst, r_dst, max ? max : 1)) {
        free(c_dst); free(r_dst);
        return fail(name, "C/Rust destination bytes differ");
    }
    if (expected_prefix && memcmp(c_dst, src, expected_prefix) != 0) {
        free(c_dst); free(r_dst);
        return fail(name, "expected copied prefix is absent");
    }
    if (expect_terminator && c_dst[max - 1] != 0) {
        free(c_dst); free(r_dst);
        return fail(name, "expected destination terminator is absent");
    }
    if (!expect_terminator && max && c_dst[max - 1] != 0xa5) {
        free(c_dst); free(r_dst);
        return fail(name, "fault path unexpectedly wrote a terminator");
    }
    free(c_dst); free(r_dst);
    return 0;
}

int main(void) {
    test_arena = aligned_alloc(PAGE_SIZE, 4u * PAGE_SIZE);
    if (!test_arena) return fail("setup", "aligned_alloc failed");
    test_task.mm = &test_mm;
    memset(test_arena, 'x', 4u * PAGE_SIZE);
    int failed = 0;

    memcpy(test_arena + 16, "a20\0tail", 8);
    failed |= pair_len("embedded-nul", (char *)test_arena + 16, 7,
                       (uintptr_t)test_arena + 4u * PAGE_SIZE, 4, 3, 1);
    failed |= pair_copy("copy-through-nul", (char *)test_arena + 16, 8,
                        (uintptr_t)test_arena + 4u * PAGE_SIZE, 4, 3, 4, 0, 1, 1);
    failed |= pair_copy("truncate", (char *)test_arena + 16, 3,
                        (uintptr_t)test_arena + 4u * PAGE_SIZE, 4, 2, 2, 1, 1, 1);

    failed |= pair_copy("max-one-no-probe", (char *)test_arena + 3u * PAGE_SIZE,
                        1, (uintptr_t)test_arena + 3u * PAGE_SIZE, 0, 0, 0, 1, 0, 0);
    failed |= pair_copy("max-zero-no-probe", (char *)test_arena + 3u * PAGE_SIZE,
                        0, (uintptr_t)test_arena + 3u * PAGE_SIZE, 0, -EINVAL, 0, 0, 0, 0);
    failed |= pair_len("strnlen-zero-no-probe", (char *)test_arena + 3u * PAGE_SIZE,
                       0, (uintptr_t)test_arena + 3u * PAGE_SIZE, 0, 0, 0);

    test_arena[PAGE_SIZE - 1] = 0;
    failed |= pair_len("nul-at-page-end-no-next-probe",
                       (char *)test_arena + PAGE_SIZE - 1, 4,
                       (uintptr_t)test_arena + 4u * PAGE_SIZE, 1, 0, 1);
    failed |= pair_copy("copy-nul-at-page-end-no-next-probe",
                        (char *)test_arena + PAGE_SIZE - 1, 4,
                        (uintptr_t)test_arena + 4u * PAGE_SIZE, 1, 0, 1, 0, 1, 1);

    test_arena[PAGE_SIZE - 1] = 'd';
    test_arena[PAGE_SIZE] = 0;
    reset(1, (uintptr_t)test_arena + 4u * PAGE_SIZE);
    test_fault_page = 1;
    long demand_c = user_strnlen((char *)test_arena + PAGE_SIZE - 1, 2);
    unsigned demand_c_calls = test_accesses, demand_c_grants = test_grants;
    unsigned demand_c_unlocks = test_unlocks;
    unsigned demand_c_faults = test_fault_calls;
    int demand_c_lock_error = test_lock_error || test_held != 0;
    reset(1, (uintptr_t)test_arena + 4u * PAGE_SIZE);
    test_fault_page = 1;
    long demand_r = a20_user_strnlen_rust(&test_task, &test_mm,
                                          (char *)test_arena + PAGE_SIZE - 1, 2);
    unsigned demand_r_calls = test_accesses, demand_r_grants = test_grants;
    unsigned demand_r_unlocks = test_unlocks;
    unsigned demand_r_faults = test_fault_calls;
    if (demand_c != 1 || demand_r != 1 || demand_c_calls != 2 ||
        demand_r_calls != 3 || demand_c_grants != 2 || demand_r_grants != 2 ||
        demand_c_unlocks != demand_c_grants || demand_r_unlocks != demand_r_grants ||
        demand_c_faults != 1 || demand_r_faults != 1 || demand_c_lock_error ||
        test_lock_error || test_held != 0)
        failed |= fail("demand-fault-retry", "resolver retry or page grant mismatch");

    reset(1, (uintptr_t)test_arena + 4u * PAGE_SIZE);
    test_fault_page = 1;
    test_fault_fail = 1;
    long fault_c = user_strnlen((char *)test_arena + PAGE_SIZE - 1, 2);
    unsigned fault_c_calls = test_accesses, fault_c_grants = test_grants;
    unsigned fault_c_unlocks = test_unlocks;
    unsigned fault_c_resolves = test_fault_calls;
    int fault_c_lock_error = test_lock_error || test_held != 0;
    reset(1, (uintptr_t)test_arena + 4u * PAGE_SIZE);
    test_fault_page = 1;
    test_fault_fail = 1;
    long fault_r = a20_user_strnlen_rust(&test_task, &test_mm,
                                         (char *)test_arena + PAGE_SIZE - 1, 2);
    unsigned fault_r_calls = test_accesses, fault_r_grants = test_grants;
    unsigned fault_r_unlocks = test_unlocks;
    unsigned fault_r_resolves = test_fault_calls;
    if (fault_c != -EFAULT || fault_r != -EFAULT || fault_c_calls != 2 ||
        fault_r_calls != 2 || fault_c_grants != 1 || fault_r_grants != 1 ||
        fault_c_unlocks != fault_c_grants || fault_r_unlocks != fault_r_grants ||
        fault_c_resolves != 1 || fault_r_resolves != 1 || fault_c_lock_error ||
        test_lock_error || test_held != 0)
        failed |= fail("demand-fault-failure", "failed resolver path mismatch");

    test_arena[PAGE_SIZE - 2] = 'z';
    reset(1, (uintptr_t)test_arena + PAGE_SIZE - 1);
    long limit_c = user_strnlen((char *)test_arena + PAGE_SIZE - 2, 2);
    unsigned limit_c_calls = test_accesses, limit_c_grants = test_grants;
    unsigned limit_c_unlocks = test_unlocks;
    int limit_c_lock_error = test_lock_error || test_held != 0;
    reset(1, (uintptr_t)test_arena + PAGE_SIZE - 1);
    long limit_r = a20_user_strnlen_rust(&test_task, &test_mm,
                                         (char *)test_arena + PAGE_SIZE - 2, 2);
    unsigned limit_r_calls = test_accesses, limit_r_grants = test_grants;
    unsigned limit_r_unlocks = test_unlocks;
    int limit_r_lock_error = test_lock_error || test_held != 0;
    if (limit_c != -EFAULT || limit_r != -EFAULT || limit_c_calls != 1 ||
        limit_r_calls != 2 || limit_c_grants != 1 || limit_r_grants != 1 ||
        limit_c_unlocks != limit_c_grants || limit_r_unlocks != limit_r_grants ||
        limit_c_lock_error || limit_r_lock_error)
        failed |= fail("user-limit-next-byte-rejected",
                       "range rejection or page-access grant count mismatch");
    failed |= pair_copy("partial-copy-limit-error-no-terminator",
                        (char *)test_arena + PAGE_SIZE - 2, 4,
                        (uintptr_t)test_arena + PAGE_SIZE - 1, 1, -EFAULT, 1, 0, 1, 2);

    /* C/Rust agree on a missing second page, and the successful first-page
       writes remain visible while no synthetic NUL is promised on error. */
    memset(test_arena, 'q', 2u * PAGE_SIZE);
    failed |= pair_len("second-page-unmapped", (char *)test_arena, PAGE_SIZE + 1,
                       (uintptr_t)test_arena + 4u * PAGE_SIZE, 1, -EFAULT, 2);
    failed |= pair_copy("partial-copy-before-unmapped-page", (char *)test_arena,
                        PAGE_SIZE + 2, (uintptr_t)test_arena + 4u * PAGE_SIZE,
                        1, -EFAULT, PAGE_SIZE, 0, 2, 2);

    free(test_arena);
    if (failed) return 1;
    puts("usercopy-strings-host: C extracted helpers and production Rust module agree");
    return 0;
}
"""


class UsercopyStringsHostTests(unittest.TestCase):
    def test_rust_module_is_a_rebuild_prerequisite(self):
        makefile = (ROOT / "tools/rust-kernel.mk").read_text()
        self.assertIn("RUST_USERCOPY_SUBMODULES := kernel/mm/usercopy_strings.rs", makefile)
        self.assertIn(
            "$(RUST_USERCOPY_OBJECT): $(RUST_USERCOPY_SRC) $(RUST_USERCOPY_SUBMODULES)",
            makefile,
        )

    def test_extracted_c_and_production_rust_helpers(self):
        gcc = shutil.which("gcc")
        rustc = shutil.which("rustc")
        if not gcc or not rustc:
            self.skipTest("gcc and rustc are required for this host oracle")
        mm_source = (ROOT / "kernel/mm/mm.c").read_text()
        exact_c = "\n\n".join(
            extract_c_function(mm_source, signature)
            for signature in (
                "static inline int user_range_ok(",
                "long user_strncpy(",
                "long user_strnlen(",
            )
        )
        with tempfile.TemporaryDirectory(prefix="a20-usercopy-strings-") as tmp:
            tmp = pathlib.Path(tmp)
            c_file = tmp / "host.c"
            rust_file = tmp / "usercopy_host.rs"
            rust_lib = tmp / "libusercopy.a"
            exe = tmp / "test-usercopy-strings"
            c_file.write_text(HOST_PRELUDE + "\n" + exact_c + "\n" + HOST_MAIN)
            rust_source = (ROOT / "kernel/mm/usercopy.rs").read_text()
            rust_source = rust_source.replace("#![no_std]\n", "", 1)
            rust_source = rust_source.replace(
                "mod usercopy_strings;",
                f'#[path = "{ROOT / "kernel/mm/usercopy_strings.rs"}"] mod usercopy_strings;',
                1,
            )
            rust_file.write_text(
                "#![no_std]\n" + rust_source +
                "\n#[panic_handler]\nfn test_panic(_: &core::panic::PanicInfo<'_>) -> ! { loop {} }\n"
            )
            subprocess.run(
                [rustc, "--edition=2021", "--crate-type", "staticlib", "-C", "panic=abort",
                 "-C", "opt-level=2", str(rust_file), "-o", str(rust_lib)],
                check=True, cwd=ROOT,
            )
            subprocess.run(
                [gcc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", str(c_file),
                 str(rust_lib), "-o", str(exe)],
                check=True, cwd=ROOT,
            )
            result = subprocess.run([str(exe)], check=False, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("C extracted helpers and production Rust module agree", result.stdout)


if __name__ == "__main__":
    unittest.main()
