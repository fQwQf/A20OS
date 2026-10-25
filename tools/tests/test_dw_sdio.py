"""Execute the production DW-MSHC driver against command/FIFO hardware.

The real card protocol, register sequencing, resource/IRQ decisions and class
callbacks run unchanged. Only MMIO, clock, allocation and kernel wait machinery
are host substitutes; no SD driver implementation is duplicated in the test.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    'core/defs.h': '#pragma once\n#include "core/types.h"\n',
    'core/cpu.h': '#pragma once\nstatic inline void cpu_relax(void) {}\n',
    'core/lock.h': '''#pragma once
#include "core/types.h"
typedef struct {int depth;} spinlock_t;
static inline void spin_init(spinlock_t *p) {p->depth=0;}
static inline uint64_t spin_lock_irqsave(spinlock_t *p) {p->depth++;return 0;}
static inline void spin_unlock_irqrestore(spinlock_t *p,uint64_t f) {(void)f;p->depth--;}
''',
    'core/sync.h': '''#pragma once
#include "core/types.h"
typedef struct {int depth;} mutex_t;
typedef struct {int unused;} wait_queue_t;
typedef struct {int unused;} wait_queue_entry_t;
typedef int proc_wait_token_t;
typedef struct {int unused;} proc_wake_q_t;
#define PROC_WAIT_UNINTERRUPTIBLE 0
#define PROC_WAKE_EVENT 1
static inline void mutex_init(mutex_t *m) {m->depth=0;}
static inline void mutex_lock(mutex_t *m) {m->depth++;}
static inline void mutex_unlock(mutex_t *m) {m->depth--;}
static inline void wait_queue_init(wait_queue_t *q) {(void)q;}
static inline int proc_park_prepare(int n,uint64_t t) {(void)n;(void)t;return 0;}
static inline void wait_queue_link(wait_queue_t *q,wait_queue_entry_t *e,int t,int n) {(void)q;(void)e;(void)t;(void)n;}
static inline void proc_park_commit(int t) {(void)t;}
static inline void wait_queue_unlink(wait_queue_t *q,wait_queue_entry_t *e) {(void)q;(void)e;}
static inline void proc_park_finish(int t) {(void)t;}
static inline void proc_wake_q_init(proc_wake_q_t *q) {(void)q;}
static inline void wait_queue_collect_all(wait_queue_t *q,int k,int r,proc_wake_q_t *w,void *v) {(void)q;(void)k;(void)r;(void)w;(void)v;}
static inline void proc_wake_q_flush(proc_wake_q_t *q) {(void)q;}
''',
    'proc/proc.h': '#pragma once\nstatic inline void *proc_current(void) {return 0;}\n',
    'core/klog.h': '#pragma once\n#define kinfo(...) ((void)0)\n',
    'core/timer.h': '#pragma once\n#include "core/types.h"\nuint64_t timer_get_ticks(void);\nuint64_t clock_ticks_per_sec(void);\n',
    'drivers/core/driver_hwapi.h': '''#pragma once
#include "core/types.h"
uint32_t readl(const volatile void *p);
void writel(uint32_t v,volatile void *p);
int request_irq(uint32_t irq,int (*fn)(int,void *),uint32_t flags,void *p);
void free_irq(uint32_t irq,void *p);
void mdelay(unsigned n);
''',
}
C_FIXTURE = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/block/dw_sdio.c"
#define BASE 0xffff0000fe2b0000ULL
struct hardware {
 uint32_t r[0x400/4],csd[4],last_cmd,last_arg;
 unsigned clocks,commands,reads,writes,status_reads,cmd13,delayed;
 int legacy,stuck_reset,stuck_clock,crc_error,fifo_stuck,write,low_voltage;
 unsigned clock_div[8];
};
static struct hardware hw[2];
static uint64_t ticks;
static unsigned freed,allocated,irq_claims,irq_frees;
static int with_irq, read_only, locked_card, nonerror_status;
struct action {void (*fn)(void *);void *data;};
static struct action actions[2];
bus_type_t platform_bus;
static unsigned slot(uintptr_t address) {
 assert(address>=BASE && address<BASE+0x20000);return (address-BASE)/0x10000;
}
uint64_t clock_ticks_per_sec(void) {return 1000;}
uint64_t timer_get_ticks(void) {return ticks++;}
void mdelay(unsigned n) {ticks+=n;}
static void response(struct hardware *h,uint32_t r) {h->r[SDIO_RESP0/4]=r;}
uint32_t readl(const volatile void *address) {
 uintptr_t a=(uintptr_t)address;struct hardware *h=&hw[slot(a)];unsigned off=(a-BASE)%0x10000;
 if(off==SDIO_STATUS) {
  h->status_reads++;
  if(h->fifo_stuck) return h->write?STATUS_FIFO_FULL:STATUS_FIFO_EMPTY;
  if(h->delayed) {h->delayed--;return h->write?STATUS_FIFO_FULL:STATUS_FIFO_EMPTY;}
  return (!h->write&&h->reads>=128)?STATUS_FIFO_EMPTY:0;
 }
 if(off==0x200||off==0x100) {
  assert(!h->write&&h->reads<128);
  uint32_t v=0xabc00000+h->reads++;
  if(h->reads==128) h->r[SDIO_RINTSTS/4]|=INT_DTO;
  return v;
 }
 return h->r[off/4];
}
void writel(uint32_t value,volatile void *address) {
 uintptr_t a=(uintptr_t)address;struct hardware *h=&hw[slot(a)];unsigned off=(a-BASE)%0x10000;
 if(off==SDIO_RINTSTS) {h->r[off/4]&=~value;return;}
 if(off==SDIO_CTRL) {h->r[off/4]=h->stuck_reset?value:value&~7U;return;}
 if(off==0x200||off==0x100) {
  assert(h->write&&h->writes<128);assert(value==0x12340000+h->writes);h->writes++;
  if(h->writes==128) h->r[SDIO_RINTSTS/4]|=INT_DTO;
  return;
 }
 h->r[off/4]=value;
 if(off!=SDIO_CMD)return;
 if(value&CMD_UPDATE_CLK) {
  h->clocks++;if(h->clocks<8)h->clock_div[h->clocks]=h->r[SDIO_CLKDIV/4];
  if(!h->stuck_clock)h->r[off/4]&=~CMD_START;
  /* No command-done on clock update, as on the real controller. */
  return;
 }
 h->r[off/4]&=~CMD_START;h->commands++;h->last_cmd=value;h->last_arg=h->r[SDIO_CMDARG/4];
 unsigned cmd=value&63;
 response(h,SD_R1_READY|(4<<9));
 switch(cmd) {
 case 0: assert(value&CMD_SEND_INIT);break;
 case 55: response(h,SD_R1_READY|(4<<9)|(1U<<5)|(locked_card?(1U<<25):0)|(nonerror_status?((1U<<13)|(1U<<14)):0));break;
 case 8: if(h->legacy) {h->r[SDIO_RINTSTS/4]|=INT_RTO;return;}response(h,0x1aa);break;
 case 41: assert(!(value&CMD_CHECK_RESP_CRC));response(h,0x80000000|(h->legacy?0:0x40000000)|(h->low_voltage?0x00008000:0x00ff8000));break;
 case 3: response(h,0x12340000);break;
 case 9: for(unsigned i=0;i<4;i++)h->r[(SDIO_RESP0+4*i)/4]=h->csd[i];break;
 case 13: h->cmd13++;break;
 case 17: case 24:
  assert(value&CMD_CHECK_RESP_CRC);assert(value&CMD_WAIT_PRVDATA);
  h->write=cmd==24;h->reads=h->writes=0;h->delayed=2;
  if(h->crc_error)h->r[SDIO_RINTSTS/4]|=INT_DCRC;
  break;
 }
 h->r[SDIO_RINTSTS/4]|=INT_CD;
}
void *kcalloc(size_t n,size_t size) {allocated++;return calloc(n,size);}
void kfree(void *p) {freed++;free(p);}
int fw_node_has_property(const fw_node_t *node,const char *name) {(void)node;assert(!strcmp(name,"read-only"));return read_only;}
int fw_node_read_u32(const fw_node_t *node,const char *name,uint32_t *out) {(void)node;(void)name;(void)out;return -ENOENT;}
const char *bootargs_get(void) {return "";}
resource_t *device_get_resource(device_t *d,enum resource_type type,int index) {(void)index;return type==RES_MMIO?d->res:NULL;}
void *device_map_mmio(device_t *d,int index) {(void)index;return (void *)(uintptr_t)(BASE+d->res->start*0x10000);}
int device_add_resource_action(device_t *d,void (*fn)(void *),void *data) {actions[d->res->start]=(struct action){fn,data};return 0;}
int platform_device_irq(device_t *d) {(void)d;return with_irq?130:-ENODEV;}
int request_irq(uint32_t irq,int (*fn)(int,void *),uint32_t flags,void *p) {assert(irq==130&&fn&&p&&!flags);irq_claims++;return 0;}
void free_irq(uint32_t irq,void *p) {assert(irq==130&&p);irq_frees++;}
static void bits(uint32_t csd[4],unsigned low,unsigned count,uint32_t value) {
 for(unsigned i=0;i<count;i++) if(value&(1U<<i))csd[(low+i)/32]|=1U<<((low+i)%32);
}
static void setup(unsigned i,int legacy) {
 struct hardware *h=&hw[i];h->legacy=legacy;
 h->r[SDIO_HCON/4]=1<<7;h->r[SDIO_VERID/4]=0x240a;
 if(legacy) {bits(h->csd,80,4,9);bits(h->csd,62,12,1023);bits(h->csd,47,3,5);}
 else {bits(h->csd,126,2,1);bits(h->csd,48,22,0x12345);}
}
static void cleanup(unsigned i,device_t *dev) {
 if(dev->drv_priv)assert(!sdio_remove(dev));
 assert(actions[i].fn);actions[i].fn(actions[i].data);actions[i].fn=NULL;
}
int main(int argc,char **argv) {
 assert(argc==2);const char *mode=argv[1];
 resource_t res[2]={{.type=RES_MMIO,.start=0,.end=0x3fff},{.type=RES_MMIO,.start=1,.end=0x4000}};
 dw_sdio_platform_data_t cfg={.input_hz=12000000,.max_hz=12000000,.fifo_depth=256};
 device_t dev[2]={{.name="sd0",.res=&res[0],.res_count=1,.plat_data=&cfg},{.name="sd1",.res=&res[1],.res_count=1,.plat_data=&cfg}};
 int legacy=!strcmp(mode,"legacy");setup(0,legacy);
 if(!strcmp(mode,"reset-timeout"))hw[0].stuck_reset=1;
 if(!strcmp(mode,"clock-timeout"))hw[0].stuck_clock=1;
 if(!strcmp(mode,"low-voltage"))hw[0].low_voltage=1;
 if(!strcmp(mode,"wrong-width"))hw[0].r[SDIO_HCON/4]=2<<7;
 if(!strcmp(mode,"missing-clock"))cfg.input_hz=0;
 if(!strcmp(mode,"irq"))with_irq=1;
 if(!strcmp(mode,"firmware-read-only"))read_only=1;
 if(!strcmp(mode,"locked-card"))locked_card=1;
 if(!strcmp(mode,"nonerror-status"))nonerror_status=1;
 if(!strcmp(mode,"permanent-wp"))hw[0].csd[0]|=1U<<13;
 if(!strcmp(mode,"temporary-wp"))hw[0].csd[0]|=1U<<12;
 int rc=sdio_probe(&dev[0]);
 if(!strcmp(mode,"missing-clock")) {assert(rc==-EINVAL&&!allocated);return 0;}
 if(!strcmp(mode,"reset-timeout")||!strcmp(mode,"clock-timeout")) {assert(rc==-ETIMEDOUT);cleanup(0,&dev[0]);return 0;}
 if(!strcmp(mode,"wrong-width")) {assert(rc==-EOPNOTSUPP);cleanup(0,&dev[0]);return 0;}
 if(!strcmp(mode,"low-voltage")) {assert(rc==-EIO);cleanup(0,&dev[0]);return 0;}
 if(!strcmp(mode,"locked-card")) {assert(rc==-EOPNOTSUPP);cleanup(0,&dev[0]);return 0;}
 assert(!rc);sdio_priv_t *p=dev[0].drv_priv;
 assert(p&&p->base==BASE&&p->ready&&p->input_hz==12000000);
 assert(hw[0].clocks==6&&hw[0].clock_div[2]==15&&hw[0].r[SDIO_CLKDIV/4]==0);
 assert(hw[0].r[SDIO_FIFOTH/4]==((2U<<28)|(127<<16)|128));
 assert(p->sectors==(legacy?131072ULL:((0x12345ULL+1)*1024)));
 assert(!!p->block_addressed==!legacy);
 if(!strcmp(mode,"multi-host")) {setup(1,0);assert(!sdio_probe(&dev[1]));assert(dev[0].drv_priv!=dev[1].drv_priv);assert(sdio_capacity(&dev[1])==p->sectors);cleanup(1,&dev[1]);assert(p->ready);}
 if(!strcmp(mode,"crc-error"))hw[0].crc_error=1;
 if(!strcmp(mode,"fifo-timeout"))hw[0].fifo_stuck=1;
 uint8_t bytes[514];memset(bytes,0,sizeof(bytes));rc=sdio_read(&dev[0],7,bytes+1,1);
 if(!strcmp(mode,"crc-error")) {assert(rc==-EIO&&!p->ready);cleanup(0,&dev[0]);return 0;}
 if(!strcmp(mode,"fifo-timeout")) {assert(rc==-ETIMEDOUT&&!p->ready);cleanup(0,&dev[0]);return 0;}
 assert(!rc&&hw[0].reads==128&&hw[0].status_reads>=130);
 assert(hw[0].last_arg==(legacy?7*512:7));
 for(unsigned i=0;i<128;i++) {uint32_t word;memcpy(&word,bytes+1+4*i,4);assert(word==0xabc00000+i);word=0x12340000+i;memcpy(bytes+1+4*i,&word,4);}
 if(strstr(mode,"-wp")||!strcmp(mode,"firmware-read-only")) {assert(p->write_protected&&sdio_write(&dev[0],8,bytes+1,1)==-EROFS&&p->ready&&!hw[0].writes);cleanup(0,&dev[0]);return 0;}
 unsigned status=hw[0].cmd13;assert(!sdio_write(&dev[0],8,bytes+1,1));assert(hw[0].writes==128&&hw[0].cmd13>status);
 assert(sdio_read(&dev[0],p->sectors,bytes,1)==-EINVAL&&p->ready);
 assert(sdio_read(&dev[0],0,bytes,0)==-EINVAL&&p->ready);
 cleanup(0,&dev[0]);assert(allocated==freed);assert(irq_claims==irq_frees);
 return 0;
}
'''


class DwSdioTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
    def test_production_host_protocol(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            for name, source in STUBS.items():
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source)
            source = work / 'fixture.c'
            source.write_text(C_FIXTURE)
            executable = work / 'fixture'
            command = ['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                       '-fsanitize=undefined', '-fno-sanitize-recover=all',
                       f'-I{work}', f'-I{ROOT}/kernel/include', f'-I{ROOT}/kernel',
                       str(source), '-o', str(executable)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for mode in ['sdhc', 'legacy', 'multi-host', 'irq', 'crc-error',
                         'fifo-timeout', 'reset-timeout', 'clock-timeout',
                         'low-voltage', 'wrong-width', 'missing-clock', 'permanent-wp',
                         'temporary-wp', 'firmware-read-only', 'nonerror-status', 'locked-card']:
                with self.subTest(mode=mode):
                    subprocess.run([str(executable), mode], check=True, timeout=5)

GLUE_FIXTURE = r'''
#include <assert.h>
#include <string.h>
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
static device_t canonical;
static unsigned puts, matched=1, writes;
static uint32_t cru[0x1000/4],grf[0x1000/4],pmugrf[0x1000/4],syscon[0x1000/4];
static uint32_t *register_at(uintptr_t address) {
 uintptr_t physical=address-0xffff000000000000ULL;
 if(physical>=0xfdd20000&&physical<0xfdd21000)return &cru[(physical-0xfdd20000)/4];
 if(physical>=0xfdc60000&&physical<0xfdc61000)return &grf[(physical-0xfdc60000)/4];
 if(physical>=0xfdc20000&&physical<0xfdc21000)return &pmugrf[(physical-0xfdc20000)/4];
 if(physical>=0x13020000&&physical<0x13021000)return &cru[(physical-0x13020000)/4];
 assert(physical>=0x13030000&&physical<0x13031000);return &syscon[(physical-0x13030000)/4];
}
uint32_t readl(const volatile void *p) {return *register_at((uintptr_t)p);}
void writel(uint32_t value,volatile void *p) {
 uint32_t *reg=register_at((uintptr_t)p);writes++;
#ifdef QUARTZ
 uint32_t mask=value>>16;*reg=(*reg&~mask)|(value&mask);
#else
 *reg=value;
#endif
}
void udelay(unsigned n) {assert(n==2);}
device_t *device_find_get_by_mmio(uint64_t p) {
#ifdef QUARTZ
 assert(p==0xfe2b0000);
#else
 assert(p==0x16020000);
#endif
 return &canonical;
}
void device_put(device_t *d) {assert(d==&canonical);puts++;}
int fw_node_match_id(const fw_node_t *n,const char *id) {(void)n;(void)id;return matched;}
void host_log(const char *fmt,...) {(void)fmt;}
#ifdef QUARTZ
#include "arch/aarch64/boards/quartz64-zero/storage.c"
#else
#include "arch/riscv64/boards/visionfive2/storage.c"
#endif
int main(void) {
 memset(cru,0xff,sizeof(cru));memset(grf,0xff,sizeof(grf));memset(pmugrf,0xff,sizeof(pmugrf));
#ifdef QUARTZ
 quartz64_zero_configure_storage();
 assert(cru[0x178/4]==0xfffff8ffU); /* Only the SDMMC0 mux changed. */
 assert(cru[0x33c/4]==0xfffffffcU&&cru[0x434/4]==0xffffffe7U);
 assert(grf[0x1c/4]==0xffff111fU&&grf[0x20/4]==0xfffff111U);
 assert((grf[0x8c/4]&0xfc00)==0x5400&&(grf[0x90/4]&0x3f)==0x15);
 assert((pmugrf[4/4]&15)==1&&(cru[0x580/4]&0x7fe)==2);
 const dw_sdio_platform_data_t *host=canonical.plat_data;
 assert(host&&host->input_hz==12000000&&host->max_hz==12000000&&host->fifo_depth==256);
 matched=0;unsigned old=writes;quartz64_zero_configure_storage();assert(writes==old&&puts==2);
#else
 cru[5]=1<<24;cru[7]=3; /* PLL2 / AXI_CFG0 divider. */
 syscon[0x2c/4]=(1<<15)|(1<<16)|(99<<17);syscon[0x34/4]=2;
 vf2_configure_storage();
 const dw_sdio_platform_data_t *host=canonical.plat_data;
 /* PLL2 1188 MHz -> AXI 396 MHz -> local div8 -> CIU 49.5 MHz. */
 assert(host&&host->input_hz==49500000&&host->fifo_depth==32);
 assert((cru[94]&15)==8&&(cru[94]&(1U<<31)));
 assert((cru[92]&(1U<<31))&&!(cru[0x300/4]&2));
 assert(pll2_rate()==1188000000);
 syscon[0x2c/4]=(100<<17);syscon[0x30/4]=0x800000; /* Fraction 100.5 */
 assert(pll2_rate()==1206000000);
 syscon[0x34/4]=0;assert(pll2_rate()==0);
 canonical.plat_data=NULL;unsigned old=writes;vf2_configure_storage();assert(!canonical.plat_data&&writes==old);
 cru[5]=0;cru[7]=1;vf2_configure_storage();assert(host->input_hz==24000000);
#endif
 return 0;
}
'''


class DwSdioBoardGlueTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
    def test_production_board_clock_and_pin_registers(self):
        for quartz in (True, False):
            with self.subTest(board='quartz' if quartz else 'vf2'), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                stubs = dict(STUBS)
                stubs['core/klog.h'] += '#define kwarn(...) ((void)0)\n'
                stubs['drivers/core/driver_hwapi.h'] += 'void udelay(unsigned n);\n'
                stubs['platform.h'] = '#define PAGE_OFFSET 0xffff000000000000ULL\n'
                for name, contents in stubs.items():
                    path = work / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(contents)
                source = work / 'glue.c'
                source.write_text(GLUE_FIXTURE)
                executable = work / 'glue'
                command = ['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                           '-fsanitize=undefined', '-fno-sanitize-recover=all',
                           f'-I{work}', f'-I{ROOT}/kernel/include', f'-I{ROOT}/kernel',
                           str(source), '-o', str(executable)]
                if quartz:
                    command.insert(1, '-DQUARTZ')
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(executable)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
