/* Full production PS/2 driver; ports, IRQ registry and lifecycle modeled. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define SYN_REPORT 0
#define REL_X 0
#define REL_Y 1
#define INPUT_IOCTL_GET_INFO 1
#define DEV_CLASS_INPUT 2
#define RES_IOPORT 3
#define DRIVER_REGISTER(x)
static void host_log(const char *f,...){(void)f;}
#define kinfo host_log
#define kerr host_log
typedef struct {int held;} spinlock_t;
static void spin_init(spinlock_t *l){l->held=0;}
static uint64_t spin_lock_irqsave(spinlock_t *l){assert(!l->held);l->held=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t f){(void)f;assert(l->held);l->held=0;}
struct input_event {uint64_t time_sec,time_usec;uint16_t type,code;int32_t value;};
struct input_device_info {char name[80];struct {uint16_t bustype,vendor,product,version;} id;};
static void input_info_bit(struct input_device_info *i,unsigned type,unsigned code){(void)i;(void)type;(void)code;}
typedef struct {int type;uint64_t start,end;} resource_t;
typedef struct device {void *drv_priv;resource_t res[2];int res_count,managed,claims,stopped;int(*stop)(void*);void(*release)(void*);void *context;} device_t;
typedef struct {int(*read)(device_t*,void*,size_t);int(*poll)(device_t*,short);int(*ioctl)(device_t*,unsigned long,void*);} input_dev_ops_t;
typedef struct {const char *id;} firmware_device_id_t;
typedef struct {const char *name;const firmware_device_id_t *firmware_ids;void *bus;int(*probe)(device_t*);int(*remove)(device_t*);void(*shutdown)(device_t*);const input_dev_ops_t *class_ops;int class_type;} driver_t;
static int platform_bus;
typedef int(*irq_handler_t)(int,void*);
static uint8_t ioport_read8(uint16_t p);
static void ioport_write8(uint16_t p,uint8_t v);
static void udelay(unsigned us);
static void uart_receive_char(char c);
static unsigned wakes;
void input_mux_wake(void){wakes++;}
static resource_t *device_get_resource(device_t *d,int type,int index){assert(type==RES_IOPORT);return index<d->res_count?&d->res[index]:NULL;}
static int device_probe_use_remove(device_t *d){d->managed=1;return 0;}
static int device_add_execution_action(device_t *d,int(*stop)(void*),void(*release)(void*),void *context);
static int device_claim_resource(device_t *d,const resource_t *r);
static int firmware_legacy_irq_map(int n,int fallback){assert(n==1||n==12);return fallback;}
static int request_irq(uint32_t n,irq_handler_t fn,unsigned long flags,void *data);
static void irq_disable(uint32_t n);
static int synchronize_irq(uint32_t n);
static void free_irq(uint32_t n,void *data);
/* PRODUCTION */
static device_t dev;
static struct {irq_handler_t fn;void *data;unsigned disabled,syncs;} irqs[256];
static uint8_t fifo[1024],config;static unsigned head,tail,port_reads,port_writes,uart_chars,delays,freed_irqs;
static int dead,busy,flood,no_reply,deny_config,fail_action,fail_claim,fail_irq,fail_sync;
static int config_write,mouse_write;
static void push(uint8_t byte){assert(head-tail<1024);fifo[head++%1024]=byte;}
static void udelay(unsigned us){delays+=us;assert(delays<=5000000);}
static void uart_receive_char(char c){assert(c=='a');uart_chars++;}
static uint8_t ioport_read8(uint16_t p){
 assert(dev.managed && dev.stop && dev.claims==2);port_reads++;
 if(dead)return 0xff;
 if(p==PS2_STATUS_PORT)return (busy?PS2_STATUS_INPUT:0)|((head!=tail||flood)?PS2_STATUS_OUTPUT:0);
 assert(p==PS2_DATA_PORT);if(flood)return 0x1e;
 assert(head!=tail);return fifo[tail++%1024];
}
static void ioport_write8(uint16_t p,uint8_t v){
 assert(dev.managed && dev.stop && dev.claims==2);port_writes++;
 if(dead)return;
 if(p==PS2_CMD_PORT){
  switch(v){
   case 0xad:config|=0x10;break;case 0xa7:config|=0x20;break;
   case 0xae:config&=~0x10;break;case 0xa8:config&=~0x20;break;
   case 0x20:if(!no_reply)push(config);break;
   case 0x60:config_write=1;break;case 0xaa:push(0x55);break;
   case 0xab:case 0xa9:push(0);break;case 0xd4:mouse_write=1;break;
   default:assert(0);
  }
 }else{
  assert(p==PS2_DATA_PORT);
  if(config_write){config_write=0;if(!deny_config)config=v;return;}
  if(v==0xff){push(0xfa);push(0xaa);if(mouse_write)push(0);}
  else {assert(v==0xf4);push(0xfa);}
  mouse_write=0;
 }
}
static int device_add_execution_action(device_t *d,int(*stop)(void*),void(*release)(void*),void *context){
 assert(d->managed && d->drv_priv && context==d && !port_reads && !port_writes);
 if(fail_action)return -ENOMEM;
 d->stop=stop;d->release=release;d->context=context;return 0;
}
static int device_claim_resource(device_t *d,const resource_t *r){
 assert(d->stop && (r==&d->res[0]||r==&d->res[1]));
 if(fail_claim==d->claims+1)return -EBUSY;
 d->claims++;return 0;
}
static int request_irq(uint32_t n,irq_handler_t fn,unsigned long flags,void *data){
 assert(n<256 && !flags && data==&dev && !irqs[n].fn);
 if(fail_irq==(int)n)return -ENOMEM;
 irqs[n].fn=fn;irqs[n].data=data;return 0;
}
static void irq_disable(uint32_t n){assert(n<256 && irqs[n].fn);irqs[n].disabled++;}
static int synchronize_irq(uint32_t n){assert(n<256 && irqs[n].disabled);if(fail_sync)return -EIO;irqs[n].syncs++;return 0;}
static void free_irq(uint32_t n,void *data){
 assert(n<256 && data==irqs[n].data && irqs[n].disabled && irqs[n].syncs && g_ps2_stop_complete && (config&0x33)==0x30);
 memset(&irqs[n],0,sizeof(irqs[n]));freed_irqs++;
}
static void reset(void){
 assert(!dev.context && !dev.drv_priv && !g_ps2_owner && !dev.claims);
 memset(&dev,0,sizeof(dev));memset(irqs,0,sizeof(irqs));
 dev.res[0]=(resource_t){RES_IOPORT,0x60,0x60};dev.res[1]=(resource_t){RES_IOPORT,0x64,0x64};dev.res_count=2;
 head=tail=port_reads=port_writes=uart_chars=delays=freed_irqs=wakes=0;config=0x4c;
 dead=busy=flood=no_reply=deny_config=fail_action=fail_claim=fail_irq=fail_sync=config_write=mouse_write=0;
}
static int unbind(void){
 if(dev.stop && !dev.stopped){int ret=dev.stop(dev.context);if(ret)return ret;dev.stopped=1;}
 int ret=ps2_driver.remove(&dev);if(ret)return ret;
 dev.claims=0;if(dev.release)dev.release(dev.context);
 dev.context=NULL;dev.stop=NULL;dev.release=NULL;return 0;
}
static void retained(void){assert(dev.context==&dev && dev.drv_priv==&g_ps2 && g_ps2_owner==&dev && dev.claims==2 && !freed_irqs);}
int main(void){
 reset();dev.res_count=1;assert(ps2_driver.probe(&dev)==-ENODEV && !port_reads);assert(!unbind());
 reset();fail_action=1;assert(ps2_driver.probe(&dev)==-ENOMEM && !port_reads);assert(!unbind());
 for(int n=1;n<=2;n++){reset();fail_claim=n;assert(ps2_driver.probe(&dev)==-EBUSY && !port_reads);assert(!unbind());}
 reset();dead=1;assert(ps2_driver.probe(&dev)==-ENODEV && !port_writes);assert(!unbind());
 /* Bounded controller timeout retains claims; retry owns the same instance. */
 reset();busy=1;assert(ps2_driver.probe(&dev)==-ETIMEDOUT);assert(unbind()==-ETIMEDOUT);retained();busy=0;assert(!unbind());
 reset();fail_irq=IRQ_VECTOR_KEYBOARD;assert(ps2_driver.probe(&dev)==-ENOMEM);busy=1;
 assert(unbind()==-ETIMEDOUT);retained();busy=0;assert(!unbind());
 /* Keyboard remains available without an optional mouse interrupt. */
 reset();fail_irq=PS2_MOUSE_IRQ_VECTOR;assert(!ps2_driver.probe(&dev));assert(g_ps2_mouse_irq==-1);assert(!unbind() && freed_irqs==1);
 reset();assert(!ps2_driver.probe(&dev));assert((config&0x43)==0x43 && (config&0x0c)==0x0c);
 push(0x1e);push(0x9e);assert(irqs[IRQ_VECTOR_KEYBOARD].fn(IRQ_VECTOR_KEYBOARD,&dev)==1);
 struct input_event ev[4];assert(ps2_driver.class_ops->read(&dev,ev,sizeof(ev))==sizeof(ev));
 assert(ev[0].type==EV_KEY && ev[0].code==0x1e && ev[0].value==1 && ev[2].value==0 && uart_chars==1);
 /* A missing/stuck output port cannot trap boot cleanup or IRQ dispatch. */
 flood=1;unsigned reads=port_reads;assert(ps2_flush_output()==-ETIMEDOUT && port_reads-reads==2*PS2_RING_SIZE);
 reads=port_reads;ps2_handle_irq();assert(port_reads-reads==2*PS2_RING_SIZE);flood=0;
 fail_sync=1;assert(unbind()==-EIO);retained();fail_sync=0;
 reads=port_reads;ps2_handle_irq();assert(port_reads==reads);
 busy=1;assert(unbind()==-ETIMEDOUT);retained();busy=0;
 no_reply=1;assert(unbind()==-ETIMEDOUT);retained();no_reply=0;
 deny_config=1;assert(unbind()==-EIO);retained();deny_config=0;
 dead=1;assert(unbind()==-ENODEV);retained();dead=0;
 assert(!unbind() && freed_irqs==2 && !g_ps2_owner && (config&0x33)==0x30);
 /* Failed stop must not permit another device to take the static controller. */
 reset();assert(!ps2_driver.probe(&dev));busy=1;assert(unbind()==-ETIMEDOUT);device_t other={0};assert(ps2_driver.probe(&other)==-EBUSY);busy=0;assert(!unbind());
 puts("PS2_LIFETIME_OK: bounded port/IRQ waits, managed probe, retained failed stop and input roundtrip");
}
