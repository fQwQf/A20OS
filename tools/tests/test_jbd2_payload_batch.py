"""Exercise production journal payload I/O with fragmented and failed devices."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.tests.test_bcache_sync_batch_host import extract_function

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define PCACHE_PAGE_SIZE 4096
#define BCACHE_BLOCK_SIZE 512
typedef struct block_dev block_dev_t;
struct block_dev { int (*write_sector)(block_dev_t *,uint64_t,const void *,size_t); };
typedef struct {block_dev_t *dev;} cache_t;
typedef struct {cache_t *bc;} fs_t;
typedef struct {fs_t *fs;uint32_t block_size,maxlen;uint8_t data[4096];} jbd2_t;
typedef struct {jbd2_t j;uint32_t count;} ext4_journal_t;
static uint64_t map[1024], expected[256];
static unsigned writes,consumed,invalidated,allocations,frees,max_bytes;
static int alloc_fail, map_fail, metadata_fail, write_fail, target_fail;
static void *kmalloc(size_t n) {assert(n<=128*1024);if(alloc_fail)return NULL;allocations++;return malloc(n);}
static void kfree(void *p) {if(p){frees++;free(p);}}
static uint32_t jbd2_txn_count(ext4_journal_t *ej){return ej->count;}
static uint32_t jbd2_advance(jbd2_t *j,uint32_t n){return n+1==j->maxlen?1:n+1;}
static int jbd2_txn_block(ext4_journal_t *ej,uint32_t n,uint64_t *out){(void)ej;if((int)n==target_fail)return -EIO;*out=n;return 0;}
static int jbd2_physical_block(jbd2_t *j,uint32_t n,uint64_t *out){assert(n>0&&n<j->maxlen);if((int)n==map_fail)return -EIO;*out=map[n];return 0;}
static int jbd2_metadata_image(ext4_journal_t *ej,uint64_t n,uint8_t *out){if((int)n==metadata_fail)return -EIO;memset(out,(unsigned)n+1,ej->j.block_size);return 0;}
static void bcache_invalidate_page(cache_t *bc,uint64_t n){(void)bc;(void)n;invalidated++;}
static unsigned block_size;
static int write_sector(block_dev_t *dev,uint64_t lba,const void *buf,size_t sectors){
 (void)dev;assert(sectors&&sectors*512<=128*1024);writes++;
 if((int)writes==write_fail)return -EIO;
 assert(sectors*512%block_size==0);
 unsigned count=sectors*512/block_size;
 if(sectors*512>max_bytes)max_bytes=sectors*512;
 for(unsigned n=0;n<count;n++){
  assert(lba*512+(uint64_t)n*block_size==expected[consumed]*block_size);
  for(unsigned k=0;k<block_size;k++)assert(((const uint8_t *)buf)[n*block_size+k]==(uint8_t)(consumed+1));
  consumed++;
 }
 return 0;
}
static void reset(ext4_journal_t *ej,unsigned count,unsigned first,unsigned bytes){
 writes=consumed=invalidated=allocations=frees=max_bytes=0;
 alloc_fail=0;map_fail=metadata_fail=write_fail=target_fail=-1;
 ej->count=count;ej->j.block_size=bytes;ej->j.maxlen=256;block_size=bytes;
 for(unsigned i=1;i<1024;i++)map[i]=1000+i;
 for(unsigned i=0,log=first;i<count;i++,log=jbd2_advance(&ej->j,log))expected[i]=map[log];
}
'''

MAIN = r'''
int main(void){
 block_dev_t dev={write_sector};cache_t bc={&dev};fs_t fs={&bc};ext4_journal_t ej={.j={.fs=&fs}};
 for(unsigned bytes=1024;bytes<=4096;bytes*=2){
  reset(&ej,187,20,bytes);assert(!jbd2_write_transaction_data(&ej,20));
  assert(consumed==187&&invalidated==187&&allocations==frees);
  assert(writes==(187+128*1024/bytes-1)/(128*1024/bytes));
 }
 printf("187 contiguous 4KiB blocks: %u device writes (previously 187)\n",writes);
 reset(&ej,70,240,4096);assert(!jbd2_write_transaction_data(&ej,240));assert(writes==3&&consumed==70);
 reset(&ej,70,20,4096);for(unsigned i=0;i<70;i++){map[20+i]=2000+2*i;expected[i]=map[20+i];}
 assert(!jbd2_write_transaction_data(&ej,20));assert(writes==70&&consumed==70);
 reset(&ej,70,20,4096);alloc_fail=1;assert(!jbd2_write_transaction_data(&ej,20));assert(writes==70&&consumed==70&&!allocations&&!frees);
 reset(&ej,0,20,4096);assert(!jbd2_write_transaction_data(&ej,20));assert(!writes&&!allocations);
 reset(&ej,1,20,4096);assert(!jbd2_write_transaction_data(&ej,20));assert(writes==1&&!allocations);
 reset(&ej,70,20,4096);write_fail=2;assert(jbd2_write_transaction_data(&ej,20)==-EIO);assert(writes==2&&consumed==32&&allocations==frees);
 reset(&ej,70,20,4096);metadata_fail=40;assert(jbd2_write_transaction_data(&ej,20)==-EIO);assert(writes==1&&consumed==32&&allocations==frees);
 reset(&ej,70,20,4096);map_fail=21;assert(jbd2_write_transaction_data(&ej,20)==-EIO);assert(!writes&&allocations==frees);
 reset(&ej,70,20,4096);target_fail=2;assert(jbd2_write_transaction_data(&ej,20)==-EIO);assert(!writes&&allocations==frees);
 reset(&ej,70,20,4096);bc.dev=NULL;assert(jbd2_write_transaction_data(&ej,20)==-EIO);assert(!writes&&!allocations);
 puts("journal payload: contents, bounds, ring wrap, fragmentation, low memory, I/O errors PASS");
}
'''


class JournalPayloadBatch(unittest.TestCase):
    def test_production_payload(self):
        source = (ROOT / "kernel/fs/diskfs/ext4_journal.c").read_text()
        function = extract_function(source, "static int jbd2_write_transaction_data(")
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / "test.c"
            code.write_text(PRELUDE + function + MAIN)
            binary = code.with_suffix("")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-O2", str(code), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
