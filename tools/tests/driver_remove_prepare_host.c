/* Production core/class; prepare checkpoints must precede consumer teardown. */
struct prepare_state;
struct prepare_context {struct prepare_state *state; unsigned index;};
struct prepare_state {
 driver_t *target;device_t *device,*child;struct prepare_state *parent;
 struct prepare_context contexts[2];class_device_t *held;
 unsigned calls[2],quiesces,attempts,releases,order[8],ordered,probes;
 int error[2],remove_error,probe_error,early,oom,stopped,removed;
 void *buffer;
};
static int prepare_action(void *opaque) {
 struct prepare_context *c=opaque;struct prepare_state *s=c->state;
 assert(device_is_stopping(s->device)&&!s->quiesces&&!s->attempts&&!s->releases);
 assert(device_unregister(s->device)==-EBUSY);
 assert(device_add_remove_prepare(s->device,prepare_action,c)==-EINVAL);
 s->calls[c->index]++;s->order[s->ordered++]=c->index;
 if(s->error[c->index])return s->error[c->index];
 if(!c->index)s->stopped=1;
 return 0;
}
static void prepare_release(void *opaque) {
 struct prepare_state *s=opaque;assert(s->removed&&s->buffer);kfree(s->buffer);s->buffer=NULL;s->releases++;
}
static int prepare_probe(device_t *dev) {
 struct prepare_state *s=dev->plat_data;s->device=dev;s->probes++;
 assert(device_add_remove_prepare(NULL,prepare_action,NULL)==-EINVAL);
 assert(device_add_remove_prepare(dev,prepare_action,NULL)==-EINVAL);
 if(s->parent){dev->drv_priv=s;return 0;}
 assert(!device_probe_use_remove(dev));
 assert(device_add_remove_prepare(dev,NULL,s)==-EINVAL);
 if(s->oom) {
  probe_allocations_blocked=1;int ret=device_add_remove_prepare(dev,prepare_action,s);
  probe_allocations_blocked=0;assert(ret==-ENOMEM);return ret;
 }
 for(unsigned i=0;i<2;i++) {
  s->contexts[i]=(struct prepare_context){s,i};
  assert(!device_add_remove_prepare(dev,prepare_action,&s->contexts[i]));
 }
 if(s->early)return s->probe_error;
 s->buffer=kmalloc(64);assert(s->buffer);memset(s->buffer,0x7b,64);dev->drv_priv=s;
 assert(!device_add_resource_action(dev,prepare_release,s));
 assert(!device_claim_resource(dev,&dev->res[0]));
 if(s->child)assert(!device_register(s->child));
 return s->probe_error;
}
static void prepare_quiesce(device_t *dev) {
 struct prepare_state *s=dev->plat_data;s->quiesces++;
 assert(s->quiesces==1);
 if(s->held&&s->held->active_calls)class_device_call_end(s->held);
}
static int prepare_remove(device_t *dev) {
 struct prepare_state *s=dev->plat_data;
 assert(s->quiesces==1);
 if(s->parent)assert(s->parent->stopped);
 else assert(s->stopped||s->oom);
 s->attempts++;if(s->remove_error)return s->remove_error;
 s->removed=1;dev->drv_priv=NULL;return 0;
}
static int prepare_match(device_t *dev,const driver_t *drv) {return ((struct prepare_state *)dev->plat_data)->target==drv;}
static void prepare_case(const char *name) {
 bus_type_t bus={.name="prepare-test",.match=prepare_match};assert(!bus_register(&bus));
 char_dev_ops_t ops={.read=dummy_read};
 driver_t driver={.name="prepare",.bus=&bus,.probe=prepare_probe,.remove=prepare_remove,
  .quiesce=prepare_quiesce,.class_type=DEV_CLASS_CHAR,.class_ops=&ops};
 driver_t child_driver=driver;child_driver.name="prepare-child";child_driver.class_type=DEV_CLASS_NONE;
 struct prepare_state p={.target=&driver},c={.target=&child_driver,.parent=&p};
 resource_t resource={.type=RES_MMIO,.start=0x94000000,.end=0x94000fff};
 device_t parent={.name="prepare",.bus=&bus,.plat_data=&p,.res=&resource,.res_count=1,.references=1};
 device_t child={.name="prepare-child",.bus=&bus,.parent=&parent,.plat_data=&c,.references=1};
 p.child=&child;
 if(!strcmp(name,"oom"))p.oom=1;
 if(!strcmp(name,"probe-error")||!strcmp(name,"early")) {
  p.probe_error=-EPROBE_DEFER;p.error[1]=-EIO;
  if(!strcmp(name,"early")){p.early=1;p.child=NULL;}
 }
 assert(!driver_register(&driver)&&!driver_register(&child_driver)&&!device_register(&parent));
 if(p.oom) {
  assert(!parent.drv&&parent.probe_error==-ENOMEM&&p.quiesces==1&&p.attempts==1&&!p.ordered);
 } else if(p.probe_error) {
  assert(parent.drv==&driver&&parent.state==DEV_STATE_PROBE_CLEANUP&&parent.probe_error==-EPROBE_DEFER);
  assert(!p.quiesces&&!c.attempts&&p.calls[1]==1&&!p.calls[0]);
  assert(!driver_retry_deferred()&&p.probes==1);p.error[1]=0;
 } else {
  p.held=parent.class_dev;class_device_get(p.held);assert(!class_device_call_begin(p.held));
  p.error[1]=-ETIMEDOUT;
  assert(device_unregister(&parent)==-ETIMEDOUT&&parent.state==DEV_STATE_PREPARING);
  assert(device_is_registered(&parent)&&parent.drv==&driver&&parent.drv_priv==&p&&p.buffer);
  assert(!p.held->online&&p.held->active_calls==1&&!p.quiesces&&!p.attempts&&!p.releases&&!c.attempts);
  assert(class_device_publish(&parent)==-ESHUTDOWN);
  driver_resource_claim_t *claim=NULL;
  assert(driver_resource_request_checked(&parent,&resource,&claim)==-ENODEV&&!claim);
  device_t late={.name="late",.parent=&parent,.references=1};
  assert(device_register(&late)==-EBUSY);
  assert(device_add_dependency(&late,&parent)==-EPROBE_DEFER);
  p.error[1]=0;p.error[0]=1;
  assert(driver_unregister(&driver)==-EIO&&p.calls[1]==2&&p.calls[0]==1&&!c.attempts);
  p.error[0]=0;c.remove_error=-EBUSY;
  assert(device_unregister(&parent)==-EBUSY&&parent.state==DEV_STATE_PREPARING);
  assert(p.stopped&&p.calls[0]==2&&p.calls[1]==2&&!p.quiesces&&c.quiesces==1);
  c.remove_error=0;p.remove_error=-EIO;
  assert(device_unregister(&parent)==-EIO&&parent.state==DEV_STATE_REMOVING);
  assert(p.quiesces==1&&p.attempts==1&&!p.releases&&p.held->active_calls==0);
  p.remove_error=0;
 }
 assert(!device_unregister(&parent));
 if(!p.oom) {
  assert(p.stopped&&p.quiesces==1&&p.releases==!p.early);
  if(!p.probe_error)assert(p.ordered==4&&p.order[0]==1&&p.order[1]==1&&p.order[2]==0&&p.order[3]==0);
 }
 if(p.held)class_device_put(p.held);
 if(device_is_registered(&child))assert(!device_unregister(&child));
 if(child.parent_reference)device_put(&child);
 assert(parent.references==1&&device_add_remove_prepare(&parent,prepare_action,&p)==-EINVAL);
 assert(!driver_unregister(&child_driver)&&!driver_unregister(&driver));bus_unregister(&bus);
}
