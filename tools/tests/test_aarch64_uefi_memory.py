"""Production bounded AArch64 UEFI map conversion and reservation fixtures."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
C=r'''
#include <assert.h>
#include <string.h>
#include "arch/aarch64/boot/uefi/handoff.h"
#include "core/errno.h"
static unsigned char blob[A64_UEFI_HANDOFF_BYTES] __attribute__((aligned(4096)));
static a64_uefi_range_t ranges[64];static size_t count;
static const uint64_t kernel=0x08080000, physical=0x27000000;
static a64_uefi_handoff_t *h=(void *)blob;
static a64_uefi_descriptor_t *d(unsigned i) {return (void *)(blob+4096+i*h->descriptor_bytes);}
static void descriptor(unsigned i,uint32_t type,uint64_t start,uint64_t end,uint64_t attr) {
 *d(i)=(a64_uefi_descriptor_t){.type=type,.physical=start,.pages=(end-start)/4096,.attributes=attr};
}
static void reset(void) {
 memset(blob,0,sizeof(blob));
 *h=(a64_uefi_handoff_t){.magic=A64_UEFI_HANDOFF_MAGIC,.version=1,.header_bytes=sizeof(*h),
 .total_bytes=sizeof(blob),.map_offset=4096,.map_bytes=4*48,.descriptor_bytes=48,.descriptor_version=1,
 .kernel_physical=kernel,.kernel_bytes=0x20000};
 descriptor(0,2,kernel,kernel+0x20000,0);
 descriptor(1,7,kernel+0x20000,physical,0);
 descriptor(2,2,physical,physical+sizeof(blob),0);
 descriptor(3,7,physical+sizeof(blob),A64_UEFI_RAM_END,0);
}
static int parse(size_t capacity) {return a64_uefi_memory_ranges(blob,sizeof(blob),physical,
 A64_UEFI_RAM_BASE,A64_UEFI_RAM_END,kernel,kernel+0x1ff00,ranges,capacity,&count);}
static void basic(void) {
 reset();assert(!parse(64)&&count==2);assert(ranges[0].base==kernel&&ranges[0].size==physical-kernel);
 assert(ranges[1].base==physical+sizeof(blob));
 h->cmdline_physical=physical+A64_UEFI_CMDLINE_OFFSET;h->cmdline_bytes=2;
 blob[A64_UEFI_CMDLINE_OFFSET]='a';assert(!parse(64));
 h->cmdline_physical++;assert(parse(64)<0);
}
static void malformed(void) {
 reset();h->magic++;assert(parse(64)<0);
 reset();h->version++;assert(parse(64)<0);
 reset();h->header_bytes--;assert(parse(64)<0);
 reset();h->total_bytes--;assert(parse(64)<0);
 reset();h->descriptor_bytes=32;assert(parse(64)<0);
 reset();h->descriptor_bytes=44;assert(parse(64)<0);
 reset();h->descriptor_version=2;assert(parse(64)<0);
 reset();h->map_bytes--;assert(parse(64)<0);
 reset();h->map_bytes=40*1025;h->descriptor_bytes=40;assert(parse(64)<0);
 reset();d(1)->physical=kernel;assert(parse(64)<0);
 reset();d(1)->pages=~(uint64_t)0;assert(parse(64)<0);
 reset();d(1)->physical++;assert(parse(64)<0);
 reset();d(0)->type=7;assert(parse(64)<0); /* kernel allocation must be LoaderData */
 reset();d(0)->attributes=A64_UEFI_RUNTIME;assert(parse(64)<0);
 reset();d(2)->type=7;assert(parse(64)<0); /* handoff allocation must be LoaderData */
 reset();h->kernel_bytes=0x1000;assert(parse(64)<0);
 reset();h->cmdline_physical=physical+256;h->cmdline_bytes=1025;assert(parse(64)<0);
 reset();h->cmdline_physical=physical+256;h->cmdline_bytes=2;blob[257]='x';assert(parse(64)<0);
 reset();assert(a64_uefi_memory_ranges(blob,sizeof(*h),physical,A64_UEFI_RAM_BASE,A64_UEFI_RAM_END,
 kernel,kernel+0x1ff00,ranges,64,&count)<0);
}
static void fragmented(void) {
 reset();h->map_bytes=24*48;
 descriptor(1,9,kernel+0x20000,kernel+0x100000,0);
 for(unsigned i=0;i<20;i++)descriptor(i+2,(i&1)?9:7,kernel+0x100000+i*0x10000,kernel+0x110000+i*0x10000,0);
 descriptor(22,2,physical,physical+sizeof(blob),0);
 descriptor(23,7,physical+sizeof(blob),A64_UEFI_RAM_END,0);
 assert(!parse(64)&&count==12);assert(parse(4)==-ENOSPC);
 /* A reclaimable type with RUNTIME set still cannot enter the allocator. */
 d(2)->attributes=A64_UEFI_RUNTIME;assert(!parse(64)&&count==11);
}
static void types_and_order(void) {
 reset();d(1)->type=1;assert(!parse(64));d(1)->type=3;assert(!parse(64));d(1)->type=4;assert(!parse(64));
 a64_uefi_descriptor_t tmp=*d(0);*d(0)=*d(3);*d(3)=tmp;assert(!parse(64)&&count==2);
}
static void reservations(void) {
 reset();assert(!parse(64));
 assert(!a64_uefi_reserve_range(ranges,&count,64,kernel+0x30001,1));
 assert(count==3&&ranges[0].base+ranges[0].size==kernel+0x30000&&ranges[1].base==kernel+0x31000);
 assert(a64_uefi_reserve_range(ranges,&count,count,kernel+0x50001,1)==-ENOSPC);
 assert(a64_uefi_reserve_range(ranges,&count,64,~(uint64_t)0-8,16)==-EINVAL);
}
int main(void) {basic();malformed();fragmented();types_and_order();reservations();return 0;}
'''
@unittest.skipUnless(shutil.which('cc'),'host compiler required')
class Aarch64UefiMemoryTests(unittest.TestCase):
 def test_production_map_parser(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'test.c').write_text(C)
   subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',f'-I{ROOT}/kernel/include',f'-I{ROOT}/kernel',str(p/'test.c'),str(ROOT/'kernel/arch/aarch64/boot/uefi_memory.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
