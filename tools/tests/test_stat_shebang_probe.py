"""Stat must not read impossible shebangs or zero-size diagnostic files."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StatShebang(unittest.TestCase):
    def test_probe_eligibility_and_existing_exec_bits(self):
        text = (ROOT / "kernel/abi/linux/sys_path.c").read_text()
        start = text.index("static int path_is_shebang_script(")
        end = text.index("int64_t sys_fstat(", start)
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define O_RDONLY 0
#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IXUSR 0100
#define S_IXGRP 0010
#define S_IXOTH 0001
typedef struct { unsigned st_mode; long st_size; } kstat_t;
static int opens,reads,closes,open_fail,read_len=2;
static char bytes[2]={'#','!'};
static int vfs_open(const char *path,int flags,int mode) {
    assert(path&&flags==O_RDONLY&&mode==0);opens++;return open_fail?-1:7;
}
static int vfs_read(int fd,char *buf,unsigned long size) {
    assert(fd==7&&size==2);reads++;memcpy(buf,bytes,2);return read_len;
}
static void vfs_close(int fd) { assert(fd==7);closes++; }
''' + text[start:end] + r'''
int main(void) {
    kstat_t s={S_IFREG|0644,0};
    kstat_apply_script_exec("/proc/a20/perf",&s);assert(opens==0&&s.st_mode==(S_IFREG|0644));
    s.st_size=1;kstat_apply_script_exec("one-byte",&s);assert(opens==0);
    s.st_size=2;s.st_mode=S_IFDIR|0755;kstat_apply_script_exec("dir",&s);assert(opens==0);
    s.st_mode=S_IFREG|0700;kstat_apply_script_exec("executable",&s);assert(opens==0);
    s.st_mode=S_IFREG|0644;kstat_apply_script_exec("script",&s);
    assert(s.st_mode==(S_IFREG|0755)&&opens==1&&reads==1&&closes==1);
    s.st_mode=S_IFREG|0644;bytes[1]='x';kstat_apply_script_exec("data",&s);assert(s.st_mode==(S_IFREG|0644));
    bytes[1]='!';read_len=1;kstat_apply_script_exec("short",&s);assert(s.st_mode==(S_IFREG|0644));
    open_fail=1;kstat_apply_script_exec("missing",&s);assert(opens==4&&reads==3&&closes==3);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "stat.c", Path(tmp) / "stat"
            src.write_text(source)
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-O2",
                            "-fsanitize=address,undefined", str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
