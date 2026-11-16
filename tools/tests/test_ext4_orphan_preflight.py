"""A read-only last-close failure must retain the orphan without poisoning JBD2."""
from pathlib import Path
import subprocess,tempfile,unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define EIO 5
#define EINVAL 22
typedef struct {void *journal;int metadata_lock;} ext4_sb_info_t;
typedef struct {uint16_t i_links_count;uint32_t i_dtime;} ext4_inode_t;
typedef struct {ext4_sb_info_t *sb;uint32_t inode_num,orphan_next;int unlinked,dir_removed;} ext4_vnode_priv_t;
typedef struct vnode {void *fs_data,*mnt;struct vnode *parent;} vnode_t;
static int read_error,reap_error,poisons,reaps,freed,identity_drops,durable_orphan=1;
static int directory_error,quarantines,lock_depth;
static int ext4_read_inode(ext4_sb_info_t *s,uint32_t ino,ext4_inode_t *out){(void)s;assert(ino==12);if(read_error)return -EIO;*out=(ext4_inode_t){.i_links_count=0,.i_dtime=42};return 0;}
static int ext4_orphan_reap(ext4_sb_info_t *s,uint32_t ino){(void)s;assert(ino==12);reaps++;if(!reap_error)durable_orphan=0;return reap_error;}
static int ext4_journal_poison(ext4_sb_info_t *s){(void)s;poisons++;return 0;}
static void rw_mutex_write_lock(int*x){(void)x;assert(!lock_depth);lock_depth++;}
static void rw_mutex_write_unlock(int*x){(void)x;assert(lock_depth==1);lock_depth--;}
static void vfs_drop_time_meta_identity(void*m,uint32_t ino){(void)m;assert(ino==12);identity_drops++;}
static int ext4_reclaim_removed_directory(ext4_sb_info_t*s,uint32_t i){(void)s;(void)i;return directory_error;}
static int ext4_record_status(ext4_sb_info_t*s,int r){(void)s;assert(lock_depth==1&&r==-117);quarantines++;return r;}
static void kfree(void*p){assert(p);freed++;}
static void vnode_put(vnode_t*v){(void)v;}
static void reset(void){read_error=reap_error=poisons=reaps=freed=identity_drops=0;durable_orphan=1;directory_error=quarantines=lock_depth=0;}
'''
MAIN=r'''
int main(void){
 ext4_sb_info_t sb={.journal=&sb};ext4_vnode_priv_t p;vnode_t v;
 #define OWNER() do{p=(ext4_vnode_priv_t){.sb=&sb,.inode_num=12,.orphan_next=42,.unlinked=1};v=(vnode_t){.fs_data=&p};}while(0)
 reset();OWNER();read_error=1;ext4_release_vn(&v);
 assert(poisons==0&&reaps==0&&durable_orphan==1&&freed==2&&identity_drops==0);
 reset();OWNER();read_error=1;assert(ext4_vnode_finalize_unlinked(&v)==-EIO);
 assert(poisons==0&&reaps==0&&p.unlinked==1&&durable_orphan==1&&freed==0);
 read_error=0;assert(ext4_vnode_finalize_unlinked(&v)==0);
 assert(reaps==1&&poisons==0&&p.unlinked==0&&durable_orphan==0);
 reset();OWNER();reap_error=-EIO;ext4_release_vn(&v);
 assert(poisons==1&&reaps==1&&durable_orphan==1);
 reset();OWNER();reap_error=-EIO;assert(ext4_vnode_finalize_unlinked(&v)==-EIO);
 assert(poisons==1&&p.unlinked==1&&durable_orphan==1);
 reset();OWNER();ext4_release_vn(&v);assert(reaps==1&&poisons==0&&durable_orphan==0&&freed==2);
 reset();OWNER();p.unlinked=0;p.dir_removed=1;directory_error=-EIO;
 ext4_release_vn(&v);assert(quarantines==1&&freed==2&&identity_drops==0&&!lock_depth);
 reset();OWNER();p.unlinked=0;p.dir_removed=1;directory_error=-EIO;
 assert(ext4_vnode_finalize_unlinked(&v)==-EIO&&p.dir_removed==1&&!quarantines&&!freed);
 directory_error=0;assert(ext4_vnode_finalize_unlinked(&v)==0&&!p.dir_removed&&identity_drops==1);
 return 0;
}
'''
class OrphanPreflight(unittest.TestCase):
 def test_release_and_explicit_retry(self):
  source=(ROOT/'kernel/fs/diskfs/ext4_namei.c').read_text()
  code='\n'.join(function(source,name) for name in ('ext4_orphan_read_vnode_owner','ext4_release_vn','ext4_vnode_finalize_unlinked'))
  old=code.replace('read_ok && sb->journal','sb->journal')
  with tempfile.TemporaryDirectory() as d:
   def run(body,name):
    path=Path(d)/(name+'.c');exe=path.with_suffix('');path.write_text(PRE+body+MAIN)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(path),'-o',str(exe)],check=True,capture_output=True)
    return subprocess.run([str(exe)],capture_output=True,timeout=5).returncode
   self.assertEqual(run(code,'fixed'),0)
   self.assertNotEqual(old,code);self.assertNotEqual(run(old,'old'),0)
if __name__=='__main__':unittest.main()
