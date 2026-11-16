/* Production DW PIO controller + production adapter registry; MMIO/core modeled. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {int held;} mutex_t;
typedef struct {int held;} spinlock_t;
#define SPINLOCK_INIT {0}
#define MS_TO_TICKS(x) ((uint64_t)(x))
#define RES_MMIO 1
#define DRIVER_REGISTER(x)
static void host_log(const char *format,...){(void)format;}
#define kinfo host_log
#define kerr host_log
#define kwarn host_log
static void mutex_init(mutex_t *m){m->held=0;}
static void mutex_lock(mutex_t *m){assert(!m->held);m->held=1;}
static void mutex_unlock(mutex_t *m){assert(m->held);m->held=0;}
static int mutex_trylock(mutex_t *m){if(m->held)return 0;m->held=1;return 1;}
static uint64_t spin_lock_irqsave(spinlock_t *m){assert(!m->held);m->held=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t *m,uint64_t f){(void)f;assert(m->held);m->held=0;}
typedef struct {uint64_t start,end;} resource_t;
typedef struct {int unused;} firmware_node_t;
typedef struct device {void *drv_priv;firmware_node_t firmware_node;int managed,mapped,stopped;int(*stop)(void*);void(*release)(void*);void *context;resource_t resource;} device_t;
typedef struct {const char *id;} firmware_device_id_t;
typedef struct {const char *name;void *bus;const firmware_device_id_t *firmware_ids;int(*probe)(device_t*);int(*remove)(device_t*);} driver_t;
static int platform_bus;
static const char *bootargs_get(void){return NULL;}
static uint64_t clock_get_ticks(void);
static void udelay(unsigned us);
static void *kcalloc(size_t n,size_t s);
static void kfree(void *p);
static resource_t *device_get_resource(device_t *d,int type,int index){assert(type==RES_MMIO && index==0);return &d->resource;}
static void *device_map_mmio(device_t *d,int index);
static int fw_node_read_u32(firmware_node_t *node,const char *key,uint32_t *hz){(void)node;(void)key;(void)hz;return -ENOENT;}
static int device_probe_use_remove(device_t *d){d->managed=1;return 0;}
static int device_add_execution_action(device_t *d,int(*stop)(void*),void(*release)(void*),void *context);
static uint32_t readl(const volatile void *p);
static void writel(uint32_t v,volatile void *p);
/* HEADER */
/* REGISTRY */
/* PRODUCTION */
#define BASE 0x10000000U
static device_t dev;
static dw_i2c_sim_t sim;
static dw_i2c_controller_t wire;
static dw_i2c_slave_t slave;
static uint64_t ticks;
static unsigned allocated,freed,reads,writes,unmaps;
static int fail_action,fail_map,bad_type,dead,deny_disable,deny_mask,deny_enable;
static int cancel_on_delay,stall_fifo,fail_idle_disable,failed_idle;
static uint64_t clock_get_ticks(void){return ticks;}
static void udelay(unsigned us){
 ticks+=us;assert(ticks<2000000);
 if(cancel_on_delay){cancel_on_delay=0;assert(dev.stop(dev.context)==-EBUSY);}
}
static void *kcalloc(size_t n,size_t size){void *p=calloc(n,size);assert(p);allocated++;return p;}
static void kfree(void *p){assert(p);freed++;free(p);}
static int device_add_execution_action(device_t *d,int(*stop)(void*),void(*release)(void*),void *context){
 assert(d->managed && !d->mapped && d->drv_priv==context);
 if(fail_action)return -ENOMEM;
 d->stop=stop;d->release=release;d->context=context;return 0;
}
static void *device_map_mmio(device_t *d,int index){assert(!index && d->stop && d->context);if(fail_map)return NULL;d->mapped=1;return (void *)BASE;}
static unsigned offset(const volatile void *p){
 assert(dev.mapped && dev.context);uintptr_t a=(uintptr_t)p;
 assert(a>=BASE && a<BASE+DW_SIM_REGS);return a-BASE;
}
static uint32_t readl(const volatile void *p){
 unsigned reg=offset(p);reads++;
 if(dead)return UINT32_MAX;
 if(reg==DW_IC_COMP_TYPE && bad_type)return 0;
 if(reg==DW_IC_STATUS && stall_fifo)return 0;
 if(reg==DW_IC_INTR_MASK && deny_mask)return 1;
 return dw_sim_read(&wire,reg);
}
static void writel(uint32_t v,volatile void *p){
 unsigned reg=offset(p);writes++;assert(!bad_type && dev.managed && dev.stop);
 if(dead)return;
 if(reg==DW_IC_ENABLE){
  if(v==0 && (deny_disable || (fail_idle_disable && sim.enabled))){failed_idle=1;return;}
  if(v==1 && deny_enable)return;
 }
 dw_sim_write(&wire,reg,v);
}
static void reset(void){
 assert(allocated==freed && !dev.context && !dev.drv_priv);
 memset(&dev,0,sizeof(dev));memset(&sim,0,sizeof(sim));memset(&wire,0,sizeof(wire));memset(&slave,0,sizeof(slave));
 dev.resource=(resource_t){BASE,BASE+4095};wire.sim=&sim;sim.slave=&slave;slave.addr=0x2c;
 ticks=reads=writes=unmaps=0;fail_action=fail_map=bad_type=dead=deny_disable=deny_mask=deny_enable=0;
 cancel_on_delay=stall_fifo=fail_idle_disable=failed_idle=0;
}
/* Model only the already-tested core's stop -> remove -> devres ordering. */
static int unbind(void){
 int ret=0;
 if(dev.stop && !dev.stopped){ret=dev.stop(dev.context);if(ret)return ret;dev.stopped=1;}
 ret=dw_i2c_driver.remove(&dev);if(ret)return ret;
 if(dev.mapped){dev.mapped=0;unmaps++;}
 if(dev.release)dev.release(dev.context);
 dev.context=NULL;dev.stop=NULL;dev.release=NULL;return 0;
}
static void retained(void){assert(dev.context && dev.drv_priv==dev.context && dev.mapped && allocated==freed+1 && !unmaps);}
static int transfer(void){uint8_t v[4]={5,0,0,8};i2c_msg_t m={.addr=0x2c,.len=4,.buf=v};return i2c_transfer(0,&m,1);}
int main(void){
 assert(!a20_boot_selftest_i2c_dw()); /* real FIFO/NAK/register protocol baseline */
 /* The self-test allocated no storage. */
 reset();fail_action=1;assert(dw_i2c_driver.probe(&dev)==-ENOMEM);assert(!dev.drv_priv && !dev.mapped);assert(!unbind());
 reset();fail_map=1;assert(dw_i2c_driver.probe(&dev)==-EIO);assert(!unbind() && !unmaps);
 reset();bad_type=1;assert(dw_i2c_driver.probe(&dev)==-ENODEV);assert(!writes);assert(!unbind() && !writes);
 reset();sim.enabled=1;deny_disable=1;assert(dw_i2c_driver.probe(&dev)==-ETIMEDOUT);assert(!i2c_adapter_get(0));
 assert(unbind()==-ETIMEDOUT);retained();deny_disable=0;assert(!unbind() && unmaps==1);
 reset();deny_enable=1;assert(dw_i2c_driver.probe(&dev)==-ETIMEDOUT);assert(!unbind());
 reset();fail_idle_disable=1;assert(dw_i2c_driver.probe(&dev)==-ETIMEDOUT && failed_idle);assert(!i2c_adapter_get(0));
 assert(unbind()==-ETIMEDOUT);retained();fail_idle_disable=0;assert(!unbind());
 /* Successful PIO, then stop with an outstanding transport reference. */
 reset();assert(!dw_i2c_driver.probe(&dev));assert(!sim.enabled);assert(!transfer());
 i2c_adapter_t *held=i2c_adapter_get(0);assert(held);
 assert(unbind()==-EBUSY);retained();unsigned before=reads+writes;
 assert(transfer()==-ESHUTDOWN && reads+writes==before);i2c_adapter_put(held);
 assert(!unbind() && !i2c_adapter_get(0) && unmaps==1);
 /* Stop readback failures retain publication, mapping and exact context. */
 reset();assert(!dw_i2c_driver.probe(&dev));sim.enabled=1;deny_disable=1;
 assert(unbind()==-ETIMEDOUT);retained();before=reads+writes;
 assert(transfer()==-ESHUTDOWN && reads+writes==before);deny_disable=0;deny_mask=1;
 assert(unbind()==-EIO);retained();deny_mask=0;dead=1;
 assert(unbind()==-ENODEV);retained();dead=0;assert(!unbind());
 /* Stop during an active FIFO wait returns busy, closes admission and lets
    the original owner exit WITHOUT re-enabling the controller for recovery. */
 reset();assert(!dw_i2c_driver.probe(&dev));stall_fifo=1;cancel_on_delay=1;
 assert(transfer()==-ESHUTDOWN);retained();assert(!((dw_i2c_controller_t*)dev.context)->lock.held);
 stall_fifo=0;assert(!unbind());
 /* Successful slave work cannot hide failure to return to idle. */
 reset();assert(!dw_i2c_driver.probe(&dev));fail_idle_disable=1;
 assert(transfer()==-ETIMEDOUT && failed_idle);retained();fail_idle_disable=0;assert(!unbind());
 /* An all-ones register file is loss, never successful enable or FIFO data. */
 reset();assert(!dw_i2c_driver.probe(&dev));dead=1;
 assert(transfer()==-ENODEV);assert(unbind()==-ENODEV);retained();dead=0;assert(!unbind());
 assert(allocated==freed);puts("DW_I2C_LIFETIME_OK: managed partial probe, checked stop, active cancellation and adapter references");
}
