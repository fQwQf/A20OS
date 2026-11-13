#!/usr/bin/env python3
"""Exercise the production Landlock create ABI before any ruleset allocation."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define PAGE_SIZE 4096
#define LANDLOCK_ACCESS_FS_WRITE_FILE 2ULL
#define LANDLOCK_ACCESS_FS_READ_FILE 4ULL
#define LANDLOCK_ACCESS_FS_MAKE_FIFO (1ULL << 10)
static int allocations, copies;
static size_t readable;
static const void *user_base;
static int copy_from_user(void *dst, const void *src, size_t n) {
    copies++;
    uintptr_t off = (uintptr_t)src - (uintptr_t)user_base;
    if (off > readable || n > readable - off) return -1;
    memcpy(dst, src, n);
    return 0;
}
static int landlock_create_ruleset(uint64_t handled) {
    if (handled != 4 && handled != LANDLOCK_ACCESS_FS_MAKE_FIFO) return -99;
    allocations++;
    return 17;
}
'''
HARNESS = r'''
static int check(const void *attr, size_t size, unsigned flags, int expected) {
    allocations = copies = 0;
    int64_t r = sys_landlock_create_ruleset(attr, size, flags);
    return r == expected && allocations == (expected == 17);
}
int main(void) {
    uint64_t attr[10] = {4};
    user_base = attr; readable = sizeof(attr);
    if (!check(attr, 8, 8, -EINVAL)) return 1;
    if (!check(attr, 7, 0, -EINVAL) || copies) return 2;
    if (!check(attr, PAGE_SIZE + 1, 0, -E2BIG) || copies) return 3;
    if (!check(NULL, 8, 0, -EFAULT)) return 4;
    readable = 7;
    if (!check(attr, 8, 0, -EFAULT)) return 5;
    readable = sizeof(attr);
    attr[9] = 1;
    if (!check(attr, sizeof(attr), 0, -E2BIG)) return 6;
    attr[9] = 0;
    if (!check(attr, sizeof(attr), 0, 17)) return 7;
    readable = 8;
    if (!check(attr, 16, 0, -EFAULT)) return 8;
    attr[0] = 0;
    if (!check(attr, 8, 0, -ENOMSG)) return 9;
    for (unsigned i = 0; i < 64; i++) {
        if (i == 1 || i == 2 || i == 10) continue;
        attr[0] = 1ULL << i;
        if (!check(attr, 8, 0, -EOPNOTSUPP)) return 10;
    }
    attr[0] = LANDLOCK_ACCESS_FS_MAKE_FIFO;
    if (!check(attr, 8, 0, 17)) return 12;
    attr[0] = 4;
    return check(attr, 8, 0, 17) ? 0 : 11;
}
'''

def main():
    source = (ROOT / 'kernel/abi/linux/sys_landlock.c').read_text()
    start = source.index('int64_t sys_landlock_create_ruleset(')
    body = source[start:source.index('\nint64_t sys_landlock_add_rule(', start)]
    # Removing the flags check recreates the original false-success request.
    negative = body.replace('if (flags || size < sizeof(uint64_t))',
                            'if (size < sizeof(uint64_t))', 1)
    if negative == body:
        raise SystemExit('negative control did not modify production boundary')
    with tempfile.TemporaryDirectory(prefix='a20-landlock-create-') as tmp:
        for name, code, expected in [('current', body, 0), ('ignored-flags', negative, 1)]:
            src, exe = Path(tmp) / (name + '.c'), Path(tmp) / name
            src.write_text(PRELUDE + code + HARNESS)
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], check=False)
            if result.returncode != expected:
                raise SystemExit(f'{name}: expected {expected}, got {result.returncode}')
    print('landlock-create: PASS (production ABI, no allocation on refusal, negative control)')

if __name__ == '__main__':
    main()
