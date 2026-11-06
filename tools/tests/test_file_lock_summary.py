"""Run actual lock algebra/releases; check the summary against every used slot."""
from pathlib import Path
import subprocess, tempfile
from test_pt_cache_owner import extract
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/fs/locks.c').read_text()
s='\n'.join(line for line in s.splitlines() if not line.startswith('#include'))
s=s.replace(extract(s,'static int fs_lock_wait('),'static int fs_lock_wait(uint64_t flags) { spin_unlock_irqrestore(&g_file_lock_table_lock,flags); return -EAGAIN; }')
s=s.replace(extract(s,'static int64_t fs_file_size('),'static int64_t fs_file_size(vfile_t *vf) { (void)vf;return 4096; }')
s=s.replace('static wait_queue_t g_file_lock_waiters = WAIT_QUEUE_INIT;','')
s=s.replace(extract(s,'static void fs_lock_wake_waiters('),'static void fs_lock_wake_waiters(void) { assert(!held); __atomic_fetch_add(&wakes,1,__ATOMIC_RELAXED); }')
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <pthread.h>
#define FS_LOCK_OWNER_PID 1
#define FS_LOCK_OWNER_OFD 2
#define FS_LOCK_OWNER_NATIVE 3
#define SPINLOCK_INIT 0
typedef int spinlock_t;
typedef struct {uintptr_t ino;void *mnt;} vnode_t;
typedef struct {vnode_t *vnode;uintptr_t offset;} vfile_t;
typedef struct {int pid,tgid;} task_t;
typedef struct {short l_type,l_whence;int64_t l_start,l_len;int l_pid;} fs_flock_t;
static _Thread_local int held;
static int wakes,fail_alloc;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static task_t task={7,7};
static task_t *proc_current(void){return &task;}
static uint64_t spin_lock_irqsave(spinlock_t *p){(void)p;assert(!held);pthread_mutex_lock(&mutex);held=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t *p,uint64_t f){(void)p;(void)f;assert(held);held=0;pthread_mutex_unlock(&mutex);}
static void *kmalloc(size_t n){return fail_alloc?NULL:malloc(n);}
#define kfree free
'''
post=r'''
static void verify(void){unsigned n=0;for(int i=0;i<FS_FILE_LOCK_MAX;i++)n+=!!g_file_locks[i].used;assert(n==g_file_lock_used&&!held);}
static unsigned rng=13579;
static unsigned random_next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static vfile_t concurrent_files[4];
static void *worker(void *arg){uintptr_t id=(uintptr_t)arg;vfile_t *vf=&concurrent_files[id];
 fs_flock_t lock={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_len=100};
 for(int i=0;i<5000;i++){
  assert(!fs_locks_set(vf,&lock,FS_LOCK_OWNER_PID,id+100,0));
  fs_locks_release_process_file(vf,id+100);
  uint64_t flags=spin_lock_irqsave(&g_file_lock_table_lock);
  for(int j=0;j<FS_FILE_LOCK_MAX;j++)assert(!g_file_locks[j].used||g_file_locks[j].owner!=id+100);
  spin_unlock_irqrestore(&g_file_lock_table_lock,flags);
 }return NULL;}
int main(void){vfile_t files[3]={{0}};
 for(int i=0;i<20000;i++){
  unsigned r=random_next();int kind=1+r%3,owner=1+(r>>4)%9;vfile_t *vf=&files[(r>>8)%3];
  fs_flock_t lock={.l_type=(r&16)?F_WRLCK:F_UNLCK,.l_whence=SEEK_SET,.l_start=(r>>12)%100,.l_len=1+(r>>20)%100};
  switch(r%6){
   case 0:case 1:case 2: {int ret=fs_locks_set(vf,&lock,kind,owner,0);assert(ret==0||ret==-EAGAIN||ret==-ENOLCK);break;}
   case 3:fs_locks_release_process(owner);break;
   case 4:fs_locks_release_native((void *)(uintptr_t)owner,(r&32)?vf:NULL);break;
   default:fs_locks_release_file(vf,owner);fs_locks_release_process_file(vf,owner);break;
  }verify();
 }
 for(int i=0;i<FS_FILE_LOCK_MAX;i++)g_file_locks[i].used=0;g_file_lock_used=0;
 fs_flock_t lock={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_len=1};
 for(int i=0;i<256;i++){lock.l_start=i*2;assert(!fs_locks_set(&files[0],&lock,FS_LOCK_OWNER_PID,i+1,0));verify();}
 lock.l_start=1000;assert(fs_locks_set(&files[0],&lock,FS_LOCK_OWNER_PID,999,0)==-ENOLCK);verify();
 for(int i=0;i<256;i++){fs_locks_release_process(i+1);verify();}
 assert(!g_file_lock_used);fail_alloc=1;assert(fs_locks_set(&files[0],&lock,1,1,0)==-ENOMEM);verify();fail_alloc=0;
 assert(!fs_flocks_apply(&files[0],LOCK_EX|LOCK_NB));fs_locks_release_process_file(&files[0],7);
 assert(fs_flocks_apply(&files[1],LOCK_EX|LOCK_NB)==0);fs_locks_release_file(&files[0],(uintptr_t)&files[0]);verify();
 pthread_t threads[4];for(uintptr_t i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
 for(int i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));verify();assert(!g_file_lock_used);
 puts("FILE_LOCK_SUMMARY: PASS 20000 concurrent acquire/release pairs; 20000 mutations, split/merge, ENOLCK/OOM rollback, all owner releases");return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'locks.c';p.write_text(pre+s+post);exe=Path(d)/'locks'
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-pthread',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
