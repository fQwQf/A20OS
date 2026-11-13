/* Included by driver_remove_host.c's host compiler: real core/class and both
 * registry backends; hardware ownership is represented by a retained buffer. */
struct probe_cleanup_state {
 driver_t *target;
 device_t *child,*ancestor,*provider;
 int arm,early,error,cleanup_error,recursive,inside,fallback;
 unsigned probes,quiesces,attempts,releases;
 void *buffer;
 int safe;
};
static driver_t *probe_fallback_driver;
static unsigned probe_fallback_calls;
static const device_id_t probe_match_id={.vendor=0xa20,.device=1};
static int cleanup_probe_match(device_t *dev,const driver_t *driver) {
 struct probe_cleanup_state *s=dev->plat_data;
 int match=s->target==driver||(s->fallback&&driver==probe_fallback_driver);
 if(match)dev->matched_id=&probe_match_id;
 return match;
}
static void cleanup_probe_resource(void *opaque) {
 struct probe_cleanup_state *s=opaque;assert(s->safe&&!s->inside);
 assert(s->buffer&&((unsigned char *)s->buffer)[0]==0x6b);
 kfree(s->buffer);s->buffer=NULL;s->releases++;
}
static int cleanup_probe(device_t *dev) {
 struct probe_cleanup_state *s=dev->plat_data;s->inside=1;s->probes++;
 assert(device_probe_use_remove(NULL)==-EINVAL);
 if(s->ancestor) {
  assert(device_probe_use_remove(s->ancestor)==-EINVAL);
  assert(device_unregister(s->ancestor)==-EBUSY);
 }
 if(s->arm) {
  probe_allocations_blocked=1;
  int ret=device_probe_use_remove(dev);
  probe_allocations_blocked=0;
  if(ret){s->inside=0;return ret;}
  assert(!device_probe_use_remove(dev)); /* Idempotent, no extra ownership. */
 }
 assert(!s->quiesces&&!s->attempts&&!s->releases);
 if(s->early){s->inside=0;return s->error;}
 if(s->provider)assert(!device_add_dependency(dev,s->provider));
 s->buffer=kmalloc(64);assert(s->buffer);memset(s->buffer,0x6b,64);
 dev->drv_priv=s;
 assert(!device_add_resource_action(dev,cleanup_probe_resource,s));
 assert(!device_claim_resource(dev,&dev->res[0]));
 if(s->child) {
  assert(!device_register(s->child));
  if(s->arm)assert(!device_probe_use_remove(dev)); /* Nested probe restored top. */
 }
 if(s->recursive) {
  assert(device_unregister(dev)==-EBUSY);
  assert(driver_unregister(s->target)==-EBUSY);
 }
 if(s->error&&!s->arm)s->safe=1; /* Legacy probe owns its hardware rollback. */
 s->inside=0;
 return s->error;
}
static void cleanup_probe_quiesce(device_t *dev) {
 struct probe_cleanup_state *s=dev->plat_data;assert(!s->inside);s->quiesces++;
 assert(s->quiesces==1);
}
static int cleanup_probe_remove(device_t *dev) {
 struct probe_cleanup_state *s=dev->plat_data;assert(!s->inside&&s->quiesces==1);
 s->attempts++;
 if(s->cleanup_error)return s->cleanup_error;
 s->safe=1;dev->drv_priv=NULL;return 0;
}
static int cleanup_fallback(device_t *dev) {(void)dev;probe_fallback_calls++;return -ENODEV;}
static void cleanup_held(device_t *dev,struct probe_cleanup_state *s,int expected_state) {
 assert(device_is_registered(dev)&&dev->drv==s->target&&dev->drv_priv==s);
 assert(dev->probe_error==s->error&&dev->state==expected_state&&device_is_stopping(dev));
 assert(dev->matched_id==&probe_match_id&&s->buffer&&!s->safe&&!s->releases);
 assert(!dev->class_dev&&class_device_publish(dev)==-ESHUTDOWN);
 assert(bus_probe_device(dev)==-EBUSY&&device_probe_use_remove(dev)==-EINVAL);
 driver_probe_all();assert(!driver_retry_deferred()&&s->probes==1&&!probe_fallback_calls);
 driver_resource_claim_t *claim=NULL;
 assert(driver_resource_request_checked(dev,&dev->res[0],&claim)==-ENODEV&&!claim);
 device_t peer={.references=1};
 assert(driver_resource_request(&peer,&dev->res[0],&claim)==-EBUSY&&!claim);
 assert(device_add_dependency(&peer,dev)==-EPROBE_DEFER&&!peer.dependencies);
}
static void probe_cleanup_case(const char *name) {
 bus_type_t bus={.name="probe-cleanup",.match=cleanup_probe_match};assert(!bus_register(&bus));
 char_dev_ops_t ops={.read=dummy_read};
 driver_t driver={.name="probe-cleanup",.bus=&bus,.probe=cleanup_probe,.quiesce=cleanup_probe_quiesce,
  .remove=cleanup_probe_remove,.class_type=DEV_CLASS_CHAR,.class_ops=&ops};
 driver_t child_driver=driver;child_driver.name="probe-child";child_driver.class_type=DEV_CLASS_NONE;
 driver_t fallback={.name="probe-fallback",.bus=&bus,.probe=cleanup_fallback};
 probe_fallback_driver=&fallback;
 struct probe_cleanup_state p={.target=&driver,.arm=1,.error=-EIO};
 struct probe_cleanup_state c={.target=&child_driver};
 resource_t pr={.type=RES_MMIO,.start=0x91000000,.end=0x91000fff};
 resource_t cr={.type=RES_MMIO,.start=0x92000000,.end=0x92000fff};
 device_t parent={.name="probe-parent",.bus=&bus,.plat_data=&p,.references=1,.res=&pr,.res_count=1};
 device_t child={.name="probe-child",.bus=&bus,.plat_data=&c,.references=1,.parent=&parent,.res=&cr,.res_count=1};
 device_t provider={.name="probe-provider",.references=1};
 assert(device_probe_use_remove(&parent)==-EINVAL);
 if(!strcmp(name,"missing-remove"))driver.remove=NULL;
 if(!strcmp(name,"early"))p.early=1;
 if(!strcmp(name,"positive"))p.error=1;
 if(!strcmp(name,"deferred")||!strcmp(name,"deferred-held"))p.error=-EPROBE_DEFER;
 if(!strcmp(name,"legacy"))p.arm=0;
 if(!strcmp(name,"success")){p.error=0;p.recursive=1;}
 if(!strcmp(name,"held")||!strcmp(name,"deferred-held")) {
  p.cleanup_error=-ETIMEDOUT;p.fallback=1;p.provider=&provider;
  assert(!device_register(&provider));
 }
 if(!strcmp(name,"nested")||!strcmp(name,"child-held")) {
  p.child=&child;c.ancestor=&parent;c.arm=1;
  if(!strcmp(name,"child-held"))c.cleanup_error=-ETIMEDOUT;
 }
 assert(!driver_register(&driver)&&!driver_register(&child_driver)&&!driver_register(&fallback));
 if(!strcmp(name,"unpublished")) {
  assert(bus_probe_device(&parent)==-ENODEV&&!parent.drv&&!p.buffer);
 } else {
  assert(!device_register(&parent));
  if(!strcmp(name,"held")||!strcmp(name,"deferred-held")) {
   cleanup_held(&parent,&p,DEV_STATE_REMOVING);
   assert(parent.dependencies&&provider.references==3);
   assert(p.quiesces==1&&p.attempts==1&&driver_unregister(&driver)==-ETIMEDOUT);
   assert(p.quiesces==1&&p.attempts==2&&parent.probe_error==p.error);
   p.cleanup_error=0;assert(!device_unregister(&parent)&&p.releases==1&&!parent.dependencies);
   assert(!device_unregister(&provider));
  } else if(!strcmp(name,"child-held")) {
   cleanup_held(&parent,&p,DEV_STATE_PROBE_CLEANUP);
   assert(!p.quiesces&&!p.attempts&&c.quiesces==1&&c.attempts==1);
   assert(child.state==DEV_STATE_REMOVING&&child.drv==&child_driver);
   device_t late={.name="late",.parent=&parent,.references=1};
   assert(device_register(&late)==-EBUSY&&!late.parent_reference);
   c.cleanup_error=0;assert(!device_unregister(&parent));
   assert(p.quiesces==1&&p.attempts==1&&p.releases==1&&c.quiesces==1&&c.releases==1);
  } else if(!strcmp(name,"success")) {
   assert(parent.drv==&driver&&parent.class_dev&&parent.state==DEV_STATE_PROBED&&!p.quiesces);
   assert(!device_unregister(&parent)&&p.quiesces==1&&p.releases==1);
  } else if(!strcmp(name,"missing-remove")) {
   assert(parent.probe_error==-EOPNOTSUPP&&!parent.drv&&!p.buffer&&!p.quiesces&&!p.attempts);
   assert(!device_unregister(&parent));
  } else {
   int error=p.error>0?-EIO:p.error;
   assert(!parent.drv&&!parent.class_dev&&parent.probe_error==error&&!p.buffer);
   assert(parent.state==(error==-EPROBE_DEFER?DEV_STATE_DEFERRED:DEV_STATE_UNINIT));
   if(p.arm)assert(p.quiesces==1&&p.attempts==1);
   else assert(!p.quiesces&&!p.attempts);
   assert(p.releases==!p.early);assert(!device_unregister(&parent));
  }
 }
 assert(device_probe_use_remove(&parent)==-EINVAL&&!device_is_stopping(NULL));
 if(device_is_registered(&child))assert(!device_unregister(&child));
 if(child.parent_reference)device_put(&child);
 assert(parent.references==1);
 assert(!driver_unregister(&fallback)&&!driver_unregister(&child_driver)&&!driver_unregister(&driver));
 bus_unregister(&bus);
}
