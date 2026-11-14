/* Production core/class/GPU selection + real consumer bodies; hardware modeled. */
#include "drivers/gpu/gpu_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/gpu/framebuffer.h"
#include "core/errno.h"
#include "core/consts.h"
#define ROUND_UP(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
unsigned host_cpu;
static _Thread_local char task_identity;
struct task_t *proc_current(void) {return (void *)&task_identity;}
void *kmalloc(size_t n){return malloc(n);}
void *kcalloc(size_t n,size_t z){return calloc(n,z);}
void *krealloc(void *p,size_t n){return realloc(p,n);}
void kfree(void *p){free(p);}
void panic(const char *fmt,...){(void)fmt;abort();}
void host_log(const char *fmt,...){(void)fmt;}
int firmware_device_prepare(device_t *d){(void)d;return 0;}
void *arch_device_map_mmio(uint64_t p,size_t n,uint32_t f){(void)p;(void)n;(void)f;return NULL;}
void arch_device_unmap_mmio(void *p,size_t n){(void)p;(void)n;}
void netlink_uevent_emit(const char *a,const char *s,const char *n,uint64_t d){(void)a;(void)s;(void)n;(void)d;}
_Thread_local int watch_mutex;
int mutex_waiting;
static _Thread_local int delay_lookup;
static void pause_call(void);
static class_device_t *delayed_class_ref(const device_t *d) {
 if(delay_lookup)pause_call();return class_device_ref_for_device(d);
}
__asm__(".global __driver_init_start\n.global __driver_init_end\n__driver_init_start:\n__driver_init_end:\n");
#define class_device_ref_for_device delayed_class_ref
#include "drivers/gpu/gpu_core.c"
#undef class_device_ref_for_device

struct sample {
 device_t device;driver_t driver;gpu_dev_ops_t *ops;
 unsigned id;_Atomic unsigned reads,flushes,removes;int info_error,fb_error,platform;
};
static _Atomic unsigned destroyed,created;
static pthread_mutex_t rendezvous=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int pause_info,pause_identity,entered,proceed;
static void pause_call(void) {
 assert(!pthread_mutex_lock(&rendezvous));entered=1;assert(!pthread_cond_broadcast(&changed));
 while(!proceed)assert(!pthread_cond_wait(&changed,&rendezvous));assert(!pthread_mutex_unlock(&rendezvous));
}
static int info(device_t *d,uint32_t *w,uint32_t *h,uint32_t *b) {
 struct sample *s=d->plat_data;assert(d->drv==&s->driver);s->reads++;
 if(pause_info)pause_call();if(s->info_error)return s->info_error;
 *w=640+s->id;*h=480+s->id;*b=32;return 0;
}
static int framebuffer(device_t *d,uintptr_t *physical,size_t *size) {
 struct sample *s=d->plat_data;if(s->fb_error)return s->fb_error;
 *physical=0x100000+s->id*65536;*size=4096;return 0;
}
static int flush(device_t *d,uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
 struct sample *s=d->plat_data;assert(!x&&!y&&w==640+s->id&&h==480+s->id);s->flushes++;return 0;
}
static void destroy(device_t *d) {
 assert(!pthread_mutex_trylock(&g_gpu_lock.native));assert(!pthread_mutex_unlock(&g_gpu_lock.native));
 struct sample *s=d->plat_data;destroyed++;free(s->ops);free(s);
}
static int match(device_t *d,const driver_t *r){return &((struct sample *)d->plat_data)->driver==r;}
static int probe(device_t *d){return gpu_device_register(d);}
static int plain_probe(device_t *d){(void)d;return 0;}
static int remove_gpu(device_t *d){((struct sample *)d->plat_data)->removes++;gpu_device_unregister(d);return 0;}
static bus_type_t bus={.name="gpu-test",.match=match};
static struct sample *create(unsigned id,int capable) {
 struct sample *s=calloc(1,sizeof(*s));assert(s);s->id=id;created++;s->ops=calloc(1,sizeof(*s->ops));assert(s->ops);
 *s->ops=(gpu_dev_ops_t){.get_info=info,.get_fb=framebuffer,.flush=flush};
 /* Only presence is inspected; these 3D transport entries are never called. */
 if(capable>0){s->ops->get_capset=(void *)1;s->ops->capset_info=(void *)1;s->ops->resource_attach_backing=(void *)1;
  s->ops->ctx_create=(void *)1;s->ops->ctx_destroy=(void *)1;s->ops->resource_create_3d=(void *)1;
  s->ops->resource_unref=(void *)1;s->ops->submit_3d=(void *)1;}
 s->driver=(driver_t){.name="gpu-test",.bus=&bus,.probe=capable<0?plain_probe:probe,
  .remove=remove_gpu,.class_type=capable<0?DEV_CLASS_NET:DEV_CLASS_DISPLAY,.class_ops=s->ops};
 s->device=(device_t){.name="display-test",.bus=&bus,.references=1,.release=destroy,.plat_data=s};
 assert(!driver_register(&s->driver)&&!device_register(&s->device)&&s->device.drv==&s->driver);return s;
}
static void dispose(struct sample *s) {
 assert(!driver_unregister(&s->driver));assert(!device_unregister(&s->device));device_put(&s->device);
}
static void *withdraw(void *arg){struct sample *s=arg;assert(!driver_unregister(&s->driver));return NULL;}
static void *lookup_after_capture(void *arg){(void)arg;delay_lookup=1;assert(!gpu_device_default_get());return NULL;}
static int mutation_result,mutation_done;
static void *register_after_removal(void *arg) {
 struct sample *s=arg;watch_mutex=1;mutation_result=gpu_device_register(&s->device);watch_mutex=0;
 __atomic_store_n(&mutation_done,1,__ATOMIC_RELEASE);device_put(&s->device);return NULL;
}
static int reader_result,reader_done;
static void *try_reader(void *arg) {
 (void)arg;struct gpu_device_access access;reader_result=gpu_device_default_begin(&access);
 assert(reader_result==-ENODEV&&!access.device&&!access.ops&&!access.publication);
 assert(!pthread_mutex_lock(&rendezvous));reader_done=1;assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&rendezvous));return NULL;
}

/* PCI and userspace copy are simulated; every metadata access must still
 * hold a class call, including names/config generated during promotion. */
static void require_access(device_t *d) {assert(d&&d->class_dev&&d->class_dev->active_calls);}
static int pci_device_location(device_t *d,unsigned *b,unsigned *slot,unsigned *fn) {
 if(!d)return -ENODEV;require_access(d);struct sample *s=d->plat_data;if(s->platform)return -ENODEV;
 if(pause_identity)pause_call();if(b)*b=0;if(slot)*slot=s->id;if(fn)*fn=0;return 0;
}
static uint32_t pci_class_code(device_t *d){require_access(d);return 0x030000;}
static uint16_t pci_cfg_read16(device_t *d,uint32_t reg){require_access(d);return (uint16_t)(((struct sample *)d->plat_data)->id*256+reg);}
static uint8_t pci_cfg_read8(device_t *d,uint32_t reg){return pci_cfg_read16(d,reg);}
static uint32_t pci_cfg_read32(device_t *d,uint32_t reg){return pci_cfg_read16(d,reg);}
static int copy_to_user(void *out,const void *in,size_t n){if(!out)return -1;memcpy(out,in,n);return 0;}
static int copy_from_user(void *out,const void *in,size_t n){if(!in)return -1;memcpy(out,in,n);return 0;}
static void fb_autoflush_kick(void){}
typedef void vfile_t;
typedef struct {unsigned loopback_up,loopback_mtu;} resource_namespace_t;
static int sysfs_devchar_name(uint64_t d,char *name,size_t n,const char **sub){(void)d;(void)name;(void)n;(void)sub;assert(0);return -1;}
/* PRODUCTION_CONSUMERS */
static sysfs_priv_t *snapshot;
static void *read_metadata(void *arg){(void)arg;snapshot=sysfs_priv_create(NULL,SF_DRM_MODALIAS,0,0,0,0);return NULL;}
static void *refresh(void *arg){(void)arg;fb_autoflush_once();return NULL;}
static void wait_enter(void) {assert(!pthread_mutex_lock(&rendezvous));while(!entered)assert(!pthread_cond_wait(&changed,&rendezvous));assert(!pthread_mutex_unlock(&rendezvous));}
static void resume_call(void) {assert(!pthread_mutex_lock(&rendezvous));proceed=1;assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&rendezvous));}

int main(int argc,char **argv) {
 assert(argc==2);const char *name=argv[1];driver_core_init();assert(!bus_register(&bus));
 if(!strcmp(name,"arguments")) {
  struct gpu_device_access a={(void *)1,(void *)1,(void *)1};
  assert(!gpu_device_default_get()&&gpu_device_default_begin(&a)==-ENODEV&&!a.device&&!a.ops&&!a.publication);
  assert(gpu_device_access_begin(NULL,&a)==-ENODEV&&gpu_device_access_begin(NULL,NULL)==-EINVAL);
  assert(gpu_device_default_begin(NULL)==-EINVAL&&gpu_device_register(NULL)==-EINVAL);
  gpu_device_access_end(&a);gpu_device_access_end(&a);gpu_device_access_end(NULL);
  gpu_dev_ops_t ops={0};driver_t r={.class_type=DEV_CLASS_DISPLAY,.class_ops=&ops};device_t d={.drv=&r,.references=1};
  assert(gpu_device_register(&d)==-ENODEV&&d.references==1);
  struct sample *network=create(1,-1);class_device_t *c=class_device_ref_for_device(&network->device);
  assert(c&&gpu_device_access_begin(c,&a)==-ENODEV&&!a.ops&&!c->active_calls);
  assert(gpu_device_register(&network->device)==-EINVAL);class_device_put(c);dispose(network);
 } else if(!strcmp(name,"captured-device")) {
  struct sample *s=create(1,0);pthread_t thread;assert(!pthread_create(&thread,NULL,lookup_after_capture,NULL));
  wait_enter();dispose(s);assert(!destroyed);resume_call();assert(!pthread_join(thread,NULL)&&destroyed==1);
 } else if(!strcmp(name,"mutation")) {
  struct sample *s=create(1,0);assert(device_get(&s->device));driver_lifecycle_begin();pthread_t thread;
  assert(!pthread_create(&thread,NULL,register_after_removal,s));
  while(!__atomic_load_n(&mutex_waiting,__ATOMIC_ACQUIRE)){assert(!__atomic_load_n(&mutation_done,__ATOMIC_ACQUIRE));sched_yield();}
  assert(!__atomic_load_n(&mutation_done,__ATOMIC_ACQUIRE));dispose(s);assert(!destroyed);
  driver_lifecycle_end();assert(!pthread_join(thread,NULL)&&mutation_result==-ENODEV&&destroyed==1);
 } else if(!strcmp(name,"selection")) {
  struct sample *a=create(1,0),*b=create(2,0);class_device_t *old=gpu_device_default_get();assert(old->dev==&a->device);
  unsigned references=a->device.references;assert(!gpu_device_register(&a->device)&&a->device.references==references);
  struct sample *c=create(3,1),*d=create(4,1);assert(a->device.references==references-1);
  struct gpu_device_access call;assert(!gpu_device_default_begin(&call)&&call.device==&c->device);gpu_device_access_end(&call);
  assert(!gpu_device_access_begin(old,&call)&&call.device==&a->device);class_device_put(old);
  uint32_t w,h,bpp;assert(!call.ops->get_info(call.device,&w,&h,&bpp)&&w==641);gpu_device_access_end(&call);
  dispose(c);assert(!gpu_device_default_begin(&call)&&call.device==&d->device);gpu_device_access_end(&call);
  dispose(d);dispose(b);dispose(a);
 } else if(!strcmp(name,"election")) {
  struct sample *all[21];all[0]=create(1,1);
  /* Unpublish swaps the last registry entry into the departing slot. Keep
   * a 2D last entry so the live 3D fallback stays beyond the old limit. */
  for(unsigned i=1;i<=20;i++)all[i]=create(i+1,i==1||i==19);
  driver_lifecycle_begin();class_device_mark_offline(&all[1]->device);driver_lifecycle_end();
  /* Both rank APIs skip the retained offline publication. Its persistent
   * index is still not reusable and cannot be admitted through index lookup. */
  assert(!class_device_get_by_index(DEV_CLASS_DISPLAY,all[1]->device.class_dev->index));
  for(unsigned i=0;i<20;i++){
   device_t *d=device_find_get_by_class(DEV_CLASS_DISPLAY,i);assert(d&&d!=&all[1]->device);device_put(d);
   class_device_t *c=class_device_get_nth(i);assert(c&&c->dev!=&all[1]->device);class_device_put(c);
  }
  assert(!device_find_get_by_class(DEV_CLASS_DISPLAY,20)&&!class_device_get_nth(20));
  dispose(all[0]);struct gpu_device_access call;assert(!gpu_device_default_begin(&call)&&call.device==&all[19]->device);gpu_device_access_end(&call);
  for(unsigned i=1;i<=20;i++)dispose(all[i]);
 } else if(!strcmp(name,"drain")) {
  struct sample *s=create(1,0);class_device_t *old=gpu_device_default_get();struct gpu_device_access held;
  assert(!gpu_device_access_begin(old,&held));pthread_t thread,reader;assert(!pthread_create(&thread,NULL,withdraw,s));
  while(__atomic_load_n(&old->online,__ATOMIC_ACQUIRE))sched_yield();assert(!s->removes);
  assert(!pthread_create(&reader,NULL,try_reader,NULL));assert(!pthread_mutex_lock(&rendezvous));
  struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec++;
  while(!reader_done)assert(!pthread_cond_timedwait(&changed,&rendezvous,&deadline));assert(!pthread_mutex_unlock(&rendezvous));
  assert(!pthread_join(reader,NULL));gpu_device_access_end(&held);assert(!pthread_join(thread,NULL)&&s->removes==1);
  free(s->ops);s->ops=NULL;s->device.drv=(void *)1;
  assert(gpu_device_access_begin(old,&held)==-ENODEV&&!held.ops);s->device.drv=NULL;
  assert(!device_unregister(&s->device));device_put(&s->device);assert(!destroyed);class_device_put(old);assert(destroyed==1);
 } else if(!strcmp(name,"rebind")) {
  struct sample *s=create(1,0);class_device_t *old=gpu_device_default_get();assert(!driver_unregister(&s->driver));
  assert(!driver_register(&s->driver));class_device_t *current=gpu_device_default_get();assert(current&&current!=old);
  struct gpu_device_access call;assert(gpu_device_access_begin(old,&call)==-ENODEV);
  assert(!gpu_device_access_begin(current,&call));gpu_device_access_end(&call);class_device_put(current);class_device_put(old);dispose(s);
 } else if(!strcmp(name,"metadata-pin")) {
  struct sample *s=create(1,0);s->driver.remove=NULL;assert(!driver_unregister(&s->driver));
  free(s->ops);s->ops=NULL;assert(!device_unregister(&s->device));device_put(&s->device);
  assert(!destroyed&&!gpu_device_default_get());struct gpu_device_access call;assert(gpu_device_default_begin(&call)==-ENODEV);
  assert(device_get(&s->device));struct sample *replacement=create(2,1);
  gpu_device_unregister(&s->device);device_put(&s->device);assert(destroyed==1);dispose(replacement);
 } else if(!strcmp(name,"promotion-refresh")||!strcmp(name,"promotion-sysfs")) {
  struct sample *a=create(1,0);pthread_t thread;
  if(!strcmp(name,"promotion-refresh")){pause_info=1;assert(!pthread_create(&thread,NULL,refresh,NULL));}
  else{pause_identity=1;assert(!pthread_create(&thread,NULL,read_metadata,NULL));}
  wait_enter();struct sample *b=create(2,1);resume_call();assert(!pthread_join(thread,NULL));
  if(pause_info)assert(a->flushes==1&&!b->flushes);
  else{assert(snapshot&&!strncmp(snapshot->content,"pci:v00000100d00000102",21));free(snapshot);}
  pause_info=pause_identity=0;dispose(b);dispose(a);
 } else if(!strcmp(name,"fb")) {
  struct sample *s=create(1,0);class_device_t *c=gpu_device_default_get();struct fb_var_screeninfo var;
  assert(!fb_ioctl(NULL,FBIOGET_VSCREENINFO,&var)&&var.xres==641&&!c->active_calls);
  assert(fb_ioctl(NULL,FBIOGET_VSCREENINFO,NULL)==-EFAULT&&!c->active_calls);
  s->info_error=-EIO;assert(fb_ioctl(NULL,FBIO_FLUSH,NULL)==-EIO&&!c->active_calls);s->info_error=0;
  s->ops->flush=NULL;assert(fb_ioctl(NULL,FBIO_FLUSH,NULL)==-EOPNOTSUPP&&!c->active_calls);s->ops->flush=flush;
  s->fb_error=-EIO;assert(fbdev_linux_mmap(0,4096,0,0,0)==-EIO&&!c->active_calls);s->fb_error=0;
  assert(fbdev_linux_mmap(0,4096,0,0,1)==-EINVAL&&!c->active_calls);
  assert(fbdev_linux_mmap(0,0,0,0,0)==-EINVAL&&!c->active_calls);
  assert(fbdev_linux_mmap(0,4096,0,0,0)==0x110000&&!c->active_calls);
  class_device_put(c);dispose(s);
 } else if(!strcmp(name,"sysfs")) {
  struct sample *s=create(1,0);class_device_t *c=gpu_device_default_get();
  sf_type_t types[]={SF_DRM_MODALIAS,SF_DEV_CHAR_DEVICE_ATTR,SF_DEV_CHAR_DEVICE_CONFIG,SF_DEV_CHAR_DEVICE_UEVENT,SF_FB_BPP,SF_FB_STRIDE,SF_FB_VSIZE};
  for(unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++) {
   sysfs_priv_t *p=sysfs_priv_create(NULL,types[i],0,0,0,0);assert(p&&!c->active_calls&&p->content_len);free(p);
  }
  s->platform=1;sysfs_priv_t *p=sysfs_priv_create(NULL,SF_DEV_CHAR_DEVICE_CONFIG,0,0,0,0);assert(p&&!p->content_len&&!c->active_calls);free(p);
  char root[32],node[64];assert(!sysfs_gpu_names(root,sizeof(root),node,sizeof(node))&&!strcmp(root,"platform")&&!c->active_calls);
  uint32_t w,h;sysfs_display_size(&w,&h);assert(w==641&&h==481&&!c->active_calls);
  class_device_put(c);dispose(s);
 } else assert(0);
 assert(!gpu_device_default_get()&&destroyed==created);bus_unregister(&bus);return 0;
}
