"""Execute explicit GPT distro selection without touching unrelated filesystems."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from .test_firmware_model import ROOT, STUBS

HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "kernel/fs/mount_setup.c"
struct disk { uint8_t header[512], entries[16384]; unsigned present, sector_size, reads, writes, flushes; };
static struct disk disks[3];
static device_t devices[3];
static class_device_t bindings[3];
static block_dev_t *mounted;
static unsigned mounts, pseudo_mounts, mount_failure;
static block_dev_ops_t operations;
class_device_t *class_device_get_by_index(uint32_t type,unsigned index) {
 assert(type==DEV_CLASS_BLOCK);return index<3&&disks[index].present?&bindings[index]:NULL;
}
void class_device_get(class_device_t *d){(void)d;}
void class_device_put(class_device_t *d){(void)d;}
int class_device_ops_begin(class_device_t *d,uint32_t type,const void **ops) {
 if(!d)return -ENODEV;assert(type==DEV_CLASS_BLOCK);*ops=&operations;return 0;
}
void class_device_call_end(class_device_t *d){assert(d);}
int class_device_set_offline(class_device_t *d,void(*cb)(class_device_t*,void*),void*p) {
 assert(d&&cb&&p);return 0;
}
static int disk_read(device_t *dev,uint64_t lba,void *out,size_t count) {
 struct disk *disk=dev->plat_data;assert(count==1&&lba<20000&&disk->sector_size==512);
 disk->reads++;memset(out,0,512);
 if(lba==1)memcpy(out,disk->header,512);
 else if(lba>=2&&lba<34)memcpy(out,disk->entries+(lba-2)*512,512);
 else if(lba==2048)((uint8_t*)out)[0]=0xa2;
 /* Metadata-only discovery must not probe arbitrary Windows/raw-ext4 sectors. */
 else assert(0 && "unexpected filesystem payload read");
 return 0;
}
static int disk_write(device_t *dev,uint64_t lba,const void *p,size_t count) {
 struct disk *disk=dev->plat_data;assert(disk==&disks[1]);assert(lba==2048&&p&&count==1);disk->writes++;return 0;
}
static int disk_flush(device_t *dev){((struct disk*)dev->plat_data)->flushes++;return 0;}
static uint64_t disk_capacity(device_t *dev){(void)dev;return 20000;}
static uint32_t disk_sector_size(device_t *dev){return ((struct disk*)dev->plat_data)->sector_size;}
bcache_t *bcache_create(block_dev_t *dev){static bcache_t cache;cache.block=dev;return &cache;}
void bcache_destroy(bcache_t *cache){assert(cache);}
int vfs_mkdir(const char *path,int mode){assert(path&&mode);return 0;}
int vfs_mount_bc(const char *path,const char *type,bcache_t *cache) {
 assert(!strcmp(path,"/extra")&&!strcmp(type,"ext4"));mounts++;
 mounted=cache->block;uint8_t sector[512];
 assert(mounted->read_sector(mounted,0,sector,1)==0&&sector[0]==0xa2);
 if(mount_failure){mounted=NULL;return -EINVAL;}return 0;
}
int vfs_mount(const char *dev,const char *path,const char *type,int flags,const char *data) {
 assert(dev&&path&&type&&!flags&&!data);assert(!strncmp(path,"/extra/",7));pseudo_mounts++;return 0;
}
static void le32(uint8_t *p,uint32_t value){for(unsigned i=0;i<4;i++)p[i]=value>>(i*8);}
static void le64(uint8_t *p,uint64_t value){for(unsigned i=0;i<8;i++)p[i]=value>>(i*8);}
static void checksum(struct disk *disk) {
 uint32_t count=read_le32(disk->header+80),size=read_le32(disk->header+84);
 le32(disk->header+88,~gpt_crc32(~0U,disk->entries,(size_t)count*size));
 le32(disk->header+16,0);le32(disk->header+16,~gpt_crc32(~0U,disk->header,92));
}
static void partition(struct disk *disk,unsigned index,int linux_type,const char *name,uint64_t first,uint64_t last) {
 uint8_t *entry=disk->entries+index*read_le32(disk->header+84);
 memset(entry,0,128);memcpy(entry,distro_type_guid,16);
 if(!linux_type)entry[0]^=1;
 entry[16]=index+1;le64(entry+32,first);le64(entry+40,last);
 for(unsigned i=0;i<36&&name[i];i++)entry[56+i*2]=name[i];
}
static void init_disk(unsigned index) {
 struct disk *disk=&disks[index];disk->present=1;disk->sector_size=512;
 devices[index].plat_data=disk;bindings[index].dev=&devices[index];
 memcpy(disk->header,"EFI PART",8);le32(disk->header+8,0x10000);le32(disk->header+12,92);
 le64(disk->header+24,1);le64(disk->header+32,19999);le64(disk->header+40,34);le64(disk->header+48,19000);
 le64(disk->header+72,2);le32(disk->header+80,32);le32(disk->header+84,128);
 partition(disk,0,0,"Windows",40,1023);checksum(disk);
}
int main(int argc,char **argv) {
 assert(argc==2);unsigned scenario=atoi(argv[1]);
 operations=(block_dev_ops_t){.read=disk_read,.write=disk_write,.flush=disk_flush,
  .capacity=disk_capacity,.sector_size=disk_sector_size};
 init_disk(0);init_disk(1);partition(&disks[1],1,1,"A20OS distro",2048,6143);
 int success=scenario==0||scenario==14||scenario==17||scenario==18||scenario==19;
 switch(scenario) {
 case 0:break;
 case 1:partition(&disks[1],1,1,"ordinary Linux",2048,6143);break;
 case 2:partition(&disks[1],1,0,"A20OS distro",2048,6143);break;
 case 3:break; /* Corrupt header after checksums. */
 case 4:break; /* Corrupt array after checksums. */
 case 5:partition(&disks[1],1,1,"A20OS distro",2048,21000);break;
 case 6:partition(&disks[1],2,1,"A20OS distro",7000,9000);break;
 case 7:partition(&disks[1],31,1,"A20OS distro",7000,9000);break;
 case 8:init_disk(2);partition(&disks[2],1,1,"A20OS distro",2048,6143);checksum(&disks[2]);break;
 case 9:le64(disks[1].entries+128+48,1ULL<<60);break;
 case 10:le64(disks[1].entries+128+48,1ULL<<63);break;
 case 11:partition(&disks[1],2,0,"Windows",3000,7000);break;
 case 12:mount_failure=1;break;
 case 13:disks[1].sector_size=4096;break;
 case 14:memset(disks[1].entries+128,0,128);partition(&disks[1],20,1,"A20OS distro",2048,6143);break;
 case 15:disks[1].entries[128+56+13*2]='x';break;
 case 16:memset(disks[1].header,0,512);break;
 case 17:break; /* Independently owned view survives the media parser. */
 case 18:le32(disks[1].header+84,256);memset(disks[1].entries,0,sizeof(disks[1].entries));partition(&disks[1],1,1,"A20OS distro",2048,6143);break;
 case 19:le32(disks[1].header+84,512);memset(disks[1].entries,0,sizeof(disks[1].entries));partition(&disks[1],1,1,"A20OS distro",2048,6143);break;
 default:abort();
 }
 if(scenario!=16)checksum(&disks[1]);
 if(scenario==3)disks[1].header[16]^=1;
 if(scenario==4)disks[1].entries[128+32]^=1;
 int rc=mount_ramfs_distro();
 if(success) {
  assert(!rc&&mounts==1&&mounted&&pseudo_mounts==5);
  assert(mounted->capacity==4096&&((partition_block_dev_t*)mounted->priv)->first_lba==2048);
  if(scenario==17){block_dev_t *alias=gpt_partition(mount_setup_block_device(1),1,1);assert(alias&&alias!=mounted);((partition_block_dev_t*)alias->priv)->read_only=1;}
  uint8_t payload[512]={0};assert(!mounted->write_sector(mounted,0,payload,1));
  assert(mounted->write_sector(mounted,4096,payload,1)!=0);
  assert(mounted->read_sector(mounted,4096,payload,1)!=0);
  assert(!mounted->flush(mounted));assert(disks[1].writes==1&&disks[1].flushes==1);
 } else {assert(rc<0&&!mounted&&!pseudo_mounts);assert(mounts==(scenario==12));assert(!disks[1].writes&&!disks[1].flushes);}
 assert(!disks[0].writes&&!disks[0].flushes&&!disks[2].writes&&!disks[2].flushes);
 if(scenario==13)assert(!disks[1].reads);
 return 0;
}
'''

class RamfsDistroMount(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-distro-mount-')
        work = Path(cls.temp.name)
        stubs = dict(STUBS)
        stubs.update({
            'fs/vfs.h': 'struct bcache;\n#pragma once\n#define VFS_MOUNT_RDONLY 1\nint vfs_mkdir(const char *,int);\nint vfs_mount_bc(const char *,const char *,struct bcache *);\nint vfs_mount(const char *,const char *,const char *,int,const char *);\nint vfs_mount_bc_flags(const char *,const char *,struct bcache *,int,const char *);\n',
            'fs/block_cache.h': '#pragma once\n#include "drivers/block/block_dev.h"\ntypedef struct bcache { block_dev_t *block; } bcache_t;\nbcache_t *bcache_create(block_dev_t *);\nvoid bcache_destroy(bcache_t *);\n',
        })
        for name, text in stubs.items():
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        (work / 'test.c').write_text(HARNESS)
        cls.executable = work / 'test'
        compiler = [
            'cc', '-std=gnu99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wno-misleading-indentation', '-DBRINGUP',
            '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
            '-fno-sanitize-recover=all', '-fno-pie', '-no-pie',
            '-I'+str(work), '-I'+str(ROOT), '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'kernel'),
        ]
        result = subprocess.run(compiler + ['-DCONFIG_RAMFS_DISTRO',
            str(work/'test.c'), '-Wl,--gc-sections', '-o', str(cls.executable),
        ], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        disabled = work / 'disabled.c'
        disabled.write_text('#include <assert.h>\n#include "kernel/fs/mount_setup.c"\n'
                            'int main(void) { assert(mount_ramfs_distro() == -EOPNOTSUPP); return 0; }\n')
        cls.disabled = work / 'disabled'
        result = subprocess.run(compiler + [str(disabled), '-Wl,--gc-sections', '-o', str(cls.disabled)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_explicit_partition_policy(self):
        for scenario in range(20):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.executable), str(scenario)], check=True, timeout=5,
                               env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'},
                               capture_output=True)

    def test_disabled_profile_has_no_mount_path(self):
        subprocess.run([str(self.disabled)], check=True, timeout=5, capture_output=True)


if __name__ == '__main__':
    unittest.main()
