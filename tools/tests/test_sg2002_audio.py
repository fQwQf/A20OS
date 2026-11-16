"""Execute SG2002 PCM, 64-byte LLI and non-coherent ownership contracts.

Models register/IRQ sequencing; does not measure physical analog output.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_sunxi_audio import STUB
ROOT=Path(__file__).resolve().parents[2]
HARNESS=r'''
#include "test_stub.h"
uint64_t readq(const volatile void *p);
void writeq(uint64_t v,volatile void *p);
#include "PRODUCTION"
static uint32_t i2s[64],dac[64];static uint64_t dma[512];
static unsigned allocations,frees,syncs,prepare_count,tail_us,alloc_calls,unmaps;
static int dead_regs,stuck_signal,stuck_enable,stuck_i2s,fail_irq,fail_action,fail_map,fail_alloc,fail_probe_abort;
static int stop_wait,busy_stops;
static device_t *active_device;
static int prepare_result,stuck_abort,progress=1,identity=1;
static unsigned links;
static device_t providers[7];
static uint64_t provider_pa[]={0x0300a000,0x04330000,0x03000000,0x03002000,0x03003000,0x04108000,0x03001000};
int fw_node_match_id(const int*n,const char*id){(void)n;assert(!strcmp(id,"cvitek,cv1835-i2s"));return identity;}
device_t *device_find_get_by_mmio(uint64_t pa){for(unsigned i=0;i<7;i++)if(provider_pa[i]==pa)return &providers[i];return NULL;}
resource_t *device_get_resource(device_t*d,int t,int index){assert(t==RES_MMIO&&!index);return &d->resource;}
int device_add_dependency(device_t*c,device_t*p){assert(c&&p>=providers&&p<providers+7);links++;return 0;}
void device_put(device_t*d){assert(d>=providers&&d<providers+7);}
static uint64_t ticks;
static void *payload,*descriptors;
static int(*handler)(int,void*);static void *handler_arg;
static int platform_prepare(void){prepare_count++;return prepare_result;}
uint32_t readl(const volatile void*p){return *(const volatile uint32_t*)p;}
void writel(uint32_t v,volatile void*p){if(stuck_i2s&&p==&i2s[0x18/4]&&!v)return;*(volatile uint32_t*)p=v;}
uint64_t readq(const volatile void*p){if(dead_regs)return ~0ULL;return *(const volatile uint64_t*)p;}
void writeq(uint64_t v,volatile void*p){
 if(!v&&((stuck_signal&&p==&dma[SG_CH_INT_SIGNAL/8])||(stuck_enable&&p==&dma[SG_CH_INT_EN/8])))return;
 if(p==&dma[SG_DMA_ENABLE/8]){
  if(v&(SG_DMA_MASK<<40)){if(!stuck_abort)dma[SG_DMA_ENABLE/8]&=~SG_DMA_MASK;return;}
  if(v&(SG_DMA_MASK<<8))dma[SG_DMA_ENABLE/8]=(dma[SG_DMA_ENABLE/8]&~SG_DMA_MASK)|(v&SG_DMA_MASK);
  return;
 }
 if(p==&dma[SG_CH_INT_CLEAR/8]){dma[SG_CH_INT_STATUS/8]&=~v;return;}
 *(volatile uint64_t*)p=v;
}
void udelay(unsigned us){ticks+=us;if(us==2000)tail_us+=us;}
uint64_t clock_get_ticks(void){return ticks;}
uint64_t clock_ticks_per_sec(void){return 1000000;}
void mdelay(unsigned ms){
 ticks+=ms*1000;
 if(stop_wait){stop_wait=0;assert(active_device&&((struct sg2002_audio*)active_device->drv_priv)->writer.value);assert(active_device->execution(active_device->execution_ctx)==-EBUSY&&!frees&&!unmaps&&handler);busy_stops++;}
 struct sg2002_audio*s=handler_arg;
 if(progress&&s&&s->ring.running){
  ticks+=20000;
  dma[SG_CH_SAR/8]=s->data_pa+((s->ring.consumer+1)%PCM_PERIODS)*PCM_PERIOD_BYTES;
  dma[SG_CH_INT_STATUS/8]=1;assert(handler(29,s)==1);
 }
}
void *arch_device_map_mmio(uint64_t pa,size_t n,uint32_t flags){
 (void)n;(void)flags;if((fail_map==1&&pa==0x04130000)||(fail_map==2&&pa==0x0300a000)||(fail_map==3&&pa==0x04330000))return NULL;if(pa==0x04130000)return i2s;if(pa==0x0300a000)return dac;if(pa==0x04330000)return dma;abort();
}
void arch_device_unmap_mmio(void*p,size_t n){assert(p&&n&&!handler);unmaps++;}
void *dma_alloc_coherent_aligned(device_t*d,size_t n,size_t a,uint64_t*pa){
 assert(d->managed&&d->execution&&a==64);if(++alloc_calls==(unsigned)fail_alloc)return NULL;void*p=aligned_alloc(64,n);assert(p);memset(p,0,n);
 if(alloc_calls==1){payload=p;*pa=0x81000000;}else{descriptors=p;*pa=0x82000000;}
 allocations++;return p;
}
void dma_free_coherent_aligned(void*p,size_t n,uint64_t pa){
 assert(!(dma[SG_DMA_ENABLE/8]&SG_DMA_MASK)&&!handler);assert(n&&pa);frees++;free(p);
}
void dma_sync_for_device(void*p,size_t n){assert(p&&n);syncs++;}
void dma_sync_for_cpu(void*p,size_t n){assert(p&&n);}
int dma_set_mask(device_t*d,uint64_t m){(void)d;return m==DMA_MASK_32BIT?0:-1;}
int request_irq(uint32_t n,int(*fn)(int,void*),unsigned long flags,void*p){assert(n==29&&flags==IRQF_SHARED);if(fail_irq){if(fail_probe_abort){stuck_abort=1;dma[SG_DMA_ENABLE/8]|=SG_DMA_MASK;}return -EBUSY;}handler=fn;handler_arg=p;return 0;}
void free_irq(uint32_t n,void*p){assert(n==29&&p==handler_arg&&!(dma[SG_DMA_ENABLE/8]&SG_DMA_MASK));assert(!dma[SG_CH_INT_SIGNAL/8]&&!dma[SG_CH_INT_EN/8]);handler_arg=NULL;handler=NULL;}
void irq_enable(uint32_t n){assert(n==29);}
int device_probe_use_remove(device_t*d){d->managed=1;return 0;}
int device_add_execution_action(device_t*d,int(*stop)(void*),void(*release)(void*),void*p){assert(d->managed&&d->drv_priv==p&&!release);if(fail_action)return -ENOMEM;d->execution=stop;d->execution_ctx=p;return 0;}
/* Core action/class-drain ordering is represented by this adapter; the real
 * provider functions, including failed-probe cleanup, execute unchanged. */
static int unbind(device_t*d){if(d->drv_priv&&d->execution){int r=d->execution(d->execution_ctx);if(r)return r;}int r=remove_device(d);if(!r){d->execution=NULL;d->execution_ctx=NULL;links=0;}return r;}
static int bind(device_t*d){assert(!d->drv_priv);int r=probe(d);if(r&&d->drv_priv)(void)unbind(d);if(r&&!d->drv_priv)links=0;return r;}
static void reset_model(void){assert(!handler);memset(i2s,0,sizeof(i2s));memset(dac,0,sizeof(dac));memset(dma,0,sizeof(dma));allocations=frees=syncs=prepare_count=tail_us=alloc_calls=unmaps=links=0;prepare_result=stuck_abort=dead_regs=stuck_signal=stuck_enable=stuck_i2s=fail_irq=fail_action=fail_map=fail_alloc=fail_probe_abort=stop_wait=busy_stops=0;progress=identity=1;active_device=NULL;ticks=0;payload=descriptors=NULL;}
static void lifecycle_failures(device_t *dev){
 uint8_t data[PCM_BYTES*2]={0};
 for(unsigned fault=0;fault<5;fault++){
  reset_model();assert(!bind(dev));struct sg2002_audio *saved=dev->drv_priv;
  assert(audio_write(dev,data,8192)==8192);dma[SG_DMA_ENABLE/8]|=4; /* peer channel remains live */
  if(fault==0)dead_regs=1;
  if(fault==1)stuck_signal=1;
  if(fault==2)stuck_enable=1;
  if(fault==3)stuck_i2s=1;
  if(fault==4)stuck_abort=1;
  assert(unbind(dev)<0&&dev->drv_priv==saved&&!frees&&!unmaps&&handler&&links==7);
  assert(audio_write(dev,data,4)==-ENODEV&&audio_stop(dev)==-ENODEV&&audio_poll(dev,POLLOUT)==POLLERR);
  if(!dead_regs){unsigned consumer=saved->ring.consumer;dma[SG_CH_INT_STATUS/8]=1;assert(handler(29,saved)==1&&saved->ring.consumer==consumer);}
  dead_regs=stuck_signal=stuck_enable=stuck_i2s=stuck_abort=0;
  assert(!unbind(dev)&&!dev->drv_priv&&frees==2&&unmaps==3&&dma[SG_DMA_ENABLE/8]==4);
 }
 for(unsigned drain=0;drain<2;drain++){
  reset_model();assert(!bind(dev));active_device=dev;progress=0;stop_wait=1;
  if(drain){assert(audio_write(dev,data,8192)==8192);assert(audio_drain(dev)==-ENODEV);}
  else assert(audio_write(dev,data,sizeof(data))==PCM_BYTES);
  assert(busy_stops==1&&!frees&&!unmaps&&handler);assert(!unbind(dev)&&frees==2);
 }
 reset_model();assert(!bind(dev));struct sg2002_audio*s=dev->drv_priv;uint64_t f=spin_lock_irqsave(&s->lock);
 assert(dev->execution(s)==-EBUSY&&!frees&&!unmaps&&handler);spin_unlock_irqrestore(&s->lock,f);assert(!unbind(dev)&&frees==2);
 for(unsigned allocation=1;allocation<3;allocation++){reset_model();fail_alloc=allocation;assert(bind(dev)==-ENOMEM&&!dev->drv_priv&&allocations==frees);}
 for(unsigned mapping=1;mapping<4;mapping++){reset_model();fail_map=mapping;assert(bind(dev)==-ENXIO&&!dev->drv_priv&&!allocations&&unmaps==2);}
 reset_model();fail_action=1;assert(bind(dev)==-ENOMEM&&!dev->drv_priv&&!prepare_count&&!unmaps);
 reset_model();fail_irq=fail_probe_abort=1;assert(bind(dev)==-EBUSY&&dev->drv_priv&&!frees&&!unmaps&&links==7&&!handler);stuck_abort=0;assert(!unbind(dev)&&frees==2&&unmaps==3);
 reset_model();dma[SG_DMA_ENABLE/8]=SG_DMA_MASK|4;i2s[0x18/4]=1;dac[0]=3;
 assert(bind(dev)==-EBUSY&&!dev->drv_priv&&!allocations&&dma[SG_DMA_ENABLE/8]==(SG_DMA_MASK|4)&&i2s[0x18/4]==1&&dac[0]==3);
}
static sg2002_audio_platform_t platform={.magic=SG2002_AUDIO_PLATFORM_MAGIC,.dma_irq=29,.i2s_pa=0x04130000,.dac_pa=0x0300a000,.dma_pa=0x04330000,.prepare=platform_prepare};
int main(void){
 (void)registered_driver;
 device_t dev={.plat_data=&platform,.resource={0x04130000,0x04131fff}};
 for(unsigned i=0;i<7;i++){platform.provider_pa[i]=provider_pa[i];platform.provider_bytes[i]=0x100;providers[i].resource=(resource_t){provider_pa[i],provider_pa[i]+0xfff};}
 identity=-EINVAL;assert(bind(&dev)==-ENODEV&&!links&&!allocations);identity=1;
 providers[1].drv=(void*)1;assert(bind(&dev)==-EBUSY&&!allocations);providers[1].drv=NULL;links=0;
 prepare_result=-EOPNOTSUPP;assert(bind(&dev)==-EOPNOTSUPP&&!allocations&&!handler);
 assert(!links); /* Completed failed-probe cleanup released dependencies. */
 prepare_result=0;assert(!bind(&dev)&&allocations==2&&prepare_count==2&&links==7);
 struct sg2002_audio*s=dev.drv_priv;assert(!audio_drain(&dev));
 assert(i2s[4/4]==0x000f200f&&i2s[8/4]==0x000f0f01&&i2s[0x10/4]==2);
 assert(i2s[0x64/4]==0x00100002);
 struct sg2002_lli*lli=descriptors;
 for(unsigned i=0;i<SG_LLI_COUNT;i++){
  assert(lli[i].source==0x81000000ULL+i*64&&lli[i].destination==0x041300c0);
  assert(lli[i].block_ts==15&&lli[i].next==0x82000000ULL+((i+1)%SG_LLI_COUNT)*64);
  assert(!!(lli[i].control&SG_LLI_IOC)==((i+1)%SG_MICROS_PER_PERIOD==0));
  assert(lli[i].control&SG_LLI_VALID);
 }
 a20_audio_format_t format={44100,2,A20_AUDIO_FORMAT_S16_LE};assert(audio_format(&dev,&format)==-EINVAL);
 format.rate=48000;assert(!audio_format(&dev,&format));
 uint8_t data[PCM_BYTES*2];memset(data,0x5a,sizeof(data));
 assert(audio_format(&dev,NULL)==-EINVAL);
 assert(audio_write(&dev,NULL,4)==-EINVAL&&audio_write(&dev,data,3)==-EINVAL);
 assert(audio_write(&dev,data,0x80000000ULL)==-EOVERFLOW);
 assert(audio_write(&dev,data,132)==132&&s->partial==132&&!s->ring.running);
 assert(!audio_drain(&dev)&&tail_us==2000&&!s->ring.count&&!s->ring.running);
 assert(audio_write(&dev,data,sizeof(data))==(int)sizeof(data));assert(s->ring.running&&syncs);
 assert(!audio_drain(&dev)&&!s->ring.error&&tail_us==4000);
 for(unsigned i=0;i<PCM_BYTES;i++)assert(!((uint8_t*)payload)[i]);
 assert(audio_write(&dev,data,PCM_PERIOD_BYTES*4)==PCM_PERIOD_BYTES*4);
 dma[SG_CH_SAR/8]=s->data_pa+PCM_BYTES;
 assert(audio_write(&dev,data,4)==4&&!s->ring.error);assert(!audio_stop(&dev));
 assert(audio_write(&dev,data,PCM_PERIOD_BYTES*2)==PCM_PERIOD_BYTES*2);
 dma[SG_CH_INT_STATUS/8]=1ULL<<9;assert(handler(29,s)==1&&s->ring.error==-EIO&&!i2s[0x18/4]);
 assert(audio_poll(&dev,POLLOUT)==POLLERR);assert(!audio_stop(&dev));
 assert(audio_write(&dev,data,PCM_PERIOD_BYTES*2)==PCM_PERIOD_BYTES*2);
 dma[SG_CH_SAR/8]=s->data_pa+4*PCM_PERIOD_BYTES;dma[SG_CH_INT_STATUS/8]=1;
 assert(handler(29,s)==1&&s->ring.error==-EPIPE);assert(!audio_stop(&dev));
 assert(audio_write(&dev,data,PCM_PERIOD_BYTES*2)==PCM_PERIOD_BYTES*2);
 progress=0;assert(audio_write(&dev,data,PCM_BYTES*2)>0);assert(s->ring.error==-ETIMEDOUT);assert(!audio_stop(&dev));
 progress=1;assert(audio_write(&dev,data,PCM_PERIOD_BYTES*2)==PCM_PERIOD_BYTES*2);
 s->ring.consumer=7;s->ring.count=1;s->ring.ready[7]=1;s->ring.draining=1;
 dma[SG_CH_SAR/8]=s->data_pa+PCM_BYTES;dma[SG_CH_INT_STATUS/8]=1;assert(handler(29,s)==1&&!s->ring.error&&!s->ring.count);
 stuck_abort=1;assert(unbind(&dev)==-ETIMEDOUT&&!frees&&handler_arg==s&&dev.drv_priv==s&&handler&&!unmaps);
 stuck_abort=0;assert(!unbind(&dev)&&!dev.drv_priv&&frees==2&&unmaps==3);
 assert(!unbind(&dev));lifecycle_failures(&dev);
 puts("SG2002 PCM/DMA model: PASS");return 0;
}
'''
class Sg2002AudioModel(unittest.TestCase):
    def test_production_pcm_dma_and_quarantine(self):
        with tempfile.TemporaryDirectory() as directory:
            stage=Path(directory)
            stub=STUB.replace('void *plat_data,*drv_priv;', 'void *plat_data,*drv_priv,*drv;int user_owned;').replace('#define TEST_SUNXI_STUB','#define TEST_SUNXI_STUB\n#define DMA_MASK_32BIT 0xffffffffULL')
            (stage/'test_stub.h').write_text(stub)
            headers=('drivers/audio/audio_core.h','drivers/core/driver_register.h','drivers/core/driver_hwapi.h','drivers/core/driver_core.h','drivers/bus/platform_bus.h','core/types.h','core/errno.h','core/lock.h','core/sync.h','core/string.h','core/stdio.h','core/poll.h','mm/slab.h','firmware/device.h')
            for h in headers:
                p=stage/h;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "test_stub.h"\n')
            (stage/'model.c').write_text(HARNESS.replace('PRODUCTION',str(ROOT/'kernel/drivers/audio/sg2002_audio.c')))
            build=subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-fno-sanitize-recover=all','-I',str(stage),'-I',str(ROOT/'kernel/include'),'-I',str(ROOT/'kernel'),str(stage/'model.c'),'-o',str(stage/'model')],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            result=subprocess.run([str(stage/'model')],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('SG2002 PCM/DMA model: PASS',result.stdout)

class Sg2002AudioPlatformModel(unittest.TestCase):
    def test_production_clock_policy_and_unprepared_irq_provider(self):
        source=r'''
#include "test_stub.h"
typedef struct {fw_node_t hardware_root;} boot_context_t;
const boot_context_t *boot_context_get(void);
int fw_node_match_id(const fw_node_t*,const char*);
int firmware_device_irq(device_t*,size_t);
int riscv_sg2002_dma_cache_prepare(void);
uint64_t readq(const volatile void*);
#include "PRODUCTION"
static boot_context_t context={{1}};
static device_t devices[8];
static uint64_t starts[]={0x04130000,0x0300a000,0x04330000,0x03000000,0x03002000,0x03003000,0x04108000,0x03001000};
static uint8_t memory[8][4096];
static unsigned writes,links,irq_queries,cache_calls;
static int cache_result;
const boot_context_t *boot_context_get(void){return &context;}
int fw_node_match_id(const fw_node_t*n,const char*id){
 if(n->id==1)return !strcmp(id,"sophgo,sg2002")||!strcmp(id,"sipeed,licheerv-nano");
 if(n->id==2)return !strcmp(id,"cvitek,cv1835-i2s");
 if(n->id==3)return !strcmp(id,"cvitek,cv182xadac");
 if(n->id==4)return !strcmp(id,"snps,dmac-bm");
 return 0;
}
device_t *device_find_get_by_mmio(uint64_t pa){for(unsigned i=0;i<8;i++)if(starts[i]==pa)return &devices[i];return NULL;}
resource_t *device_get_resource(device_t*d,int type,int index){assert(type==RES_MMIO&&!index);return &d->resource;}
int firmware_device_irq(device_t*d,size_t index){assert(d==&devices[2]&&!index);irq_queries++;return 29;}
int device_add_dependency(device_t*c,device_t*p){assert(c==&devices[0]&&p!=c);links++;return 0;}
void device_put(device_t*d){assert(d>=devices&&d<devices+8);}
int riscv_sg2002_dma_cache_prepare(void){cache_calls++;return cache_result;}
void *arch_device_map_mmio(uint64_t pa,size_t n,uint32_t flags){(void)flags;assert(n<=4096);for(unsigned i=0;i<8;i++)if(starts[i]==pa)return memory[i];return NULL;}
uint32_t readl(const volatile void*p){return *(const volatile uint32_t*)p;}
uint64_t readq(const volatile void*p){return *(const volatile uint64_t*)p;}
void writel(uint32_t v,volatile void*p){writes++;*(volatile uint32_t*)p=v;}
void udelay(unsigned us){assert(us==10);}
static void reg(unsigned device,unsigned offset,uint32_t value){memcpy(memory[device]+offset,&value,4);}
int main(void){
 (void)platform_bus;
 for(unsigned i=0;i<8;i++){devices[i].resource=(resource_t){starts[i],starts[i]+4095};devices[i].firmware_node.id=i+2;}
 devices[2].drv=(void*)1;sg2002_audio_configure();assert(!devices[0].plat_data);devices[2].drv=NULL;
 sg2002_audio_configure();assert(devices[0].plat_data==&platform&&platform.dma_irq==29&&irq_queries==1&&links==0);
 cache_result=-EOPNOTSUPP;assert(platform.prepare()==-EOPNOTSUPP&&cache_calls==1&&!writes);
 cache_result=0;reg(0,0x18,1);assert(platform.prepare()==-EBUSY&&!writes);reg(0,0x18,0);
 reg(1,0,3);assert(platform.prepare()==-EBUSY&&!writes);reg(1,0,0);
 reg(7,0x12c,3);assert(platform.prepare()==-EOPNOTSUPP&&!writes);reg(7,0x12c,0);
 assert(platform.prepare()==-EOPNOTSUPP&&!writes); /* uninitialized clock */
 reg(4,0x808,0x05488101);reg(4,0x854,614400000);reg(4,0x840,1);reg(4,0x80c,0x00128201);
 reg(4,0x04,0x100);reg(4,0x30,0xffffffff);reg(3,0x158,0x123456);reg(3,0x298,0x155);
 reg(4,0x138,0x00010109); /* C906B clock must remain unchanged */
 reg(5,8,0xffffffff);
 assert(!platform.prepare());assert(readl(memory[4]+0xa4)==0x00120008);
 assert(readl(memory[4]+0x138)==0x00010109&&readl(memory[3]+0x158)==0x87123456);
 assert(readl(memory[3]+0x298)==(0x155|(1<<17)|(1<<18)));
 assert(readl(memory[5]+8)==0xffffffff);
 assert((readl(memory[4]+4)&((1<<1)|(1<<5)|(1<<27)))==((1<<1)|(1<<5)|(1<<27)));
 assert((readl(memory[4]+0x30)&(1<<14))==0);
 reg(4,0x80c,0x00208201);assert(!platform.prepare()&&readl(memory[4]+0xa4)==0x00200008);
 unsigned before=writes;reg(2,0x18,1<<7);assert(platform.prepare()==-EBUSY&&writes==before);
 return 0;
}
'''.replace('PRODUCTION',str(ROOT/'kernel/arch/riscv64/platform/sg2002_audio.c'))
        with tempfile.TemporaryDirectory() as directory:
            stage=Path(directory)
            stub=STUB.replace('void *plat_data,*drv_priv;', 'void *plat_data,*drv_priv,*drv;int user_owned;').replace('typedef struct {uint64_t start,end;} resource_t;',
                              'typedef struct {uint64_t start,end;} resource_t;\ntypedef struct {int id;} fw_node_t;')
            stub=stub.replace('int firmware_node;', '').replace('const int *n,const char *id', 'const fw_node_t *n,const char *id')
            stub=stub.replace('void *plat_data,*drv_priv,*drv;int user_owned;resource_t resource;',
                              'void *plat_data,*drv_priv,*drv;int user_owned;fw_node_t firmware_node;resource_t resource;')
            (stage/'test_stub.h').write_text(stub)
            headers=('sg2002_audio.h','dma_cache.h','boot/context.h','drivers/core/driver_core.h','drivers/core/driver_hwapi.h','firmware/device.h','core/errno.h','core/types.h')
            for h in headers:
                p=stage/h;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "test_stub.h"\n')
            (stage/'model.c').write_text(source)
            build=subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(stage),'-I',str(ROOT/'kernel/include'),str(stage/'model.c'),'-o',str(stage/'model')],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            result=subprocess.run([str(stage/'model')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)

if __name__=='__main__':unittest.main()
