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
if __name__=='__main__':unittest.main()
