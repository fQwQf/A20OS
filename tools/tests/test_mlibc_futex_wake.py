#!/usr/bin/env python3
"""The real A20 mlibc sysdep must return errno, not the Native wake count."""
from pathlib import Path
import subprocess
import tempfile
from test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
using a20_status_t = int;
struct FutexWake {};
template<typename T> struct Sysdeps;
template<> struct Sysdeps<FutexWake> { int operator()(int *, bool); };
static int status;
static uint32_t count, seen_count;
static int *seen_pointer;
static a20_status_t a20_rt_futex_wake(uint32_t *pointer, uint32_t maximum, uint32_t *woken) {
    seen_pointer = (int *)pointer; seen_count = maximum; *woken = count; return status;
}
static int a20_to_errno(int st) { return -st + 100; }
'''
HARNESS = r'''
int main() {
    int word = 0;
    for (bool all : {false, true}) {
        for (uint32_t woke : {0U, 1U, 7U, UINT32_MAX}) {
            for (int st : {0, -1, -5}) {
                status=st; count=woke;
                int error=Sysdeps<FutexWake>{}(&word, all);
                if (seen_pointer != &word || seen_count != (all ? UINT32_MAX : 1) ||
                    error != (st < 0 ? a20_to_errno(st) : 0)) {
                    printf("wake count=%u status=%d all=%d returned=%d\n", woke, st, all, error);
                    return 23;
                }
            }
        }
    }
    return 0;
}
'''


def main():
    source = (ROOT / 'user/external/mlibc/sysdeps/a20/sysdeps.cpp').read_text()
    body = extract_function(source, 'int Sysdeps<FutexWake>::operator()')
    old = body.replace('return 0;', 'return (int)woken;')
    assert old != body
    with tempfile.TemporaryDirectory(prefix='a20-mlibc-wake-') as directory:
        directory = Path(directory)
        for label, code, expected in [('fixed', body, 0), ('legacy', old, 23)]:
            src, exe = directory / (label + '.cpp'), directory / label
            src.write_text('#include <initializer_list>\n' + PRELUDE + code + HARNESS)
            subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], check=False)
            if result.returncode != expected:
                raise SystemExit(f'{label}: expected {expected}, got {result.returncode}')
    print('test-mlibc-futex-wake: PASS (real sysdep, counts, errors, legacy negative)')


if __name__ == '__main__':
    main()
