"""Execute the production RK817/I2S/PL330 code against bounded MMIO models.

PL330 bytecode is interpreted, including loop counters, widths, peripheral
handshakes and completion events. This is not electrical or physical DMA proof.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.tests.test_sunxi_audio import STUB
HARNESS = r'''
#include "test_stub.h"
void irq_disable(uint32_t n);
#include "PRODUCTION"
static uint32_t i2s[1024],dma[4096];
static uint8_t codec[256],*program;
static uint32_t *samples;
static uint64_t ticks,last_period;
static uint32_t sar,dar,ccr;
static unsigned pc,lc,loop,program_size,events,words,transfers,cache_syncs;
static int (*handlers[2])(int,void*);static void *args[2];
static int frees,stuck,codec_fail,prepare_calls,missing,go_fault,disabled,restoring;
static int unmaps,alloc_calls,fail_alloc,fail_map,fail_action,fail_irq,fail_probe_stop;
static int dead_dma,stuck_mask,stuck_tx,stop_wait,busy_stops,progress=1;
static int codec_error_reg=-1,codec_mismatch_reg=-1,stop_codec;
static unsigned debug_writes;
static device_t *active_device;
static device_t providers[6];static unsigned dependency_mask;
static const uint64_t pas[]={0x400000,0x500000,0x600000,0x700000,0x800000,0x900000};
uint32_t readl(const volatile void*p){if(dead_dma&&(uintptr_t)p>=(uintptr_t)dma&&(uintptr_t)p<(uintptr_t)dma+sizeof(dma))return UINT32_MAX;return *(const volatile uint32_t*)p;}
static uint32_t word(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
void writel(uint32_t v,volatile void*p){
 if(stuck_mask&&p==&dma[0x20/4]&&!(v&1))return;
 if(stuck_tx&&p==&i2s[0x1c/4]&&!(v&1))return;
 if(p==&dma[0x2c/4]){dma[0x28/4]&=~v;return;}
 if(p==&i2s[0x20/4]){i2s[0x20/4]=0;return;}
 if(p==&i2s[0x14/4]){i2s[0x18/4]&=~((v&4)?2:0);i2s[0x14/4]=v&~4;return;}
 *(volatile uint32_t*)p=v;
 if(p==&dma[0xd04/4]){
  debug_writes++;
  uint32_t op=dma[0xd08/4];
  if(op==(0xa2U<<16)){assert(dma[0xd0c/4]==0x45000000);pc=0;dma[0x100/4]=go_fault?0:7;dma[0x30/4]=go_fault?1:0;last_period=ticks;}
  else {assert(op==((1U<<16)|1)||op==(1U<<16));if(!stuck){if(op==((1U<<16)|1)){dma[0x100/4]=0;dma[0x34/4]=0;}else{dma[0x30/4]=0;dma[0]=0;}}}
 }
}
static void period(void){
 unsigned begin=transfers;
 for(unsigned budget=0;budget<3000;budget++){
  assert(pc<program_size);unsigned instruction=pc;uint8_t op=program[pc++];
  if(op==0xbc){unsigned dst=program[pc++];uint32_t v=word(program+pc);pc+=4;if(dst==0)sar=v;else if(dst==1)ccr=v;else{assert(dst==2);dar=v;}}
  else if(op==0x35){assert(program[pc++]==16);}
  else if(op==0x20){lc=(unsigned)program[pc++]+1;loop=pc;}
  else if(op==0x32){assert(program[pc++]==16);assert(i2s[0x10/4]&256);}
  else if(op==7){assert(ccr==PL330_AUDIO_CCR);assert(sar>=0x44000000&&sar+32<=0x44000000+DATA_BYTES);sar+=32;words+=8;}
  else if(op==0x2b){assert(program[pc++]==16&&dar==0x100024);transfers+=32;}
  else if(op==0x38){unsigned distance=program[pc++];assert(instruction-distance==loop);if(--lc)pc=instruction-distance;}
  else if(op==0x28){unsigned distance=program[pc++];pc=instruction-distance;}
  else if(op==0x13){assert(transfers-begin==8192);}
  else if(op==0x34){assert(program[pc++]==0);events++;dma[0x400/4]=sar;dma[0x104/4]=pc;dma[0x28/4]|=1;if(handlers[0])handlers[0](48,args[0]);return;}
  else assert(!"invalid DMA opcode");
 }
 assert(!"DMA program did not reach period event");
}
void udelay(unsigned us){ticks+=us;}
void mdelay(unsigned ms){for(unsigned i=0;i<ms;i++){if(stop_wait){stop_wait=0;assert(active_device&&((struct rk817_audio*)active_device->drv_priv)->writer.value);assert(active_device->execution(active_device->execution_ctx)==-EBUSY&&!frees&&!unmaps&&handlers[0]&&handlers[1]);busy_stops++;}ticks+=1000;if(progress&&dma[0x100/4]&&(i2s[0x10/4]&256)&&ticks-last_period>=21334){last_period=ticks;period();}}}
uint64_t clock_get_ticks(void){return ticks;}uint64_t clock_ticks_per_sec(void){return 1000000;}
void *arch_device_map_mmio(uint64_t pa,size_t n,uint32_t f){(void)n;(void)f;if((fail_map==1&&pa==0x100000)||(fail_map==2&&pa==0x400000))return NULL;if(pa==0x100000)return i2s;if(pa==0x400000)return dma;return NULL;}
void arch_device_unmap_mmio(void*p,size_t n){assert(p&&n&&!handlers[0]&&!handlers[1]);unmaps++;}
void *dma_alloc_coherent_aligned(device_t*d,size_t n,size_t a,uint64_t*pa){assert(d->managed&&d->execution);if(++alloc_calls==fail_alloc)return NULL;void*p=aligned_alloc(a,n);assert(p);if(n==4096){program=p;*pa=0x45000000;}else{samples=p;*pa=0x44000000;}return p;}
void dma_free_coherent_aligned(void*p,size_t n,uint64_t pa){assert(n&&pa&&!handlers[0]&&!handlers[1]&&!(dma[0x100/4]&15)&&!(dma[0]&15)&&!(dma[0x30/4]&1));frees++;free(p);}
void dma_sync_for_device(void*p,size_t n){assert(p&&n);cache_syncs++;}
int dma_set_mask(device_t*d,uint64_t m){assert(d&&m==0xffffffffULL);return 0;}
int request_irq(uint32_t n,int(*f)(int,void*),unsigned long flags,void*p){assert(n==48||n==47);assert(flags==IRQF_SHARED);unsigned i=n==48?0:1;if(fail_irq==(int)i+1){if(fail_probe_stop){stuck=1;dma[0x100/4]=7;}return -EBUSY;}handlers[i]=f;args[i]=p;return 0;}
void free_irq(uint32_t n,void*p){unsigned i=n==48?0:1;assert(args[i]==p&&!(dma[0x100/4]&15)&&!(dma[0]&15)&&!(dma[0x30/4]&1)&&!(dma[0x20/4]&1));assert(((struct rk817_audio*)p)->closing&&!((struct rk817_audio*)p)->codec_touched);handlers[i]=NULL;args[i]=NULL;}
void irq_enable(uint32_t n){assert(n==48||n==47);}
void irq_disable(uint32_t n){assert(n==48||n==47);disabled++;}
device_t *device_find_get_by_mmio(uint64_t pa){for(unsigned i=0;i<6;i++)if(pa==pas[i]&&missing!=(int)i+1)return &providers[i];return NULL;}
int device_add_dependency(device_t*a,device_t*b){assert(a&&b);dependency_mask|=1U<<(unsigned)(b-providers);return 0;}
void device_put(device_t*d){assert(d);}
resource_t*device_get_resource(device_t*d,int t,int i){assert(t==RES_MMIO&&i==0);return &d->resource;}
static int prepare(void){prepare_calls++;return 0;}
static int transfer(uint8_t reg,uint8_t*v,int write){if(codec_fail||(write&&codec_error_reg==reg))return -ENXIO;if(write){if(restoring&&reg==0x38&&!(*v&1))assert(codec[0x3d]==0xe0&&codec[0x3f]==9&&codec[0x17]==6&&codec[0x47]==1&&codec[0x15]==0);codec[reg]=*v;}else *v=codec[reg]^((stop_codec&&codec_mismatch_reg==reg)?1:0);return 0;}
int device_probe_use_remove(device_t*d){d->managed=1;return 0;}
int device_add_execution_action(device_t*d,int(*stop)(void*),void(*release)(void*),void*p){assert(d->managed&&d->drv_priv==p&&!release);if(fail_action)return -ENOMEM;d->execution=stop;d->execution_ctx=p;return 0;}
/* Model core sequencing; all provider stop/restore/remove functions are real. */
static int unbind(device_t*d){if(d->drv_priv&&d->execution){int r=d->execution(d->execution_ctx);if(r)return r;}int r=rk817_remove(d);if(!r){d->execution=NULL;d->execution_ctx=NULL;dependency_mask=0;}return r;}
static int bind(device_t*d){assert(!d->drv_priv);int r=rk817_probe(d);if(r&&d->drv_priv)(void)unbind(d);if(r&&!d->drv_priv)dependency_mask=0;return r;}
static rk817_audio_platform_t platform(void){rk817_audio_platform_t p={.magic=RK817_AUDIO_MAGIC,.dma_irq={48,47},.dma_request=2,.dma_pa=0x400000,.prepare=prepare,.codec_register=transfer};for(unsigned i=0;i<5;i++)p.provider_pa[i]=pas[i+1];return p;}
static void reset(void){assert(!handlers[0]&&!handlers[1]);memset(i2s,0,sizeof(i2s));memset(dma,0,sizeof(dma));memset(codec,0,sizeof(codec));memset(handlers,0,sizeof(handlers));dma[0xfe0/4]=0x30;dma[0xfe4/4]=0x13;dma[0xfe8/4]=4;dma[0xe00/4]=5|(7U<<4)|(31U<<12)|(31U<<17);dma[0xe0c/4]=1;dma[0xe10/4]=4;ticks=last_period=0;pc=lc=loop=events=words=transfers=cache_syncs=0;frees=stuck=codec_fail=prepare_calls=missing=dependency_mask=go_fault=disabled=restoring=0;codec[0x38]=0xa0;codec[0x3d]=0xe0;codec[0x3f]=9;codec[0x17]=6;codec[0x47]=1;codec[0x42]=4;unmaps=alloc_calls=fail_alloc=fail_map=fail_action=fail_irq=fail_probe_stop=dead_dma=stuck_mask=stuck_tx=stop_wait=busy_stops=stop_codec=debug_writes=0;codec_error_reg=codec_mismatch_reg=-1;progress=1;active_device=NULL;}
static void playback(void){
 reset();rk817_audio_platform_t p=platform();device_t d={.plat_data=&p,.resource={0x100000,0x100fff}};
 assert(registered_driver==&rk817_driver);assert(bind(&d)==0&&dependency_mask==63&&prepare_calls==1);
 program_size=pl330_audio_program(program,4096,0x44000000,0x100024,2,0);assert(program_size==160);
 assert((codec[0x38]&1)&&codec[0x4b]==15&&codec[0x35]==2&&codec[0x44]==0x2d);
 uint8_t input[65540];for(unsigned i=0;i<sizeof(input);i+=4){input[i]=0x34;input[i+1]=0x12;input[i+2]=0xfe;input[i+3]=0xff;}
 assert(rk817_write(&d,input,3)==-EINVAL);assert(rk817_write(&d,input,4096)==4096);assert(samples[0]==0x1234&&samples[1]==0xfffe&&samples[2047]==0xfffe);
 assert(rk817_write(&d,input+4096,sizeof(input)-4096)==(int)sizeof(input)-4096);assert(events>0&&cache_syncs>3);
 assert(rk817_drain(&d)==0&&dma[0x100/4]==0&&!(i2s[0x1c/4]&1));assert(words*4==transfers&&events==17);
 assert(rk817_write(&d,input,8192)==8192);mdelay(45);assert(((struct rk817_audio*)d.drv_priv)->ring.error==-EPIPE&&rk817_poll(&d,POLLOUT)==POLLERR);
 assert(rk817_stop(&d)==0);assert(rk817_write(&d,input,4)==4&&rk817_drain(&d)==0);
 restoring=1;assert(unbind(&d)==0&&frees==2&&!handlers[0]&&!handlers[1]);assert(codec[0x38]==0xa0);
}
static void failures(void){
 reset();rk817_audio_platform_t p=platform();device_t d={.plat_data=&p,.resource={0x100000,0x100fff}};
 missing=6;assert(bind(&d)==-EPROBE_DEFER&&!prepare_calls);missing=0;
 dma[0x100/4]=7;assert(bind(&d)==-EBUSY&&dma[0x100/4]==7&&!frees);dma[0x100/4]=0;
 codec_fail=1;assert(bind(&d)==-ENXIO&&frees==2);codec_fail=0;
 reset();assert(bind(&d)==0);program_size=pl330_audio_program(program,4096,0x44000000,0x100024,2,0);uint8_t input[8192]={0};assert(rk817_write(&d,input,sizeof(input))==sizeof(input));
 dma[0x34/4]=1;handlers[1](47,args[1]);assert(((struct rk817_audio*)d.drv_priv)->ring.error==-EIO&&!dma[0x34/4]&&!dma[0x100/4]);assert(rk817_stop(&d)==0);
 go_fault=1;assert(rk817_write(&d,input,sizeof(input))==sizeof(input));assert(((struct rk817_audio*)d.drv_priv)->ring.error==-EIO&&!dma[0x30/4]);go_fault=0;assert(rk817_stop(&d)==0);
 assert(rk817_write(&d,input,sizeof(input))==sizeof(input));
 stuck=1;assert(rk817_stop(&d)==-ETIMEDOUT&&rk817_write(&d,input,4)==-EIO);struct rk817_audio *saved=d.drv_priv;assert(unbind(&d)==-ETIMEDOUT&&d.drv_priv==saved&&!frees&&!unmaps&&handlers[0]&&handlers[1]);stuck=0;assert(!unbind(&d)&&frees==2&&unmaps==2);
}
static void lifecycle(void){
 uint8_t input[PCM_BYTES*2]={0};
 for(unsigned fault=0;fault<9;fault++){
  reset();rk817_audio_platform_t p=platform();device_t d={.plat_data=&p,.resource={0x100000,0x100fff}};assert(!bind(&d));program_size=pl330_audio_program(program,4096,0x44000000,0x100024,2,0);assert(rk817_write(&d,input,8192)==8192);struct rk817_audio *saved=d.drv_priv;
  if(fault==0)stuck=1;
  if(fault==1)dead_dma=1;
  if(fault==2)stuck_mask=1;
  if(fault==3)stuck_tx=1;
  if(fault==4)dma[0xd00/4]=1;
  if(fault==5){stuck=1;dma[0]=1;dma[0x30/4]=1;}
  if(fault==6)codec_error_reg=0x31;
  if(fault==7){stop_codec=1;codec_mismatch_reg=0x31;}
  if(fault==8){stuck=1;dma[0x100/4]=0;dma[0x34/4]=1;} /* STOPPED alone does not clear abort. */
  assert(unbind(&d)<0&&d.drv_priv==saved&&!frees&&!unmaps&&handlers[0]&&handlers[1]&&dependency_mask==63&&saved->codec_touched);
  assert(rk817_write(&d,input,4)==-ENODEV&&rk817_drain(&d)==-ENODEV&&rk817_stop(&d)==-ENODEV&&rk817_poll(&d,POLLOUT)==POLLERR);
  if(!dead_dma){unsigned debug=debug_writes,consumer=saved->ring.consumer;dma[0x28/4]=1;assert(handlers[0](48,saved)==1&&saved->ring.consumer==consumer&&debug_writes==debug);}
  stuck=dead_dma=stuck_mask=stuck_tx=stop_codec=0;dma[0xd00/4]=0;codec_error_reg=codec_mismatch_reg=-1;restoring=1;
  assert(!unbind(&d)&&!d.drv_priv&&frees==2&&unmaps==2&&codec[0x38]==0xa0&&!handlers[0]&&!handlers[1]);
 }
 for(unsigned drain=0;drain<2;drain++){
  reset();rk817_audio_platform_t p=platform();device_t d={.plat_data=&p,.resource={0x100000,0x100fff}};assert(!bind(&d));program_size=pl330_audio_program(program,4096,0x44000000,0x100024,2,0);active_device=&d;progress=0;stop_wait=1;
  if(drain){assert(rk817_write(&d,input,8192)==8192);assert(rk817_drain(&d)==-ENODEV);}else assert(rk817_write(&d,input,sizeof(input))==PCM_BYTES);
  assert(busy_stops==1&&!frees&&!unmaps);assert(!unbind(&d)&&frees==2);
 }
 for(unsigned which=1;which<3;which++){
  reset();rk817_audio_platform_t p=platform();device_t d={.plat_data=&p,.resource={0x100000,0x100fff}};fail_irq=which;fail_probe_stop=1;
  assert(bind(&d)==-EBUSY&&d.drv_priv&&!frees&&!unmaps&&dependency_mask==63);assert((handlers[0]!=NULL)==(which==2));stuck=0;assert(!unbind(&d)&&frees==2);
 }
 reset();rk817_audio_platform_t p=platform();device_t d={.plat_data=&p,.resource={0x100000,0x100fff}};codec_error_reg=0x30;
 assert(bind(&d)==-ENXIO&&d.drv_priv&&!frees&&!unmaps&&((struct rk817_audio*)d.drv_priv)->codec_touched);codec_error_reg=-1;assert(!unbind(&d)&&frees==2&&codec[0x38]==0xa0);
 for(unsigned allocation=1;allocation<3;allocation++){reset();fail_alloc=allocation;assert(bind(&d)==-ENOMEM&&!d.drv_priv&&frees==1);}
 for(unsigned mapping=1;mapping<3;mapping++){reset();fail_map=mapping;assert(bind(&d)==-ENOMEM&&!d.drv_priv&&!frees&&unmaps==1);}
 reset();fail_action=1;assert(bind(&d)==-ENOMEM&&!d.drv_priv&&!prepare_calls&&!unmaps);
 reset();providers[0].drv=(void*)1;assert(bind(&d)==-EBUSY&&!d.drv_priv&&!prepare_calls);providers[0].drv=NULL;providers[0].user_owned=1;assert(bind(&d)==-EBUSY&&!d.drv_priv&&!prepare_calls);providers[0].user_owned=0;
}
int main(void){playback();failures();lifecycle();return 0;}
'''

GLUE = r'''
#include "test_stub.h"
typedef int fw_node_t;
typedef struct {fw_node_t node;uint32_t args[16];size_t count;} fw_reference_t;
int fw_node_equal(const fw_node_t*a,const fw_node_t*b);
int fw_node_read_string(const fw_node_t*n,const char*k,size_t i,const char**out);
int fw_node_reference(const fw_node_t*n,const char*k,const char*c,size_t i,fw_reference_t*out);
#define RES_IRQ 2
#define PLATFORM_IRQ_MAX_LINES 256
const char*fw_node_name(const fw_node_t*n);
int fw_node_next_child(const fw_node_t*n,const fw_node_t*p,fw_node_t*out);
int fw_node_available(const fw_node_t*n);
int fw_node_read_u32(const fw_node_t*n,const char*key,uint32_t*out);
#include "PRODUCTION"
static uint32_t cru[1024],grf[1024],pmucru[1024],pmugrf[1024],i2c[1024],dai[1024];
static uint8_t pmic[256];static unsigned maps,mmio_writes,pmic_writes,put_count;
static int enabled,wrong_child,bus_fault,irq_fallback_calls,irq_reference_error,irq_resource_reads;static device_t nodes[7];
static const uint64_t pas[]={0xfe410000,0xfe550000,0xfdd40000,0xfdd20000,0xfdc60000,0xfdd00000,0xfdc20000};
static const unsigned sizes[]={0x1000,0x4000,0x1000,0x1000,0x1000,0x1000,0x1000};
static const char*ids[]={"rockchip,rk3568-i2s-tdm","arm,pl330","rockchip,rk3568-i2c","rockchip,rk3568-cru","rockchip,rk3568-grf","rockchip,rk3568-pmucru","rockchip,rk3568-pmugrf"};
uint32_t readl(const volatile void*p){return *(const volatile uint32_t*)p;}
void writel(uint32_t v,volatile void*p){
 mmio_writes++;
 if(p==&i2c[0x1c/4]){i2c[0x1c/4]&=~v;return;}
 if(p==&i2c[0]){
  i2c[0]=v;if(bus_fault==1)return;
  if(bus_fault==2){i2c[0x1c/4]|=64;return;}
  if(v&8)i2c[0x1c/4]|=16;if(v&16)i2c[0x1c/4]|=32;return;
 }
 if(p==&i2c[0x14/4]){assert(v==1&&i2c[2]==0x1000041);i2c[0x200/4]=pmic[i2c[3]&255];i2c[0x1c/4]|=8;return;}
 if(p==&i2c[0x10/4]){assert(v==3);uint32_t word=i2c[0x100/4];assert((word&255)==0x40);pmic[(word>>8)&255]=word>>16;pmic_writes++;i2c[0x1c/4]|=4;return;}
 if((uintptr_t)p>=(uintptr_t)i2c&&(uintptr_t)p<(uintptr_t)(i2c+1024)){*(volatile uint32_t*)p=v;return;}
 if((uintptr_t)p>=(uintptr_t)cru&&(uintptr_t)p<(uintptr_t)(cru+1024)){
  if(p==&cru[0x140/4]){*(volatile uint32_t*)p=v;return;}
 }
 *(volatile uint32_t*)p=(*(volatile uint32_t*)p&~(v>>16))|(v&(v>>16));
}
void udelay(unsigned us){(void)us;}void mdelay(unsigned ms){(void)ms;}
void*arch_device_map_mmio(uint64_t pa,size_t n,uint32_t f){(void)n;(void)f;maps++;if(pa==pas[0])return dai;if(pa==pas[2])return i2c;if(pa==pas[3])return cru;if(pa==pas[4])return grf;if(pa==pas[5])return pmucru;if(pa==pas[6])return pmugrf;return NULL;}
void arch_device_unmap_mmio(void*p,size_t n){(void)p;(void)n;}
device_t*device_find_get_by_mmio(uint64_t pa){if(enabled)for(unsigned i=0;i<7;i++)if(pa==pas[i])return &nodes[i];return NULL;}
void device_put(device_t*d){assert(d);put_count++;}
resource_t*device_get_resource(device_t*d,int kind,int index){(void)index;if(kind==RES_IRQ){irq_resource_reads++;return NULL;}return &d->resource;}
/* The auxiliary DMA provider has no materialized IRQ slots before probe_all. */
int firmware_device_irq(device_t*d,size_t index){assert(d==&nodes[1]&&index<2);irq_fallback_calls++;return irq_reference_error?irq_reference_error:index?47:48;}
int fw_node_match_id(const int*n,const char*id){return *n>=0&&*n<7?!strcmp(ids[*n],id):*n==7&&!strcmp(id,"rockchip,rk817");}
const char*fw_node_name(const int*n){return *n==8?"codec":"other";}
int fw_node_next_child(const int*n,const int*p,int*out){if(p)return -ENOENT;if(*n==2){*out=7;return 0;}if(*n==7){*out=8;return 0;}return -ENOENT;}
int fw_node_available(const int*n){return !wrong_child||*n!=8;}
int fw_node_read_u32(const int*n,const char*key,uint32_t*out){assert(*n==7);if(!strcmp(key,"reg"))*out=0x20;else{assert(!strcmp(key,"#sound-dai-cells"));*out=0;}return 0;}
int fw_node_equal(const int*a,const int*b){return *a==*b;}
int fw_node_read_string(const int*n,const char*k,size_t i,const char**out){assert(*n==7&&!strcmp(k,"clock-names")&&!i);*out="mclk";return 0;}
int fw_node_reference(const int*n,const char*k,const char*c,size_t i,fw_reference_t*out){assert(*n==7&&!strcmp(k,"clocks")&&!strcmp(c,"#clock-cells")&&!i);out->node=3;out->args[0]=72;out->count=1;return 0;}
static void reset(void){memset(dai,0,sizeof(dai));memset(cru,0,sizeof(cru));memset(grf,0,sizeof(grf));memset(pmucru,0,sizeof(pmucru));memset(pmugrf,0,sizeof(pmugrf));memset(i2c,0,sizeof(i2c));memset(pmic,0,sizeof(pmic));memset(nodes,0,sizeof(nodes));maps=mmio_writes=pmic_writes=put_count=enabled=wrong_child=bus_fault=irq_fallback_calls=irq_reference_error=irq_resource_reads=0;for(unsigned i=0;i<7;i++){nodes[i].resource=(resource_t){pas[i],pas[i]+sizes[i]-1};nodes[i].firmware_node=i;}cru[0xc0/4]=1U<<6;cru[0x40/4]=99U|(2U<<12);cru[0x44/4]=1U|(1U<<6)|(1U<<10)|(1U<<12);pmic[0xed]=0x81;pmic[0xee]=0x70;pmic[0xb2]=0xa;pmic[0xd2]=108;i2c[1]=0x12340012;}
int main(void){
 (void)platform_bus;
 reset();assert(rk3566_board_prepare_audio()==-ENODEV&&!maps&&!mmio_writes);
 for(unsigned i=0;i<7;i++)for(unsigned bad=0;bad<4;bad++){
  reset();enabled=1;if(bad==0)nodes[i].resource.start--;if(bad==1)nodes[i].resource.end=nodes[i].resource.start-1;if(bad==2)nodes[i].resource.end=nodes[i].resource.start+sizes[i]-2;if(bad==3)nodes[i].firmware_node=-1;
  assert(rk3566_board_prepare_audio()==-ENODEV&&!nodes[0].plat_data&&!maps&&!mmio_writes);
 }
 reset();enabled=1;irq_reference_error=-EPROBE_DEFER;assert(rk3566_board_prepare_audio()==-EPROBE_DEFER&&!nodes[0].plat_data&&!maps&&!mmio_writes);
 reset();enabled=wrong_child=1;assert(rk3566_board_prepare_audio()==-ENODEV&&!maps);
 reset();enabled=1;assert(rk3566_board_prepare_audio()==0&&!maps&&!mmio_writes&&put_count==7);assert(rk3566_audio_platform.dma_request==2&&rk3566_audio_platform.dma_irq[0]==48&&rk3566_audio_platform.dma_irq[1]==47&&irq_fallback_calls==2&&!irq_resource_reads);
 uint32_t pll0=cru[0x40/4],pll1=cru[0x44/4];assert(rk3566_audio_prepare()==0&&pmic_writes==0);assert(cru[0x40/4]==pll0&&cru[0x44/4]==pll1);assert(cru[0x140/4]==(128U<<16|12375));assert(grf[0]==0x1100&&grf[1]==0x1010&&grf[0x508/4]==2);assert(pmugrf[0x144/4]==2);assert(i2c[1]==0x12340012);
 uint8_t value=0xa5;assert(rk3566_codec_register(0x31,&value,1)==0);value=0;assert(rk3566_codec_register(0x31,&value,0)==0&&value==0xa5);
 bus_fault=1;assert(rk3566_codec_register(0x31,&value,0)==-ETIMEDOUT);bus_fault=2;assert(rk3566_codec_register(0x31,&value,0)==-ENXIO);bus_fault=0;
 i2c[0]=1;assert(rk3566_codec_register(0x31,&value,0)==-EBUSY);i2c[0]=0;i2c[0x18/4]=32;assert(rk3566_codec_register(0x31,&value,0)==0&&i2c[0x18/4]==32);i2c[0x18/4]=0;
 pmic[0xd2]=100;assert(rk3566_audio_prepare()==-EACCES&&pmic_writes==1);
 return 0;
}
'''

class Rk817AudioTests(unittest.TestCase):
    def test_production_i2c_clock_power_and_topology(self):
        self.run_model(GLUE, "kernel/arch/common/rk3566_audio.h")

    def test_production_pcm_codec_and_interpreted_pl330(self):
        self.run_model(HARNESS, "kernel/drivers/audio/rk817_audio.c")

    def run_model(self, harness, production):
        with tempfile.TemporaryDirectory(prefix="a20-rk817-audio-") as directory:
            work = Path(directory)
            (work / "test_stub.h").write_text(STUB.replace("void *plat_data,*drv_priv;", "void *plat_data,*drv_priv,*drv;int user_owned;"))
            for path in ("drivers/audio/audio_core.h", "drivers/bus/platform_bus.h",
                         "drivers/core/driver_register.h", "drivers/core/driver_hwapi.h",
                         "drivers/core/driver_core.h", "core/types.h", "core/errno.h",
                         "core/lock.h", "core/sync.h", "core/string.h", "core/stdio.h",
                         "core/poll.h", "mm/slab.h", "firmware/node.h", "firmware/device.h"):
                dest = work / path
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text('#include "test_stub.h"\n')
            source = work / "test.c"
            source.write_text(harness.replace("PRODUCTION", str(ROOT / production)))
            result = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                     "-Wno-misleading-indentation", "-O1", "-fsanitize=address,undefined",
                                     "-fno-omit-frame-pointer", "-I", str(work), "-I", str(ROOT / "kernel/include"),
                                     "-I", str(ROOT / "kernel"), str(source), "-o", str(work / "test")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(work / "test")], check=True)

if __name__ == "__main__":
    unittest.main()
