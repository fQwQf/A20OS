#!/usr/bin/env python3
"""Run the real Native MMIO ABI boundary with a bounded device window."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define PAGE_SIZE 4096
#define A20_ERR_FAULT 12
#define A20_ERR_BAD_HANDLE 9
#define A20_ERR_ACCESS 13
#define A20_ERR_INVALID_ARGUMENT 22
#define A20_OK 0
#define A20_ARG(n) (args->arg[n])
typedef struct { uint64_t arg[6]; } a20_syscall_args_t;
typedef struct {
    uint32_t size, version, prot, pad;
    uint64_t phys_base, length, out_addr;
} a20_device_map_mmio_args_t;
#define A20_VALIDATE_AND_COPY(p, local) memcpy(&(local), p, sizeof(local))
typedef struct { void *mm; int pid; } task_t;
static task_t task = { (void *)1, 7 };
static int maps, live, owner = 7, fault;
static uint64_t window_end = 0x3000, last_length;
static task_t *proc_current(void) { return &task; }
int udriver_mmio_user_owned(uint64_t pa) { (void)pa; return 1; }
int udriver_claim_owner(uint64_t pa) { (void)pa; return owner; }
static int udriver_map_mmio(void *mm, uint64_t pa, uint64_t len,
                            uint32_t prot, uint64_t *out) {
    assert(mm == task.mm && prot && !(prot & ~3U));
    maps++; last_length = len;
    if (pa < 0x2000 || pa + len > window_end) return -1;
    live++; *out = 0x100000;
    return 0;
}
static int a20_copy_struct_to_user(void *p, const void *src, size_t n) {
    if (fault) return -1;
    memcpy(p, src, n); return 0;
}
static int mm_munmap(void *mm, uint64_t va, size_t len) {
    assert(mm == task.mm && va == 0x100000 && len == 4096);
    assert(live == 1); live--; return 0;
}
'''
HARNESS = r'''
static int call(uint64_t pa, uint64_t len, uint32_t prot, int want) {
    a20_device_map_mmio_args_t req = { sizeof(req), 1, prot, 0, pa, len, 0xfeed };
    a20_syscall_args_t args = {{ (uintptr_t)&req }};
    maps = 0; last_length = 0;
    int64_t r = sys_a20_device_map_mmio(&args);
    return r == want && req.out_addr == (want ? 0xfeed : 0x100000);
}
int main(void) {
    if (!call(0x2000, 1, 3, 0) || last_length != 4096 || live != 1) return 1;
    live = 0;
    if (!call(0x2000, 0, 3, -A20_ERR_INVALID_ARGUMENT) || maps) return 2;
    if (!call(0x2000, UINT64_MAX, 3, -A20_ERR_INVALID_ARGUMENT) || maps) return 3;
    if (!call(UINT64_MAX - 4095, 4096, 3, -A20_ERR_INVALID_ARGUMENT) || maps) return 4;
    if (!call(0x2001, 1, 3, -A20_ERR_INVALID_ARGUMENT) || maps) return 5;
    if (!call(0x2000, 1, 7, -A20_ERR_INVALID_ARGUMENT) || maps) return 6;
    if (!call(0x2000, 1, 0, -A20_ERR_INVALID_ARGUMENT) || maps) return 7;
    owner = 8;
    if (!call(0x2000, 1, 3, -A20_ERR_ACCESS) || maps || live) return 8;
    owner = 7; window_end = 0x2800;
    if (!call(0x2000, 1, 3, -A20_ERR_ACCESS) || live) return 9;
    window_end = 0x3000; fault = 1;
    if (!call(0x2000, 4096, 3, -A20_ERR_FAULT) || live) return 10;
    return 0;
}
'''

def main():
    source = (ROOT / 'kernel/abi/native/sys_native_device.c').read_text()
    start = source.index('int64_t sys_a20_device_map_mmio(')
    body = source[start:source.index('\nint64_t sys_a20_device_irq_listen(', start)]
    # The old byte-count authorization accepts a sub-page window even though
    # the hardware exposes the entire page. The output-fault control catches
    # losing the only address of an otherwise live mapping.
    negative = body.replace('kargs.phys_base, length,', 'kargs.phys_base, kargs.length,', 1)
    leak = body.replace('mm_munmap(cur->mm, va, (size_t)length);', '', 1)
    assert negative != body and leak != body
    with tempfile.TemporaryDirectory(prefix='a20-security-mmio-') as tmp:
        for name, code, expected in [('current', body, 0),
                                      ('byte-window', negative, 1),
                                      ('copyout-leak', leak, 10)]:
            src, exe = Path(tmp) / (name + '.c'), Path(tmp) / name
            src.write_text(PRELUDE + code + HARNESS)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], check=False)
            if result.returncode != expected:
                raise SystemExit(f'{name}: expected {expected}, got {result.returncode}')
    print('security-mmio: PASS (production ABI, ownership/range/copyout, negative controls)')

if __name__ == '__main__':
    main()
