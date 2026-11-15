"""ARM _llseek returns zero and copies a 64-bit offset through its fourth argument."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]


class ArmLlseek(unittest.TestCase):
    def test_legacy_and_split_offset_result_contracts(self):
        source = (ROOT / 'kernel/abi/linux/syscall_table.c').read_text()
        body = extract_function(source, 'static int64_t linux_sys_lseek_call(const linux_syscall_args_t *args)')
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "core/syscall_nr.h"
#define _ABI_LINUX_SYSCALL_ENTRY_H
#define CONFIG_ARM32 1
#define EFAULT 14
#define A(n) (args->arg[(n)])
typedef struct { uint64_t nr, arg[6]; void *ctx; } linux_syscall_args_t;
#include "syscall_nr_arm32.h"
static int fd_seen, whence_seen, fail_copy;
static int64_t offset_seen, seek_result;
static int64_t sys_lseek(int fd, int64_t off, int whence) {
 fd_seen=fd; offset_seen=off; whence_seen=whence; return seek_result;
}
static int copy_to_user(void *p, const void *data, size_t n) {
 if (fail_copy) return -1;
 assert(p && n == 8); memcpy(p, data, n); return 0;
}
''' + body + r'''
int main(void) {
 uint64_t out=0xabcdef;
 linux_syscall_args_t a={.nr=140,.arg={9,0xffffffff,0xfffffff0,(uintptr_t)&out,2,99}};
 arm32_syscall_rewrite_args(140,&a);
 seek_result=123456789;
 assert(linux_sys_lseek_call(&a)==0);
 assert(fd_seen==9 && whence_seen==2 && offset_seen==-16 && out==123456789);
 out=99; seek_result=-9;
 assert(linux_sys_lseek_call(&a)==-9 && out==99);
 seek_result=14;fail_copy=1;
 assert(linux_sys_lseek_call(&a)==-EFAULT && out==99);
 fail_copy=0;
 a=(linux_syscall_args_t){.nr=19,.arg={8,0xfffffff0,1,0,0,1}};
 arm32_syscall_rewrite_args(19,&a);seek_result=23;
 assert(linux_sys_lseek_call(&a)==23);
 assert(fd_seen==8 && whence_seen==1 && offset_seen==-16);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='a20-arm-llseek-') as tmp:
            path=Path(tmp); c=path/'test.c'; c.write_text(fixture)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                            '-fsanitize=undefined','-fno-sanitize-recover=all',
                            '-I'+str(ROOT/'kernel/include'),
                            '-I'+str(ROOT/'kernel/arch/arm32/include'),str(c),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)


if __name__ == '__main__': unittest.main()
