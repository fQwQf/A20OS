"""Production RAMFS media selection, GPT checksums and read-only block views."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
if __package__:
    from .test_firmware_model import ROOT, STUBS
else:
    from test_firmware_model import ROOT, STUBS

HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "kernel/fs/mount_setup.c"
static unsigned writes,flushes,mounts,reads;
static uint8_t boot[512],header[512],entries[512];
static int kind;
static block_dev_t *mounted;
bcache_t *bcache_create(block_dev_t *dev){static bcache_t bc;bc.block=dev;return &bc;}
void bcache_destroy(bcache_t *bc){(void)bc;}
int vfs_mkdir(const char *path,int mode){(void)mode;assert(!strcmp(path,"/media"));return 0;}
int vfs_mount_bc_flags(const char *path,const char *type,bcache_t *bc,int flags,const char *data){
 (void)data;assert(!strcmp(path,"/media")&&!strcmp(type,"fat32"));
 assert(flags==VFS_MOUNT_RDONLY);mounted=bc->block;mounts++;return 0;
}
static int read_sector(block_dev_t *d,uint64_t lba,void *out,size_t n){
 assert(d->sector_size==512);assert(n==1);assert(lba<d->capacity);reads++;
 memset(out,0,512);
 if(kind==0&&lba==0)memcpy(out,boot,512);
 else if(kind&&lba==1)memcpy(out,header,512);
 else if(kind&&lba==2)memcpy(out,entries,512);
 else if(kind&&lba==2048)memcpy(out,boot,512);
 return 0;
}
static int write_sector(block_dev_t *d,uint64_t lba,const void *p,size_t n){
 (void)d;(void)lba;(void)p;(void)n;writes++;return 0;
}
static int disk_flush(block_dev_t *d){(void)d;flushes++;return 0;}
static void le32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(i*8);}
static void le64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=v>>(i*8);}
static void checksums(void){
 le32(header+88,~gpt_crc32(~0U,entries,512));le32(header+16,0);
 le32(header+16,~gpt_crc32(~0U,header,92));
}
static void init(void){
 memset(boot,0,512);memset(header,0,512);memset(entries,0,512);
 boot[510]=0x55;boot[511]=0xaa;boot[12]=2;boot[13]=1;boot[14]=32;boot[16]=2;
 le32(boot+32,131072);le32(boot+36,1024);le32(boot+44,2);
 memcpy(header,"EFI PART",8);le32(header+8,0x10000);le32(header+12,92);
 le64(header+24,1);le64(header+32,999999);le64(header+40,34);le64(header+48,999966);
 le64(header+72,2);le32(header+80,4);le32(header+84,128);
 entries[0]=1;le64(entries+32,2048);le64(entries+40,2048+131072-1);checksums();
}
int main(int argc,char **argv){
 assert(argc==2);int scenario=atoi(argv[1]);kind=scenario?1:0;init();
 block_dev_t disk={.read_sector=read_sector,.write_sector=write_sector,
  .flush=disk_flush,.capacity=1000000,.sector_size=512};
 if(scenario==2)header[16]^=1;
 if(scenario==3)entries[32]^=1;
 if(scenario==4){le64(entries+40,1000000);checksums();}
 if(scenario==5)disk.sector_size=4096;
 if(scenario==6){boot[13]=3;}
 if(scenario==7){le64(header+72,999999);checksums();}
 if(scenario==8){le32(header+84,129);checksums();}
 int rc=mount_media_disk(&disk,0);
 if(scenario==0||scenario==1){
  assert(rc==0&&mounts==1&&mounted);
  uint8_t data[512];assert(mounted->read_sector(mounted,0,data,1)==0&&data[510]==0x55);
  assert(mounted->write_sector(mounted,0,data,1)==-EROFS);
  assert(mounted->flush(mounted)==0);
  assert(mounted->read_sector(mounted,mounted->capacity,data,1)!=0);
 }else assert(rc!=0&&!mounts);
 assert(!writes&&!flushes);if(scenario==5)assert(!reads);
 return 0;
}
'''

class MediaMount(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-media-mount-')
        work=Path(cls.temp.name)
        stubs=dict(STUBS)
        stubs.update({
            'fs/vfs.h': '#pragma once\n#define VFS_MOUNT_RDONLY 1\nint vfs_mkdir(const char *,int);\nint vfs_mount_bc(const char *,const char *,struct bcache *);\nint vfs_mount(const char *,const char *,const char *,int,const char *);\nint vfs_mount_bc_flags(const char *,const char *,struct bcache *,int,const char *);\n',
            'fs/block_cache.h': '#pragma once\n#include "drivers/block/block_dev.h"\ntypedef struct bcache { block_dev_t *block; } bcache_t;\nbcache_t *bcache_create(block_dev_t *);\nvoid bcache_destroy(bcache_t *);\n',
        })
        # Keep the cache tag visible to the VFS declaration.
        stubs['fs/vfs.h']='struct bcache;\n'+stubs['fs/vfs.h']
        for name,text in stubs.items():
            path=work/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
        (work/'test.c').write_text(HARNESS)
        cls.executable=work/'test'
        result=subprocess.run(['cc','-std=gnu99','-O1','-g','-Wall','-Wextra','-Werror',
            '-DBRINGUP','-DCONFIG_RAMFS_MEDIA','-ffunction-sections','-fdata-sections',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(work),'-I'+str(ROOT),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),
            str(work/'test.c'),'-Wl,--gc-sections','-o',str(cls.executable)],capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stderr)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def run_scenario(self,n):
        subprocess.run([str(self.executable),str(n)],check=True,timeout=5,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},capture_output=True)
    def test_raw_fat32_read_only(self):self.run_scenario(0)
    def test_gpt_fat32_read_only(self):self.run_scenario(1)
    def test_corrupt_header(self):self.run_scenario(2)
    def test_corrupt_entry_array(self):self.run_scenario(3)
    def test_out_of_disk_partition(self):self.run_scenario(4)
    def test_non512_sector_rejected_before_read(self):self.run_scenario(5)
    def test_invalid_fat_geometry(self):self.run_scenario(6)
    def test_entry_array_outside_metadata(self):self.run_scenario(7)
    def test_invalid_entry_stride(self):self.run_scenario(8)

if __name__=='__main__':unittest.main()
