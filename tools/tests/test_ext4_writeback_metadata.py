"""Writeback preserves orphan metadata and initializes new ordered data."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]
PRE = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define VFS_FT_DIR 2
typedef struct { pthread_mutex_t metadata_lock; unsigned block_size; void *bc; } ext4_sb_info_t;
typedef struct { uint16_t i_links_count; uint32_t i_dtime; uint64_t size; } ext4_inode_t;
typedef struct { uint64_t file_size; } ext4_vnode_priv_t;
typedef struct { size_t size; void *fs_data; } vnode_t;
typedef struct { ext4_sb_info_t *sb; unsigned inode_num; int inode_valid,ext_valid,is_dir; ext4_inode_t inode; size_t file_size,file_off; } ext4_fctx_t;
typedef struct { void *priv; vnode_t *vnode; size_t offset; } vfile_t;
static ext4_inode_t disk;
static pthread_mutex_t rendezvous=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static int pause_io,entered,proceed,writes;
static int new_block,fail_alloc,fail_data,freed,grown,data_written;
static unsigned char block_image[4096];
static void *kmalloc(size_t n){return fail_alloc?NULL:malloc(n);}
static void kfree(void *p){free(p);}
static int ext4_record_status(ext4_sb_info_t *s,int status){(void)s;return status;}
static inline void rw_mutex_write_lock(pthread_mutex_t *p){assert(!pthread_mutex_lock(p));}
static inline void rw_mutex_write_unlock(pthread_mutex_t *p){assert(!pthread_mutex_unlock(p));}
static int ext4_is_inconsistent(ext4_sb_info_t *p){(void)p;return 0;}
static uint64_t ext4_inode_size(ext4_inode_t *p){return p->size;}
static void ext4_inode_set_size(ext4_inode_t *p,uint64_t n){p->size=n;}
static int ext4_read_inode(ext4_sb_info_t *p,unsigned i,ext4_inode_t *out){(void)p;assert(i==12);*out=disk;return 0;}
static int ext4_write_inode(ext4_sb_info_t *p,unsigned i,ext4_inode_t *in){(void)p;assert(i==12);disk=*in;writes++;return 0;}
static uint64_t ext4_block_map_cached(ext4_fctx_t *f,ext4_inode_t *i,uint32_t l){(void)f;(void)i;(void)l;return new_block?0:20;}
static size_t ext4_alloc_blocks_uninitialized(ext4_sb_info_t*s,size_t n,uint64_t*p){(void)s;assert(new_block&&n==1);*p=21;return 1;}
static int ext4_block_grow(ext4_sb_info_t*s,unsigned n,ext4_inode_t*i,uint32_t l,uint64_t p){(void)s;(void)n;(void)i;(void)l;assert(new_block&&p==21&&data_written);grown++;return 0;}
static int ext4_free_block(ext4_sb_info_t*s,uint64_t p){(void)s;assert(new_block&&p==21);freed++;return 0;}
static int bcache_write_bytes(void *p,uint64_t off,const void *data,size_t n){
 (void)p;
 if(new_block){assert(off==21*4096&&n==4096);memcpy(block_image,data,n);if(fail_data)return -EIO;data_written=1;}
 if(pause_io){pthread_mutex_lock(&rendezvous);entered=1;pthread_cond_broadcast(&condition);
  while(!proceed)pthread_cond_wait(&condition,&rendezvous);pthread_mutex_unlock(&rendezvous);}
 return 0;
}
'''
MAIN = r'''
static ext4_sb_info_t sb={.metadata_lock=PTHREAD_MUTEX_INITIALIZER,.block_size=4096};
static vnode_t vnode;
static ext4_fctx_t fc;
static vfile_t vf;
static void setup(void){
 disk=(ext4_inode_t){.i_links_count=1,.size=4096};
 vnode=(vnode_t){.size=8192};
 fc=(ext4_fctx_t){.sb=&sb,.inode_num=12,.inode_valid=1,.inode=disk,.file_size=8192,.file_off=4096};
 vf=(vfile_t){.priv=&fc,.vnode=&vnode};writes=0;new_block=fail_alloc=fail_data=freed=grown=data_written=0;
}
static void unlink_inode(void){disk.i_links_count=0;disk.i_dtime=42;}
static void *writeback(void *unused){(void)unused;assert(ext4_fwrite(&vf,"x",1)==1);return NULL;}
int main(void){
 /* EOF is unchanged in the open context, but unlink changed link metadata. */
 setup();unlink_inode();assert(ext4_fwrite(&vf,"x",1)==1);
 assert(writes==1&&disk.i_links_count==0&&disk.i_dtime==42);
 /* Deterministic second mutator at the read/data-write/inode-write gap. */
 setup();fc.inode_valid=0;pause_io=1;pthread_t thread;
 assert(!pthread_create(&thread,NULL,writeback,NULL));
 pthread_mutex_lock(&rendezvous);
 while(!entered)pthread_cond_wait(&condition,&rendezvous);
 int lock=pthread_mutex_trylock(&sb.metadata_lock);
 assert(lock==0||lock==EBUSY);
 if(lock==0){unlink_inode();pthread_mutex_unlock(&sb.metadata_lock);}
 proceed=1;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&rendezvous);
 pthread_join(thread,NULL);
 if(lock==EBUSY){pthread_mutex_lock(&sb.metadata_lock);unlink_inode();pthread_mutex_unlock(&sb.metadata_lock);}
 assert(writes==1&&disk.i_links_count==0&&disk.i_dtime==42);
 /* A partial new file block has zero padding and uses ordinary data I/O.
  * Allocation/data initialization failures publish no extent or inode. */
 pause_io=0;setup();new_block=1;fc.file_off=17;
 assert(ext4_fwrite(&vf,"Z",1)==1&&grown==1&&freed==0);
 for(unsigned j=0;j<sizeof(block_image);j++)assert(block_image[j]==(j==17?'Z':0));
 setup();new_block=1;fail_alloc=1;fc.file_off=17;
 assert(ext4_fwrite(&vf,"Z",1)==-ENOMEM&&grown==0&&freed==0&&writes==0);
 setup();new_block=1;fail_data=1;fc.file_off=17;
 assert(ext4_fwrite(&vf,"Z",1)==-EIO&&grown==0&&freed==1&&writes==0);
 return 0;
}
'''


class Ext4WritebackMetadata(unittest.TestCase):
    def test_stale_open_and_concurrent_unlink(self):
        source = (ROOT / "kernel/fs/diskfs/ext4_file.c").read_text()
        names = ("ext4_fctx_inode", "ext4_fctx_inode_dirty", "ext4_publish_size",
                 "ext4_fctx_set_size", "ext4_discard_data_block",
                 "ext4_prepare_data_block", "ext4_fwrite_unlocked", "ext4_fwrite")
        code = "\n".join(function(source, name) for name in names)
        stale = code.replace("    ext4_fctx_inode_dirty(fc);\n    int ret", "    int ret")
        unlocked = code.replace("    rw_mutex_write_lock(&fc->sb->metadata_lock);", "").replace(
            "    rw_mutex_write_unlock(&fc->sb->metadata_lock);", "")
        with tempfile.TemporaryDirectory() as directory:
            def run(body, name):
                path = Path(directory) / (name + ".c")
                exe = path.with_suffix("")
                path.write_text(PRE + body + MAIN)
                result = subprocess.run(["cc", "-std=gnu11", "-pthread", "-Wall", "-Wextra",
                                         "-Werror", "-Wno-misleading-indentation", "-fsanitize=undefined",
                                         str(path), "-o", str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                return subprocess.run([str(exe)], capture_output=True, timeout=5).returncode
            self.assertEqual(run(code, "fixed"), 0)
            self.assertNotEqual(stale, code)
            self.assertNotEqual(run(stale, "stale-open"), 0)
            self.assertNotEqual(unlocked, code)
            self.assertNotEqual(run(unlocked, "unlocked-writeback"), 0)


if __name__ == "__main__":
    unittest.main()
