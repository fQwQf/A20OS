"""Exercise the real Rust user-copy loops while a COW replacement is ready.

The page-at-a-time loop moved from C to kernel/mm/usercopy.rs; this host oracle
compiles that module with the same callbacks the kernel links against and drives
it through a mapping that is replaced exactly at unlock time.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define PAGE_SIZE 4096u
#define USER_VA_LIMIT 0x10000u
#define EFAULT 14
#define NEED_FAULT 1
#define VM_READ 1
#define VM_WRITE 2
#define PTE_D 8

typedef uintptr_t pte_t;
typedef struct { int lock; } mm_t;
typedef struct { unsigned vm_flags; } mm_seg_t;
typedef struct { mm_t *mm; } task_t;

static mm_t mm;
static pte_t leaf;
static task_t task = { &mm };
static mm_seg_t vma = { VM_READ | VM_WRITE };
static unsigned char old_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static unsigned char new_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static unsigned pending, faults, grants, unlocks;

/* The COW replacement a fork/munmap would run once the page lock is dropped.
 * It retires the frame the copy resolved and installs a fresh leaf, so a copy
 * that did not hold the mapping would be lost with the old frame. */
static void replacement(void) {
    assert(!mm.lock);
    memcpy(new_page, old_page, PAGE_SIZE);
    leaf = (uintptr_t)new_page | 3;
    memset(old_page, 0xa5, PAGE_SIZE); /* retiring frame gets reused */
    pending = 0;
}

int a20_usercopy_page_access(void *t, void *m, uintptr_t address, int write,
                             unsigned char **kaddr, size_t *chunk,
                             uint64_t *irq, unsigned *preempt, unsigned *guard) {
    assert(t == &task && m == &mm);
    assert(!mm.lock);                 /* the caller must not already hold it */
    assert(address >= 0x1000 && address < 0x2000);
    if (!(vma.vm_flags & (write ? VM_WRITE : VM_READ)))
        return -EFAULT;               /* permission comes from the VMA */
    if (!(leaf & 1))
        return NEED_FAULT;            /* no present leaf: resolve outside the lock */
    mm.lock = 1;                      /* mapping held until page_unlock */
    if (write) leaf |= PTE_D;
    *kaddr = (unsigned char *)(leaf & ~(uintptr_t)(PAGE_SIZE - 1)) + (address - 0x1000);
    *chunk = PAGE_SIZE - (address & (PAGE_SIZE - 1));
    *irq = 42; *preempt = 0; *guard = 0;
    grants++;
    return 0;
}

int a20_usercopy_resolve_fault(void *t, void *m, uintptr_t address, int write,
                               uint64_t irq, unsigned preempt) {
    (void)t; (void)m; (void)write; (void)irq; (void)preempt;
    assert(!mm.lock);                 /* the resolver runs outside the lock */
    assert(address >= 0x1000 && address < 0x2000);
    faults++;
    leaf = (uintptr_t)old_page | 3;
    return 0;
}

void a20_usercopy_page_unlock(void *m, uint64_t irq, unsigned preempt,
                              unsigned guard) {
    (void)preempt; (void)guard;
    assert(m == &mm);
    assert(mm.lock && irq == 42);     /* still held across the copy */
    mm.lock = 0;
    unlocks++;
    if (pending) replacement();       /* a replacement racing the unlock */
}
'''


MAIN = r'''
long a20_usercopy_copy_from_rust(void *, void *, void *, const void *, size_t);
long a20_usercopy_copy_to_rust(void *, void *, void *, const void *, size_t);
long a20_user_strncpy_rust(void *, void *, char *, const char *, size_t);
long a20_user_strnlen_rust(void *, void *, const char *, size_t);

static void reset(void) {
    assert(!mm.lock);
    memset(old_page, 0, PAGE_SIZE);
    memset(new_page, 0, PAGE_SIZE);
    leaf = (uintptr_t)old_page | 3;
    vma.vm_flags = VM_READ | VM_WRITE;
    pending = faults = grants = unlocks = 0;
}

int main(void) {
    uint64_t value = 0xabc123456789ULL;

    /* A write resolves the old frame; the replacement runs at unlock, so the
     * value can only reach new_page if the copy ran while the mapping held. */
    reset(); pending = 1;
    assert(a20_usercopy_copy_to_rust(&task, &mm, (void *)0x1000, &value, 8) == 8);
    assert(!pending && !mm.lock);
    assert(*(uint64_t *)new_page == value);
    assert((leaf & PTE_D) == 0);      /* replacement built a fresh leaf */
    assert(grants == 1 && unlocks == 1);

    /* A read of the pinned frame; the frame is unchanged by the replacement. */
    reset(); memcpy(old_page, &value, 8); pending = 1;
    uint64_t got = 0;
    assert(a20_usercopy_copy_from_rust(&task, &mm, &got, (void *)0x1000, 8) == 8);
    assert(got == value && !mm.lock && !pending);

    /* Demand fault: the resolver runs outside the lock, the retry copies. */
    reset(); leaf = 0;
    assert(a20_usercopy_copy_to_rust(&task, &mm, (void *)0x1000, &value, 8) == 8);
    assert(faults == 1 && !mm.lock && (leaf & PTE_D));

    /* Negative control: a read-only VMA refuses the write with no fault call. */
    reset(); vma.vm_flags = VM_READ;
    assert(a20_usercopy_copy_to_rust(&task, &mm, (void *)0x1000, &value, 8) == -EFAULT);
    assert(!mm.lock && !faults && !grants);

    /* String helpers share the same held-mapping contract. */
    reset(); memcpy(old_page, "hello", 6);
    char str[16];
    assert(a20_user_strncpy_rust(&task, &mm, str, (void *)0x1000, sizeof(str)) == 5);
    assert(!strcmp(str, "hello") && !mm.lock);
    assert(a20_user_strnlen_rust(&task, &mm, (void *)0x1000, 16) == 5 && !mm.lock);
    assert(a20_user_strncpy_rust(&task, &mm, str, (void *)0x1000, 4) == 3 &&
           str[3] == 0 && !mm.lock);

    puts("usercopy-mapping-races: Rust loop holds the mapping across copy/string access");
    return 0;
}
'''


class UsercopyMappingRaces(unittest.TestCase):
    def test_copy_and_string_access_hold_the_mapping(self):
        rustc, cc = shutil.which("rustc"), shutil.which("cc")
        if not rustc or not cc:
            self.skipTest("rustc and cc are required for this host oracle")
        rust_source = (ROOT / "kernel/mm/usercopy.rs").read_text()
        rust_source = rust_source.replace("#![no_std]\n", "", 1)
        rust_source = rust_source.replace(
            "mod usercopy_strings;",
            f'#[path = "{ROOT / "kernel/mm/usercopy_strings.rs"}"] mod usercopy_strings;',
            1,
        )
        with tempfile.TemporaryDirectory(prefix="a20-usercopy-races-") as directory:
            tmp = Path(directory)
            rust_file, rust_lib = tmp / "usercopy.rs", tmp / "libusercopy.a"
            c_file, exe = tmp / "probe.c", tmp / "probe"
            rust_file.write_text(
                "#![no_std]\n" + rust_source +
                "\n#[panic_handler]\nfn panic(_: &core::panic::PanicInfo<'_>) -> ! { loop {} }\n"
            )
            c_file.write_text(PRELUDE + MAIN)
            subprocess.run(
                [rustc, "--edition=2021", "--crate-type", "staticlib", "-C", "panic=abort",
                 "-C", "opt-level=2", str(rust_file), "-o", str(rust_lib)],
                check=True, cwd=ROOT,
            )
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                 str(c_file), str(rust_lib), "-o", str(exe)],
                check=True, cwd=ROOT,
            )
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("holds the mapping", result.stdout)


if __name__ == "__main__":
    unittest.main()
