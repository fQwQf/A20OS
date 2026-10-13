#!/usr/bin/env python3
"""Actual shared application bodies: a failed worker must release its peer.

Hosted syscall adapters inject a file error; this is application synchronization
coverage, not an OS boot or a file-backend acceptance test.
"""
from pathlib import Path
import subprocess
import tempfile
from test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#define A20_NATIVE_FILE_TEST 1
#define A20_OK 0
#define A20_ERR_WOULD_BLOCK 7
typedef int64_t a20_status_t;
typedef struct { unsigned unused; } a20_work_range_t;
static struct {
    volatile uint32_t input[128];
    uint32_t online, mask[2], jobs[2], sum[2], taken[2], ready[2], error[2];
    a20_work_range_t work;
} native_balance_shared;
static unsigned active, yields, files[2];
static int a20_atomic32_load(uint32_t *p, uint32_t *out) { *out=*p; return 0; }
static int a20_atomic32_store(uint32_t *p, uint32_t value) { *p=value; return 0; }
static int a20_thread_yield(void) {
    /* Finite observation of a peer that already returned its file error. */
    if (++yields > 3) exit(23);
    return 0;
}
static int native_file_contract(int serialized) {
    assert(!serialized);
    files[active]++;
    return active == 1 ? 12 : 0;
}
static int a20_work_range_take(a20_work_range_t *w, uint32_t *index) {
    (void)w; (void)index; abort();
}
static int a20_thread_get_cpu(uint32_t *cpu) { (void)cpu; abort(); }
'''
HARNESS = r'''
int main(void) {
    native_balance_shared.ready[0] = 1;
    active = 1;
    assert(native_balance_work(1) == 112);
    active = 0;
    assert(native_balance_work(0) == 6);
    assert(native_balance_shared.error[1] == 112);
    assert(native_balance_shared.error[0] == 6);
    assert(files[1] == 1 && files[0] == 0 && !yields);
    assert(!native_balance_shared.jobs[0] && !native_balance_shared.jobs[1]);
    return 0;
}
'''

def main():
    source = (ROOT / 'user/tests/native-balance-contract.h').read_text()
    wait = extract_function(source, 'static int native_balance_wait(')
    inner = extract_function(source, 'static int native_balance_work_inner(')
    wrapper = extract_function(source, 'static int native_balance_work(unsigned part)')
    legacy = wrapper[:wrapper.index('    if (result &&')] + '    return result;\n}'
    with tempfile.TemporaryDirectory(prefix='a20-balance-failure-') as temporary:
        directory = Path(temporary)
        for name, body, expected in [('current', wrapper, 0),
                                     ('no-publication-negative', legacy, 23)]:
            src, exe = directory / (name + '.c'), directory / name
            src.write_text(PRELUDE + wait + '\n' + inner + '\n' + body + HARNESS)
            subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra',
                            '-Werror', str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], check=False)
            if result.returncode != expected:
                raise SystemExit(f'{name}: expected {expected}, got {result.returncode}')
    print('test-native-balance-failure: PASS (actual application bodies, failed peer, negative control)')

if __name__ == '__main__':
    main()
