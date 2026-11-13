"""Run the production SDHCI engine against width-aware SD/SDHCI hardware.

The fixture models register side effects (W1C, FIFO consumption, command
responses, reset/clock handshakes). It covers both selected board clock models,
SDHC and legacy byte addressing, plus failed-controller/card paths.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/block/sdhci_core.c"

struct model {
 unsigned char reg[0x100];
 uint64_t usecs;
 unsigned fifo, reads, writes, commands, resets, write_checks;
 unsigned arg17[8], n17, arg24;
 unsigned long_csd[4];
 int legacy, never_powerup, bad_echo, bad_csd, select_error, wrong_voltage;
 int stuck_reset, stuck_clock, stuck_inhibit, stuck_data;
 int command_error, data_error, hook_error;
 unsigned r1_flags;
};
static uint32_t get(struct model *m,unsigned r,unsigned width) {
 uint32_t v=0;for(unsigned i=0;i<width;i++)v|=(uint32_t)m->reg[r+i]<<(8*i);return v;
}
static void put(struct model *m,unsigned r,unsigned width,uint32_t v) {
 assert(r+width<=sizeof(m->reg));for(unsigned i=0;i<width;i++)m->reg[r+i]=v>>(i*8);
}
static void status(struct model *m,uint32_t bits) {put(m,INT_STATUS,4,get(m,INT_STATUS,4)|bits);}
static uint32_t read_reg(void *p,unsigned r,unsigned width) {
 struct model *m=p;assert(r+width<=0x100);
 if(r==BUFFER) {
  assert(width==4&&m->fifo<128);uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)(unsigned char)(m->fifo*4+i)<<(i*8);
  m->fifo++;m->reads++;
  if(m->fifo==128) {
   status(m,m->data_error?ERROR|(1U<<21):DATA_DONE);put(m,PRESENT_STATE,4,0);
  }
  return v;
 }
 if(r==PRESENT_STATE&&m->stuck_inhibit)return 3;
 return get(m,r,width);
}
static void write_reg(void *p,unsigned r,unsigned width,uint32_t v) {
 struct model *m=p;assert(r+width<=0x100);
 if(r==INT_STATUS){assert(width==4);put(m,r,4,get(m,r,4)&~v);return;}
 if(r==SOFTWARE_RESET){assert(width==1);m->resets++;put(m,r,1,m->stuck_reset?v:0);return;}
 if(r==CLOCK_CONTROL){assert(width==2);put(m,r,2,v|((v&1)&&!m->stuck_clock?2:0));return;}
 if(r==BUFFER) {
  assert(width==4&&m->fifo<128);assert(v==0xa5a5a5a5U);m->fifo++;m->writes++;
  if(m->fifo==128){status(m,DATA_DONE);put(m,PRESENT_STATE,4,0);}return;
 }
 put(m,r,width,v);
 if(r!=COMMAND)return;
 assert(width==2);unsigned cmd=v>>8;m->commands++;put(m,RESPONSE,4,0);
 if(cmd==8) {
  if(m->legacy){status(m,ERROR|(1U<<16));return;}
  put(m,RESPONSE,4,m->bad_echo?0x2aa:0x1aa);
 } else if(cmd==41) {
  put(m,RESPONSE,4,(m->wrong_voltage?0x00008000U:0x00ff8000U)|(!m->never_powerup?1U<<31:0)|(!m->legacy?1U<<30:0));
 } else if(cmd==3)put(m,RESPONSE,4,0x12340000);
 else if(cmd==9) {
  for(unsigned i=0;i<4;i++) {
   uint32_t value=m->long_csd[i]>>8;
   if(i)value|=m->long_csd[i-1]<<24;
   put(m,RESPONSE+(3-i)*4,4,value);
  }
 } else if(cmd==7&&m->select_error)put(m,RESPONSE,4,1U<<22);
 else if(cmd==17||cmd==24) {
  if(m->command_error){status(m,ERROR|(1U<<17));return;}
  assert(get(m,BLOCK_SIZE,2)==512&&get(m,BLOCK_COUNT,2)==1);
  assert((get(m,TRANSFER_MODE,2)&1)==0); /* DMA must remain disabled. */
  if(cmd==17){assert(m->n17<8);m->arg17[m->n17++]=get(m,ARGUMENT,4);}
  else m->arg24=get(m,ARGUMENT,4);
  m->fifo=0;put(m,PRESENT_STATE,4,2);
  if(!m->stuck_data)status(m,cmd==17?READ_READY:WRITE_READY);
 } else if(cmd==13) {
  m->write_checks++;
  put(m,RESPONSE,4,m->write_checks<3?7U<<9:(4U<<9)|(1U<<8));
 }
 if(cmd==55||cmd==13)put(m,RESPONSE,4,get(m,RESPONSE,4)|m->r1_flags);
 status(m,CMD_DONE);
}
static uint64_t time_ms(void *p) {return ((struct model*)p)->usecs/1000;}
static void delay_us(void *p,unsigned n) {((struct model*)p)->usecs+=n;}
static int hook(void *p) {return ((struct model*)p)->hook_error;}
static sdhci_host_t host(struct model *m,int legacy) {
 memset(m,0,sizeof(*m));m->legacy=legacy;
 if(legacy) {
  /* 1024 * 512 blocks of 512 bytes (256MiB SDSC). */
  m->long_csd[1]=(9U<<16)|(1023U>>2);m->long_csd[2]=(3U<<30)|(7U<<15);
 } else {
  m->long_csd[0]=1U<<30;m->long_csd[2]=15U<<16; /* 16384 sectors. */
 }
 return (sdhci_host_t){.io={m,read_reg,write_reg,time_ms,delay_us},
  .input_hz=25000000,.max_hz=25000000,.spec_version=2,
  .before_idle=hook,.vendor_context=m};
}
int main(int argc,char **argv) {
 assert(argc==2);struct model m;sdhci_host_t h=host(&m,0);
 unsigned char backing[1025],*buf=backing+1;
 if(!strcmp(argv[1],"clocks")) {
  uint16_t encoded;uint32_t actual;
  assert(!sdhci_clock_divider(25000000,400000,2,&encoded,&actual)&&actual<=400000);
  assert(encoded==(32U<<8)&&actual==390625);
  assert(!sdhci_clock_divider(96000000,400000,0,&encoded,&actual)&&encoded==0x8000&&actual==375000);
  assert(!sdhci_clock_divider(96000000,25000000,0,&encoded,&actual)&&encoded==0x200&&actual==24000000);
  assert(!sdhci_clock_divider(375000000,400000,2,&encoded,&actual)&&actual<=400000);
  assert(sdhci_clock_divider(96000000,100000,0,&encoded,&actual)==-ERANGE);
  assert(sdhci_clock_divider(0,400000,2,&encoded,&actual)==-EINVAL);return 0;
 }
 if(!strcmp(argv[1],"bl808")){h.input_hz=96000000;h.spec_version=0;}
 if(!strcmp(argv[1],"sdsc"))h=host(&m,1);
 if(!strcmp(argv[1],"bad-echo"))m.bad_echo=1;
 if(!strcmp(argv[1],"nonerror-status"))m.r1_flags=(1U<<14)|(1U<<13);
 if(!strcmp(argv[1],"locked-card"))m.r1_flags=1U<<25;
 if(!strcmp(argv[1],"power-timeout"))m.never_powerup=1;
 if(!strcmp(argv[1],"reset-timeout"))m.stuck_reset=1;
 if(!strcmp(argv[1],"clock-timeout"))m.stuck_clock=1;
 if(!strcmp(argv[1],"inhibit-timeout"))m.stuck_inhibit=1;
 if(!strcmp(argv[1],"hook-error"))m.hook_error=-EIO;
 if(!strcmp(argv[1],"select-error"))m.select_error=1;
 if(!strcmp(argv[1],"bad-csd"))m.long_csd[0]=2U<<30;
 if(!strcmp(argv[1],"bad-ocr-voltage"))m.wrong_voltage=1;
 if(!strcmp(argv[1],"permanent-wp"))m.long_csd[3]|=1U<<13;
 if(!strcmp(argv[1],"temporary-wp"))m.long_csd[3]|=1U<<12;
 int rc=sdhci_card_initialize(&h);
 if(strstr(argv[1],"timeout")){assert(rc==-ETIMEDOUT&&!h.ready);return 0;}
 if(!strncmp(argv[1],"bad-",4)||!strcmp(argv[1],"hook-error")||!strcmp(argv[1],"select-error")||!strcmp(argv[1],"locked-card")) {
  assert(rc<0&&!h.ready);return 0;
 }
 assert(!rc&&h.ready&&h.rca==0x1234&&h.sectors==(m.legacy?524288:16384));
 assert(!get(&m,SIGNAL_ENABLE,4));
 if(!strcmp(argv[1],"command-error"))m.command_error=1;
 if(!strcmp(argv[1],"data-error"))m.data_error=1;
 if(!strcmp(argv[1],"data-stall"))m.stuck_data=1;
 unsigned resets=m.resets;
 rc=sdhci_card_transfer(&h,2,buf,2,0);
 if(m.command_error||m.data_error||m.stuck_data) {
  assert(rc==(m.stuck_data?-ETIMEDOUT:-EIO)&&m.resets>resets);
  assert(m.reads==(m.data_error?128:0)&&!h.ready);
  assert(sdhci_card_transfer(&h,2,buf,1,0)==-ENODEV);return 0;
 }
 assert(!rc&&m.reads==256&&m.n17==2);
 assert(m.arg17[0]==(m.legacy?1024:2)&&m.arg17[1]==(m.legacy?1536:3));
 for(unsigned i=0;i<1024;i++)assert(buf[i]==(unsigned char)i);
 unsigned commands=m.commands;
 assert(sdhci_card_transfer(&h,h.sectors,buf,1,0)==-ERANGE);
 assert(sdhci_card_transfer(&h,h.sectors-1,buf,2,0)==-ERANGE);
 assert(sdhci_card_transfer(&h,0,buf,0,0)==-EINVAL);
 assert(m.commands==commands);
 if(strstr(argv[1],"-wp")) {assert(h.write_protected);assert(sdhci_card_transfer(&h,7,buf,1,1)==-EROFS&&h.ready&&!m.writes);return 0;}
 memset(buf,0xa5,512);assert(!sdhci_card_transfer(&h,7,buf,1,1));
 assert(m.writes==128&&m.arg24==(m.legacy?3584:7)&&m.write_checks==3);
 sdhci_card_stop(&h);assert(!h.ready&&!get(&m,CLOCK_CONTROL,2));
 assert(sdhci_card_transfer(&h,0,buf,1,0)==-ENODEV);return 0;
}
'''

BOARD_FIXTURE = r'''
#include <assert.h>
#include <string.h>
#include "drivers/core/driver_core.h"
#include "core/errno.h"
#define _DRIVER_HWAPI_H
static uint32_t clock_regs[1024], pin_regs[1024], host_regs[1024], vendor_regs[8];
static unsigned map_calls,unmap_calls,puts;
static uint64_t ticks;
static int fail_mapping,stall_pad;
static device_t device;
static uint16_t readw(const volatile void*p) {return *(const volatile uint16_t*)p;}
static uint32_t readl(const volatile void*p) {return *(const volatile uint32_t*)p;}
static void writew(uint16_t v,volatile void*p) {
 if(p==(void*)((unsigned char*)vendor_regs+0x0e)) {
  uint16_t old=readw(p);*(volatile uint16_t*)p=(v&~4U)|(old&4U&~v);return;
 }
 *(volatile uint16_t*)p=v;
}
static void writel(uint32_t v,volatile void*p) {
 *(volatile uint32_t*)p=v;
 if(p==(void*)vendor_regs&&(v&(1U<<6))&&!stall_pad)
  *(uint16_t*)((unsigned char*)vendor_regs+0x0e)|=4;
}
static void udelay(unsigned n) {ticks+=n;}
static uint64_t clock_get_ticks(void) {return ticks;}
static uint64_t clock_ticks_per_sec(void) {return 1000000;}
void *arch_device_map_mmio(uint64_t pa,size_t size,uint32_t flags) {
 (void)flags;map_calls++;if(fail_mapping)return NULL;
 if(pa==0x03002000&&size==0x1000)return clock_regs;
 if(pa==0x03001000&&size==0x1000)return pin_regs;
 if(pa==0x20000000&&size==0x1000)return clock_regs;
 if(pa==0x20060100&&size==0x20)return vendor_regs;
 assert(0);return NULL;
}
void arch_device_unmap_mmio(void*p,size_t size) {(void)p;(void)size;unmap_calls++;}
static const char *identity;
static int match_id(const fw_node_t *n,const char *id) {(void)n;return !strcmp(identity,id);}
static fw_node_ops_t node_ops={.match_id=match_id};
device_t *device_find_get_by_mmio(uint64_t pa) {assert(pa==0x04310000||pa==0x20060000);return &device;}
void device_put(device_t*d) {assert(d==&device);puts++;}
#define CONFIG_BOARD_LICHEERV_NANO
#define CONFIG_BOARD_OX64
#include "arch/riscv64/boards/licheerv-nano/storage.c"
#include "arch/riscv64/boards/ox64/storage.c"
int main(int argc,char**argv) {
 assert(argc==2);device.firmware_node=(fw_node_t){.ops=&node_ops};
 if(!strcmp(argv[1],"nano")) {
  identity="sophgo,cv1800b-dwcmshc";licheerv_nano_configure_storage();
  assert(device.plat_data==&nano_sd_platform&&puts==1);
  clock_regs[0]=1;pin_regs[0x1c/4]=0xffffffff;host_regs[0xe8/4]=0x200;
  assert(!nano_sd_prepare(&device,host_regs));
  assert((clock_regs[0]&(3U<<18))==(3U<<18)&&clock_regs[0]&1);
  assert(clock_regs[0x30/4]&(1U<<6));
  assert(pin_regs[0x1c/4]==0xfffffff8&&map_calls==unmap_calls);
  assert(!nano_sd_before_idle(&device,host_regs));
  assert(host_regs[0x200/4]==2&&host_regs[0x240/4]==0x01000100&&host_regs[0x24c/4]&1);
  host_regs[0xe8/4]=0xff0;assert(nano_sd_before_idle(&device,host_regs)==-EINVAL);
 } else if(!strcmp(argv[1],"sg2002-id")) {
  identity="sophgo,sg2002-dwcmshc";licheerv_nano_configure_storage();
  assert(device.plat_data==&nano_sd_platform);
 } else if(!strcmp(argv[1],"ownership")) {
  identity="sophgo,cv1800b-dwcmshc";device.user_owned=1;
  licheerv_nano_configure_storage();assert(!device.plat_data);
  identity="bflb,bl808-sdhci";ox64_configure_storage();assert(!device.plat_data);
  device.user_owned=0;device.plat_data=&device;ox64_configure_storage();
  assert(device.plat_data==&device&&puts==3);
 } else if(!strcmp(argv[1],"wrong-id")) {
  identity="snps,dw-mshc";licheerv_nano_configure_storage();ox64_configure_storage();
  assert(!device.plat_data&&puts==2);
 } else if(!strcmp(argv[1],"map-failure")) {
  fail_mapping=1;assert(nano_sd_prepare(&device,host_regs)==-ENOMEM);
  assert(ox64_sd_prepare(&device,host_regs)==-ENOMEM);
  assert(ox64_sd_before_idle(&device,host_regs)==-ENOMEM);
 } else {
  identity="bflb,bl808-sdhci";ox64_configure_storage();assert(device.plat_data==&ox64_sd_platform);
  assert(!ox64_sd_prepare(&device,host_regs));
  assert(clock_regs[0x430/4]==(1U<<13)&&clock_regs[0x584/4]&(1U<<22));
  assert(!(clock_regs[0x540/4]&(1U<<22))&&clock_regs[0x510/4]&(1U<<22));
  for(unsigned pin=0;pin<6;pin++)assert(clock_regs[(0x8c4+pin*4)/4]==0x400017);
  if(!strcmp(argv[1],"pad-timeout"))stall_pad=1;
  assert(ox64_sd_before_idle(&device,host_regs)==(stall_pad?-ETIMEDOUT:0));
  assert(vendor_regs[0x18/4]&(1U<<30));assert((vendor_regs[0]>>24)==80);
  assert(map_calls==unmap_calls);
 }
 return 0;
}
'''

DRIVER_FIXTURE = r'''
#include <assert.h>
#include <stdlib.h>
#include "drivers/block/sdhci.c"
static unsigned char hardware[4096];
static resource_t resource={.type=RES_MMIO,.start=0x04310000,.end=0x04310fff};
static void (*release_action)(void*);
static void *release_data;
static unsigned allocated,freed,prepared,initialized,stopped,locked;
static int failure;
int fw_node_has_property(const fw_node_t *node,const char *name) {
 (void)node;assert(!strcmp(name,"read-only"));return failure==8?1:failure==9?-EINVAL:0;
}
bus_type_t platform_bus;
int klog_level=10;
void klog_write(const char*fmt,...) {(void)fmt;}
int klog_write_ex(int level,const char*fmt,...) {(void)level;(void)fmt;return 0;}
void *kmalloc(size_t n) {if(failure==1)return NULL;allocated++;return malloc(n);}
void kfree(void *p) {assert(p);freed++;free(p);}
void mutex_init(mutex_t*m) {(void)m;}
void mutex_lock(mutex_t*m) {(void)m;assert(!locked);locked=1;}
void mutex_unlock(mutex_t*m) {(void)m;assert(locked);locked=0;}
uint64_t clock_get_ticks(void) {return 123456789;}
uint64_t clock_ticks_per_sec(void) {return 1000000;}
void udelay(unsigned n) {(void)n;}
resource_t *device_get_resource(device_t*d,enum resource_type t,int index) {
 (void)d;assert(t==RES_MMIO&&index==0);return &resource;
}
void *device_map_mmio(device_t*d,int index) {(void)d;assert(index==0);return failure==2?NULL:hardware;}
int device_add_resource_action(device_t*d,void(*release)(void*),void*p) {
 (void)d;if(failure==3)return -ENOMEM;release_action=release;release_data=p;return 0;
}
static int prepare(device_t*d,void*base) {(void)d;assert(base==hardware);prepared++;return failure==4?-EIO:0;}
int sdhci_card_initialize(sdhci_host_t*h) {
 assert(prepared&&h->input_hz==25000000&&h->io.read&&h->io.write&&h->io.time_ms);
 initialized++;h->sectors=12345;h->ready=1;return failure==5?-ETIMEDOUT:0;
}
void sdhci_card_stop(sdhci_host_t*h) {stopped++;h->ready=0;}
int sdhci_card_transfer(sdhci_host_t*h,uint64_t lba,void*buf,size_t count,int write) {
 assert(locked&&h->ready&&lba==7&&buf&&count==2&&(write==0||write==1));return h->write_protected&&write?-EROFS:0;
}
int main(int argc,char**argv) {
 assert(argc==2);failure=atoi(argv[1]);
 sdhci_platform_data_t platform={.input_hz=25000000,.max_hz=25000000,.spec_version=2,.prepare=prepare};
 device_t dev={.name="sd",.plat_data=&platform};
 if(failure==6)dev.plat_data=NULL;
 if(failure==7)resource.end=resource.start+0xfe;
 int rc=sdhci_probe(&dev);
 if(failure&&failure!=8) {
  assert(rc<0&&!dev.drv_priv);
  if(failure==4)assert(prepared&&!initialized&&!stopped);
  if(failure==5)assert(prepared&&initialized&&stopped==1);
 } else {
  assert(!rc&&dev.drv_priv&&sdhci_block_ops.capacity(&dev)==12345);
  unsigned char buf[1024];assert(!sdhci_block_ops.read(&dev,7,buf,2));
  if(failure==8)assert(((sdhci_device_t*)dev.drv_priv)->host.write_protected);
  assert(sdhci_block_ops.write(&dev,7,buf,2)==(failure==8?-EROFS:0));
  assert(!sdhci_remove(&dev)&&!dev.drv_priv&&stopped==1&&!locked);
  assert(sdhci_block_ops.read(&dev,7,buf,2)==-ENODEV);
 }
 if(release_action)release_action(release_data);
 assert(allocated==freed);return 0;
}
'''


class SdhciTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc"):
            raise unittest.SkipTest("host C compiler required")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / "fixture.c"
        source.write_text(FIXTURE)
        cls.exe = Path(cls.temp.name) / "fixture"
        result = subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
            "-fno-builtin", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
            "-DCONFIG_64BIT", f"-I{ROOT}/kernel/include", f"-I{ROOT}/kernel",
            str(source), "-o", str(cls.exe),
        ], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        board_source = Path(cls.temp.name) / "board.c"
        board_source.write_text(BOARD_FIXTURE)
        cls.board_exe = Path(cls.temp.name) / "board"
        result = subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
            "-fno-builtin", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
            "-DCONFIG_X86_64", "-DCONFIG_64BIT", "-DCONFIG_NR_CPUS=1",
            f"-I{ROOT}/kernel/arch/x86_64/include", f"-I{ROOT}/kernel/include", f"-I{ROOT}/kernel",
            str(board_source), str(ROOT / "kernel/firmware/node.c"), "-o", str(cls.board_exe),
        ], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

        driver_source = Path(cls.temp.name) / "driver.c"
        driver_source.write_text(DRIVER_FIXTURE)
        cls.driver_exe = Path(cls.temp.name) / "driver"
        result = subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
            "-fno-builtin", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
            "-DCONFIG_X86_64", "-DCONFIG_64BIT", "-DCONFIG_NR_CPUS=1",
            f"-I{ROOT}/kernel/arch/x86_64/include", f"-I{ROOT}/kernel/include", f"-I{ROOT}/kernel",
            str(driver_source), "-o", str(cls.driver_exe),
        ], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def driver_case(self, failure):
        result = subprocess.run([str(self.driver_exe), str(failure)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def board_case(self, name):
        result = subprocess.run([str(self.board_exe), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def case(self, name):
        result = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_driver_probe_io_remove_and_release(self): self.driver_case(0)
    def test_driver_allocation_failure(self): self.driver_case(1)
    def test_driver_mapping_failure(self): self.driver_case(2)
    def test_driver_release_action_failure_frees_allocation(self): self.driver_case(3)
    def test_driver_does_not_access_unprepared_host(self): self.driver_case(4)
    def test_driver_failed_card_initialization_stops_and_releases(self): self.driver_case(5)
    def test_driver_requires_board_clock_and_pins(self): self.driver_case(6)
    def test_driver_rejects_short_mmio_resource(self): self.driver_case(7)
    def test_driver_firmware_read_only(self): self.driver_case(8)
    def test_driver_malformed_read_only(self): self.driver_case(9)
    def test_csd_permanent_write_protection(self): self.case("permanent-wp")
    def test_csd_temporary_write_protection(self): self.case("temporary-wp")
    def test_ocr_rejects_wrong_supply_voltage(self): self.case("bad-ocr-voltage")
    def test_nano_board_pins_clock_and_phy(self): self.board_case("nano")
    def test_nano_modern_compatible(self): self.board_case("sg2002-id")
    def test_board_preserves_existing_and_user_ownership(self): self.board_case("ownership")
    def test_board_does_not_claim_older_dw_mci_controller(self): self.board_case("wrong-id")
    def test_board_mapping_failure(self): self.board_case("map-failure")
    def test_ox64_board_pins_reset_and_pad_clock(self): self.board_case("ox64")
    def test_ox64_pad_clock_is_bounded(self): self.board_case("pad-timeout")
    def test_dividers_never_exceed_requested_rate(self): self.case("clocks")
    def test_sg2002_sd_memory_and_unaligned_pio(self): self.case("sg2002")
    def test_bl808_legacy_divider_and_no_irq_or_dma(self): self.case("bl808")
    def test_sdsc_csd_and_byte_addressing(self): self.case("sdsc")
    def test_nonerror_card_status_flags(self): self.case("nonerror-status")
    def test_locked_card_is_not_published(self): self.case("locked-card")
    def test_card_voltage_echo_is_validated(self): self.case("bad-echo")
    def test_unsupported_csd_does_not_publish_disk(self): self.case("bad-csd")
    def test_acmd41_powerup_timeout(self): self.case("power-timeout")
    def test_controller_reset_timeout(self): self.case("reset-timeout")
    def test_clock_stability_timeout(self): self.case("clock-timeout")
    def test_command_inhibit_timeout(self): self.case("inhibit-timeout")
    def test_vendor_initial_clock_failure(self): self.case("hook-error")
    def test_card_select_error(self): self.case("select-error")
    def test_command_crc_error_recovers_without_fifo_read(self): self.case("command-error")
    def test_data_crc_error_is_not_success(self): self.case("data-error")
    def test_stalled_fifo_is_bounded(self): self.case("data-stall")


if __name__ == "__main__":
    unittest.main()
