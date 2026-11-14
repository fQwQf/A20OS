"""Closing a probe/old DRM file must not restore over the live scanout owner."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.tests.test_bcache_sync_batch_host import extract_function

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
typedef pthread_mutex_t mutex_t;
typedef struct {int id;} drm_context_t;
typedef struct {drm_context_t *context;} drm_call_t;
typedef struct {int unused;} drm_gem_t;
static mutex_t g_scanout_owner_lock=PTHREAD_MUTEX_INITIALIZER;
static drm_context_t *g_scanout_owner;
static unsigned resumed;
static atomic_int entered,allow_present,finished;
static int present_result,blocking;
static void mutex_lock(mutex_t *m){assert(!pthread_mutex_lock(m));}
static void mutex_unlock(mutex_t *m){assert(!pthread_mutex_unlock(m));}
static void fbcon_resume(void){resumed++;}
#define kinfo(...) ((void)0)
static int drm_present_buffer_at_locked(drm_call_t *call,drm_gem_t *b,uint32_t x,uint32_t y){
 drm_context_t *ctx=call->context; (void)b;(void)x;(void)y;
 if(blocking){atomic_store(&entered,1);while(!atomic_load(&allow_present))sched_yield();}
 if(!present_result)g_scanout_owner=ctx;
 return present_result;
}
'''
MAIN = r'''
static int present_ctx(drm_context_t *ctx,drm_gem_t *b,uint32_t x,uint32_t y){drm_call_t call={ctx};return drm_present_buffer_at(&call,b,x,y);}
static drm_context_t a={1},b={2},probe={3};
static void *present(void *p){(void)p;assert(!present_ctx(&a,0,0,0));return 0;}
static void *release(void *p){(void)p;drm_console_release(&a);atomic_store(&finished,1);return 0;}
int main(void){
 assert(!present_ctx(&a,0,0,0));drm_console_release(&probe);assert(!resumed&&g_scanout_owner==&a);
 assert(!present_ctx(&b,0,0,0));drm_console_release(&a);assert(!resumed&&g_scanout_owner==&b);
 drm_console_release(&b);assert(resumed==1&&!g_scanout_owner);drm_console_release(&b);assert(resumed==1);
 present_result=-5;assert(present_ctx(&a,0,0,0)==-5);drm_console_release(&a);assert(resumed==1);
 present_result=0;blocking=1;pthread_t t1,t2;assert(!pthread_create(&t1,0,present,0));
 while(!atomic_load(&entered))sched_yield();
 assert(!pthread_create(&t2,0,release,0));
 for(unsigned n=0;n<10000;n++)sched_yield();
 assert(!atomic_load(&finished));atomic_store(&allow_present,1);
 assert(!pthread_join(t1,0));assert(!pthread_join(t2,0));
 assert(resumed==2&&!g_scanout_owner&&atomic_load(&finished));
}
'''


class DrmConsoleOwner(unittest.TestCase):
    def test_production_ownership_and_present_lock(self):
        source = (ROOT / "kernel/drivers/gpu/drm.c").read_text()
        selected = "\n".join(extract_function(source, signature) for signature in (
            "static void drm_console_release(", "static int drm_present_buffer_at("))
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / "test.c"
            code.write_text(PRELUDE + selected + MAIN)
            binary = code.with_suffix("")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-pthread", "-O2", str(code), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
