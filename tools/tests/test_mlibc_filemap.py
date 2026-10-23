#!/usr/bin/env python3
"""Real mlibc vm_map layout/sysdep regression against the canonical Native SDK."""
from pathlib import Path
import subprocess
import tempfile
from test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
struct VmMap {};
template<class T> struct Sysdeps;
template<> struct Sysdeps<VmMap> {
    int operator()(void *, size_t, int, int, int, off_t, void **);
};
static int error, mismatch, calls;
static int a20_to_errno(int64_t value) { return (int)-value + 100; }
static a20_handle_t fd_handle(int fd) { return fd == 14 ? 9 : A20_HANDLE_NULL; }
static int64_t a20_syscall6(uint64_t number, uint64_t pointer,
                          uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    calls++;
    if (number == A20_SYS_vm_map) {
        auto &a = *(a20_vm_map_args *)pointer;
        mismatch |= a.size != sizeof(canonical_map) || a.version != 1 ||
            a.vmar != A20_HANDLE_NULL || a.length != 4096 || a.flags != MAP_PRIVATE;
        if (a.source == A20_HANDLE_NULL) {
            mismatch |= a.addr_hint != 0 || a.offset != 0 || a.prot != 3;
            a.out_addr = 0x9876000;
        } else {
            mismatch |= a.source != 9 || a.addr_hint != 0x10000 ||
                a.offset != 8192 || a.prot != 5;
            a.out_addr = 0x1234000;
        }
    } else if (number == A20_SYS_vm_alloc) {
        auto &a = *(a20_vm_alloc_args *)pointer;
        mismatch |= a.length != 4096 || a.prot != 3;
        a.out_addr = 0x9876000;
    } else mismatch++;
    return error;
}
'''
MAIN = r'''
int main() {
    void *out = nullptr;
    if (sizeof(a20_vm_map_args) != sizeof(canonical_map) ||
        offsetof(a20_vm_map_args, vmar) != offsetof(canonical_map, vmar)) return 24;
    int result = Sysdeps<VmMap>{}((void *)0x10000, 4096, PROT_READ | PROT_EXEC,
                                 MAP_PRIVATE, 14, 8192, &out);
    if (result || mismatch || out != (void *)0x1234000 || calls != 1) return 23;
    error = -A20_ERR_FAULT;
    if (Sysdeps<VmMap>{}((void *)0x10000, 4096, PROT_READ | PROT_EXEC,
                        MAP_PRIVATE, 14, 8192, &out) != a20_to_errno(error)) return 25;
    error = 0;
    if (Sysdeps<VmMap>{}(nullptr, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, &out) ||
                        mismatch || out != (void *)0x9876000) return 26;
    int previous = calls;
    if (Sysdeps<VmMap>{}(nullptr, 4096, PROT_READ, MAP_PRIVATE, -1, 0, &out) != EBADF ||
        calls != previous) return 27;
    return 0;
}
'''


def main():
    cpp = (ROOT / 'user/external/mlibc/sysdeps/a20/sysdeps.cpp').read_text()
    sdk = (ROOT / 'user/liba20rt/a20_types.h').read_text()
    canonical = extract_function(sdk, 'typedef struct a20_vm_map_args')
    canonical = canonical.replace('typedef struct a20_vm_map_args', 'struct canonical_map') + ';'
    body = extract_function(cpp, 'int Sysdeps<VmMap>::operator()')
    prot = extract_function(cpp, 'static uint32_t prot_to_a20')
    negative = body.replace('.vmar = A20_HANDLE_NULL', '.vmar = 0')
    assert body != negative
    prefix = '''#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <sys/mman.h>
#define a20_syscall6 a20_unused_syscall6
#include "user/external/mlibc/sysdeps/a20/a20.hpp"
#undef a20_syscall6
'''
    with tempfile.TemporaryDirectory(prefix='a20-filemap-') as directory:
        directory = Path(directory)
        for name, function, expected in [('fixed', body, 0), ('null-handle-negative', negative, 23)]:
            source, program = directory / (name + '.cpp'), directory / name
            source.write_text(prefix + canonical + HARNESS + prot + function + MAIN)
            subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT), str(source), '-o', str(program)], check=True)
            result = subprocess.run([str(program)], check=False)
            if result.returncode != expected:
                raise SystemExit(f'{name}: expected {expected}, got {result.returncode}')
    print('test-mlibc-filemap: PASS real layout, Native NULL handle, mapping/error/anonymous paths, negative control')


if __name__ == '__main__':
    main()
