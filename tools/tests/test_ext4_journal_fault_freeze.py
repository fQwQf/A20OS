"""A poisoned C journal must reject metadata before changing its cache image."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]
PRE = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#define PCACHE_PAGE_SIZE 4096
typedef struct { int enabled,txn_faulted,lock; } ext4_journal_t;
typedef struct { ext4_journal_t *journal; void *bc; unsigned block_size; } ext4_sb_info_t;
static int writes,holds,records,write_error;
static int ext4_is_inconsistent(ext4_sb_info_t *fs){(void)fs;return 0;}
static void mutex_lock(int *p){assert(!*p);*p=1;}
static void mutex_unlock(int *p){assert(*p);*p=0;}
static int bcache_write_bytes(void *bc,uint64_t off,const void *buf,size_t n){(void)bc;(void)off;(void)buf;(void)n;writes++;return write_error;}
static void bcache_hold_page(void *bc,uint64_t p){(void)bc;(void)p;holds++;}
static int jbd2_record_block(ext4_journal_t *ej,uint64_t b){(void)ej;(void)b;records++;return 0;}
#define printf(...) ((void)0)
'''
MAIN = r'''
int main(void){
 ext4_journal_t ej={.enabled=1,.txn_faulted=1};
 ext4_sb_info_t fs={.journal=&ej,.block_size=4096};char byte=1;
 assert(ext4_journal_meta_write(&fs,1024,&byte,1)==-EIO);
 assert(!writes&&!holds&&!records&&!ej.lock);
 ej.txn_faulted=0;
 assert(ext4_journal_meta_write(&fs,1024,&byte,1)==0);
 assert(writes==1&&holds==1&&records==1&&!ej.lock);
 write_error=-EIO;
 assert(ext4_journal_meta_write(&fs,2048,&byte,1)==-EIO&&ej.txn_faulted);
 assert(writes==2&&holds==1&&records==1);
 write_error=0;
 assert(ext4_journal_meta_write(&fs,1024,&byte,1)==-EIO);
 assert(writes==2&&holds==1&&records==1&&!ej.lock);
 return 0;
}
'''


class JournalFaultFreeze(unittest.TestCase):
    def test_c_collector_freezes_before_cache_mutation(self):
        source = (ROOT / 'kernel/fs/diskfs/ext4_journal.c').read_text()
        code = function(source, 'ext4_journal_meta_write')
        guard = '''    if (ej->txn_faulted) {
        mutex_unlock(&ej->lock);
        return -EIO;
    }
'''
        old = code.replace(guard, '')
        self.assertNotEqual(code, old)
        with tempfile.TemporaryDirectory() as directory:
            def run(body, name):
                path = Path(directory) / (name + '.c')
                exe = path.with_suffix('')
                path.write_text(PRE + body + MAIN)
                subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=undefined', str(path), '-o', str(exe)],
                               check=True, capture_output=True)
                return subprocess.run([str(exe)], capture_output=True, timeout=5).returncode
            self.assertEqual(run(code, 'fixed'), 0)
            self.assertNotEqual(run(old, 'old'), 0)


if __name__ == '__main__':
    unittest.main()
