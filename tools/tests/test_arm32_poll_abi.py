"""ARM poll passes milliseconds, whereas ppoll passes a timespec pointer."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Arm32PollAbiTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
    def test_poll_translation_preserves_timeout_value(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / 'poll_abi.c'
            source.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include "core/syscall_nr.h"
#define _ABI_LINUX_SYSCALL_ENTRY_H
/* Only the entry record is needed; no ARM instructions enter this fixture. */
typedef struct linux_syscall_args {
    uint64_t nr;
    uint64_t arg[6];
    void *ctx;
} linux_syscall_args_t;
#include "syscall_nr_arm32.h"

static void check_poll(uint32_t timeout) {
    linux_syscall_args_t args = { .nr = 168, .arg = {0, 0, timeout, 0, 0, 0} };
    assert(arm32_syscall_to_kernel_nr(168) == SYS_poll);
    assert(arm32_syscall_to_kernel_nr(168) != SYS_ppoll);
    arm32_syscall_rewrite_args(168, &args);
    assert(args.arg[0] == 0 && args.arg[1] == 0);
    /* 200 must reach sys_poll as 200 ms, never as a user address 0xc8. */
    assert((int32_t)args.arg[2] == (int32_t)timeout);
}
int main(void) {
    check_poll(0);
    check_poll(200);
    check_poll(UINT32_MAX); /* infinite timeout is signed -1 */
    linux_syscall_args_t fds = { .arg = {0x123400, 2, 200, 0, 0, 0} };
    arm32_syscall_rewrite_args(168, &fds);
    assert(fds.arg[0] == 0x123400 && fds.arg[1] == 2 && fds.arg[2] == 200);
    /* Real ppoll calls retain their separate timeout-pointer ABI. */
    assert(arm32_syscall_to_kernel_nr(336) == SYS_ppoll);
    assert(arm32_syscall_to_kernel_nr(414) == SYS_ppoll);
    assert(arm32_syscall_to_kernel_nr(1024) == UINT32_MAX);
    return 0;
}
''')
            result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     f'-I{ROOT}/kernel/include',
                                     f'-I{ROOT}/kernel/arch/arm32/include',
                                     str(source), '-o', str(work / 'poll_abi')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(work / 'poll_abi')], check=True)


if __name__ == '__main__':
    unittest.main()
