"""Execute production transport and finalization against a dead Native peer."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def run_c(test, fixture, source, main, mutation):
    with tempfile.TemporaryDirectory(prefix="a20-dead-uxfs-") as directory:
        work = Path(directory)
        for negative in (False, True):
            code = source if not negative else mutation(source)
            path, binary = work / 'test.c', work / 'test'
            path.write_text(fixture + code + main)
            subprocess.run(['cc', '-std=gnu11', '-O2', str(path), '-o', str(binary)],
                           check=True, capture_output=True)
            result = subprocess.run([str(binary)], capture_output=True)
            if negative:
                test.assertNotEqual(result.returncode, 0)
            else:
                test.assertEqual(result.returncode, 0, result.stderr.decode())


class DeadNativePeer(unittest.TestCase):
    def test_actual_channel_bridges_reject_dead_tasks_before_io(self):
        text = (ROOT / 'kernel/fs/uxfs/uxfs.c').read_text()
        source = text[text.index('static int uxfs_peer_gone_locked('):text.index('static int a20_uxfs_bridge_has_ep(')]
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define PROC_UNUSED 0
#define PROC_ZOMBIE 5
#define A20_MSG_NONBLOCK 1
#define A20_ERR_WOULD_BLOCK 11
struct task {int state;};
struct endpoint {int closed;};
typedef struct {struct endpoint *ep;struct task *server;} uxfs_sb_t;
typedef int a20_ch_handle_info_t;
static int sends,receives;
static int a20_channel_peer_closed(struct endpoint *e){assert(e);return e->closed;}
static int64_t a20_channel_send(struct endpoint *e,void*p,uint32_t n,void*h,int nh,void*x,uint32_t f){(void)p;(void)n;(void)h;(void)nh;(void)x;(void)f;assert(e);sends++;return 0;}
static int64_t a20_channel_recv_begin(struct endpoint*e,uint32_t f,uint32_t*n,uint32_t*nh){(void)f;(void)nh;assert(e);receives++;*n=48;return 0;}
static void a20_channel_recv_abort(struct endpoint*e){(void)e;assert(0);}
static int64_t a20_channel_recv_finish(struct endpoint*e,void*b,uint32_t*n,void*h,uint32_t*nh){(void)b;(void)n;(void)h;(void)nh;assert(e);return 0;}
'''
        main = r'''
int main(void){
 struct task task={1};struct endpoint endpoint={0};uxfs_sb_t sb={&endpoint,&task};
 uint8_t data[64]={0};uint32_t len=0;
 assert(a20_uxfs_bridge_send(&sb,data,48,1000,1)==0&&sends==1);
 assert(a20_uxfs_bridge_recv(&sb,data,64,1000,1,&len)==48&&receives==1&&len==48);
 for(int reason=0;reason<5;reason++){
  task.state=1;endpoint.closed=0;sb.ep=&endpoint;sb.server=&task;
  if(reason==0)task.state=PROC_ZOMBIE;
  if(reason==1)task.state=PROC_UNUSED;
  if(reason==2)endpoint.closed=1;
  if(reason==3)sb.server=0;
  if(reason==4)sb.ep=0;
  int oldsend=sends,oldrecv=receives;
  assert(a20_uxfs_bridge_send(&sb,data,48,1000,1)==2&&sends==oldsend);
  assert(a20_uxfs_bridge_recv(&sb,data,64,1000,1,&len)==-2&&receives==oldrecv);
 }
 return 0;
}
'''
        def mutation(code):
            old = 'state == PROC_UNUSED || state == PROC_ZOMBIE ||'
            self.assertIn(old, code)
            return code.replace(old, '0 ||')
        run_c(self, fixture, source, main, mutation)

    def test_actual_finalizer_releases_retired_peer_and_preserves_error(self):
        text = (ROOT / 'kernel/fs/vfs.c').read_text()
        source = text[text.index('int vfs_finalize_closed_vfile('):text.index('/*\n * Release a descriptor slot.', text.index('int vfs_finalize_closed_vfile('))]
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#define ktrace_vfs(...) ((void)0)
typedef struct {int held;} mount_t;
typedef struct {mount_t *mnt;} vnode_t;
typedef struct {vnode_t *vnode;uint64_t identity;int finalize_started,backend_close_done,shutdown_deferred_close,mount_open_counted,refs;} vfile_t;
static int error,retired,freed,puts,finished,events;
static void a20_eventq_on_vfile_destroy(vfile_t*v){(void)v;events++;}
static void vfs_release_open_file_locks(vfile_t*v,int n){(void)v;(void)n;}
static int vfile_backend_close(vfile_t*v){if(retired)v->backend_close_done=1;return error;}
static void vfile_ref_init(vfile_t*v,int n){v->refs=n;}
static void vfile_finalize_done(void){finished++;}
static void vfs_mount_vfile_put(mount_t*m){assert(m->held==1);m->held--;}
static void vfile_free(vfile_t*v){(void)v;freed++;}
static void vnode_put(vnode_t*v){(void)v;puts++;}
'''
        main = r'''
int main(void){
 for(int mode=0;mode<3;mode++){
  mount_t m={1};vnode_t vn={&m};vfile_t vf={.vnode=&vn,.mount_open_counted=1};
  error=mode?-5:0;retired=mode==2;freed=puts=finished=events=0;
  assert(vfs_finalize_closed_vfile(&vf)==error);
  assert(finished==1&&events==1);
  if(mode==1){assert(!freed&&!puts&&m.held==1&&vf.refs==1&&vf.shutdown_deferred_close);}
  else {assert(freed==1&&puts==1&&m.held==0&&!vf.mount_open_counted);}
 }
 return 0;
}
'''
        def mutation(code):
            old = 'close_error < 0 && !vf->backend_close_done'
            self.assertIn(old, code)
            return code.replace(old, 'close_error < 0')
        run_c(self, fixture, source, main, mutation)


    def test_actual_identity_clear_drops_only_retired_peer_cache_ownership(self):
        text = (ROOT / 'kernel/fs/uxfs/uxfs.c').read_text()
        source = text[text.index('static void uxfs_identity_forget('):text.index('/* Drop every cache-only')]
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {int refs,cache,dirty;} vnode_t;
typedef struct {void *ep;} uxfs_sb_t;
static int g_uxfs_identity_lock,indexed,invalidates,discards;
static vnode_t node;
static uint64_t spin_lock_irqsave(int*p){(void)p;return 0;}
static void spin_unlock_irqrestore(int*p,uint64_t f){(void)p;(void)f;}
static void vnode_put(vnode_t*v){assert(v==&node&&v->refs>0);v->refs--;}
static void page_cache_invalidate(vnode_t*v){invalidates++;if(v->cache&&!v->dirty){v->cache=0;vnode_put(v);}}
static void page_cache_discard_unlinked(vnode_t*v){discards++;if(v->cache){v->cache=0;vnode_put(v);}}
static uintptr_t a20_vnode_identity_remove(uintptr_t sb,uint64_t ino){assert(sb&&ino==19);if(!indexed)return 0;indexed=0;return(uintptr_t)&node;}
static size_t a20_vnode_identity_clear_sb(uintptr_t sb,uintptr_t*out,size_t n){assert(sb&&n);if(!indexed)return 0;indexed=0;out[0]=(uintptr_t)&node;return 1;}
'''
        main = r'''
int main(void){
 for(int live=0;live<=1;live++)for(int dirty=0;dirty<=1;dirty++)for(int mapped=0;mapped<=1;mapped++){
  uxfs_sb_t sb={(void*)(uintptr_t)live};indexed=1;invalidates=discards=0;
  node=(vnode_t){2+mapped,1,dirty};
  uxfs_identity_clear_sb(&sb);
  assert(!indexed&&node.refs==mapped+(live&&dirty));
  assert(discards==!live&&invalidates==live);
 }
 uxfs_sb_t sb={(void*)1};indexed=1;node=(vnode_t){2,1,0};invalidates=discards=0;
 uxfs_identity_forget(&sb,19);assert(node.refs==0&&invalidates==1&&!discards);
 return 0;
}
'''
        def mutation(code):
            old = 'page_cache_discard_unlinked(victim);'
            self.assertIn(old, code)
            return code.replace(old, '(void)victim; /* missing old page ownership release */')
        run_c(self, fixture, source, main, mutation)

    def test_actual_unmount_drops_the_transferred_root_only_after_success(self):
        text = (ROOT / 'kernel/fs/uxfs/uxfs.c').read_text()
        release = text[text.index('void uxfs_release_vn('):text.index('/* ------------------------------------------------------------------ */', text.index('void uxfs_release_vn('))]
        source = release + text[text.index('int uxfs_unmount('):text.index('int uxfs_block_io(')]
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
struct vnode;
struct mount {struct vnode *root;};
typedef struct {uint32_t live_vnodes;} uxfs_sb_t;
typedef struct {uxfs_sb_t *sb;int is_dir;uint32_t mount_root_ref;} uxfs_vpriv_t;
typedef struct vnode {struct mount *mnt;int refs;void *fs_data;struct vnode *parent;} vnode_t;
static int g_uxfs_ns,backend_error,puts,frees;
static int a20_uxfs_ns_unmount(void*n,void*r){(void)n;(void)r;return backend_error;}
static void kfree(void*p){assert(p);frees++;}
void uxfs_release_vn(vnode_t*v);
static void vnode_put(vnode_t*v){assert(v->refs>0);v->refs--;puts++;if(!v->refs)uxfs_release_vn(v);}
'''
        main = r'''
int main(void){
 for(int external=0;external<=1;external++){
  struct mount m={0};uxfs_sb_t sb={1};uxfs_vpriv_t priv={&sb,1,1};
  vnode_t v={&m,1+external,&priv,0};m.root=&v;puts=frees=0;
  backend_error=-5;assert(uxfs_unmount(&v)==-5&&m.root==&v&&v.refs==1+external&&!puts);
  backend_error=0;assert(uxfs_unmount(&v)==0&&v.refs==external&&puts==1);
  if(external){
   assert(m.root==&v&&sb.live_vnodes==1&&!frees);
   assert(uxfs_unmount(&v)==0&&v.refs==1&&puts==1);
   vnode_put(&v);
  }
  assert(m.root==0&&v.fs_data==0&&sb.live_vnodes==0&&frees==2);
  assert(uxfs_unmount(m.root)==0&&puts==1+external);
 }
 return 0;
}
'''
        def mutation(code):
            old = '            vnode_put(root);'
            self.assertIn(old, code)
            return code.replace(old, '            (void)root; /* missing mount root release */')
        run_c(self, fixture, source, main, mutation)


if __name__ == '__main__':
    unittest.main()
