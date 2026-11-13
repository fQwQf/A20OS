/* Production core/class/I2C registry: failed teardown keeps live storage. */
#include "drivers/i2c/i2c.h"
static driver_t *fallback_driver;
static const device_id_t retained_match = {.vendor=1,.device=1};
static unsigned fallback_calls, hotplug_removes, metadata_freed;
static void release_metadata(device_t *d) {metadata_freed++;kfree(d);}
struct remove_state {
 driver_t *target;
 device_t *provider;
 class_device_t *stale;
 unsigned probes, quiesces, attempts, removed, released, offline;
 int error, use_i2c, allow_fallback;
 void *buffer;
 device_t *late;
 int reenter;
 int late_error;
 int provider_pending;
 i2c_adapter_t adapter;
};
static void release_storage(void *opaque) {
 struct remove_state *s=opaque;assert(s->removed==1&&!s->buffer);s->released++;
}
static int dummy_transfer(i2c_adapter_t *a,i2c_msg_t *m,int n) {(void)a;(void)m;return n;}
static int remove_probe(device_t *dev) {
 struct remove_state *s=dev->plat_data;s->probes++;
 if(s->provider) {
  if(!device_is_registered(s->provider))return -EPROBE_DEFER;
  int ret=device_add_dependency(dev,s->provider);if(ret)return ret;
 }
 s->buffer=kmalloc(64);assert(s->buffer);memset(s->buffer,0x5a,64);dev->drv_priv=s;
 assert(!device_add_resource_action(dev,release_storage,s));
 assert(!device_claim_resource(dev,&dev->res[0]));
 if(s->use_i2c) {
  s->adapter=(i2c_adapter_t){.nr=0,.transfer=dummy_transfer};
  assert(!i2c_adapter_add_for_device(dev,&s->adapter));
 }
 return device_set_devfs_name(dev,"remove-test");
}
static void remove_quiesce(device_t *dev) {
 struct remove_state *s=dev->drv_priv;s->quiesces++;
 if(s->stale&&s->stale->active_calls)class_device_call_end(s->stale);
}
static int remove_callback(device_t *dev) {
 struct remove_state *s=dev->drv_priv;assert(s&&s->buffer&&!dev->class_dev);
 assert(s->quiesces==1&&!s->released);s->attempts++;
 if(s->provider_pending) {
  device_t fresh={.references=1};
  assert(device_add_dependency(&fresh,s->provider)==-EPROBE_DEFER&&!fresh.dependencies);
 }
 if(s->reenter) {
  assert(device_unregister(dev)==-EBUSY);
  assert(driver_unregister(s->target)==-EBUSY);
 }
 if(s->late) {
  assert(device_register(s->late)==s->late_error&&!s->late->drv);
  if(!s->late_error)assert(bus_probe_device(s->late)==-ENODEV);
  else assert(!device_is_registered(s->late)&&!s->late->parent_reference);
  s->late=NULL;
 }
 if(s->error)return s->error;
 if(s->use_i2c){int ret=i2c_adapter_remove(&s->adapter);if(ret)return ret;}
 assert(((unsigned char *)s->buffer)[0]==0x5a);kfree(s->buffer);s->buffer=NULL;s->removed++;
 dev->drv_priv=NULL;return 0;
}
static int remove_matches(device_t *dev,const driver_t *drv) {
 struct remove_state *s=dev->plat_data;
 int match=drv==s->target||(s->allow_fallback&&drv==fallback_driver);
 if(match)dev->matched_id=&retained_match;
 return match;
}
static int fallback_probe(device_t *dev) {(void)dev;fallback_calls++;return -ENODEV;}
static void remove_hotplug(device_t *dev,int event) {assert(dev->name&&!metadata_freed);if(event==BUS_EVENT_REMOVE)hotplug_removes++;}
static void remove_offline(class_device_t *c,void *opaque) {
 struct remove_state *s=opaque;assert(!c->online);s->offline++;
}
static int dummy_read(device_t *d,void *p,size_t n) {(void)d;(void)p;return n;}
static void check_retained(device_t *d,struct remove_state *s,driver_t *drv) {
 assert(device_is_registered(d)&&d->state==DEV_STATE_REMOVING&&d->drv==drv&&d->drv_priv==s);
 assert(d->matched_id==&retained_match);
 assert(s->buffer&&!s->released&&s->quiesces==1&&!d->class_dev);
 driver_resource_claim_t *claim=NULL;resource_t range=d->res[0];device_t other={.references=1};
 assert(driver_resource_request(&other,&range,&claim)==-EBUSY&&!claim);
 assert(class_device_ref_for_device(d)==NULL);
 device_t fresh={.references=1};assert(device_add_dependency(&fresh,d)==-EPROBE_DEFER&&!fresh.dependencies);
}
int main(int argc,char **argv) {
 assert(argc==2);driver_core_init();
 bus_type_t bus={.name="remove-test",.match=remove_matches,.hotplug=remove_hotplug};assert(!bus_register(&bus));
 char_dev_ops_t ops={.read=dummy_read};
 driver_t driver={.name="remove-test",.bus=&bus,.probe=remove_probe,.quiesce=remove_quiesce,
  .remove=remove_callback,.class_type=DEV_CLASS_CHAR,.class_ops=&ops};
 driver_t second=driver;second.name="second";
 struct remove_state a={.target=&driver},b={.target=&driver};
 resource_t ar={.type=RES_MMIO,.start=0x81000000,.end=0x81000fff};
 resource_t br={.type=RES_MMIO,.start=0x82000000,.end=0x82000fff};
 device_t x={.name="remove-a",.bus=&bus,.references=1,.plat_data=&a,.res=&ar,.res_count=1};
 device_t y={.name="remove-b",.bus=&bus,.references=1,.plat_data=&b,.res=&br,.res_count=1};
 assert(!driver_register(&driver));
 if(!strcmp(argv[1],"arguments")) {
  assert(device_unregister(NULL)==-EINVAL&&device_unregister(&x)==-ENOENT);
  assert(driver_unregister(NULL)==-EINVAL&&driver_unregister(&second)==-ENOENT);
 } else if(!strcmp(argv[1],"publication")) {
  second.probe=fallback_probe;fallback_driver=&second;b.allow_fallback=1;b.error=-ETIMEDOUT;
  assert(!driver_register(&second));assert(!device_register(&x));assert(!device_register(&y));
  check_retained(&y,&b,&driver);assert(y.probe_error==-EEXIST&&!fallback_calls&&b.probes==1);
  driver_probe_all();assert(b.probes==1&&!fallback_calls);
  b.error=0;assert(!device_unregister(&y));assert(!device_unregister(&x));
  assert(!driver_unregister(&second));
 } else if(!strcmp(argv[1],"dependency")||!strcmp(argv[1],"parent")||!strcmp(argv[1],"provider")) {
  /* Distinct devfs names are unnecessary for providers without a class. */
  driver.class_type=DEV_CLASS_NONE;second.class_type=DEV_CLASS_NONE;b.target=&second;
  assert(!driver_register(&second));assert(!device_register(&x));
  if(!strcmp(argv[1],"parent"))y.parent=&x;else b.provider=&x;
  assert(!device_register(&y));
  driver_t unused={.name="unused"};struct remove_state late_state={.target=&unused};
  device_t late={.name="late",.bus=&bus,.references=1,.parent=&x,.plat_data=&late_state};
  if(!strcmp(argv[1],"dependency")) {b.late=&late;b.late_error=-EBUSY;b.provider_pending=1;}
  if(!strcmp(argv[1],"provider")) {
   a.error=-ETIMEDOUT;assert(device_unregister(&x)==-ETIMEDOUT);check_retained(&x,&a,&driver);
   assert(!y.drv&&y.state==DEV_STATE_DEFERRED&&b.released==1);
   assert(!driver_retry_deferred()&&!y.drv&&b.probes==2);a.error=0;
  } else {
   b.error=-ETIMEDOUT;assert(device_unregister(&x)==-ETIMEDOUT);check_retained(&y,&b,&second);
   assert(x.drv==&driver&&x.state==DEV_STATE_PROBED&&!a.quiesces&&!a.released);b.error=0;
  }
  assert(!device_unregister(&x));assert(!device_unregister(&y));
  if(y.parent_reference)device_put(&y);
  assert(!driver_unregister(&second));
 } else if(!strcmp(argv[1],"last-reference")) {
  device_t *heap=kmalloc(sizeof(*heap));assert(heap);*heap=x;heap->release=release_metadata;
  assert(!device_register(heap));device_put(heap); /* Registry/class/devres own it now. */
  assert(!device_hotplug(heap,BUS_EVENT_REMOVE));
  assert(metadata_freed==1&&hotplug_removes==1&&a.released==1);
 } else if(!strcmp(argv[1],"parent-publication")) {
  assert(!driver_register(&second));b.target=&second;y.parent=&x;
  assert(!device_register(&x));a.late=&y;a.late_error=-EBUSY;
  assert(!device_unregister(&x)&&!b.probes&&!device_is_registered(&y));
  assert(!driver_unregister(&second));
 } else if(!strcmp(argv[1],"withdrawal")) {
  driver.class_type=DEV_CLASS_NONE;assert(!device_register(&x));a.late=&y;a.reenter=1;
  assert(!driver_unregister(&driver)&&!b.probes&&!y.drv&&device_is_registered(&y));
  assert(!device_unregister(&x)&&!device_unregister(&y));
  assert(!driver_register(&driver));
 } else if(!strcmp(argv[1],"multi")) {
  driver.class_type=DEV_CLASS_NONE;assert(!device_register(&x)&&!device_register(&y));
  b.error=-ETIMEDOUT;assert(driver_unregister(&driver)==-ETIMEDOUT);
  assert(!x.drv&&a.released==1&&device_is_registered(&x));check_retained(&y,&b,&driver);
  assert(driver_register(&driver)==-EEXIST);b.error=0;assert(!driver_unregister(&driver));
  assert(!device_unregister(&x)&&!device_unregister(&y));
 } else {
  a.use_i2c=!strcmp(argv[1],"i2c");assert(!device_hotplug(&x,BUS_EVENT_ADD));
  a.stale=class_device_ref_for_device(&x);assert(a.stale&&!class_device_set_offline(a.stale,remove_offline,&a));
  assert(!class_device_call_begin(a.stale));
  i2c_adapter_t *adapter=a.use_i2c?i2c_adapter_get_for_device(&x):NULL;
  if(!a.use_i2c)a.error=!strcmp(argv[1],"positive")?1:-ETIMEDOUT;
  int expected=a.use_i2c?-EBUSY:a.error>0?-EIO:a.error;
  int ret=!strcmp(argv[1],"driver")?driver_unregister(&driver):device_hotplug(&x,BUS_EVENT_REMOVE);
  assert(ret==expected);check_retained(&x,&a,&driver);
  assert(a.offline==1&&class_device_call_begin(a.stale)==-ENODEV&&!hotplug_removes);
  assert(driver_register(&driver)==-EEXIST&&bus_probe_device(&x)==-EBUSY);
  assert(device_unregister(&x)==expected&&a.quiesces==1&&a.attempts==2&&a.offline==1);
  if(adapter)i2c_adapter_put(adapter);
  a.error=0;
  assert(!device_hotplug(&x,BUS_EVENT_REMOVE));assert(hotplug_removes==1&&a.removed==1&&a.released==1);
  assert(!device_is_registered(&x)&&x.state==DEV_STATE_REMOVED&&!x.drv);
  class_device_put(a.stale);assert(x.references==1);
 }
 if(!strcmp(argv[1],"multi"))assert(driver_unregister(&driver)==-ENOENT);
 else assert(!driver_unregister(&driver));
 bus_unregister(&bus);puts("fallible driver removal: PASS");return 0;
}
