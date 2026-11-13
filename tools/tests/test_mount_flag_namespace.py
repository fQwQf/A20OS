"""Run the C mount adapter and Rust matcher with Linux mount attributes.

MS_NOSUID must not turn /run into a detached mount. Exercise remount and the
real detach transition as well, including a negative control for the old bit.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.tests.test_bcache_sync_batch_host import extract_function

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define MNTNS_MAX_MOUNTS 4
#define VFS_MOUNT_NS_SHARED 0x40000000
typedef struct {int flags;unsigned detached,publish_ready,teardown_running;const char *path;} mount_t;
typedef struct {mount_t *mounts[4];int nmounts;} mnt_namespace_t;
typedef struct {const char *path;uint32_t ready,teardown,detached;} vfs_mount_match_t;
extern int a20_vfs_find_mount(const char *,int,int,const vfs_mount_match_t *,size_t);
static mnt_namespace_t ns;
static int locked;
static mnt_namespace_t *mntns_current(void){return &ns;}
static uint64_t mntns_mount_table_lock(void){assert(!locked);locked=1;return 0;}
static void mntns_mount_table_unlock(uint64_t f){(void)f;assert(locked);locked=0;}
static int vfs_mount_count(void){return ns.nmounts;}
static mount_t *vfs_mount_at(int n){return ns.mounts[n];}
static void vfs_dcache_invalidate_all(void){}
void rust_eh_personality(void){}
'''
MAIN = r'''
int main(void){
 mount_t root={.publish_ready=1,.path="/extra"};
 mount_t run={.publish_ready=1,.path="/extra/run"};
 ns=(mnt_namespace_t){.mounts={&root,&run},.nmounts=2};
 for(int attributes=0;attributes<16;attributes++){
  assert(mount_update_existing("/extra/run",attributes)==1);
  if(vfs_find_mount_common("/extra/run/dbus/system_bus_socket",0,0)!=&run)return 7;
  assert(!run.detached&&run.flags==attributes);
 }
 run.flags|=VFS_MOUNT_NS_SHARED;
 assert(mount_update_existing("/extra/run",6)==1);
 assert(run.flags==(6|VFS_MOUNT_NS_SHARED));
 assert(vfs_find_mount_common("/extra/run/dbus",0,0)==&run);
 mt_set_detached(&run);assert(run.detached&&run.flags==(6|VFS_MOUNT_NS_SHARED));
 assert(vfs_find_mount_common("/extra/run/dbus",0,0)==&root);
 assert(vfs_find_mount_common("/extra/run/dbus",1,0)==&run);
 assert(mount_update_existing("/extra/run",0)==1);assert(run.detached);
 assert(vfs_find_mount_common("/extra/run/dbus",0,0)==&root);
}
'''


class MountFlagNamespace(unittest.TestCase):
    def test_linux_attributes_and_real_detach(self):
        mount = (ROOT / "kernel/fs/vfs/mount.c").read_text()
        ops = (ROOT / "kernel/fs/vfs/mount_ops.c").read_text()
        selected = extract_function(mount, "static void mt_set_detached(") + "\n"
        selected += extract_function(mount, "static mount_t *vfs_find_mount_common(")
        selected += extract_function(ops, "static int mount_update_existing(")
        rust = (ROOT / "kernel/fs/vfs/mount_state.rs").read_text().replace(
            "#![cfg_attr(not(test), no_std)]", "")
        prelude = ('#![no_std]\nextern "C" {fn abort()->!;}\n'
                   '#[panic_handler] fn panic(_: &core::panic::PanicInfo)->!{unsafe{abort()}}\n')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rs = root / "mount.rs"
            rs.write_text(rust.replace("use core::ffi::{c_int, c_void};", prelude + "use core::ffi::{c_int, c_void};"))
            lib = root / "libmount.a"
            subprocess.run(["rustc", "--edition=2021", "--crate-type=staticlib",
                            "-C", "panic=abort", "-O", str(rs), "-o", str(lib)], check=True)
            for broken in (False, True):
                code = root / ("old.c" if broken else "new.c")
                body = selected.replace("cands[n].detached = m->detached;",
                                        "cands[n].detached = !!(m->flags & 2);") if broken else selected
                code.write_text(PRELUDE + body + MAIN)
                binary = code.with_suffix("")
                subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                "-O2", str(code), str(lib), "-o", str(binary)], check=True)
                result = subprocess.run([str(binary)])
                self.assertEqual(result.returncode, 7 if broken else 0)


if __name__ == "__main__":
    unittest.main()
