"""Run the actual Native read/write/readdir adapters with counted file owners."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TemporaryFiles(unittest.TestCase):
    def test_rpc_local_files_release_on_success_and_io_error(self):
        source = (ROOT / "user/svc/ufs_vnfs_backend.c").read_text()
        ranges = (("static void vn_close_temporary(", "static int64_t vn_getattr("),
                  ("static int64_t vn_readdir(", "static int64_t vn_create("),
                  ("static int64_t vn_rw_open(", "static int64_t vn_rename("))
        actual = "\n".join(source[source.index(start):source.index(end)] for start, end in ranges)
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define U_ESTALE 116
#define U_EROFS 30
#define U_ENOSYS 38
#define U_EIO 5
#define U_EMFILE 24
#define UFS_FT_DIR 4
#define UFS_FT_SYMLINK 10
#define UFS_FT_FILE 8
typedef struct vnode vnode_t;
typedef struct vfile vfile_t;
struct fops {int(*close)(vfile_t*);int(*read)(vfile_t*,char*,uint32_t);
 int(*write)(vfile_t*,const char*,uint32_t);int(*readdir)(vfile_t*,void*,uint32_t);
 long(*lseek)(vfile_t*,long,int);};
struct nops {vfile_t*(*open)(vnode_t*,int);};
struct vnode {struct nops *ops;size_t size;int refs;};
struct vfile {struct fops *ops;vnode_t *vnode;uint64_t offset;int closed;};
typedef struct {uint64_t d_ino;int64_t d_off;uint16_t d_reclen;uint8_t d_type;char d_name[256];} vfs_dirent64_t;
typedef struct {int status;uint64_t out0;uint32_t payload_len;} ufs_resp_hdr_t;
static vnode_t node;static int g_readonly,io_error,live_files,close_calls;
static uint8_t payload[4096];
static void *ufs_payload_buf(void){return payload;}
static uint32_t ufs_payload_cap(void){return sizeof(payload);}
static vnode_t *vn_map_get(uint64_t ino){assert(ino==19);return &node;}
static void put_fail(ufs_resp_hdr_t *r,int e){r->status=e;}
static int close_file(vfile_t *vf){assert(!vf->closed);vf->closed=1;close_calls++;return 0;}
static int read_file(vfile_t *vf,char *p,uint32_t n){assert(vf->vnode==&node);(void)p;return io_error?-5:(int)n;}
static int write_file(vfile_t *vf,const char *p,uint32_t n){return read_file(vf,(char*)p,n);}
static int read_dir(vfile_t *vf,void *p,uint32_t n){(void)p;(void)n;assert(vf->vnode==&node);return io_error?-5:0;}
static struct fops fops={close_file,read_file,write_file,read_dir,0};
static vfile_t *open_file(vnode_t *vn,int flags){(void)flags;vfile_t *f=calloc(1,sizeof(*f));assert(f);f->ops=&fops;f->vnode=vn;vn->refs++;live_files++;return f;}
static void vnode_put(vnode_t *vn){assert(vn==&node);assert(vn->refs>1);vn->refs--;}
static void vfile_free(vfile_t *f){assert(f->closed);assert(f->vnode->refs==1);live_files--;free(f);}
static struct nops nops={open_file};
'''
        main = r'''
int main(void){
 node.ops=&nops;node.refs=1;
 for(int error=0;error<2;error++){
  io_error=error;
  for(int i=0;i<1000;i++){
   ufs_resp_hdr_t r={0};int before=close_calls;
   assert(vn_read(19,4096,16,&r)==(error?-5:0));
   assert(live_files==0&&node.refs==1&&close_calls==before+1);
   r=(ufs_resp_hdr_t){0};before=close_calls;
   assert(vn_write(19,4096,payload,16,&r)==(error?-5:0));
   assert(live_files==0&&node.refs==1&&close_calls==before+1);
   r=(ufs_resp_hdr_t){0};before=close_calls;
   (void)vn_readdir(19,0,&r);
   assert(live_files==0&&node.refs==1&&close_calls==before+1);
  }
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="a20-vnfs-temporary-") as directory:
            work = Path(directory)
            for removed in (None, "    vnode_put(vn);", "    vfile_free(vf);", "    vn_close_temporary(vf);"):
                code = actual if removed is None else actual.replace(removed, "    /* missing owner release */")
                path, binary = work / "test.c", work / "test"
                path.write_text(fixture + code + main)
                subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra",
                                str(path), "-o", str(binary)], check=True, capture_output=True)
                result = subprocess.run([str(binary)], capture_output=True)
                if removed is None:
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                else:
                    self.assertNotEqual(result.returncode, 0, removed)


if __name__ == "__main__":
    unittest.main()
