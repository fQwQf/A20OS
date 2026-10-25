"""Production Multiboot decoder and x86 UEFI map exporter boundary tests."""
import ctypes
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
MAGIC=0x2badb002


def record(base,size,kind=1,extra=b''):
    return struct.pack('<IQQI',20+len(extra),base,size,kind)+extra


def info(flags=64,*,map_address=0x4000,map_size=24,upper=0,modules=0,modules_address=0x5000,syms=(0,0,0,0)):
    return struct.pack('<13I',flags,0,upper,0,0,modules,modules_address,*syms,map_size,map_address)


def descriptor(kind,base,pages,attributes=0):
    return struct.pack('<IIQQQQ',kind,0,base,0,pages,attributes)


class BootMemory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='a20-x86-memory-');work=Path(cls.tmp.name)
        (work/'wrapper.c').write_text(r'''
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "kernel/arch/x86_64/platform/boot_memory.h"
#define KERNEL_MEMORY_BYTES 33554432ULL
#define KERNEL_LOAD_ADDRESS 0x200000ULL
#include "kernel/arch/x86_64/boot/uefi/x86_64_loader.c"
const uint8_t _binary_kernel_bin_start[1]={0};
const uint8_t _binary_kernel_bin_end[1]={0};
struct region { uint64_t physical;const uint8_t *data;size_t bytes; };
static struct region regions[8];static size_t region_count;
void reset(void){region_count=0;}
void region(uint64_t physical,const void *data,size_t bytes){regions[region_count++]=(struct region){physical,data,bytes};}
static const void *reader(void *context,uint64_t physical,size_t bytes){
 (void)context;
 for(size_t i=0;i<region_count;i++)if(physical>=regions[i].physical &&
  physical-regions[i].physical<=regions[i].bytes && bytes<=regions[i].bytes-(physical-regions[i].physical))
   return regions[i].data+(physical-regions[i].physical);
 return NULL;
}
int decode(uint32_t magic,uint32_t physical,size_t capacity,const x86_boot_range_t *excluded,size_t excluded_count,uint64_t *out){
 x86_boot_range_t ranges[64];size_t count=0;enum x86_boot_memory_source source;
 int rc=x86_boot_memory_decode(magic,physical,reader,NULL,0x40000000,0x200000000ULL,
  excluded,excluded_count,ranges,capacity,&count,&source);
 out[0]=count;out[1]=source;
 if(!rc)for(size_t i=0;i<count;i++){out[2+i*2]=ranges[i].base;out[3+i*2]=ranges[i].end;}
 return rc;
}
int efi_export(const void *map,size_t bytes,size_t stride,uint32_t version,uint64_t scratch,
               uint64_t scratch_bytes,void *out,size_t capacity,size_t *count){
 return export_memory_map(map,bytes,stride,version,scratch,scratch_bytes,out,capacity,count);
}
int map_flag(void){return MB_INFO_MEM_MAP;}
int target_safe(const void *map,size_t bytes,uint64_t base,uint64_t length){
 return target_reclaimable(map,bytes,40,base,length);
}
static uint64_t translate(uint64_t root,uint64_t address){
 uint64_t *t=(void *)(uintptr_t)root;
 for(unsigned shift=39;shift>=30;shift-=9){
  uint64_t e=t[(address>>shift)&511];if(!(e&1))return UINT64_MAX;
  t=(void *)(uintptr_t)(e&~4095ULL);
 }
 uint64_t e=t[(address>>21)&511];
 return (e&0x81)==0x81 ? (e&~0x1fffffULL)|(address&0x1fffff) : UINT64_MAX;
}
int transition_fixture(void){
 void *scratch=mmap(NULL,SCRATCH_PAGES*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(scratch==MAP_FAILED)return 100;
 uint64_t root=prepare_transition((uintptr_t)scratch,0x12340000000,0x1800000);
 int rc=!root;
 uint64_t addresses[]={KERNEL_LOAD_ADDRESS,KERNEL_LOAD_ADDRESS+KERNEL_MEMORY_BYTES-1,
  (uintptr_t)scratch,(uintptr_t)scratch+SCRATCH_PAGES*4096-1,
  0x12340000000,0x123417fffff};
 for(size_t i=0;i<sizeof(addresses)/sizeof(addresses[0]);i++)
  if(root&&translate(root,addresses[i])!=addresses[i])rc=1;
 if(root && translate(root,0x20000000000)!=UINT64_MAX)rc=2;
 if(prepare_transition((uintptr_t)scratch,0xfffffffffffff000ULL,0x2000))rc=3;
 munmap(scratch,SCRATCH_PAGES*4096);return rc;
}
static void checksum(uint8_t *p,size_t length,size_t offset){
 p[offset]=0;uint8_t sum=0;for(size_t i=0;i<length;i++)sum+=p[i];p[offset]=(uint8_t)-sum;
}
static void sdt(uint8_t *p,const char *signature,uint32_t bytes){
 memset(p,0,bytes);memcpy(p,signature,4);memcpy(p+4,&bytes,4);checksum(p,bytes,9);
}
int acpi_fixture(int kind){
 /* Low VA fixtures exercise actual physical overlap with the fixed image.
  * MAP_FIXED_NOREPLACE never overwrites the test runner's mappings. */
 uint8_t *low=mmap((void *)0x300000,4096,PROT_READ|PROT_WRITE,
  MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 if(low==MAP_FAILED)return 100;
 uint8_t *high=mmap(NULL,5*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(high==MAP_FAILED){munmap(low,4096);return 101;}
 uint8_t *rsdp=high,*xsdt=high+4096,*fadt=high+8192,*dsdt=high+12288,*facs=high+16384;
 if(kind==1)rsdp=(void *)0x300000;
 if(kind==2)dsdt=(void *)0x300000;
 if(kind==3)facs=(void *)0x300000;
 memset(rsdp,0,36);memcpy(rsdp,"RSD PTR ",8);rsdp[15]=2;
 uint32_t length=36;memcpy(rsdp+20,&length,4);uint64_t address=(uintptr_t)xsdt;
 memcpy(rsdp+24,&address,8);checksum(rsdp,20,8);checksum(rsdp,36,32);
 sdt(xsdt,"XSDT",44);address=(uintptr_t)fadt;memcpy(xsdt+36,&address,8);checksum(xsdt,44,9);
 sdt(fadt,"FACP",148);address=(uintptr_t)facs;memcpy(fadt+132,&address,8);
 address=(uintptr_t)dsdt;memcpy(fadt+140,&address,8);checksum(fadt,148,9);
 sdt(dsdt,"DSDT",36);memset(facs,0,64);memcpy(facs,"FACS",4);length=64;memcpy(facs+4,&length,4);
 if(kind==4)dsdt[9]++;
 if(kind==5){length=0xffffffff;memcpy(xsdt+4,&length,4);}
 if(kind==6){address=0xdead000;memcpy(xsdt+36,&address,8);checksum(xsdt,44,9);}
 struct efi_memory_descriptor d[2]={{.type=4,.physical_start=0x300000,.number_of_pages=1},
  {.type=4,.physical_start=(uintptr_t)high,.number_of_pages=5}};
 int rc=acpi_destination_safe(d,sizeof(d),sizeof(d[0]),(uintptr_t)rsdp);
 munmap(low,4096);munmap(high,5*4096);return rc;
}

static uint8_t loaded_image_bytes[96];
static efi_status_t loaded_image_protocol(efi_handle_t image,const struct efi_guid *guid,void **out){
 (void)image;(void)guid;*out=loaded_image_bytes;return 0;
}
void options_copy(char *out){
 efi_char16_t options[]={'a','2','0','.','x','=','1',0};uint8_t bogus_image[1024]={'M','Z'};
 uint32_t bytes=sizeof(options);void *pointer=options;memset(loaded_image_bytes,0,sizeof(loaded_image_bytes));
 memcpy(loaded_image_bytes+48,&bytes,4);memcpy(loaded_image_bytes+56,&pointer,8);
 pointer=bogus_image;memcpy(loaded_image_bytes+64,&pointer,8);
 struct efi_boot_services services={.handle_protocol=loaded_image_protocol};
 struct efi_system_table system={.boot_services=&services};
 static uint8_t scratch[SCRATCH_PAGES*4096];memset(scratch,0,sizeof(scratch));
 (void)collect_load_options(&system,NULL,(uintptr_t)scratch);
 memcpy(out,scratch+MB_CMDLINE_OFFSET,16);
}
int guarded(unsigned length){
 size_t page=(size_t)sysconf(_SC_PAGESIZE);
 uint8_t *p=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(p==MAP_FAILED||length>page)return 100;
 if(mprotect(p+page,page,PROT_NONE))return 101;
 uint8_t *map=p+page-length;memset(map,0,length);
 if(length>=4){uint32_t payload=20;memcpy(map,&payload,4);}
 struct x86_mb_info mi={.flags=64,.mmap_addr=0x4000,.mmap_length=length};
 reset();region(0x1000,&mi,sizeof(mi));region(0x4000,map,length);
 uint64_t out[130];int rc=decode(X86_MB_BOOTLOADER_MAGIC,0x1000,64,NULL,0,out);
 munmap(p,page*2);return rc;
}
''')
        cls.so=work/'memory.so'
        subprocess.run(['cc','-shared','-fPIC','-O2','-std=gnu11','-fno-builtin',
            '-Wall','-Wextra','-Werror','-I',str(ROOT),'-I',str(ROOT/'kernel/include'),
            str(work/'wrapper.c'),str(ROOT/'kernel/arch/x86_64/platform/boot_memory.c'),
            '-o',str(cls.so)],check=True)
        cls.lib=ctypes.CDLL(str(cls.so));cls.lib.region.argtypes=[ctypes.c_uint64,ctypes.c_void_p,ctypes.c_size_t]
        cls.lib.decode.argtypes=[ctypes.c_uint32,ctypes.c_uint32,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p]
        cls.lib.efi_export.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_size_t,ctypes.c_uint32,
            ctypes.c_uint64,ctypes.c_uint64,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def regions(self,*regions):
        self.lib.reset();self.buffers=[]
        for physical,data in regions:
            buffer=ctypes.create_string_buffer(data);self.buffers.append(buffer)
            self.lib.region(physical,buffer,len(data))

    def decode(self,map_data=None,*,capacity=64,magic=MAGIC,header=None,info_address=0x1000,map_address=0x4000,other=(),excluded=()):
        if header is None:header=info(map_size=len(map_data or b''),map_address=map_address)
        self.regions((info_address,header),*(([(map_address,map_data)] if map_data is not None else [])),*other)
        exclusions=(ctypes.c_uint64*(len(excluded)*2))(*[x for pair in excluded for x in pair])
        out=(ctypes.c_uint64*130)();rc=self.lib.decode(magic,info_address,capacity,exclusions,len(excluded),out)
        ranges=[tuple(out[2+i*2:4+i*2]) for i in range(out[0])]
        return rc,ranges,out[1]

    def export(self,data,*,stride=40,version=1,scratch=0x10000000,scratch_bytes=0x18000,capacity=512):
        buffer=ctypes.create_string_buffer(data);out=ctypes.create_string_buffer(512*24);count=ctypes.c_size_t()
        rc=self.lib.efi_export(buffer,len(data),stride,version,scratch,scratch_bytes,out,capacity,ctypes.byref(count))
        records=[struct.unpack_from('<IQQI',out.raw,i*24) for i in range(count.value)]
        return rc,records,out.raw[:count.value*24]

    def test_private_transition_maps_kernel_image_and_scratch_with_2m_pages(self):
        self.assertEqual(self.lib.transition_fixture(),0)

    def test_fixed_destination_requires_complete_reclaimable_coverage(self):
        self.lib.target_safe.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64,ctypes.c_uint64]
        def safe(data):
            buf=ctypes.create_string_buffer(data)
            return self.lib.target_safe(buf,len(data),0x200000,0x1200000)
        self.assertEqual(safe(descriptor(7,0x100000,0x700)+descriptor(4,0x800000,0xc00)),1)
        self.assertEqual(safe(descriptor(7,0x100000,0x1300)+descriptor(10,0x800000,8)),0)
        self.assertEqual(safe(descriptor(7,0x200000,0x1200)+descriptor(4,0x800000,8,1<<63)),0)
        self.assertEqual(safe(descriptor(7,0x200000,0x600)+descriptor(4,0x801000,0xbff)),0)
        self.assertEqual(safe(descriptor(7,0x200000,0x1000)),0)

    def test_acpi_bootservices_tables_are_protected_before_fixed_copy(self):
        self.assertEqual(self.lib.acpi_fixture(0),1)
        for fixture in range(1,7):
            self.assertEqual(self.lib.acpi_fixture(fixture),0,fixture)

    def test_make_load_address_rejects_bad_values_and_isolates_artifacts(self):
        command=['make','-s','ARCH=x86_64','print-build-dir']
        def run(value=None):
            return subprocess.run(command+([] if value is None else ['X86_KERNEL_LOAD_ADDRESS='+value]),
                cwd=ROOT,text=True,capture_output=True)
        default=run();self.assertEqual(default.returncode,0,default.stderr)
        decimal=run('33554432');self.assertEqual(decimal.returncode,0,decimal.stderr)
        self.assertEqual(default.stdout,decimal.stdout)
        alternate=run('0x04000000');self.assertEqual(alternate.returncode,0,alternate.stderr)
        self.assertNotEqual(default.stdout,alternate.stdout)
        for value in ['0x100000','0x300000','0x40000000','not-an-address','-2']:
            result=run(value)
            self.assertNotEqual(result.returncode,0,value)
            self.assertIn('X86_KERNEL_LOAD_ADDRESS must be',result.stderr)
        sentinel=Path(self.tmp.name)/'unexpected-shell-command'
        malicious="0x200000'; touch "+str(sentinel)+"; echo '"
        result=run(malicious)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('X86_KERNEL_LOAD_ADDRESS must be',result.stderr)
        self.assertFalse(sentinel.exists(),'load address escaped its shell argument')

    def test_standard_uefi_map_flag(self):self.assertEqual(self.lib.map_flag(),64)

    def test_uefi_loaded_image_uses_real_protocol_load_options(self):
        out=ctypes.create_string_buffer(16);self.lib.options_copy(out)
        self.assertEqual(out.value,b'a20.x=1')

    def test_no_map_legacy_and_basic_memory_contract(self):
        self.assertEqual(self.decode(magic=0), (0,[(0x100000,0x40000000)],0))
        self.assertEqual(self.decode(header=info(flags=0)), (0,[(0x100000,0x40000000)],0))
        self.assertEqual(self.decode(header=info(flags=1,upper=31*1024)), (0,[(0x100000,0x2000000)],1))
        self.assertEqual(self.decode(header=info(flags=1,upper=0))[0],-22)

    def test_reserved_wins_over_overlapping_ram_in_any_order(self):
        ram=record(0,0x4000000)
        holes=[record(0x800000,0x200000,2),record(0x1000000,0x1000,3)]
        for data in [ram+b''.join(holes),b''.join(reversed(holes))+ram]:
            self.assertEqual(self.decode(data),(0,[(0x100000,0x800000),(0xa00000,0x1000000),(0x1001000,0x4000000)],2))

    def test_extended_record_payload_and_unaligned_next_record(self):
        data=record(0,0x4000000,extra=b'abcdef')+record(0x800000,4096,4)
        self.assertEqual(self.decode(data)[1],[(0x100000,0x800000),(0x801000,0x4000000)])

    def test_malformed_record_sizes_never_fall_back(self):
        for data in [b'',struct.pack('<I',0),struct.pack('<I',19)+bytes(19),
                     struct.pack('<I',0xffffffff)+bytes(20),record(0,0x4000000)+bytes(4)]:
            self.assertLess(self.decode(data)[0],0,data)
        for length in range(4,24):self.assertEqual(self.decode(record(0,0x4000000)[:length])[0],-22)

    def test_short_record_guard_page(self):
        for length in [20,21,22,23]:
            code=f'import ctypes;lib=ctypes.CDLL({str(self.so)!r});raise SystemExit(0 if lib.guarded({length})==-22 else 2)'
            result=subprocess.run([sys.executable,'-c',code],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_pointer_window_and_uint64_overflow(self):
        self.assertEqual(self.decode(record(0,0x4000000),info_address=0xfffffff0)[0],-14)
        self.assertEqual(self.decode(record(0,0x4000000),map_address=0xfffffff0)[0],-14)
        for kind in [1,2]:self.assertEqual(self.decode(record(0xfffffffffffff000,0x2000,kind))[0],-75)

    def test_present_map_without_available_ram_is_an_error(self):
        self.assertEqual(self.decode(record(0,0x40000000,2))[0],-19)

    def test_capacity_failure_is_explicit_and_counts_reset(self):
        data=record(0,0x4000000)+record(0x800000,4096,2)
        rc,ranges,_=self.decode(data,capacity=1);self.assertEqual((rc,ranges),(-28,[]))

    def test_handoff_header_and_map_are_reserved(self):
        data=record(0,0x4000000)
        self.assertEqual(self.decode(data,info_address=0x300000,map_address=0x400000)[1],
            [(0x100000,0x300000),(0x301000,0x400000),(0x401000,0x4000000)])

    def test_module_image_and_symbol_table_are_reserved(self):
        data=record(0,0x4000000);mods=struct.pack('<4I',0x800000,0x900000,0,0)
        rc,ranges,_=self.decode(data,header=info(flags=64|8,modules=1),other=[(0x5000,mods)])
        self.assertEqual((rc,ranges),(0,[(0x100000,0x800000),(0x900000,0x4000000)]))
        self.assertEqual(self.decode(data,header=info(flags=64|32,syms=(4,64,0x800000,1)))[1],
                         [(0x100000,0x800000),(0x801000,0x4000000)])

    def test_extra_acpi_reservation_from_boot_services_data(self):
        self.assertEqual(self.decode(record(0,0x4000000),excluded=[(0x800000,0x801000)])[1],
                         [(0x100000,0x800000),(0x801000,0x4000000)])

    def test_high_ram_maps_only_complete_chunks_without_reserved_holes(self):
        data=record(0,0x8000000)+record(0x100000000,0x100000000)+record(0x180000000,4096,2)
        self.assertEqual(self.decode(data)[1],[(0x100000,0x8000000),(0x100000000,0x180000000),(0x1c0000000,0x200000000)])

    def test_uefi_types_and_runtime_attribute(self):
        data=b''.join(descriptor(kind,0x1000000+kind*0x100000,4) for kind in range(1,11))
        rc,records,_=self.export(data)
        self.assertEqual(rc,0)
        for kind,row in zip(range(1,11),records):self.assertEqual(row[3],1 if kind in [1,2,3,4,7] else 2)
        rc,records,_=self.export(descriptor(7,0x400000,4,1<<63));self.assertEqual((rc,records[0][3]),(0,2))

    def test_uefi_scratch_cut_preserves_both_descriptor_sides(self):
        rc,records,raw=self.export(descriptor(2,0x200000,1024),scratch=0x300000,scratch_bytes=0x4000)
        self.assertEqual(rc,0);self.assertTrue(all(row[0]==20 for row in records));self.assertEqual(len(raw),len(records)*24)
        self.assertEqual(self.decode(raw)[1],[(0x200000,0x300000),(0x304000,0x600000)])

    def test_uefi_does_not_truncate_at_old_64_descriptor_limit(self):
        data=b''.join(descriptor(7,0x100000+i*0x10000,16) for i in range(128))
        rc,records,raw=self.export(data);self.assertEqual((rc,len(records)),(0,129))
        self.assertEqual(self.decode(raw)[1],[(0x100000,0x900000)])

    def test_uefi_descriptor_bounds_overflow_and_capacity(self):
        valid=descriptor(7,0x400000,8)
        for settings in [{'stride':0},{'stride':39},{'version':2},{'stride':48}]:self.assertEqual(self.export(valid,**settings)[0],-1)
        for data in [descriptor(7,0x400001,8),descriptor(7,0x400000,1<<52),descriptor(7,0xfffffffffffff000,2)]:
            self.assertEqual(self.export(data)[0],-1)
        self.assertEqual(self.export(valid,capacity=1)[:2],(-2,[]))
        self.assertEqual(self.export(valid+descriptor(7,0xfffffffffffff000,2))[:2],(-1,[]))

    def test_uefi_padded_descriptor_stride(self):
        data=descriptor(4,0x400000,8)+bytes(8)+descriptor(5,0x500000,8)+bytes(8)
        rc,rows,_=self.export(data,stride=48)
        self.assertEqual((rc,[(row[1],row[3]) for row in rows[:2]]),
                         (0,[(0x400000,1),(0x500000,2)]))

if __name__=='__main__':unittest.main()
