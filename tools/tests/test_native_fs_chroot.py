"""Exercise Native mkdir through the real shared dirfd/chroot resolver."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeFSChroot(unittest.TestCase):
    def test_rooted_mkdir_dirfd_errors_and_unrooted_negative_control(self):
        source = (ROOT / "kernel/abi/native/sys_native_fs.c").read_text()
        start = source.index("int64_t sys_a20_path_create(")
        stop = source.index("int64_t sys_a20_path_unlink(", start)
        function = source[start:stop]
        types = (ROOT / "kernel/include/abi/native/types.h").read_text()
        start = types.index("typedef struct a20_path_create_args {")
        stop = types.index("} a20_path_create_args_t;", start) + len("} a20_path_create_args_t;")
        types = types[start:stop]
        resolver = (ROOT / "kernel/syscall/syscall_common.c").read_text()
        resolver = resolver[resolver.index("static int path_build("):]
        fixture = (ROOT / "tools/tests/native_fs_chroot_host.c").read_text()
        main = r'''
static void check(const char *root,const char *cwd,const char *path,
                  uint32_t dir,const char *expected,int64_t error) {
    strcpy(current.fs.root_path,root);strcpy(current.fs.cwd,cwd);
    creates=closed_fds=0;created[0]=0;
    a20_path_create_args_t a={0};a.size=sizeof(a);a.version=1;a.type=1;
    a.mode=0755;a.dir=dir;a.path=(uintptr_t)path;a.path_len=strlen(path);
    a20_syscall_args_t call={{(uintptr_t)&a}};
    assert(sys_a20_path_create(&call)==error);
    if(expected) {assert(creates==1);assert(!strcmp(created,expected));assert(a.out_handle==23);}
    else assert(creates==0);
    if(dir&&dir!=99)assert(closed_fds==(expected?2:1));
}
int main(void) {
    check("/","/","/ufs",0,"/ufs",0);
    check("/extra","/","/ufs",0,"/extra/ufs",0);
    check("/extra","/work","../ufs",0,"/extra/ufs",0);
    check("/extra","/","../../ufs",0,"/extra/ufs",0);
    check("/extra","/","child",1,"/extra/cap/child",0);
    check("/extra","/","../child",1,"/extra/child",0);
    strcpy(capability.path,"/outside/cap");
    check("/extra","/","child",1,NULL,-EACCES);
    check("/extra","/","child",99,NULL,-EBADF);
    return 0;
}
void rust_eh_personality(void) {}
'''
        with tempfile.TemporaryDirectory(prefix="a20-native-fs-chroot-") as tmp:
            work = Path(tmp)
            rust = (ROOT / "kernel/fs/vfs/path_state.rs").read_text().split(
                "#![cfg_attr(not(test), no_std)]", 1)[1]
            rs = work / "path.rs"
            rs.write_text('''#![no_std]
extern "C" {fn abort() -> !;}
#[panic_handler] fn panic(_: &core::panic::PanicInfo) -> ! {unsafe {abort()}}
''' + rust)
            lib = work / "libpath.a"
            subprocess.run(["rustc", "--edition=2021", "--crate-type", "staticlib",
                            "-C", "panic=abort", "-C", "opt-level=2", str(rs),
                            "-o", str(lib)], check=True)
            for negative in (False, True):
                code = function
                if negative:
                    old = "syscall_path_at(dirfd, kpath, full, sizeof(full))"
                    self.assertIn(old, code)
                    code = code.replace(old, '(snprintf(full, sizeof(full), "%s", kpath) < 0 ? -EINVAL : 0)')
                c = work / "test.c"
                c.write_text(fixture + types + "\n" + resolver + "\n" + code + main)
                binary = work / ("negative" if negative else "positive")
                subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                                str(c), str(lib), "-o", str(binary)], check=True)
                run = subprocess.run([str(binary)], capture_output=True)
                if negative:
                    self.assertNotEqual(run.returncode, 0, "unrooted mkdir must fail")
                else:
                    self.assertEqual(run.returncode, 0, run.stderr.decode())


if __name__ == "__main__":
    unittest.main()
