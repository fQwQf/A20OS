"""Execute production AArch64 EFI loader control flow with CPU-only hooks.

Fixed low mappings model the physical VBox allocation window. Firmware mocks
reject allocations/logging between GetMemoryMap and ExitBootServices.
"""
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
C=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#define A20_A64_EFI_LOADER_ARCH_H
static void sync_loaded_code(void *,size_t);
static __attribute__((noreturn)) void enter_kernel(uintptr_t,uintptr_t,uintptr_t,uintptr_t);
#define KERNEL_MEMORY_BYTES 0x18000
#include "arch/aarch64/boot/uefi/aarch64_loader.c"
__asm__(".pushsection .rodata\n.global _binary_kernel_bin_start\n_binary_kernel_bin_start:\n.byte 0xab\n.skip 4095\n.global _binary_kernel_bin_end\n_binary_kernel_bin_end:\n.popsection\n");
static jmp_buf entered;
static int allocations,snapshot,exits,maps,syncs,frees;
static uintptr_t handoff_pa=0x27000000;
static struct efi_loaded_image loaded;
static uint16_t options[1025];
static efi_status_t allocation(uint32_t how,uint32_t type,uint64_t pages,uint64_t *address) {
 assert(!snapshot&&type==EFI_LOADER_DATA);allocations++;
 if(allocations==1)assert(how==EFI_ALLOCATE_ADDRESS&&*address==KERNEL_LOAD_ADDRESS&&pages==24);
 else {assert(allocations==2&&how==EFI_ALLOCATE_MAX_ADDRESS&&*address==A64_UEFI_RAM_END-1&&pages==17);*address=handoff_pa;}
 void*p=mmap((void *)(uintptr_t)*address,pages*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(p==(void *)(uintptr_t)*address);memset(p,0x5a,pages*4096);return 0;
}
static efi_status_t release(uint64_t address,uint64_t pages) {assert(!snapshot);frees++;assert(!munmap((void *)(uintptr_t)address,pages*4096));return 0;}
static efi_status_t protocol(void *image,const struct efi_guid *guid,void **out) {
 assert(!snapshot&&image==(void *)1&&guid_equal(guid,&loaded_image_protocol_guid));*out=&loaded;return 0;
}
static efi_status_t output(struct efi_simple_text_output *console,uint16_t *text) {
 (void)console;(void)text;assert(!snapshot);return 0;
}
static efi_status_t memory_map(uint64_t *bytes,void *buffer,uint64_t *key,uint64_t *stride,uint32_t *version) {
 assert(!snapshot&&allocations==2);maps++;snapshot=1;
 assert(*bytes==A64_UEFI_MAP_CAPACITY);*bytes=2*48;*stride=48;*version=1;*key=maps;
 a64_uefi_descriptor_t*d=buffer;*d=(a64_uefi_descriptor_t){.type=2,.physical=KERNEL_LOAD_ADDRESS,.pages=24};
 d=(void *)((uint8_t *)buffer+48);*d=(a64_uefi_descriptor_t){.type=2,.physical=handoff_pa,.pages=17};return 0;
}
static efi_status_t exit_services(void *image,uint64_t key) {
 assert(snapshot&&image==(void *)1&&key==(uint64_t)maps);snapshot=0;exits++;
 return exits==1?EFI_INVALID_PARAMETER:0; /* Retry must only take a fresh map. */
}
static void sync_loaded_code(void *pointer,size_t bytes) {
 assert(!snapshot);syncs++;
 if(syncs==1)assert(pointer==(void *)KERNEL_LOAD_ADDRESS&&bytes==KERNEL_MEMORY_BYTES);
 else assert(syncs==2&&exits==2&&pointer==(void *)handoff_pa&&bytes==A64_UEFI_HANDOFF_BYTES);
}
static __attribute__((noreturn)) void enter_kernel(uintptr_t entry,uintptr_t rsdp,uintptr_t cmdline,uintptr_t handoff) {
 assert(entry==KERNEL_LOAD_ADDRESS&&rsdp==0x20000000&&handoff==handoff_pa&&exits==2&&maps==2&&syncs==2);
 const uint8_t*k=(void *)entry;assert(k[0]==0xab&&k[4096]==0&&k[KERNEL_MEMORY_BYTES-1]==0);
 a64_uefi_handoff_t*h=(void *)handoff;assert(h->magic==A64_UEFI_HANDOFF_MAGIC&&h->version==1);
 assert(h->kernel_bytes==KERNEL_MEMORY_BYTES&&h->descriptor_bytes==48&&h->map_bytes==96);
 if(loaded.load_options_size)assert(cmdline==handoff_pa+256&&!strcmp((const char *)cmdline,"a20.x=1")&&h->cmdline_bytes==8);
 else assert(!cmdline&&!h->cmdline_bytes);
 longjmp(entered,1);
}
int main(int argc,char**argv) {
 assert(argc==2);
 struct efi_boot_services bs={.allocate_pages=allocation,.free_pages=release,.handle_protocol=protocol,
 .get_memory_map=memory_map,.exit_boot_services=exit_services};
 struct efi_simple_text_output console={.output_string=output};
 struct efi_configuration_table acpi={.vendor_guid=acpi20_table_guid,.vendor_table=(void *)0x20000000};
 struct efi_system_table st={.boot_services=&bs,.con_out=&console,.number_of_table_entries=1,.configuration_table=&acpi};
 if(!strcmp(argv[1],"too-long")) {
  for(unsigned i=0;i<1024;i++){options[i]='a';}options[1024]=0;
  loaded.load_options=options;loaded.load_options_size=sizeof(options);
  assert(efi_main((void *)1,&st)==EFI_BAD_BUFFER_SIZE&&frees==2&&!maps&&!exits);return 0;
 }
 if(!strcmp(argv[1],"options")) {
  const char*s="a20.x=1";for(unsigned i=0;i<7;i++)options[i]=(uint16_t)s[i];
  loaded.load_options=options;loaded.load_options_size=16;
 }
 if(!setjmp(entered)){(void)efi_main((void *)1,&st);assert(0);}
 return 0;
}
'''
@unittest.skipUnless(platform.system()=='Linux' and shutil.which('cc'),'Linux mmap + host C compiler required')
class Aarch64UefiLoaderTests(unittest.TestCase):
 def test_production_loader_transactions(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);(p/'test.c').write_text(C)
   subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-no-pie',f'-I{ROOT}/kernel',str(p/'test.c'),'-o',str(p/'test')],check=True)
   for case in ('options','none','too-long'):subprocess.run([str(p/'test'),case],check=True)
if __name__=='__main__':unittest.main()
