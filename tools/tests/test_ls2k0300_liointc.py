"""Run production LS2K0300 interrupt-bank mechanics against W1C register hardware."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'kernel/arch/loongarch64/boards/ls2k0300'
FIXTURE = r'''
#include <assert.h>
#include <string.h>
#include "liointc.h"
static ls2k0300_lio_t chip;
static unsigned char regs[128];
static unsigned enables[2], calls[64], nr, mask_during_handler;
static uint32_t read32(void *ctx,unsigned off) {
 (void)ctx;uint32_t value;assert(off<=124);memcpy(&value,regs+off,4);return value;
}
static void write32(void *ctx,unsigned off,uint32_t value) {
 (void)ctx;assert(off<=124);
 if(off%64==0x28)enables[off/64]|=value;
 else if(off%64==0x2c)enables[off/64]&=~value;
 else memcpy(regs+off,&value,4);
}
static void write8(void *ctx,unsigned off,uint8_t value) {(void)ctx;assert(off<128);regs[off]=value;}
static void dispatch(uint32_t irq) {
 assert(nr<64);calls[nr++]=irq;
 assert(enables[irq/32]&(1U<<(irq%32))); /* A new edge can latch now. */
 if(mask_during_handler)ls2k0300_lio_disable(&chip,irq);
}
int run(unsigned test) {
 memset(regs,0xee,sizeof(regs));enables[0]=enables[1]=~0U;nr=mask_during_handler=0;
 ls2k0300_lio_io_t io={0,read32,write32,write8};
 assert(!ls2k0300_lio_init(&chip,&io));
 assert(!enables[0]&&!enables[1]);
 for(unsigned i=0;i<32;i++){assert(regs[i]==0x10);assert(regs[64+i]==0x40);}
 if(test==0)return 0;
 if(test==1){
  assert(!ls2k0300_lio_configure(&chip,63,2));
  assert(read32(0,0x70)==0x80000000);assert(read32(0,0x74)==0x80000000);
  assert(!ls2k0300_lio_configure(&chip,63,4));assert(!read32(0,0x70));assert(!read32(0,0x74));
  assert(ls2k0300_lio_configure(&chip,64,4)<0);assert(ls2k0300_lio_configure(&chip,0,3)<0);
 }else if(test==2){
  assert(!ls2k0300_lio_enable(&chip,2));assert(ls2k0300_lio_configure(&chip,2,8)<0);
  ls2k0300_lio_disable(&chip,2);assert(!ls2k0300_lio_configure(&chip,2,8));
  assert(ls2k0300_lio_enable(&chip,64)<0);
 }else if(test==3){
  ls2k0300_lio_enable(&chip,32);ls2k0300_lio_enable(&chip,63);
  ls2k0300_lio_dispatch(&chip,1,~0U,dispatch);
  assert(nr==2&&calls[0]==32&&calls[1]==63);assert(enables[1]==0x80000001);
  ls2k0300_lio_dispatch(&chip,2,~0U,dispatch);assert(nr==2);
 }else if(test==4||test==5){
  assert(!ls2k0300_lio_configure(&chip,1,1));ls2k0300_lio_enable(&chip,1);
  mask_during_handler=test==5;ls2k0300_lio_dispatch(&chip,0,2,dispatch);
  assert(nr==1&&calls[0]==1);assert(enables[0]==(test==5?0:2));
 }else return 1;
 return 0;
}
'''

class Liointc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-liointc-')
        work = Path(cls.temp.name)
        (work/'fixture.c').write_text(FIXTURE)
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT/'kernel/include'), '-I', str(SOURCE),
                        str(work/'fixture.c'), str(SOURCE/'liointc.c'),
                        '-o', str(work/'test.so')], check=True)
        cls.library = ctypes.CDLL(str(work/'test.so'))
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def test_masked_initial_routes(self): self.assertEqual(self.library.run(0),0)
    def test_trigger_polarity_and_bounds(self): self.assertEqual(self.library.run(1),0)
    def test_live_configuration_rejected(self): self.assertEqual(self.library.run(2),0)
    def test_second_bank_masked_dispatch(self): self.assertEqual(self.library.run(3),0)
    def test_edge_rearm(self): self.assertEqual(self.library.run(4),0)
    def test_handler_disable_survives_edge_ack(self): self.assertEqual(self.library.run(5),0)

if __name__=='__main__': unittest.main()
