"""Production sunxi PCM/DMA and A64/V3s electrical preparation models.

The model validates register protocol and ownership, not analog waveform,
clock jitter, interrupt latency or real DMA cache coherence on hardware.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
STUB = r'''
#ifndef TEST_SUNXI_STUB
#define TEST_SUNXI_STUB
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#define EPROBE_DEFER 517
#define BIT(n) (1UL<<(n))
#define IRQF_SHARED 1
#define RES_MMIO 1
#define DEV_CLASS_AUDIO 4
#define POLLOUT 4
#define POLLERR 8
typedef struct {unsigned value;} spinlock_t;
typedef struct {unsigned value;} mutex_t;
static inline void spin_init(spinlock_t *p){p->value=0;}
static inline uint64_t spin_lock_irqsave(spinlock_t *p){assert(!p->value);p->value=1;return 0;}
static inline void spin_unlock_irqrestore(spinlock_t *p,uint64_t f){(void)f;assert(p->value);p->value=0;}
static inline void mutex_init(mutex_t *p){p->value=0;}
static inline void mutex_lock(mutex_t *p){assert(!p->value);p->value=1;}
static inline void mutex_unlock(mutex_t *p){assert(p->value);p->value=0;}
typedef struct {uint64_t start,end;} resource_t;
typedef struct device {void *plat_data,*drv_priv;resource_t resource;int irq;int identity;int firmware_node;} device_t;
typedef struct {const char *id;} firmware_device_id_t;
typedef struct {const char *name;void *bus;const firmware_device_id_t *firmware_ids;int(*probe)(device_t*);int(*remove)(device_t*);int class_type;const void *class_ops;int irq_optional;} driver_t;
static int platform_bus;
#define DRIVER_REGISTER(d) static const void *registered_driver=&d
#include "uapi/a20/audio.h"
typedef struct audio_dev_ops {a20_audio_caps_t caps;a20_audio_format_t pcm_format;int(*read)(device_t*,void*,size_t);int(*write)(device_t*,const void*,size_t);int(*set_format)(device_t*,const a20_audio_format_t*);int(*tone)(device_t*,const a20_audio_tone_t*);int(*stop)(device_t*);int(*drain)(device_t*);int(*poll)(device_t*,short);int(*ioctl)(device_t*,unsigned long,void*);int(*close)(device_t*);} audio_dev_ops_t;
static inline void *kcalloc(size_t n,size_t s){return calloc(n,s);}
static inline void kfree(void *p){free(p);}
uint32_t readl(const volatile void *p);
void writel(uint32_t v,volatile void *p);
void udelay(unsigned us);
void mdelay(unsigned ms);
uint64_t clock_get_ticks(void);
uint64_t clock_ticks_per_sec(void);
void *arch_device_map_mmio(uint64_t pa,size_t n,uint32_t f);
void arch_device_unmap_mmio(void *p,size_t n);
void *dma_alloc_coherent_aligned(device_t *d,size_t n,size_t a,uint64_t *pa);
void dma_free_coherent_aligned(void *p,size_t n,uint64_t pa);
void dma_sync_for_device(void *p,size_t n);
int dma_set_mask(device_t *d,uint64_t m);
int request_irq(uint32_t n,int(*fn)(int,void*),unsigned long f,void *p);
void free_irq(uint32_t n,void *p);
void irq_enable(uint32_t n);
device_t *device_find_get_by_mmio(uint64_t pa);
int device_add_dependency(device_t *a,device_t *b);
void device_put(device_t *d);
resource_t *device_get_resource(device_t *d,int t,int index);
int platform_device_irq(device_t *d);
int firmware_device_irq(device_t *d,size_t index);
int fw_node_match_id(const int *n,const char *id);
#endif
'''
HARNESS = r'''
#include "test_stub.h"
#include "drivers/audio/sunxi_audio.h"
#include "PRODUCTION"
static uint32_t dai_mmio[128],digital_mmio[384],analog_port,dma_mmio[1024];
static uint8_t analog_regs[32];
static unsigned analog_selected;
static uint64_t ticks,last_dma;
static int (*handler)(int,void*);
static void *handler_arg;
static int dma_frees,stuck_stop,power_on,power_off,variant,fail_power,fail_irq;
static device_t providers[5];
static const uint64_t provider_pas[]={0x400000,0x300000,0x200000,0x500000,0x600000};
static int provider_refs[5],missing_provider,dependency_fail,prepare_calls;
static unsigned link_mask;
static uint8_t *dma_data;
static struct sunxi_lli *dma_lli;
uint32_t readl(const volatile void *p){
 if((variant==SUNXI_AUDIO_A64&&p==&dai_mmio[0x18/4]))return 128U<<16;
 if((variant==SUNXI_AUDIO_V3S&&p==&dai_mmio[0x08/4]))return (128U<<8)|(dai_mmio[0x08/4]&6);

 if(p==&analog_port)return (analog_port&~255U)|analog_regs[analog_selected];
 return *(const volatile uint32_t*)p;
}
void writel(uint32_t v,volatile void *p){
 if((variant==SUNXI_AUDIO_A64&&p==&dai_mmio[0x0c/4])||(variant==SUNXI_AUDIO_V3S&&p==&dai_mmio[0x08/4])){*(volatile uint32_t*)p&=~v;return;}
 if(p==&analog_port){analog_port=v;analog_selected=(v>>16)&31;if(v&BIT(24))analog_regs[analog_selected]=(v>>8)&255;return;}
 if(p==&dma_mmio[0x10/4]){dma_mmio[0x10/4]&=~v;return;}
 if(p==&dma_mmio[DMA_EN/4]){
  if(!v&&stuck_stop)return;
  dma_mmio[DMA_EN/4]=v;
  if(v){uint32_t at=dma_mmio[DMA_LLI/4];unsigned i=(at-0x45000000U)/sizeof(*dma_lli);dma_mmio[DMA_SRC/4]=dma_lli[i].source;last_dma=ticks;}
  return;
 }
 *(volatile uint32_t*)p=v;
}
uint64_t clock_get_ticks(void){return ticks;}
uint64_t clock_ticks_per_sec(void){return 1000000;}
void udelay(unsigned us){ticks+=us;}
void mdelay(unsigned ms){
 for(unsigned j=0;j<ms;j++){
  ticks+=1000;
  if(dma_mmio[DMA_EN/4] && handler && ticks-last_dma>=21334){
   unsigned next=((dma_mmio[DMA_SRC/4]-0x44000000U)/4096+1)%PCM_PERIODS;
   dma_mmio[DMA_SRC/4]=0x44000000U+next*4096;
   dma_mmio[0x10/4]|=DMA_IRQ_PACKAGE;last_dma=ticks;handler(82,handler_arg);
  }
 }
}
void *arch_device_map_mmio(uint64_t pa,size_t n,uint32_t f){(void)n;(void)f;
 if(pa==0x100000)return dai_mmio;if(pa==0x200000)return digital_mmio;
 if(pa==0x300000)return &analog_port;if(pa==0x400000)return dma_mmio;return NULL;}
void arch_device_unmap_mmio(void *p,size_t n){(void)p;(void)n;}
void *dma_alloc_coherent_aligned(device_t *d,size_t n,size_t a,uint64_t *pa){(void)d;void *p=aligned_alloc(a,n);assert(p);if(n==4096){*pa=0x45000000;dma_lli=p;}else{*pa=0x44000000;dma_data=p;}return p;}
void dma_free_coherent_aligned(void *p,size_t n,uint64_t pa){(void)n;(void)pa;dma_frees++;free(p);}
void dma_sync_for_device(void *p,size_t n){(void)p;(void)n;}
int dma_set_mask(device_t *d,uint64_t mask){(void)d;assert(mask==0xffffffffULL);return 0;}
int request_irq(uint32_t n,int(*fn)(int,void*),unsigned long f,void *p){assert(n==82&&f==IRQF_SHARED);if(fail_irq)return -EBUSY;handler=fn;handler_arg=p;return 0;}
void free_irq(uint32_t n,void *p){assert(n==82&&p==handler_arg);handler=NULL;handler_arg=NULL;}
void irq_enable(uint32_t n){assert(n==82);}
device_t *device_find_get_by_mmio(uint64_t pa){for(unsigned i=0;i<5;i++)if(pa==provider_pas[i]&&missing_provider!=(int)i+1){provider_refs[i]++;return &providers[i];}return NULL;}
int device_add_dependency(device_t *a,device_t *b){assert(a&&b);unsigned i=(unsigned)(b-providers);assert(i<5);if(dependency_fail==(int)i+1)return -ENOMEM;if(!(link_mask&BIT(i))){link_mask|=BIT(i);provider_refs[i]++;}return 0;}
void device_put(device_t *d){unsigned i=(unsigned)(d-providers);assert(i<5&&provider_refs[i]>0);provider_refs[i]--;}
static void release_model_dependencies(void){for(unsigned i=0;i<5;i++)if(link_mask&BIT(i))device_put(&providers[i]);link_mask=0;}
/* Mirror the core's probe-attempt ownership boundary; the production probe
 * itself is compiled below and must reconstruct links on every retry. */
static int probe_attempt(device_t*d){release_model_dependencies();int r=audio_probe(d);if(r&&r!=-EPROBE_DEFER)release_model_dependencies();return r;}
static void remove_attempt(device_t*d){assert(audio_remove(d)==0);release_model_dependencies();for(unsigned i=0;i<5;i++)assert(provider_refs[i]==0);}
resource_t *device_get_resource(device_t *d,int t,int index){assert(t==RES_MMIO&&index==0);return &d->resource;}
static int prepare(void){prepare_calls++;return 0;}
static int power(int on){if(on){power_on++;if(fail_power)return -EIO;}else power_off++;return 0;}
static void clear_model(void){memset(dai_mmio,0,sizeof(dai_mmio));memset(digital_mmio,0,sizeof(digital_mmio));memset(dma_mmio,0,sizeof(dma_mmio));memset(analog_regs,0,sizeof(analog_regs));ticks=0;last_dma=0;dma_frees=0;stuck_stop=0;power_on=power_off=fail_power=fail_irq=0;handler=NULL;memset(provider_refs,0,sizeof(provider_refs));link_mask=missing_provider=dependency_fail=prepare_calls=0;}
static sunxi_audio_platform_t platform(unsigned variant){return (sunxi_audio_platform_t){.magic=SUNXI_AUDIO_PLATFORM_MAGIC,.variant=variant,.digital_pa=variant==SUNXI_AUDIO_A64?0x200000:0,.analog_pa=0x300000,.dma_pa=0x400000,.ccu_pa=0x500000,.rsb_pa=variant==SUNXI_AUDIO_A64?0x600000:0,.dma_irq=82,.prepare=prepare,.power=power};}
static void valid_playback(unsigned v){
 variant=v;
 clear_model();sunxi_audio_platform_t p=platform(variant);device_t d={.plat_data=&p,.resource={0x100000,0x1001ff}};
 assert(registered_driver==&sunxi_audio_driver);assert(audio_probe(&d)==0);assert(power_on==1);
 struct sunxi_audio *s=d.drv_priv;assert(s&&!s->ring.running);
 assert((analog_regs[variant==SUNXI_AUDIO_A64?0x0a:3]&12)==0); /* muted until valid PCM */
 assert(dma_lli[0].config==SUNXI_DMA_CFG&&dma_lli[0].destination==0x100020);
 assert(dma_lli[7].next==0x45000000&&dma_lli[0].length==4096&&dma_lli[0].wait==8);
 assert(((dma_lli[0].config>>16)&31)==15);assert(((dma_lli[0].config>>9)&3)==1);
 if(variant==SUNXI_AUDIO_A64){assert(dai_mmio[0x24/4]==0x92);assert(digital_mmio[0x018/4]==0x8000);assert(digital_mmio[0x130/4]==0x8800);assert(dma_mmio[0x28/4]&4);}
 else{assert(dai_mmio[0x04/4]&(1U<<24));assert(dma_mmio[0x20/4]&4);}
 uint8_t input[65540];memset(input,0x53,sizeof(input));
 assert(audio_write(&d,input,3)==-EINVAL);
 assert(audio_write(&d,input,4096)==4096&&!s->ring.running);
 assert(audio_write(&d,input+4096,sizeof(input)-4096)==(int)sizeof(input)-4096);
 assert(s->ring.running&&s->partial==4);assert(audio_poll(&d,POLLOUT)==POLLOUT);
 assert(audio_drain(&d)==0&&!s->ring.running&&s->ring.count==0);
 assert(dma_mmio[DMA_EN/4]==0&&s->partial==0);
 assert(audio_write(&d,input,8192)==8192);mdelay(45);
 assert(s->ring.error==-EPIPE);assert(audio_poll(&d,POLLOUT)==POLLERR);assert(audio_write(&d,input,4)==-EPIPE);
 assert(audio_stop(&d)==0&&s->ring.error==0);assert(audio_write(&d,input,4)==4);assert(audio_drain(&d)==0);
 a20_audio_format_t bad={44100,2,A20_AUDIO_FORMAT_S16_LE};assert(audio_format(&d,&bad)==-EINVAL);
 assert(audio_remove(&d)==0&&d.drv_priv==NULL&&dma_frees==2&&power_off==1);
}
static void busy_and_quarantine(void){
 variant=SUNXI_AUDIO_A64;clear_model();sunxi_audio_platform_t p=platform(SUNXI_AUDIO_A64);device_t d={.plat_data=&p,.resource={0x100000,0x1001ff}};
 dma_mmio[DMA_EN/4]=1;dma_mmio[0x00/4]=4;assert(audio_probe(&d)==-EBUSY);assert(dma_mmio[DMA_EN/4]==1&&dma_mmio[0x00/4]==4&&dma_frees==0);
 clear_model();assert(audio_probe(&d)==0);uint8_t input[8192]={0};assert(audio_write(&d,input,sizeof(input))==sizeof(input));
 stuck_stop=1;assert(audio_stop(&d)==-ETIMEDOUT);assert(((struct sunxi_audio*)d.drv_priv)->quarantined);assert(audio_write(&d,input,4)==-EIO);
 assert(audio_remove(&d)==0&&dma_frees==0);/* DMA retains both buffers. */
 free(dma_data);free(dma_lli);/* Test-only hardware reset after quarantine check. */
}
static void lifecycle(void){
 variant=SUNXI_AUDIO_A64;clear_model();sunxi_audio_platform_t p=platform(variant);device_t d={.plat_data=&p,.resource={0x100000,0x1001ff}};
 missing_provider=5;assert(probe_attempt(&d)==-EPROBE_DEFER);assert(link_mask==15&&prepare_calls==0&&!power_on);
 for(unsigned i=0;i<5;i++)assert(provider_refs[i]==(i<4?1:0));
 missing_provider=0;assert(probe_attempt(&d)==0&&link_mask==31&&prepare_calls==1);for(unsigned i=0;i<5;i++)assert(provider_refs[i]==1);remove_attempt(&d);
 clear_model();dependency_fail=4;assert(probe_attempt(&d)==-ENOMEM&&link_mask==0&&!prepare_calls&&!power_on);for(unsigned i=0;i<5;i++)assert(provider_refs[i]==0);
 dependency_fail=0;fail_power=1;assert(probe_attempt(&d)==-EIO&&link_mask==0&&dma_frees==2&&power_off==1);for(unsigned i=0;i<5;i++)assert(provider_refs[i]==0);
 fail_power=0;assert(probe_attempt(&d)==0&&link_mask==31);remove_attempt(&d);
 clear_model();variant=SUNXI_AUDIO_V3S;p=platform(variant);assert(probe_attempt(&d)==0&&link_mask==(BIT(0)|BIT(1)|BIT(3)));remove_attempt(&d);
}
static void failures(void){
 variant=SUNXI_AUDIO_A64;clear_model();sunxi_audio_platform_t p=platform(variant);device_t d={.plat_data=&p,.resource={0x100000,0x1001ff}};
 fail_power=1;assert(audio_probe(&d)==-EIO&&d.drv_priv==NULL&&dma_frees==2&&power_off==1);
 clear_model();fail_irq=1;assert(audio_probe(&d)==-EBUSY&&d.drv_priv==NULL&&dma_frees==2&&power_off==1);
 clear_model();assert(audio_probe(&d)==0);uint8_t input[16384]={0};assert(audio_write(&d,input,sizeof(input))==sizeof(input));
 ticks+=50000;dma_mmio[DMA_SRC/4]=0x44002000;dma_mmio[0x10/4]=DMA_IRQ_PACKAGE;handler(82,handler_arg);
 assert(((struct sunxi_audio*)d.drv_priv)->ring.error==-EPIPE&&dma_mmio[DMA_EN/4]==0);assert(audio_remove(&d)==0&&dma_frees==2);
 clear_model();assert(audio_probe(&d)==0);assert(audio_write(&d,input,sizeof(input))==sizeof(input));mdelay(22);
 dai_mmio[0x0c/4]|=BIT(6);mdelay(22);assert(((struct sunxi_audio*)d.drv_priv)->ring.error==-EPIPE);assert(audio_remove(&d)==0&&dma_frees==2);
}
int main(void){lifecycle();failures();valid_playback(SUNXI_AUDIO_A64);valid_playback(SUNXI_AUDIO_V3S);busy_and_quarantine();return 0;}
'''

GLUE_HARNESS = r'''
#include "test_stub.h"
#include "PRODUCTION"
static uint32_t ccu[256],rsb[16];static uint8_t pmic[256];
static int lock_fail,bus_fail,maps,writes,enumerate,dependencies,model_v3s,irq_fallback_calls;
static device_t nodes[6];
static const uint64_t addresses[]={0x01c22c00,0x01c02000,0x01c20000,0x01f015c0,0x01c22e00,0x01f03400};
static const unsigned lengths[]={0x200,0x1000,0x400,4,0x600,0x40};
static const char *ids[]={"allwinner,sun50i-a64-codec-i2s","allwinner,sun50i-a64-dma","allwinner,sun50i-a64-ccu","allwinner,sun50i-a64-codec-analog","allwinner,sun50i-a64-codec","allwinner,sun8i-a23-rsb"};
uint32_t readl(const volatile void *p){return *(const volatile uint32_t*)p;}
void writel(uint32_t v,volatile void *p){
 if(p==&rsb[3]){rsb[3]&=~v;return;}
 if(p==&rsb[0]&&(v&128)){
  rsb[0]=0;if(bus_fail==1)return;if(bus_fail==2){rsb[3]=2;return;}
  assert(rsb[12]==(0x2dU<<16));unsigned reg=rsb[4];assert(reg<256);
  if(rsb[11]==0x8b)rsb[7]=pmic[reg];else{assert(rsb[11]==0x4e);pmic[reg]=rsb[7];writes++;}rsb[3]=1;return;
 }
 *(volatile uint32_t*)p=v;
 if(p==&ccu[2]&&(v&(1U<<31))&&!lock_fail)ccu[2]|=1U<<28;
}
void udelay(unsigned us){(void)us;}void mdelay(unsigned ms){(void)ms;}
void *arch_device_map_mmio(uint64_t pa,size_t n,uint32_t f){(void)n;(void)f;maps++;if(pa==1)return ccu;if(pa==2)return rsb;return NULL;}
void arch_device_unmap_mmio(void *p,size_t n){(void)p;(void)n;}
device_t *device_find_get_by_mmio(uint64_t pa){if(enumerate)for(unsigned i=0;i<6;i++)if(pa==(model_v3s&&i==3?0x01c23000:addresses[i]))return &nodes[i];return NULL;}
void device_put(device_t *d){(void)d;}
int device_add_dependency(device_t*a,device_t*b){assert(a&&b);dependencies++;return 0;}
resource_t *device_get_resource(device_t*d,int t,int i){(void)t;(void)i;return &d->resource;}
int platform_device_irq(device_t*d){return d->irq?d->irq:-ENODEV;}
/* Auxiliary provider was enumerated, but never prepared/bound: no RES_IRQ.
 * The generic firmware IRQ API can translate its immutable reference anyway. */
int firmware_device_irq(device_t*d,size_t index){assert(d==&nodes[1]&&index==0);if(d->irq)return d->irq;irq_fallback_calls++;return 82;}
int fw_node_match_id(const int*n,const char*id){
 static const char *v3s[]={"allwinner,sun8i-v3s-codec","allwinner,sun8i-v3s-dma","allwinner,sun8i-v3s-ccu","allwinner,sun8i-v3s-codec-analog"};
 return *n>=0&&*n<(model_v3s?4:6)&&!strcmp(model_v3s?v3s[*n]:ids[*n],id);
}
static void reset(void){memset(ccu,0,sizeof(ccu));memset(rsb,0,sizeof(rsb));memset(pmic,0,sizeof(pmic));sunxi_audio_ccu_pa=1;sunxi_audio_rsb_pa=2;sunxi_audio_platform.variant=SUNXI_AUDIO_A64;sunxi_audio_supply_changed=0;lock_fail=bus_fail=maps=writes=enumerate=dependencies=model_v3s=irq_fallback_calls=0;pmic[3]=0x41;
 memset(nodes,0,sizeof(nodes));for(unsigned i=0;i<6;i++){nodes[i].resource=(resource_t){addresses[i],addresses[i]+lengths[i]-1};nodes[i].firmware_node=i;nodes[i].irq=0;}}
static void resources(void){
 for(unsigned v=0;v<2;v++)for(unsigned i=0;i<(v?4:6);i++)for(unsigned defect=0;defect<4;defect++){
  reset();enumerate=1;model_v3s=v;if(v)nodes[3].resource=(resource_t){0x01c23000,0x01c23003};
  if(defect==0)nodes[i].resource.start--; /* enclosing parent */
  if(defect==1)nodes[i].resource.end=nodes[i].resource.start-1;
  if(defect==2)nodes[i].resource.end--; /* short register bank */
  if(defect==3)nodes[i].firmware_node=-1;
  sunxi_audio_platform.magic=0x1234;
  assert(sunxi_board_prepare_audio(v?SUNXI_AUDIO_V3S:SUNXI_AUDIO_A64,1)==-ENODEV);
  assert(!nodes[0].plat_data&&!dependencies&&!maps&&!writes);
  assert(sunxi_audio_platform.magic==0x1234&&sunxi_audio_ccu_pa==1&&sunxi_audio_rsb_pa==2);
 }
 reset();enumerate=model_v3s=1;nodes[3].resource=(resource_t){0x01c23000,0x01c23003};assert(sunxi_board_prepare_audio(SUNXI_AUDIO_V3S,1)==0&&dependencies==0&&!maps&&!writes);assert(sunxi_audio_platform.variant==SUNXI_AUDIO_V3S&&!sunxi_audio_platform.digital_pa&&!sunxi_audio_rsb_pa);
 reset();enumerate=1;nodes[1].irq=-EINVAL;assert(sunxi_board_prepare_audio(SUNXI_AUDIO_A64,1)==-EINVAL&&!dependencies&&!nodes[0].plat_data);
 reset();enumerate=1;assert(sunxi_board_prepare_audio(SUNXI_AUDIO_A64,1)==0);assert(dependencies==0&&!maps&&!writes&&irq_fallback_calls==1);assert(nodes[0].plat_data==&sunxi_audio_platform&&sunxi_audio_platform.dma_irq==82);
}
int main(void){
 resources();(void)platform_bus;reset();assert(sunxi_board_prepare_audio(SUNXI_AUDIO_V3S,0)==0&&maps==0);
 ccu[0x060/4]=8;assert(sunxi_audio_clocks()==0);assert(ccu[0x284/4]==0xc000ac02U);assert(ccu[0x060/4]==72);assert(ccu[0x2c0/4]==64);assert(ccu[0x140/4]&(1U<<31));
 reset();ccu[2]=1U<<31;ccu[0x284/4]=17;assert(sunxi_audio_clocks()==-EBUSY);assert(ccu[2]==(1U<<31)&&ccu[0x284/4]==17&&ccu[0x060/4]==0);
 reset();lock_fail=1;ccu[2]=3;ccu[0x284/4]=19;assert(sunxi_audio_clocks()==-ETIMEDOUT);assert(ccu[2]==3&&ccu[0x284/4]==19&&ccu[0x060/4]==0);
 reset();pmic[0x12]=0xa4;pmic[0x19]=0x83;assert(sunxi_audio_power(1)==0);assert(pmic[0x12]==0xa5&&pmic[0x19]==0x96);assert(sunxi_audio_power(0)==0&&pmic[0x12]==0xa4&&pmic[0x19]==0x83);
 reset();pmic[0x12]=1;pmic[0x19]=22;assert(sunxi_audio_power(1)==0&&sunxi_audio_power(0)==0&&writes==0);
 reset();pmic[0x12]=1;pmic[0x19]=21;assert(sunxi_audio_power(1)==-EBUSY&&writes==0);
 reset();pmic[3]=0;assert(sunxi_audio_power(1)==-ENODEV&&writes==0);
 reset();rsb[0]=128;assert(sunxi_audio_power(1)==-EBUSY&&writes==0);
 reset();bus_fail=1;assert(sunxi_audio_power(1)==-ETIMEDOUT&&writes==0);
 reset();bus_fail=2;assert(sunxi_audio_power(1)==-EIO&&writes==0);
 reset();sunxi_audio_platform.variant=SUNXI_AUDIO_V3S;assert(sunxi_audio_power(1)==0&&sunxi_audio_power(0)==0&&maps==0);
 return 0;
}
'''


class SunxiAudioTests(unittest.TestCase):
    def test_production_electrical_preparation(self):
        self.run_model(GLUE_HARNESS, "kernel/arch/common/sunxi_audio.h")

    def test_production_dma_pcm_and_codec_variants(self):
        self.run_model(HARNESS, "kernel/drivers/audio/sunxi_audio.c")

    def run_model(self, harness, production):
        with tempfile.TemporaryDirectory(prefix='a20-sunxi-audio-') as directory:
            work = Path(directory)
            (work / 'test_stub.h').write_text(STUB)
            for path in ('drivers/audio/audio_core.h', 'drivers/bus/platform_bus.h',
                         'drivers/core/driver_register.h', 'drivers/core/driver_hwapi.h',
                         'drivers/core/driver_core.h', 'core/types.h', 'core/errno.h',
                         'core/lock.h', 'core/sync.h', 'core/string.h', 'core/stdio.h',
                         'core/poll.h', 'mm/slab.h', 'firmware/device.h'):
                destination = work / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text('#include "test_stub.h"\n')
            source = work / 'test.c'
            source.write_text(harness.replace('PRODUCTION', str(ROOT / production)))
            result = subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra',
                                     '-Werror', '-Wno-misleading-indentation',
                                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                     '-I', str(work), '-I', str(ROOT / 'kernel/include'),
                                     '-I', str(ROOT / 'kernel'), str(source), '-o', str(work / 'test')],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(work / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
