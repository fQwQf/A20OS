"""Host harness for the production Rust block-cache sync transaction."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def extract_function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise AssertionError(f"expected exactly one definition of {signature!r}")
    start = source.index(signature)
    brace = source.find("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(f"unterminated function {signature!r}")


RUST_PRELUDE = r'''
#![no_std]
use core::ffi::c_int;
#[repr(C)] pub struct BcacheEntry { pub lba:u64, pub dirty:c_int, pub dirty_gen:u64, pub ref_:c_int, pub valid:c_int, pub accessed:u8, pub data:[u8;512], pub prev:*mut BcacheEntry, pub next:*mut BcacheEntry, pub hnext:*mut BcacheEntry }
#[repr(C)] pub struct PcacheEntry { pub page_no:u64, pub dirty:c_int, pub dirty_gen:u64, pub ref_:c_int, pub valid:c_int, pub accessed:u8, pub held:c_int, pub data:*mut u8, pub prev:*mut PcacheEntry, pub next:*mut PcacheEntry, pub hnext:*mut PcacheEntry }
#[repr(C)] pub struct PcacheDirtySet { pub count:usize, pub pool:*mut PcacheEntry, pub pool_count:usize, pub words:*mut u64 }
pub const SYNC_ALL:i32=0; pub const SYNC_SCOPED:i32=1; pub const SYNC_HELD:i32=2;
pub type SyncCacheLockFn=extern "C" fn(*mut core::ffi::c_void)->u64;
pub type SyncCacheUnlockFn=extern "C" fn(*mut core::ffi::c_void,u64);
pub type SyncClearPageBatchFn=extern "C" fn(*mut core::ffi::c_void,*const usize,*const u64,*const u64,usize);
pub type SyncClearBlockFn=extern "C" fn(*mut core::ffi::c_void,usize,u64,u64);
pub type SyncWriteFn=extern "C" fn(*mut core::ffi::c_void,u64,*const u8,usize)->c_int;
#[repr(C)] #[derive(Clone,Copy)] pub struct SyncWrite { pub lba:u64, pub buf:*const u8, pub count:usize }
pub type SyncWriteManyFn=extern "C" fn(*mut core::ffi::c_void,*const SyncWrite,usize)->c_int;
pub type SyncFlushFn=extern "C" fn(*mut core::ffi::c_void)->c_int;
pub type SyncLockFn=extern "C" fn(*mut core::ffi::c_void);
pub type SyncErrorFn=extern "C" fn(*mut core::ffi::c_void);
pub type SyncAllocFn=extern "C" fn(*mut core::ffi::c_void,usize)->*mut u8;
pub type SyncFreeFn=extern "C" fn(*mut core::ffi::c_void,*mut u8);
'''

C_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define PAGE 4096
#define PCACHE_PAGE_SIZE PAGE
#define BCACHE_SYNC_BATCH_PAGES 256
void rust_eh_personality(void) {}
static void panic(const char *message, ...){(void)message;abort();}
typedef struct BcacheEntry { uint64_t lba; int dirty; uint64_t dirty_gen; int ref_,valid; uint8_t accessed; uint8_t data[512]; struct BcacheEntry *prev,*next,*hnext; } BcacheEntry;
typedef struct PcacheEntry { uint64_t page_no; int dirty; uint64_t dirty_gen; int ref_,valid; uint8_t accessed; int held; uint8_t *data; struct PcacheEntry *prev,*next,*hnext; } PcacheEntry;
typedef struct { size_t count; PcacheEntry *pool; size_t pool_count; uint64_t *words; } PcacheDirtySet;
typedef PcacheEntry pcache_entry_t;
typedef PcacheDirtySet pcache_dirty_set_t;
typedef struct { uint64_t lba; const uint8_t *buf; size_t count; } SyncWrite;
extern int a20_bcache_sync_transaction(void *,size_t,size_t,const uint64_t *,size_t,int,PcacheEntry *,PcacheDirtySet *,BcacheEntry *,uint64_t (*)(void *),void (*)(void *,uint64_t),void (*)(void *,size_t,uint64_t,uint64_t),void (*)(void *,const size_t *,const uint64_t *,const uint64_t *,size_t),int (*)(void *,uint64_t,const uint8_t *,size_t),int (*)(void *,const SyncWrite *,size_t),int (*)(void *),void (*)(void *),void (*)(void *),void (*)(void *),uint8_t *(*)(void *,size_t),void (*)(void *,uint8_t *));
typedef struct { int *dev; char *writeback_buffer; PcacheDirtySet dirty_pages; int page_pool_size; } bcache_t;
static int fake_dev; static uint8_t shared_writeback_buffer[256 * PAGE];
static int wb_locked;
static bcache_t sync_bc;
static PcacheEntry *pcache_find_adapter(void *,uint64_t);
static uint64_t pcache_get_global_lock(void *);
static void pcache_get_global_unlock(void *,uint64_t);
static int pcache_flush_write(void *,uint64_t,const uint8_t *,size_t);
static int a20_pcache_flush_transaction(void *,PcacheEntry *,uint8_t *,PcacheDirtySet *,size_t,PcacheEntry *(*)(void *,uint64_t),uint64_t (*)(void *),void (*)(void *,uint64_t),int (*)(void *,uint64_t,const uint8_t *,size_t));
/* ACTUAL_BORROW_RELEASE_HELPERS */
/* ACTUAL_PCACHE_FLUSH_RUN */
static unsigned eviction_flush_calls;
static PcacheEntry *pcache_find_adapter(void *ctx,uint64_t page_no){(void)ctx;(void)page_no;return NULL;}
static uint64_t pcache_get_global_lock(void *ctx){(void)ctx;return 0;}
static void pcache_get_global_unlock(void *ctx,uint64_t flags){(void)ctx;(void)flags;}
static int pcache_flush_write(void *ctx,uint64_t lba,const uint8_t *data,size_t sectors){(void)ctx;(void)lba;(void)data;(void)sectors;return 0;}
static int a20_pcache_flush_transaction(void *ctx,PcacheEntry *first,uint8_t *buffer,PcacheDirtySet *dirty,size_t max_pages,PcacheEntry *(*find)(void *,uint64_t),uint64_t (*lock)(void *),void (*unlock)(void *,uint64_t),int (*write)(void *,uint64_t,const uint8_t *,size_t)){(void)first;(void)find;(void)lock;(void)unlock;(void)write;assert(ctx!=NULL&&wb_locked&&buffer==shared_writeback_buffer&&dirty==&sync_bc.dirty_pages&&max_pages==256);eviction_flush_calls++;return 0;}
static PcacheEntry *pool; static PcacheDirtySet dirty_set; static uint64_t dirty_words[5]; static uint8_t payload[257][PAGE]; static int cache_locked, fail_alloc, fail_write, mutate_on_write;
static int unlock_count, io_calls, flush_calls, error_calls, free_calls, alloc_calls, release_calls, clear_count, write_batches;
static size_t last_nr; static SyncWrite last_vectors[16]; static unsigned char *saved_write;
static int vector_content_ok;
static bcache_t sync_bc={.dev=&fake_dev,.writeback_buffer=(char *)shared_writeback_buffer};
static int borrow_mode; static uint8_t *last_borrowed;
static uint64_t cache_lock(void *p){(void)p;assert(!cache_locked);cache_locked=1;return 7;}
static void cache_unlock(void *p,uint64_t f){(void)p;assert(cache_locked&&f==7);cache_locked=0;unlock_count++;}
static void clear_block(void *p,size_t i,uint64_t l,uint64_t g){(void)p;(void)i;(void)l;(void)g;}
static void clear_pages(void *p,const size_t *ix,const uint64_t *no,const uint64_t *gen,size_t n){(void)p;assert(!cache_locked);for(size_t j=0;j<n;j++){PcacheEntry *e=&pool[ix[j]];if(e->valid&&e->page_no==no[j]&&e->dirty_gen==gen[j]){e->dirty=0;size_t word=ix[j]/64;uint64_t bit=UINT64_C(1)<<(ix[j]%64);if(dirty_words[word]&bit){dirty_words[word]&=~bit;dirty_set.count--;}}}clear_count+=(int)n;}
static int write_one(void *p,uint64_t l,const uint8_t *b,size_t n){(void)p;(void)l;(void)b;(void)n;assert(!cache_locked);io_calls++;return -99;}
static int write_many(void *p,const SyncWrite *v,size_t n){(void)p;assert(!cache_locked);write_batches++;io_calls+=(int)n;last_nr=n;assert(n<=16);memcpy(last_vectors,v,n*sizeof(*v));if(n) saved_write=(unsigned char *)v[0].buf;if(n==2&&v[1].count==16)vector_content_ok=((const uint8_t*)v[1].buf)[0]==3&&((const uint8_t*)v[1].buf)[PAGE]==4;if(mutate_on_write&&pool)pool[0].dirty_gen++;return fail_write?-5:0;}
static int flush_cb(void *p){(void)p;assert(!cache_locked&&!wb_locked);flush_calls++;return 0;}
static void wb_lock(void *p){(void)p;assert(!wb_locked);wb_locked=1;}
static void wb_unlock(void *p){(void)p;assert(wb_locked);wb_locked=0;}
static void note_error(void *p){(void)p;error_calls++;}
static uint8_t *alloc_cb(void *p,size_t n){alloc_calls++;if(borrow_mode){assert(wb_locked);last_borrowed=bcache_sync_borrow_buffer(p,n);return last_borrowed;}if(fail_alloc)return NULL;return malloc(n);}
static void free_cb(void *p,uint8_t *buffer){if(borrow_mode){assert(wb_locked);release_calls++;bcache_sync_release_buffer(p,buffer);return;}free_calls++;free(buffer);}
static void reset(void){cache_locked=wb_locked=fail_alloc=fail_write=mutate_on_write=borrow_mode=0;unlock_count=io_calls=flush_calls=error_calls=free_calls=alloc_calls=release_calls=clear_count=write_batches=0;last_nr=0;saved_write=NULL;last_borrowed=NULL;vector_content_ok=0;memset(last_vectors,0,sizeof(last_vectors));memset(dirty_words,0,sizeof(dirty_words));dirty_set=(PcacheDirtySet){0};}
static void init(PcacheEntry *p,size_t n){memset(p,0,n*sizeof(*p));pool=p;dirty_set=(PcacheDirtySet){.count=n,.pool=p,.pool_count=n,.words=dirty_words};for(size_t i=0;i<n;i++){p[i].page_no=i*10;p[i].valid=1;p[i].dirty=1;p[i].dirty_gen=1;p[i].data=payload[i];memset(p[i].data,(int)(i+1),PAGE);dirty_words[i/64]|=UINT64_C(1)<<(i%64);}}
static int run(PcacheEntry *p,size_t n,const uint64_t *sel,size_t ns,int mode){BcacheEntry b={0};return a20_bcache_sync_transaction(&sync_bc,n,0,sel,ns,mode,p,&dirty_set,&b,cache_lock,cache_unlock,clear_block,clear_pages,write_one,write_many,flush_cb,wb_lock,wb_unlock,note_error,alloc_cb,free_cb);}
static void test_vectors_and_selection(void){
 static PcacheEntry p[4];uint64_t sel[]={0,20,21};reset();init(p,4);p[3].page_no=21;assert(run(p,4,sel,3,1)==0);assert(write_batches==1&&last_nr==2);assert(last_vectors[0].lba==0&&last_vectors[0].count==8);assert(last_vectors[1].lba==160&&last_vectors[1].count==16);assert(vector_content_ok);assert(p[0].dirty==0&&p[1].dirty==1&&p[2].dirty==0&&p[3].dirty==0);assert(clear_count==3&&flush_calls==1&&free_calls==1);
 reset();init(p,4);p[0].held=1;p[1].held=0;p[2].held=1;p[3].held=0;assert(run(p,4,NULL,0,2)==0);assert(clear_count==2&&p[0].dirty==0&&p[1].dirty==1&&p[2].dirty==0&&p[3].dirty==1&&flush_calls==1);
}
static void test_vector_and_page_limits(void){
 static PcacheEntry p[257];reset();init(p,17);for(int i=0;i<17;i++)p[i].page_no=(uint64_t)i*2;assert(run(p,17,NULL,0,0)==0);assert(write_batches==2&&last_nr==1&&clear_count==17&&flush_calls==1);
 reset();init(p,257);for(size_t i=0;i<257;i++)p[i].page_no=i;assert(run(p,257,NULL,0,0)==0);assert(write_batches==2&&last_nr==1&&last_vectors[0].count==8&&clear_count==257);
}
static void test_generation_error_and_alloc_failure(void){
 static PcacheEntry p[2];reset();init(p,2);for(int i=0;i<2;i++)p[i].page_no=i;mutate_on_write=1;assert(run(p,2,NULL,0,0)==0);assert(p[0].dirty==1&&p[1].dirty==0);
 reset();init(p,2);for(int i=0;i<2;i++)p[i].page_no=i;fail_write=1;assert(run(p,2,NULL,0,0)==-5);assert(p[0].dirty==1&&p[1].dirty==1&&clear_count==0&&flush_calls==0&&error_calls==1&&free_calls==1&&!wb_locked);
 reset();init(p,2);fail_alloc=1;assert(run(p,2,NULL,0,0)==-12);assert(io_calls==0&&flush_calls==0&&free_calls==0&&!wb_locked&&cache_locked==0&&unlock_count>0);
}
static void test_borrowed_writeback_buffer(void){
 static PcacheEntry p[2];
 reset();init(p,2);borrow_mode=1;assert(run(p,2,NULL,0,0)==0);assert(alloc_calls==1&&release_calls==1&&free_calls==0&&!wb_locked&&last_borrowed==shared_writeback_buffer);
 reset();init(p,2);borrow_mode=1;assert(run(p,2,NULL,0,0)==0);assert(alloc_calls==1&&release_calls==1&&free_calls==0&&last_borrowed==shared_writeback_buffer);
 reset();init(p,2);borrow_mode=1;fail_write=1;assert(run(p,2,NULL,0,0)==-5);assert(alloc_calls==1&&release_calls==1&&free_calls==0&&!wb_locked&&shared_writeback_buffer[0]==1);
 reset();init(p,2);borrow_mode=1;p[0].held=p[1].held=1;assert(run(p,2,NULL,0,0)==0);assert(alloc_calls==0&&release_calls==0&&free_calls==0&&!wb_locked&&flush_calls==1);
 /* Eviction flushes serialize on the same exclusive lock and reuse this owner buffer. */
 wb_lock(&sync_bc);uint8_t *eviction_buffer=bcache_sync_borrow_buffer(&sync_bc,sizeof(shared_writeback_buffer));assert(eviction_buffer==shared_writeback_buffer);bcache_sync_release_buffer(&sync_bc,eviction_buffer);assert(pcache_flush_run(&sync_bc,&p[0])==0&&eviction_flush_calls==1);wb_unlock(&sync_bc);assert(free_calls==0);
 /* The old per-sync kmalloc/free contract remains detectable as a negative control. */
 reset();init(p,2);assert(run(p,2,NULL,0,0)==0);assert(alloc_calls==1&&free_calls==1);reset();init(p,2);assert(run(p,2,NULL,0,0)==0);assert(alloc_calls==1&&free_calls==1);
 reset();wb_lock(&sync_bc);assert(bcache_sync_borrow_buffer(&sync_bc,sizeof(shared_writeback_buffer)+1)==NULL);wb_unlock(&sync_bc);
}
int main(void){test_vectors_and_selection();test_vector_and_page_limits();test_generation_error_and_alloc_failure();test_borrowed_writeback_buffer();puts("bcache-sync-batch: production Rust transaction host tests PASS");return 0;}
'''

C_ADAPTER_PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define PCACHE_PAGE_SIZE 4096
#define BCACHE_BLOCK_SIZE 512
#define A20_PERF_PCACHE_WRITEBACK_IOS 1
#define A20_PERF_PCACHE_WRITEBACK_PAGES 2
typedef struct block_write { uint64_t lba; const void *buf; size_t count; } block_write_t;
typedef struct block_dev block_dev_t;
struct block_dev {
    int (*write_sector)(block_dev_t *, uint64_t, const void *, size_t);
    int (*write_batch)(block_dev_t *, const block_write_t *, size_t);
    uint32_t sector_size;
};
typedef struct { block_dev_t *dev; } bcache_t;
static uint64_t accounted_bytes;
static unsigned perf_ios;
static uint64_t perf_pages;
static void proc_io_account(uint64_t a,uint64_t b,uint64_t c,uint64_t d) { (void)a;(void)b;(void)c;accounted_bytes+=d; }
static void a20_perf_count(int c) { if(c==A20_PERF_PCACHE_WRITEBACK_IOS)perf_ios++; }
static void a20_perf_add(int c,uint64_t n) { if(c==A20_PERF_PCACHE_WRITEBACK_PAGES)perf_pages+=n; }
static uint64_t seen_lba[8]; static const void *seen_buf[8]; static size_t seen_count[8];
static unsigned sector_calls,batch_calls,batch_nr; static int fail_sector_at=-1,batch_result;
static const block_write_t *batch_seen;
static int sector_write(block_dev_t *d,uint64_t l,const void *b,size_t n) {
    (void)d;unsigned i=sector_calls++;seen_lba[i]=l;seen_buf[i]=b;seen_count[i]=n;
    return ((int)i==fail_sector_at)?-5:(int)n;
}
static int batch_write(block_dev_t *d,const block_write_t *v,size_t n) {
    (void)d;batch_calls++;batch_nr=(unsigned)n;batch_seen=v;return batch_result;
}
'''

C_ADAPTER_MAIN = r'''
int main(void) {
    block_dev_t dev={.write_sector=sector_write,.write_batch=NULL,.sector_size=512};
    bcache_t bc={.dev=&dev}; uint8_t a[4096],b[4096],c[4096],d[4096];
    block_write_t v[]={{7,a,8},{8,b,4},{20,c,2},{21,d,1}};
    memset(a,1,sizeof(a));memset(b,2,sizeof(b));memset(c,3,sizeof(c));memset(d,4,sizeof(d));
    fail_sector_at=2;
    int r=bcache_sync_write_many(&bc,v,3);
    assert(r==-5&&sector_calls==3); /* fourth vector must not be attempted */
    assert(seen_lba[0]==7&&seen_buf[0]==a&&seen_count[0]==8);
    assert(seen_lba[1]==8&&seen_buf[1]==b&&seen_count[1]==4);
    assert(seen_lba[2]==20&&seen_buf[2]==c&&seen_count[2]==2);
    assert(accounted_bytes==12*512&&perf_ios==3&&perf_pages==1);

    accounted_bytes=perf_pages=0;perf_ios=0;sector_calls=batch_calls=0;
    dev.write_batch=batch_write;batch_result=0;
    r=bcache_sync_write_many(&bc,v,4);
    assert(r==0&&batch_calls==1&&batch_nr==4&&batch_seen==v);
    assert(sector_calls==0&&accounted_bytes==15*512&&perf_ios==4&&perf_pages==1);

    accounted_bytes=perf_pages=0;perf_ios=0;batch_calls=0;batch_result=-5;
    r=bcache_sync_write_many(&bc,v,4);
    assert(r==-5&&batch_calls==1&&sector_calls==0);
    assert(accounted_bytes==0&&perf_ios==4&&perf_pages==1);
    puts("bcache-sync-write-many: production C adapter fallback and batch PASS");
    return 0;
}
'''


class BcacheSyncBatchHostTests(unittest.TestCase):
    def test_production_sync_transaction(self):
        rustc, cc = shutil.which("rustc"), shutil.which("cc")
        if not rustc or not cc:
            self.skipTest("rustc and cc are required for this x86 host harness")
        source = (ROOT / "kernel/fs/block_cache_state.rs").read_text()
        selected = extract_function(source, "pub unsafe extern \"C\" fn a20_bcache_sync_page_selected(")
        select = extract_function(source, "pub unsafe extern \"C\" fn a20_bcache_sync_select(")
        next_dirty = extract_function(source, "unsafe fn pcache_next_dirty_slot(")
        next_selected = extract_function(source, "unsafe fn pcache_next_selected_dirty_slot(")
        transaction = extract_function(source, "pub unsafe extern \"C\" fn a20_bcache_sync_transaction(")
        with tempfile.TemporaryDirectory(prefix="a20-bcache-sync-batch-") as directory:
            tmp = Path(directory)
            rs, lib, c, exe = tmp / "transaction.rs", tmp / "libtransaction.a", tmp / "host.c", tmp / "host"
            rs.write_text(RUST_PRELUDE + selected + "\n" + select + "\n" + next_dirty + "\n" + next_selected + "\n#[no_mangle]\n" + transaction +
                          "\n#[panic_handler] fn panic(_: &core::panic::PanicInfo<'_>) -> ! { loop {} }\n")
            adapter_source = (ROOT / "kernel/fs/block_cache.c").read_text()
            borrow_helpers = "\n\n".join(
                extract_function(adapter_source, signature)
                for signature in (
                    "static uint8_t *bcache_sync_borrow_buffer(",
                    "static void bcache_sync_release_buffer(",
                    "static int pcache_flush_run(",
                )
            )
            c.write_text(C_HARNESS.replace(
                "/* ACTUAL_BORROW_RELEASE_HELPERS */", borrow_helpers
            ))
            subprocess.run([rustc, "--edition=2021", "--crate-type", "staticlib", "-C", "panic=abort", "-C", "opt-level=2", str(rs), "-o", str(lib)], check=True, cwd=ROOT)
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", str(c), str(lib), "-o", str(exe)], check=True, cwd=ROOT)
            result = subprocess.run([str(exe)], check=False, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("production Rust transaction", result.stdout)

    def test_production_c_write_many_adapter(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("cc is required for this x86 host harness")
        source = (ROOT / "kernel/fs/block_cache.c").read_text()
        functions = "\n\n".join(
            extract_function(source, signature)
            for signature in (
                "static void bcache_account_device_io(",
                "static int bcache_sync_write(",
                "static int bcache_sync_write_many(",
            )
        )
        with tempfile.TemporaryDirectory(prefix="a20-bcache-write-many-") as directory:
            tmp = Path(directory)
            c_file, exe = tmp / "write_many.c", tmp / "write_many"
            c_file.write_text(C_ADAPTER_PRELUDE + functions + C_ADAPTER_MAIN)
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", str(c_file), "-o", str(exe)],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run([str(exe)], check=False, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("production C adapter", result.stdout)


if __name__ == "__main__":
    unittest.main()
