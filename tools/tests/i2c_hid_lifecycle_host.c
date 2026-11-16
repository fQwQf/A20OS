/* Production probe/stop/release/I/O/rearm; protocol, worker and core modeled. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define EPROBE_DEFER 517
#define MS_TO_TICKS(x) ((uint64_t)(x))
#define I2C_HID_MAX_REPORT_DESC 4096U
#define I2C_HID_MAX_INPUT 256U
#define I2C_HID_POWER_ON 0U
#define SYNA_VENDOR_ID 0x6cbU
#define SYNA_CE78_PRODUCT_ID 0xce78U
#define SYNA_CE78_TOUCH_REPORT 3U
#define KLOG_INFO 0
static void host_log(const char *f,...){(void)f;}
#define kinfo host_log
#define kerr host_log
#define kwarn host_log
#define klog_record_only(level,...) host_log(__VA_ARGS__)
typedef struct {int held;} spinlock_t;
typedef struct {int held;} mutex_t;
#define SPINLOCK_INIT {0}
static void spin_init(spinlock_t *m){m->held=0;}
static void mutex_init(mutex_t *m){m->held=0;}
static uint64_t spin_lock_irqsave(spinlock_t *m){assert(!m->held);m->held=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t *m,uint64_t f){(void)f;assert(m->held);m->held=0;}
static void mutex_lock(mutex_t *m){assert(!m->held);m->held=1;}
static void mutex_unlock(mutex_t *m){assert(m->held);m->held=0;}
typedef struct {int id;} fw_node_t;
typedef struct device {void *drv_priv;struct device *parent;fw_node_t firmware_node;int managed,dependency,stopped;int(*stop)(void*);void(*release)(void*);void *context;} device_t;
/* HEADER */
/* REGISTRY */
struct driver_workqueue {unsigned marker;};
struct driver_work {void (*callback)(void*);void *context;};
typedef struct i2c_hid_device {
 device_t *dev;i2c_adapter_t *adapter;uint16_t addr;
 struct {uint16_t report_desc_length,report_desc_register,max_input_length,vendor_id,product_id;} hd;
 uint8_t *report_desc,*input_buf;uint16_t input_buf_len;
 struct {const volatile uint32_t *reg;} attention;
 uint32_t poll_ms;spinlock_t event_lock;mutex_t io_lock;
 int f_x,f_y,f_tip,f_button,f_contact_id,closing,present,running,uses_report_ids;
 uint16_t max_report_bytes;unsigned active_report_id,field_count,first_event_logged,recovery_pending;
 uint64_t report_at,reads,valid_reports,read_errors,gpio_skips,touch_frames,movement_frames,queue_overflows,recoveries;
 struct driver_workqueue *queue;struct driver_work *work;
} i2c_hid_device_t;
static unsigned allocations,freed,allocation_calls,fail_allocation,work_puts,queue_allocated,queue_freed;
static unsigned io_calls,schedules,joined,attention_reads;
static int ihid_scheduler_ready=1,fail_dependency,fail_action,fail_descriptor,fail_read,fail_choose;
static int fail_queue,fail_work,fail_schedule,join_error;
static uint32_t g_ihid_boot_poll_ms;
static device_t dev,parent;
static i2c_adapter_t adapter;
static uint64_t timer_get_ticks(void){return 1;}
static uint32_t readl(const volatile void *p){assert(p);attention_reads++;return 0;}
static void mdelay(unsigned ms){assert(ms==60);}
static void *allocate(size_t n){allocation_calls++;if(allocation_calls==fail_allocation)return NULL;void *p=calloc(1,n);assert(p);allocations++;return p;}
static void *kcalloc(size_t n,size_t s){return allocate(n*s);}
static void *kmalloc(size_t n){return allocate(n);}
static void kfree(void *p){if(p){freed++;free(p);}}
static int device_probe_use_remove(device_t *d){d->managed=1;return 0;}
static int device_add_dependency(device_t *d,device_t *p){assert(p==&parent);if(fail_dependency)return -ENOMEM;d->dependency=1;return 0;}
static int device_add_execution_action(device_t *d,int(*stop)(void*),void(*release)(void*),void *context){
 assert(d->managed && d->dependency && d->drv_priv==context && !io_calls);
 if(fail_action)return -ENOMEM;
 d->stop=stop;d->release=release;d->context=context;return 0;
}
static void ihid_parse_cmdline(void){}
static void ihid_poll_work(void *context);
static int ihid_probe_descriptor(i2c_hid_device_t *h,const fw_node_t *node){
 (void)node;assert(h->adapter==&adapter && dev.stop && dev.context==h);io_calls++;
 if(fail_descriptor)return -ENXIO;
 h->hd.report_desc_length=8;h->hd.max_input_length=8;return 0;
}
static int ihid_set_power(i2c_hid_device_t *h,unsigned state){assert(h && state==I2C_HID_POWER_ON);io_calls++;return 0;}
static int ihid_read_reg(i2c_hid_device_t *h,uint16_t reg,void *buffer,uint16_t n){assert(h && !reg && buffer && n==8);io_calls++;return fail_read?-ENXIO:0;}
static void hid_parse_report_descriptor(i2c_hid_device_t *h){h->max_report_bytes=8;h->field_count=2;}
static int ihid_choose_report(i2c_hid_device_t *h){assert(h);return fail_choose?-ENODEV:0;}
static void ihid_setup_attention(i2c_hid_device_t *h,const fw_node_t *node){(void)h;(void)node;}
static int ihid_reset(i2c_hid_device_t *h){assert(h);io_calls++;return 0;}
static int ihid_configure_synaptics(i2c_hid_device_t *h){assert(h);return 0;}
static int ihid_synaptics_check_features(i2c_hid_device_t *h){assert(h);return 0;}
static int ihid_poll_device(i2c_hid_device_t *h){assert(h);return 0;}
static struct driver_workqueue *driver_workqueue_create(void){if(fail_queue)return NULL;struct driver_workqueue *p=calloc(1,sizeof(*p));assert(p);queue_allocated++;return p;}
static struct driver_work *driver_work_create(struct driver_workqueue *q,void(*callback)(void*),void *context){
 assert(q && callback==ihid_poll_work && context==dev.context);if(fail_work)return NULL;
 struct driver_work *w=calloc(1,sizeof(*w));assert(w);w->callback=callback;w->context=context;return w;
}
static int driver_work_schedule(struct driver_work *w,uint64_t deadline){assert(w && deadline<20000);schedules++;return fail_schedule?-ESHUTDOWN:1;}
static int driver_workqueue_destroy(struct driver_workqueue **q,uint64_t deadline){
 assert(q && *q && deadline==2001);if(join_error)return join_error;
 free(*q);*q=NULL;queue_freed++;joined++;return 0;
}
static void driver_work_put(struct driver_work *w){assert(w && joined);work_puts++;free(w);}
/* CALLBACKS */
static int transport(i2c_adapter_t *a,i2c_msg_t *messages,int n){assert(a==&adapter && messages && n==1);io_calls++;return 0;}
static void reset(void){
 assert(allocations==freed && queue_allocated==queue_freed && !dev.context && !dev.drv_priv);
 assert(!i2c_adapter_remove(&adapter));memset(&dev,0,sizeof(dev));dev.parent=&parent;
 allocation_calls=fail_allocation=io_calls=schedules=joined=attention_reads=0;
 fail_dependency=fail_action=fail_descriptor=fail_read=fail_choose=fail_queue=fail_work=fail_schedule=join_error=0;
 adapter=(i2c_adapter_t){.nr=0,.transfer=transport};assert(!i2c_adapter_add_for_device(&parent,&adapter));
}
/* Core ordering modeled; the adapter registry and driver paths above are real. */
static int unbind(void){
 if(dev.stop && !dev.stopped){int ret=dev.stop(dev.context);if(ret)return ret;dev.stopped=1;}
 int ret=ihid_remove(&dev);if(ret)return ret;
 if(dev.release)dev.release(dev.context);
 dev.context=NULL;dev.stop=NULL;dev.release=NULL;dev.dependency=0;return 0;
}
static void retained(void){assert(dev.context && dev.drv_priv==dev.context && dev.dependency);assert(i2c_adapter_remove(&adapter)==-EBUSY);}
int main(void){
 reset();fail_dependency=1;assert(ihid_probe(&dev)==-ENOMEM && !dev.drv_priv);assert(!unbind());
 reset();fail_allocation=1;assert(ihid_probe(&dev)==-ENOMEM && !dev.drv_priv);assert(!unbind());
 reset();fail_action=1;assert(ihid_probe(&dev)==-ENOMEM && !dev.drv_priv && !io_calls);assert(!unbind());
 /* Descriptor and buffer failures all clean through the same managed owner. */
 reset();fail_descriptor=1;assert(ihid_probe(&dev)==-ENXIO);retained();assert(!unbind());
 for(unsigned n=2;n<=3;n++){reset();fail_allocation=n;assert(ihid_probe(&dev)==-ENOMEM);retained();assert(!unbind());}
 reset();fail_read=1;assert(ihid_probe(&dev)==-EIO);retained();assert(!unbind());
 reset();fail_choose=1;assert(ihid_probe(&dev)==-ENODEV);retained();assert(!unbind());
 reset();fail_queue=1;assert(ihid_probe(&dev)==-ENOMEM);retained();assert(!unbind());
 /* A live empty worker survives a failed work allocation and failed join. */
 reset();fail_work=1;assert(ihid_probe(&dev)==-ENOMEM);retained();
 unsigned before=freed;join_error=-EINTR;assert(unbind()==-EINTR);retained();assert(freed==before);
 join_error=0;assert(!unbind());
 /* A failed first submission is a failed probe, never a ready input node. */
 reset();fail_schedule=1;assert(ihid_probe(&dev)==-ESHUTDOWN);retained();
 join_error=-ETIMEDOUT;assert(unbind()==-ETIMEDOUT);retained();join_error=0;assert(!unbind());
 /* Stop must not wait for io_lock held by a callback before closing I/O. */
 reset();assert(!ihid_probe(&dev));i2c_hid_device_t *h=dev.context;
 assert(ihid_remove(&dev)==-EBUSY);h->io_lock.held=1;join_error=-ETIMEDOUT;before=freed;
 assert(unbind()==-ETIMEDOUT && h->closing && freed==before);retained();
 uint8_t byte;i2c_msg_t msg={.buf=&byte,.len=1};unsigned calls=io_calls,queued=schedules;
 assert(ihid_xfer(h,&msg,1)==-ESHUTDOWN && io_calls==calls);
 h->io_lock.held=0;ihid_poll_work(h);assert(schedules==queued && !attention_reads);
 join_error=0;assert(!unbind());assert(!i2c_adapter_remove(&adapter));
 assert(allocations==freed && queue_allocated==queue_freed && work_puts==2);
 puts("I2C_HID_LIFETIME_OK: partial probe, failed worker join retained, I/O closed and no rearm");
}
