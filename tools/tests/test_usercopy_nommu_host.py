import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


def extract_function(source, signature):
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
    raise AssertionError(f"unterminated definition of {signature!r}")


PRELUDE = r"""
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define CONFIG_NOMMU 1
#define PAGE_SIZE 4096u
typedef uintptr_t vaddr_t;
typedef struct mm_struct { int unused; } mm_struct_t;
typedef struct task_t { mm_struct_t *mm; } task_t;
static task_t task;
static mm_struct_t mm;
static uint64_t user_limit;
#define USER_VA_LIMIT user_limit
static unsigned unlock_calls;
static int arch_irqs_enabled(void) { return 1; }
static unsigned preempt_count(void) { return 0; }
static int user_range_ok(uint64_t va, size_t n);
"""


MAIN = r"""
long a20_usercopy_copy_from_rust(void *, void *, void *, const void *, size_t);
long a20_usercopy_copy_to_rust(void *, void *, void *, const void *, size_t);
long a20_user_strncpy_rust(void *, void *, char *, const char *, size_t);
long a20_user_strnlen_rust(void *, void *, const char *, size_t);
int main(void) {
    unsigned char arena[2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
    unsigned char out[8];
    task.mm = &mm;
    memset(arena, 'n', sizeof(arena));
    user_limit = (uintptr_t)arena + PAGE_SIZE + 3;
    void *kaddr = NULL; size_t chunk = 0; uint64_t irq = 0; unsigned preempt = 99;
    int access = a20_usercopy_page_access(&task, &mm, (uintptr_t)arena + PAGE_SIZE - 2,
                                          0, &kaddr, &chunk, &irq, &preempt);
    if (access != 0 || kaddr != arena + PAGE_SIZE - 2 || chunk != 2 || irq != 1 || preempt != 0) {
        fprintf(stderr, "access=%d kaddr=%p expected=%p chunk=%zu irq=%llu preempt=%u limit=%llu\\n",
                access, kaddr, arena + PAGE_SIZE - 2, chunk, (unsigned long long)irq,
                preempt, (unsigned long long)user_limit);
        return 1;
    }
    a20_usercopy_page_unlock(&mm, irq, preempt);
    memset(out, 0x55, sizeof(out));
    if (a20_usercopy_copy_from_rust(&task, &mm, out, arena + PAGE_SIZE - 2, 6) != -EFAULT ||
        memcmp(out, "nnnnn", 5) != 0 || out[5] != 0x55)
        return 2;
    memset(out, 'z', sizeof(out));
    if (a20_user_strncpy_rust(&task, &mm, (char *)out, (char *)arena + PAGE_SIZE - 2, 7) != -EFAULT ||
        memcmp(out, "nnnnn", 5) != 0 || out[5] != 'z')
        return 3;
    if (a20_user_strnlen_rust(&task, &mm, (char *)arena + PAGE_SIZE, 4) != -EFAULT)
        return 4;
    if (a20_user_strnlen_rust(&task, &mm, (char *)arena, 0) != 0)
        return 5;
    if (unlock_calls == 0) return 6;
    puts("usercopy-nommu-host: production C direct-map adapter and Rust loops PASS");
    return 0;
}
"""


class UsercopyNommuHostTests(unittest.TestCase):
    def test_nommu_direct_map_adapter_and_rust_contract(self):
        rustc, gcc = shutil.which("rustc"), shutil.which("gcc")
        if not rustc or not gcc:
            self.skipTest("rustc and gcc are required for the NOMMU host oracle")
        mm_source = (ROOT / "kernel/mm/mm.c").read_text()
        c_functions = "\n\n".join(
            extract_function(mm_source, signature)
            for signature in (
                "static inline int user_range_ok(",
                "int a20_usercopy_page_access(",
                "int a20_usercopy_resolve_fault(",
                "void a20_usercopy_page_unlock(",
            )
        )
        # Count the production no-op unlock in the extracted host copy so the
        # RAII guard's Drop remains observable without changing kernel source.
        c_functions = c_functions.replace(
            "    (void)mm; (void)irq_flags; (void)preempt_before;",
            "    (void)mm; (void)irq_flags; (void)preempt_before; unlock_calls++;",
            1,
        )
        rust_source = (ROOT / "kernel/mm/usercopy.rs").read_text()
        rust_source = rust_source.replace("#![no_std]\n", "", 1)
        rust_source = rust_source.replace(
            "mod usercopy_strings;",
            f'#[path = "{ROOT / "kernel/mm/usercopy_strings.rs"}"] mod usercopy_strings;',
            1,
        )
        with tempfile.TemporaryDirectory(prefix="a20-usercopy-nommu-") as directory:
            tmp = pathlib.Path(directory)
            rust_file, rust_lib = tmp / "usercopy.rs", tmp / "libusercopy.a"
            c_file, exe = tmp / "host.c", tmp / "host"
            rust_file.write_text(
                "#![no_std]\n" + rust_source +
                "\n#[panic_handler]\nfn panic(_: &core::panic::PanicInfo<'_>) -> ! { loop {} }\n"
            )
            c_file.write_text(
                PRELUDE + c_functions + MAIN
            )
            subprocess.run(
                [rustc, "--edition=2021", "--crate-type", "staticlib", "-C", "panic=abort",
                 "-C", "opt-level=2", str(rust_file), "-o", str(rust_lib)],
                check=True, cwd=ROOT,
            )
            subprocess.run(
                [gcc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                 str(c_file), str(rust_lib), "-o", str(exe)],
                check=True, cwd=ROOT,
            )
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("production C direct-map adapter", result.stdout)


if __name__ == "__main__":
    unittest.main()
