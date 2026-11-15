"""Execute the production checkpoint branch with a held-page-only sync adapter."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <string.h>
#include <stdint.h>
#define JBD2_TXN_COMMITTED_NEEDS_CHECKPOINT 2U
#define JBD2_TXN_CHECKPOINTED_NEEDS_LOG_CLEAR 3U
typedef struct {int held,writes,fail,crashes;} cache_t;
typedef struct {cache_t *bc;} fs_t;
typedef struct {uint32_t txn_phase;} ext4_journal_t;
static int bcache_sync_held(cache_t *bc){
 if(!bc->held)return 0;
 bc->writes++;
 return bc->fail ? -5 : 0;
}
static void bcache_release_holds(cache_t *bc){bc->held=0;}
static void jbd2_crash_point(ext4_journal_t *ej,const char *point){(void)ej;assert(!strcmp(point,"post-checkpoint"));}
static int checkpoint(ext4_journal_t *ej,fs_t *fs){
 int ret=0;uint32_t txn_phase=ej->txn_phase;
'''
MAIN=r'''
out:return ret;
}
int main(void){
 cache_t bc={.held=1,.fail=1};fs_t fs={&bc};ext4_journal_t ej={2};
 assert(checkpoint(&ej,&fs)==-5);
 assert(bc.held==1&&bc.writes==1&&ej.txn_phase==2);
 bc.fail=0;
 assert(checkpoint(&ej,&fs)==0);
 assert(bc.held==0&&bc.writes==2&&ej.txn_phase==3);
 return 0;
}
'''
class CheckpointOrder(unittest.TestCase):
 def test_failed_checkpoint_retains_selection_and_retry_phase(self):
  s=(ROOT/'kernel/fs/diskfs/ext4_journal.c').read_text()
  a=s.index('    if (txn_phase == JBD2_TXN_COMMITTED_NEEDS_CHECKPOINT) {')
  b=s.index('    checkpoint_done = timer_get_ticks();',a)
  code=s[a:b]
  old=code.replace('        bcache_release_holds(fs->bc);\n','')
  old=old.replace('        ret = bcache_sync_held(fs->bc);','        bcache_release_holds(fs->bc);\n        ret = bcache_sync_held(fs->bc);')
  with tempfile.TemporaryDirectory(prefix='a20-jbd2-checkpoint-') as d:
   p=Path(d)
   for name,body in [('fixed',code),('released-before-sync',old)]:
    c=p/(name+'.c');c.write_text(PRE+body+MAIN);exe=p/name
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(c),'-o',str(exe)],check=True)
    r=subprocess.run([str(exe)],capture_output=True)
    if name=='fixed':self.assertEqual(r.returncode,0,r.stderr)
    else:self.assertNotEqual(r.returncode,0)
if __name__=='__main__':unittest.main()
