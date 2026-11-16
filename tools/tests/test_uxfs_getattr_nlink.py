"""The real VNFS GETATTR adapter must carry zero and multiple link counts."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UxfsGetattrLinkCount(unittest.TestCase):
    def test_sender_tag_counts_errors_and_missing_tag_negative(self):
        text = (ROOT / "user/svc/ufs_vnfs_backend.c").read_text()
        code = text[text.index("static int64_t vn_getattr("):text.index("static int64_t vn_readdir(")]
        rust = (ROOT / "kernel/fs/uxfs/uxfs_ops_state.rs").read_text()
        tag = int(re.search(r"pub const GETATTR_NLINK_TAG: u64 = (0x[0-9a-fA-F_]+);", rust)[1].replace("_", ""), 16)
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "fs/ufs_proto.h"
#define U_ESTALE 116
typedef struct {uint64_t st_size;uint32_t st_mode,st_nlink;} kstat_t;
typedef struct vnode vnode_t;
struct ops {int(*stat)(vnode_t *,kstat_t *);};
struct vnode {struct ops *ops;uint32_t mode;};
static uint32_t links;static int absent,error;
static int stat_inode(vnode_t *v,kstat_t *s){assert(v);s->st_size=8193;s->st_mode=0100640;s->st_nlink=links;return error;}
static struct ops ops={stat_inode};static vnode_t node={&ops,0100600};
static vnode_t *vn_map_get(uint64_t ino){assert(ino==19);return absent?0:&node;}
static void put_fail(ufs_resp_hdr_t*r,int e){r->status=e;}
''' + f"\n_Static_assert(UFS_GETATTR_NLINK_TAG == {tag}ULL, \"C/Rust tag agreement\");\n"
        main = r'''
int main(void){
 uint32_t cases[]={0,1,2,65535,0xffffffff};
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  links=cases[i];ufs_resp_hdr_t r={0};assert(vn_getattr(19,&r)==0);
  assert(r.out0==8193&&r.out1==((uint64_t)0100640<<32));
  assert((r.out2&UFS_GETATTR_NLINK_MASK)==UFS_GETATTR_NLINK_TAG);
  assert((uint32_t)r.out2==links);
 }
 ufs_resp_hdr_t r={0};error=-5;assert(vn_getattr(19,&r)==-5&&r.status==-5&&r.out2==0);
 error=0;absent=1;r=(ufs_resp_hdr_t){0};assert(vn_getattr(19,&r)==-116&&r.status==-116);
 absent=0;ops.stat=0;r=(ufs_resp_hdr_t){0};assert(vn_getattr(19,&r)==0&&r.out2==0);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="a20-nlink-") as temporary:
            work = Path(temporary)
            for negative in (False, True):
                body = code
                if negative:
                    line = "r->out2 = UFS_GETATTR_NLINK_TAG | (uint32_t)st.st_nlink;"
                    self.assertIn(line, body)
                    body = body.replace(line, "r->out2 = 0;")
                path, binary = work / "test.c", work / "test"
                path.write_text(fixture + body + main)
                subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                                "-I", str(ROOT / "kernel/include"), str(path), "-o", str(binary)], check=True)
                result = subprocess.run([str(binary)], capture_output=True)
                if negative:
                    self.assertNotEqual(result.returncode, 0)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == "__main__":
    unittest.main()
