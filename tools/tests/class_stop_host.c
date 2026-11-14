/* Production native core/class receipt and exact devres action release. */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "core/errno.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
unsigned host_cpu;int host_trylock_blocked;
struct task_t *proc_current(void){return NULL;}
static int fail_after=-1,remove_error,removed,resources,probes;
static long allocated;
void *kmalloc(size_t n){if(!fail_after)return NULL;if(fail_after>0)fail_after--;void *p=malloc(n);assert(p);allocated++;return p;}
void *kcalloc(size_t n,size_t z){void *p=kmalloc(n*z);if(p)memset(p,0,n*z);return p;}
void *krealloc(void *p,size_t n){if(!p)return kmalloc(n);return realloc(p,n);}
void kfree(void *p){if(p){allocated--;free(p);}}
void panic(const char *f,...){(void)f;abort();}
void host_log(const char *f,...){(void)f;}
void netlink_uevent_emit(const char *a,const char *b,const char *c,uint64_t d){(void)a;(void)b;(void)c;(void)d;}
int firmware_device_prepare(device_t *d){(void)d;return 0;}
int udriver_mmio_user_owned(uint64_t p){(void)p;return 0;}
int driver_memory_conflict(uint64_t a,uint64_t b){(void)a;(void)b;return 0;}
int driver_memory_intersects(uint64_t a,uint64_t b){(void)a;(void)b;return 0;}
void *arch_device_map_mmio(uint64_t p,size_t n,uint32_t f){(void)p;(void)n;(void)f;return NULL;}
void arch_device_unmap_mmio(void *p,size_t n){(void)p;(void)n;}
__asm__(".global __driver_init_start\n.global __driver_init_end\n__driver_init_start:\n__driver_init_end:\n");
static class_device_t *publication;
static int match(device_t *d){return !strcmp(d->name,"class-stop");}
static void resource_release(void *data){assert(data==(void *)1);if(publication)assert(class_device_binding_stopped(publication));resources++;}
static int plain_probe(device_t *d){(void)d;return 0;}
static int probe(device_t *d){probes++;d->drv_priv=(void *)1;return device_add_resource_action(d,resource_release,(void *)1);}
static int remove_device(device_t *d){assert(d->drv_priv==(void *)1);if(publication)assert(!class_device_binding_stopped(publication));removed++;return remove_error;}
static unsigned action_order[8],actions;
static void action(void *data){action_order[actions++]=(uintptr_t)data;}
static void other_action(void *data){action_order[actions++]=10+(uintptr_t)data;}
int main(int argc,char **argv){
 assert(argc==2);driver_core_init();long baseline=allocated;const char *name=argv[1];
 assert(!class_device_binding_stopped(NULL));
 if(!strcmp(name,"actions")){
  device_t dev={0};driver_lifecycle_begin();assert(device_release_resource_action(NULL,action,0)==-EINVAL&&device_release_resource_action(&dev,NULL,0)==-EINVAL);
  assert(!device_add_resource_action(&dev,action,(void *)1)&&!device_add_resource_action(&dev,action,(void *)2)&&!device_add_resource_action(&dev,other_action,(void *)1));
  assert(!device_release_resource_action(&dev,action,(void *)1)&&actions==1&&action_order[0]==1);
  assert(device_release_resource_action(&dev,action,(void *)1)==-ENOENT&&actions==1);
  device_release_resources(&dev);assert(actions==3&&action_order[1]==11&&action_order[2]==2&&!dev.resource_actions);driver_lifecycle_end();
 }else if(!strcmp(name,"missing-remove")){
  static const char_dev_ops_t ops={0};driver_t drv={.name="class-stop",.match=match,.probe=plain_probe,.class_type=DEV_CLASS_CHAR,.class_ops=&ops};device_t dev={.name="class-stop",.references=1};
  assert(!driver_register(&drv)&&!device_register(&dev));publication=class_device_ref_for_device(&dev);assert(publication);assert(!driver_unregister(&drv)&&!class_device_binding_stopped(publication));assert(!device_unregister(&dev));class_device_put(publication);publication=NULL;device_put(&dev);assert(!dev.references);
 }else if(!strncmp(name,"publish-oom-",12)){
  static const char_dev_ops_t ops={0};driver_t drv={.class_type=DEV_CLASS_CHAR,.class_ops=&ops};device_t dev={.drv=&drv,.references=1};
  fail_after=name[12]-'0';assert(class_device_publish(&dev)==-ENOMEM&&!dev.class_dev&&!dev.resource_actions&&dev.references==1);fail_after=-1;
 }else{
  static const char_dev_ops_t ops={0};driver_t drv={.name="class-stop",.match=match,.probe=probe,.remove=remove_device,.class_type=DEV_CLASS_CHAR,.class_ops=&ops};device_t dev={.name="class-stop",.references=1};
  assert(!driver_register(&drv)&&!device_register(&dev)&&dev.drv==&drv);publication=class_device_ref_for_device(&dev);assert(publication&&!class_device_binding_stopped(publication));
  if(!strcmp(name,"failed-remove")){remove_error=-ETIMEDOUT;assert(driver_unregister(&drv)==-ETIMEDOUT&&dev.drv==&drv&&dev.drv_priv==(void *)1&&!resources&&!class_device_binding_stopped(publication));assert(class_device_call_begin(publication)==-ENODEV);remove_error=0;}
  if(!strcmp(name,"drain")){assert(!class_device_call_begin(publication));class_device_mark_offline(&dev);assert(!class_device_binding_stopped(publication)&&publication->active_calls==1);class_device_call_end(publication);assert(!class_device_binding_stopped(publication));}
  assert(!driver_unregister(&drv)&&!dev.drv&&resources==1&&class_device_binding_stopped(publication));
  if(!strcmp(name,"rebind")){
   class_device_t *old=publication;publication=NULL;assert(!driver_register(&drv)&&dev.drv==&drv);publication=class_device_ref_for_device(&dev);assert(publication&&publication!=old&&!class_device_binding_stopped(publication)&&class_device_binding_stopped(old)&&class_device_call_begin(old)==-ENODEV);class_device_put(old);assert(!driver_unregister(&drv)&&class_device_binding_stopped(publication)&&resources==2);
  }else assert(!strcmp(name,"failed-remove")||!strcmp(name,"drain")||!strcmp(name,"success"));
  assert(!device_unregister(&dev));class_device_put(publication);publication=NULL;device_put(&dev);assert(!dev.references&&!dev.resource_actions);
 }
 assert(allocated==baseline);printf("CLASS_STOP: %s PASS\n",name);return 0;
}
