"""Production physical-span and exact-PC C906 probe-recovery contracts."""
from pathlib import Path
import subprocess
import re
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]

class C906CacheModel(unittest.TestCase):
    def test_physical_conversion_and_precise_probe_recovery(self):
        production=(ROOT/'kernel/arch/riscv64/platform/dma_cache.c').read_text()
        begin=production.index('int riscv_c906_cache_probe_fixup(')
        fixup=production[begin:production.index('\n}\n',begin)+3]
        begin=production.index('static void cache_range(')
        cache=production[begin:production.index('\n}\n',begin)+3]
        cache=cache.replace('__asm__("a0")','')
        cache=re.sub(r'__asm__ __volatile__\(".long 0x02b5000b"\s*:\s*"\+r"\(at\)\s*::\s*"memory"\)', 'record(at,1)', cache)
        cache=re.sub(r'__asm__ __volatile__\(".long 0x0295000b"\s*:\s*"\+r"\(at\)\s*::\s*"memory"\)', 'record(at,0)', cache)
        cache=re.sub(r'__asm__ __volatile__\(".long 0x01b0000b"\s*:::\s*"memory"\)', 'syncs++', cache)
        begin=production.index('int arch_device_dma_ready(')
        ready=production[begin:production.index('\n}\n',begin)+3]
        begin=production.index('static int current_cpu_is_c906(')
        identity=production[begin:production.index('int riscv_sg2002_dma_cache_prepare(',begin)]
        source=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include "HEADER"
#define PAGE_OFFSET 0xffffffc000000000ULL
#define CONFIG_BOARD_LICHEERV_NANO 1
static int probe_active,probe_failed,cache_ready;
char riscv_cmo_probe_clean[4],riscv_cmo_probe_invalidate[4],riscv_cmo_probe_sync[4],riscv_cmo_probe_resume[4];
typedef struct {uint64_t sstatus,sepc;} trap_context_t;
static uint64_t cause=2,value;
static uint64_t arch_read_cause(void){return cause;}
static uint64_t arch_read_tval(void){return value;}
static unsigned calls,syncs,barriers;static uint64_t addresses[8];static int operations[8];
static void record(uint64_t at,int op){assert(calls<8);addresses[calls]=at;operations[calls++]=op;}
static void arch_mb(void){barriers++;}
static void panic(const char *s){(void)s;abort();}
typedef struct {int id;} fw_node_t;
uint64_t __boot_hart_id=0;
static int root_match=1,cpu_match=1;
static int fw_node_next_child(const fw_node_t*n,const fw_node_t*prev,fw_node_t*out){
 if(prev){return -1;}out->id=n->id==1?2:3;return n->id==1||n->id==2?0:-1;
}
static const char *fw_node_name(const fw_node_t*n){return n->id==2?"cpus":"cpu@0";}
static int fw_node_read_u32(const fw_node_t*n,const char*p,uint32_t*out){(void)n;(void)p;*out=0;return 0;}
static int fw_node_match_id(const fw_node_t*n,const char*p){(void)p;return n->id==1?root_match:cpu_match;}
IDENTITY
READY
FIXUP
CACHE
int main(void){
 fw_node_t root={1};assert(platform_is_sg2002_c906(&root));
 root_match=-22;assert(!platform_is_sg2002_c906(&root));root_match=1;
 cpu_match=-22;assert(!platform_is_sg2002_c906(&root));cpu_match=1;
 __boot_hart_id=1;assert(!platform_is_sg2002_c906(&root));__boot_hart_id=0;
 uint64_t first,last;
 assert(riscv_dma_physical_span(PAGE_OFFSET+0x80000003,125,PAGE_OFFSET,&first,&last));
 assert(first==0x80000000&&last==0x80000040);
 assert(!riscv_dma_physical_span(PAGE_OFFSET-1,1,PAGE_OFFSET,&first,&last));
 assert(!riscv_dma_physical_span(UINTPTR_MAX-1,4,PAGE_OFFSET,&first,&last));
 assert(!riscv_dma_physical_span(PAGE_OFFSET,0,PAGE_OFFSET,&first,&last));
 trap_context_t ctx={1<<8,(uintptr_t)riscv_cmo_probe_clean};
 assert(!riscv_c906_cache_probe_fixup(&ctx));
 probe_active=1;cause=5;assert(!riscv_c906_cache_probe_fixup(&ctx));
 cause=2;ctx.sstatus=0;assert(!riscv_c906_cache_probe_fixup(&ctx));ctx.sstatus=1<<8;
 value=0xdeadbeef;assert(!riscv_c906_cache_probe_fixup(&ctx));
 value=0;ctx.sepc=1234;assert(!riscv_c906_cache_probe_fixup(&ctx));
 uintptr_t pcs[]={(uintptr_t)riscv_cmo_probe_clean,(uintptr_t)riscv_cmo_probe_invalidate,(uintptr_t)riscv_cmo_probe_sync};
 unsigned words[]={0x0295000b,0x02b5000b,0x01b0000b};
 for(unsigned i=0;i<3;i++){ctx.sepc=pcs[i];value=words[i];probe_failed=0;
 assert(riscv_c906_cache_probe_fixup(&ctx)&&probe_failed&&ctx.sepc==(uintptr_t)riscv_cmo_probe_resume);}
 cache_ready=0;assert(!arch_device_dma_ready());cache_range((void*)(PAGE_OFFSET+0x80000003),125,0);assert(!calls&&barriers==1);
 cache_ready=1;assert(arch_device_dma_ready());cache_range((void*)(PAGE_OFFSET+0x80000003),125,0);
 assert(calls==2&&syncs==1&&addresses[0]==0x80000000&&addresses[1]==0x80000040&&!operations[0]);
 cache_range((void*)(PAGE_OFFSET+0x80000080),64,1);assert(calls==3&&syncs==2&&addresses[2]==0x80000080&&operations[2]);
 return 0;
}
'''.replace('HEADER',str(ROOT/'kernel/arch/riscv64/include/dma_cache.h')).replace('IDENTITY',identity).replace('READY',ready).replace('FIXUP',fixup).replace('CACHE',cache)
        with tempfile.TemporaryDirectory() as directory:
            stage=Path(directory);(stage/'core').mkdir()
            (stage/'core/types.h').write_text('#include <stdint.h>\n#include <stddef.h>\n')
            (stage/'model.c').write_text(source)
            build=subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(stage),str(stage/'model.c'),'-o',str(stage/'model')],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            result=subprocess.run([str(stage/'model')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)

if __name__=='__main__': unittest.main()
