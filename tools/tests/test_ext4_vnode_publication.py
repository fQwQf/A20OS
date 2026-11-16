"""Concurrent cold lookups must return one inode owner and one final orphan reap."""
from pathlib import Path
import subprocess,tempfile,unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define EXT4_VCACHE_MAX 128
#define EXT4_VCACHE_HASH_SIZE 128
#define EXT4_VCACHE_HASH_MASK 127
#define VFS_FT_REGULAR 1
#define VFS_FT_DIR 2
#define VFS_FT_SYMLINK 3
#define a20_perf_count(x) ((void)0)
typedef struct {int unused;} ext4_sb_info_t;
typedef struct {unsigned i_mode,i_uid,i_gid;} ext4_inode_t;
typedef struct {ext4_sb_info_t *sb;uint32_t inode_num;uint64_t file_size;int type,unlinked;uint32_t orphan_next;unsigned open_count;} ext4_vnode_priv_t;
typedef struct vnode {uint64_t ino,size;int type;unsigned mode,uid,gid;int refs;struct vnode *parent;void *ops,*fs_data,*mnt;} vnode_t;
static int live,orphan_reaps,g_ext4_vnode_ops;
static void *kmalloc(size_t n){void*p=malloc(n);if(p)__atomic_add_fetch(&live,1,__ATOMIC_RELAXED);return p;}
static void kfree(void*p){if(p){__atomic_sub_fetch(&live,1,__ATOMIC_RELAXED);free(p);}}
static void vnode_get(vnode_t*v){__atomic_add_fetch(&v->refs,1,__ATOMIC_RELAXED);}
static void vnode_ref_init(vnode_t*v,int n){v->refs=n;}
static void vnode_put(vnode_t*v){if(!__atomic_sub_fetch(&v->refs,1,__ATOMIC_ACQ_REL)){
 ext4_vnode_priv_t*p=v->fs_data;if(p&&p->unlinked)assert(++orphan_reaps==1);
 kfree(p);if(v->parent)vnode_put(v->parent);kfree(v);
}}
static pthread_barrier_t cold;
static int force_cold=1;
static int ext4_read_inode(ext4_sb_info_t*s,uint32_t i,ext4_inode_t*o){(void)s;(void)i;
 if(force_cold)pthread_barrier_wait(&cold);*o=(ext4_inode_t){S_IFREG|0644,1000,1000};return 0;}
typedef struct {ext4_sb_info_t*sb;uint32_t ino;vnode_t*vn;int hash_next,hash_prev,free_next;} ext4_vcache_ent_t;
static ext4_vcache_ent_t g_ext4_vcache[EXT4_VCACHE_MAX];
static int g_ext4_vcache_hash[EXT4_VCACHE_HASH_SIZE],g_ext4_vcache_free_list,g_ext4_vcache_initialized;
static pthread_mutex_t g_ext4_vcache_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t spin_lock_irqsave(pthread_mutex_t*m){pthread_mutex_lock(m);return 0;}
static void spin_unlock_irqrestore(pthread_mutex_t*m,uint64_t f){(void)f;pthread_mutex_unlock(m);}
'''
MAIN=r'''
static ext4_sb_info_t sb;
static vnode_t parent={.refs=1};
static vnode_t *results[64];
static void *lookup(void *i){results[(uintptr_t)i]=ext4_make_vnode(&sb,1,0,VFS_FT_REGULAR,&parent);return NULL;}
int main(void){pthread_t threads[64];pthread_barrier_init(&cold,NULL,64);
 for(uintptr_t i=0;i<64;i++)assert(!pthread_create(&threads[i],NULL,lookup,(void*)i));
 for(unsigned i=0;i<64;i++)pthread_join(threads[i],NULL);
 for(unsigned i=0;i<64;i++)assert(results[i]&&results[i]==results[0]);
 assert(results[0]->refs==65);((ext4_vnode_priv_t*)results[0]->fs_data)->unlinked=1;
 vnode_put(ext4_vnode_cache_remove(&sb,1));
 for(unsigned i=0;i<64;i++)vnode_put(results[i]);
 assert(orphan_reaps==1&&parent.refs==1&&live==0);
 force_cold=0;
 for(unsigned i=0;i<EXT4_VCACHE_MAX;i++){
  vnode_t*v=ext4_make_vnode(&sb,i+2,0,VFS_FT_REGULAR,&parent);assert(v);vnode_put(v);
 }
 assert(!ext4_make_vnode(&sb,1000,0,VFS_FT_REGULAR,&parent));
 for(unsigned i=0;i<EXT4_VCACHE_MAX;i++)vnode_put(ext4_vnode_cache_remove(&sb,i+2));
 assert(parent.refs==1&&live==0);return 0;
}
'''
class Ext4VnodePublication(unittest.TestCase):
 def test_concurrent_miss_publication_and_full_cache(self):
  source=(ROOT/'kernel/fs/diskfs/ext4.c').read_text()
  funcs=('ext4_vcache_hash_key','ext4_vcache_init_locked','ext4_vcache_link_hash_locked','ext4_vcache_unlink_hash_locked','ext4_vcache_free_slot_locked','ext4_vnode_cache_lookup','ext4_vnode_cache_insert','ext4_vnode_cache_remove','ext4_make_vnode')
  code=''.join(function(source,n) for n in funcs)
  with tempfile.TemporaryDirectory() as d:
   def run(body,name):
    p=Path(d)/(name+'.c');exe=p.with_suffix('');p.write_text(PRE+body+MAIN)
    result=subprocess.run(['cc','-std=gnu11','-pthread','-fsanitize=undefined',str(p),'-o',str(exe)],capture_output=True,text=True)
    self.assertEqual(result.returncode,0,result.stderr)
    return subprocess.run([str(exe)],capture_output=True,timeout=10).returncode
   self.assertEqual(run(code,'fixed'),0)
   old=code.replace('            vnode_t *existing = g_ext4_vcache[i].vn;\n            vnode_get(existing);\n','').replace('            return existing;','            return vn;')
   self.assertNotEqual(old,code);self.assertNotEqual(run(old,'old'),0)
if __name__=='__main__':unittest.main()
