"""Production clean-journal upgrade guards and console-free diagnostics."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]

class LaptopRecoveryTest(unittest.TestCase):
    def run_c(self, code):
        with tempfile.TemporaryDirectory(prefix="a20-laptop-recovery-") as tmp:
            p=Path(tmp); (p/"test.c").write_text(code)
            result=subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror",
                "-fsanitize=undefined",str(p/"test.c"),"-o",str(p/"test")],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(p/"test")],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_clean_empty_only_upgrade(self):
        source=(ROOT/"kernel/fs/diskfs/ext4_journal.c").read_text()
        prelude=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define EXT4_VALID_FS 1
#define EXT4_FEATURE_INCOMPAT_RECOVER 4
#define JBD2_MAGIC_NUMBER 0xc03b3998U
#define JBD2_SUPERBLOCK_V2 4
#define JBD2_FEATURE_INCOMPAT_REVOKE 1
#define JBD2_FEATURE_INCOMPAT_64BIT 2
#define JBD2_FEATURE_INCOMPAT_CSUM_V3 16
#define JBD2_CRC32C_CHKSUM 4
#define JBD2_MAX_JOURNAL_BLOCKS 65536
#define JBD2_SUPERBLOCK_CHECKSUM_OFF 252
#define JBD2_SUPERBLOCK_BYTES 1024
#define printf(...) ((void)0)
typedef struct {unsigned block_size;void *bc;} ext4_sb_info_t;
typedef struct {unsigned s_state,s_last_orphan,s_feature_incompat;uint8_t s_uuid[16];} ext4_superblock_t;
typedef struct {ext4_sb_info_t*fs;struct{uint64_t size;}journal_inode;uint8_t *meta;} jbd2_t;
#define ext4_inode_size(i) ((i)->size)
static uint32_t crc32c_table[256];static int crc32c_table_ready;
static unsigned writes,flushes;static int write_error,flush_error;
static uint8_t disk[4096];
static int jbd2_write_block(jbd2_t*j,unsigned block,const void*data){
 (void)j;assert(block==0);writes++;if(write_error)return -EIO;memcpy(disk,data,4096);return 0;
}
static int bcache_sync_checked(void*bc){(void)bc;flushes++;return flush_error?-EIO:0;}
'''
        bodies="\n".join(function(source,n) for n in ['get_be32','put_be32','crc32c_init_table',
            'ext4_crc32c','jbd2_clean_unchecked_upgrade_allowed','jbd2_upgrade_clean_unchecked'])
        main=r'''
int main(void){
 uint8_t meta[4096]={0},good[4096];ext4_sb_info_t fs={4096,0};
 ext4_superblock_t sb={.s_state=1};memcpy(sb.s_uuid,"0123456789abcdef",16);
 jbd2_t j={.fs=&fs,.journal_inode={4096*1024},.meta=meta};
 put_be32(meta,JBD2_MAGIC_NUMBER);put_be32(meta+4,4);put_be32(meta+12,4096);
 put_be32(meta+16,1024);put_be32(meta+20,1);put_be32(meta+40,2);
 memcpy(meta+48,sb.s_uuid,16);meta[80]=4;memcpy(good,meta,4096);
 assert(jbd2_clean_unchecked_upgrade_allowed(&j,&sb));
 unsigned offsets[]={0,4,12,16,20,28,32,36,40,44};
 uint32_t values[]={0,3,1024,1,1024,1,5,1,8,1};
 for(unsigned i=0;i<10;i++){
  memcpy(meta,good,4096);put_be32(meta+offsets[i],values[i]);
  assert(!jbd2_clean_unchecked_upgrade_allowed(&j,&sb));
 }
 memcpy(meta,good,4096);meta[48]^=1;assert(!jbd2_clean_unchecked_upgrade_allowed(&j,&sb));
 memcpy(meta,good,4096);meta[80]=255;assert(!jbd2_clean_unchecked_upgrade_allowed(&j,&sb));
 memcpy(meta,good,4096);sb.s_state=0;assert(!jbd2_clean_unchecked_upgrade_allowed(&j,&sb));
 sb.s_state=3;assert(!jbd2_clean_unchecked_upgrade_allowed(&j,&sb));sb.s_state=1;
 sb.s_last_orphan=10;assert(!jbd2_clean_unchecked_upgrade_allowed(&j,&sb));sb.s_last_orphan=0;
 sb.s_feature_incompat=4;assert(!jbd2_clean_unchecked_upgrade_allowed(&j,&sb));sb.s_feature_incompat=0;
 j.journal_inode.size=4096;assert(!jbd2_clean_unchecked_upgrade_allowed(&j,&sb));j.journal_inode.size=4096*1024;
 assert(!jbd2_upgrade_clean_unchecked(&j));assert(writes==1&&flushes==1);
 assert(get_be32(disk+40)==18&&disk[80]==4);
 uint32_t stored=get_be32(disk+252);put_be32(disk+252,0);
 assert(ext4_crc32c(0xffffffffU,disk,1024)==stored);
 memcpy(meta,good,4096);write_error=1;flushes=0;
 assert(jbd2_upgrade_clean_unchecked(&j)==-EIO&&flushes==0);
 write_error=0;flush_error=1;memcpy(meta,good,4096);
 assert(jbd2_upgrade_clean_unchecked(&j)==-EIO&&flushes==1);
 return 0;
}
'''
        self.run_c(prelude+bodies+main)

    def test_runtime_records_keep_both_rings_without_console(self):
        source=(ROOT/"kernel/core/klog.c").read_text()
        prelude=r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#define CONFIG_KLOG_ENABLED 1
static int klog_lock,held;static unsigned text_count,record_count;static char recorded[1024];
static uint64_t spin_lock_irqsave(int*l){(void)l;assert(!held);held=1;return 0;}
static void spin_unlock_irqrestore(int*l,uint64_t f){(void)l;(void)f;assert(held);held=0;}
static void klog_append_text(const char*s,size_t n){assert(held);text_count++;memcpy(recorded,s,n);recorded[n]=0;}
static void klog_append_record(int level,const char*s,size_t n){(void)s;assert(held&&level==1&&n>0);record_count++;}
'''
        bodies="\n".join(function(source,n) for n in ['klog_buffer_text','klog_record_only'])
        # No console symbol is supplied: calling console code cannot link.
        self.run_c(prelude+bodies+r'''
int main(void){
 assert(klog_record_only(1,"[I2C-HID] runtime: reads=%u\n",42)>0);
 assert(!held&&text_count==1&&record_count==1);
 assert(strstr(recorded,"reads=42"));assert(!klog_record_only(1,"%s",""));
 assert(text_count==1&&record_count==1);return 0;
}
''')

if __name__=="__main__":unittest.main()
