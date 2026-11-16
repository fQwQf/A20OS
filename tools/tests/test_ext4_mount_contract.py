"""Existing ordered mode cannot silently accept another journal source/mode."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_pt_mcs_preempt_window import extract_function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define EOPNOTSUPP 95
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL 4U
typedef struct {uint32_t s_feature_compat,s_journal_inum,s_journal_dev;} ext4_superblock_t;
'''
MAIN=r'''
int main(void){
 const char *bad[]={"data=journal","data=writeback","journal_async_commit","journal_checksum", "barrier=0","nobarrier","fast_commit","journal_dev","journal_dev=12","journal_path=/dev/vdc","journal_async_commit=1"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(ext4_validate_mount_options(bad[i])==-EOPNOTSUPP);
 assert(!ext4_validate_mount_options(NULL));assert(!ext4_validate_mount_options("rw,data=ordered,barrier=1"));
 assert(ext4_validate_mount_options("rw,journal_dev=12,ro")==-EOPNOTSUPP);
 ext4_superblock_t sb={.s_feature_compat=4,.s_journal_inum=8};
 assert(!ext4_validate_journal_source(&sb));
 sb.s_journal_dev=1;assert(ext4_validate_journal_source(&sb)==-EOPNOTSUPP);
 sb.s_journal_inum=0;assert(ext4_validate_journal_source(&sb)==-EOPNOTSUPP);
 sb.s_journal_dev=0;assert(ext4_validate_journal_source(&sb)==-EOPNOTSUPP);
 sb.s_feature_compat=0;assert(!ext4_validate_journal_source(&sb));
 return 0;
}
'''
class MountContract(unittest.TestCase):
 def test_mode_parameters_and_external_sources_fail_closed(self):
  s=(ROOT/'kernel/fs/diskfs/ext4.c').read_text()
  options=extract_function(s,'int ext4_validate_mount_options(const char *opts)')
  source=extract_function(s,'static int ext4_validate_journal_source(const ext4_superblock_t *sb)')
  old=options.replace("(option_len == len ||\n                 (len > option_len && p[option_len] == '='))",'option_len == len')
  self.assertNotEqual(options,old)
  with tempfile.TemporaryDirectory(prefix='a20-ext4-mount-') as d:
   p=Path(d)
   for name,body in [('fixed',options),('ignored-parameter',old)]:
    c=p/(name+'.c');c.write_text(PRE+body+source+MAIN);exe=p/name
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(c),'-o',str(exe)],check=True)
    r=subprocess.run([str(exe)],capture_output=True)
    if name=='fixed':self.assertEqual(r.returncode,0,r.stderr)
    else:self.assertNotEqual(r.returncode,0)

 def test_error_retry_flushes_cached_state_to_durable_media(self):
  source=(ROOT/'kernel/fs/diskfs/ext4.c').read_text()
  transition=extract_function(source,'static uint16_t ext4_state_transition(uint16_t state, int transition)')
  persist=extract_function(source,'static int ext4_persist_state_transition(bcache_t *bc, int transition)\n{')
  record=extract_function(source,'int ext4_record_status(ext4_sb_info_t *sb, int status)\n{')
  pre=r"""
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#define EXT4_VALID_FS 1
#define EXT4_ERROR_FS 2
#define EXT4_STATE_BEGIN 1
#define EXT4_STATE_CLEAN 2
#define EXT4_STATE_ERROR 3
typedef struct {uint16_t cached,durable;int fail_flush,writes,flushes;} bcache_t;
typedef struct {void *journal;int mount_state_started,inconsistent;bcache_t *bc;} ext4_sb_info_t;
static void ext4_mark_inconsistent(ext4_sb_info_t *sb){sb->inconsistent=1;}
static int ext4_state_io_read(bcache_t *bc,uint16_t *out){*out=bc->cached;return 0;}
static int ext4_state_io_write(bcache_t *bc,uint16_t state){bc->cached=state;bc->writes++;return 0;}
static int ext4_state_io_flush(bcache_t *bc){bc->flushes++;if(bc->fail_flush){bc->fail_flush--;return -EIO;}bc->durable=bc->cached;return 0;}
"""
  main=r"""
int main(void){
 bcache_t bc={.fail_flush=2};
 assert(ext4_persist_state_transition(&bc,EXT4_STATE_ERROR)==-EIO);
 assert(bc.cached==EXT4_ERROR_FS&&bc.durable==0&&bc.writes==1&&bc.flushes==1);
 assert(ext4_persist_state_transition(&bc,EXT4_STATE_ERROR)==-EIO);
 assert(bc.durable==0&&bc.writes==1&&bc.flushes==2);
 bc.fail_flush=0;
 assert(ext4_persist_state_transition(&bc,EXT4_STATE_ERROR)==0);
 assert(bc.durable==EXT4_ERROR_FS&&bc.writes==1&&bc.flushes==3);
 /* The public error recorder gives a transient failure one bounded retry. */
 bc=(bcache_t){.fail_flush=1};
 ext4_sb_info_t sb={.mount_state_started=1,.bc=&bc};
 assert(ext4_record_status(&sb,-117)==-117&&sb.inconsistent);
 assert(bc.durable==EXT4_ERROR_FS&&bc.writes==1&&bc.flushes==2);
 /* Exhaustion stays quarantined; a later error report retries persistence. */
 bc=(bcache_t){.fail_flush=2};
 assert(ext4_record_status(&sb,-117)==-117&&bc.durable==0&&bc.flushes==2);
 assert(ext4_record_status(&sb,-117)==-117&&bc.durable==EXT4_ERROR_FS);
 return 0;
}
"""
  with tempfile.TemporaryDirectory(prefix='a20-ext4-state-') as d:
   p=Path(d);c=p/'state.c';exe=p/'state'
   c.write_text(pre+transition+persist+record+main)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
   result=subprocess.run([str(exe)],capture_output=True)
   self.assertEqual(result.returncode,0,result.stderr)
if __name__=='__main__':unittest.main()
