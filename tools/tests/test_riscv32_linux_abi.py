"""Check RV32 register-pair translation at the production syscall hook."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class Riscv32LinuxAbiTests(unittest.TestCase):
    def test_offsets_are_reassembled_before_generic_dispatch(self):
        with tempfile.TemporaryDirectory() as td:
            work = pathlib.Path(td)
            (work / 'abi/linux').mkdir(parents=True)
            (work / 'abi/linux/syscall_entry.h').write_text('''
#include <stdint.h>
#include "core/syscall_nr.h"
typedef struct { uint64_t nr; uint64_t arg[6]; void *ctx; } linux_syscall_args_t;
''')
            src = work / 'probe.c'
            src.write_text('''
#include <assert.h>
#include "arch/syscall_hook.h"
int main(void) {
    const uint32_t calls[] = { SYS_truncate, SYS_ftruncate, SYS_pread64,
                              SYS_pwrite64, SYS_preadv, SYS_pwritev };
    for (unsigned i = 0; i < sizeof(calls)/sizeof(calls[0]); i++) {
        linux_syscall_args_t a = { .nr = calls[i], .arg = {11,22,33,44,55,66} };
        unsigned slot = i < 2 ? 1 : 3;
        a.arg[slot] = 0xfeedface89abcdefULL;
        a.arg[slot+1] = 0xdeadbeef12345678ULL;
        arch_syscall_adjust_args(&a);
        assert(a.nr == calls[i]);
        assert(a.arg[slot] == 0x1234567889abcdefULL);
        assert(a.arg[0] == 11 && a.arg[5] == 66);
    }
    linux_syscall_args_t map = { .nr=SYS_mmap, .arg={0,4096,1,2,4,0x100001} };
    arch_syscall_adjust_args(&map);
    assert(map.arg[5] == 0x100001000ULL);
    linux_syscall_args_t poll = { .nr=SYS_ppoll_time64, .arg={1,2,3,4,8,6} };
    arch_syscall_adjust_args(&poll);
    assert(poll.arg[3] == 4 && poll.arg[4] == 8);
    return 0;
}
''')
            binary = work / 'probe'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I'+str(work), '-I'+str(ROOT/'kernel/arch/riscv32/include'),
                            '-I'+str(ROOT/'kernel/include'), str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
