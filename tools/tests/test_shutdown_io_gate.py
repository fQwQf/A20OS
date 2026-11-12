"""Production read/write admission: blocked IPC must not prevent shutdown."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
MODEL=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stddef.h>
typedef struct {unsigned mode;} vnode_t;
typedef struct {vnode_t *vnode;} vfile_t;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int entered,release_io,inflight,shutdown_active;
static int mntns_fileop_enter(void){
 pthread_mutex_lock(&lock);int ret=shutdown_active?-ESHUTDOWN:0;
 if(!ret)inflight++;pthread_mutex_unlock(&lock);return ret;
}
static void mntns_fileop_leave(void){pthread_mutex_lock(&lock);inflight--;pthread_mutex_unlock(&lock);}
static int operation(vfile_t *f,const char *b,size_t n){
 (void)f;(void)b;(void)n;pthread_mutex_lock(&lock);entered=1;pthread_cond_broadcast(&cv);
 while(!release_io)pthread_cond_wait(&cv,&lock);pthread_mutex_unlock(&lock);return 7;
}
static int vfs_read_file_impl(vfile_t*f,char*b,size_t n){return operation(f,b,n);}
static int vfs_write_file_impl(vfile_t*f,const char*b,size_t n){return operation(f,b,n);}
'''
MAIN=r'''
static vfile_t file;
static int writing;
static void *worker(void *unused){(void)unused;char byte=0;
 assert((writing?vfs_write_file(&file,&byte,1):vfs_read_file(&file,&byte,1))==7);return NULL;}
int main(void){
 vnode_t node;
 unsigned modes[]={0,S_IFCHR,S_IFIFO,S_IFSOCK,S_IFREG,S_IFDIR,S_IFBLK};
 for(writing=0;writing<2;writing++)for(unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);i++){
  node.mode=modes[i];file.vnode=i?&node:NULL;entered=release_io=inflight=shutdown_active=0;
  pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));
  pthread_mutex_lock(&lock);while(!entered)pthread_cond_wait(&cv,&lock);
  int persistent=i>=2;assert(inflight==persistent);
  if(!persistent)shutdown_active=1;
  release_io=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&lock);pthread_join(thread,NULL);
  assert(inflight==0);
 }
 return 0;
}
'''
class ShutdownIOGate(unittest.TestCase):
 def test_real_wrappers_with_sleeping_operations_and_negative_control(self):
  text=(ROOT/'kernel/fs/vfs/file.c').read_text()
  source=''.join(function(text,n) for n in ('vfs_rw_needs_mount_gate','vfs_read_file','vfs_write_file'))
  with tempfile.TemporaryDirectory() as temp:
   def run(code,name):
    path=Path(temp)/(name+'.c');binary=path.with_suffix('');path.write_text(MODEL+code+MAIN)
    subprocess.run(['cc','-std=gnu11','-pthread','-fsanitize=undefined',str(path),'-o',str(binary)],check=True,capture_output=True)
    return subprocess.run([str(binary)],capture_output=True,timeout=10).returncode
   self.assertEqual(run(source,'fixed'),0)
   old=source.replace('    if (!vfs_rw_needs_mount_gate(vf))\n        return vfs_read_file_impl(vf, buf, count);\n','').replace('    if (!vfs_rw_needs_mount_gate(vf))\n        return vfs_write_file_impl(vf, buf, count);\n','')
   self.assertNotEqual(old,source);self.assertNotEqual(run(old,'old'),0)
 def test_mounted_character_streams_are_not_force_closed(self):
  code=function((ROOT/'kernel/fs/file.c').read_text(),'vfile_shutdown_needs_snapshot')
  model=r'''
#include <assert.h>
#include <sys/stat.h>
'''
  model+=r'''
typedef struct {unsigned mode;} vnode_t;
typedef struct {int mount_open_counted,ref_count;vnode_t *vnode;} vfile_t;
static int refcount_read(const int*p){return *p;}
'''
  main=r'''
int main(void){vnode_t vn={S_IFCHR};vfile_t vf={1,2,&vn};
assert(!vfile_shutdown_needs_snapshot(&vf));vn.mode=S_IFREG;
assert(vfile_shutdown_needs_snapshot(&vf));vn.mode=S_IFBLK;
assert(vfile_shutdown_needs_snapshot(&vf));vf.ref_count=0;
assert(!vfile_shutdown_needs_snapshot(&vf));return 0;}
'''
  with tempfile.TemporaryDirectory() as d:
   path=Path(d)/'snapshot.c';exe=path.with_suffix('');path.write_text(model+code+main)
   subprocess.run(['cc','-std=gnu11','-fsanitize=undefined',str(path),'-o',str(exe)],check=True,capture_output=True)
   subprocess.run([str(exe)],check=True,capture_output=True)
if __name__=='__main__':unittest.main()
