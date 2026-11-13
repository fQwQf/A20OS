/* Real USB/core publication and ownership; only hardware/allocator are fake. */
static int setup_error=-EIO,stop_error=-ETIMEDOUT,slot_error,software_address=1;
static unsigned slot_calls,descriptor_calls,stop_calls,release_calls,remove_calls;
static unsigned fail_stage=1,publication_fault,child_remove_error;
static int stopped,force_allocation,connection=1;
static device_t *published;
static usb_device_t *retained;
static int setup_start(usb_hcd_t *h) {(void)h;return 0;}
static int setup_connected(usb_hcd_t *h,unsigned port) {(void)h;assert(port==1);return connection;}
static int setup_slot(usb_hcd_t *h,unsigned port,uint8_t speed,uint8_t hub,uint8_t address,usb_slot_t *slot) {
 (void)h;assert(port==1&&speed==USB_SPEED_HIGH&&!hub&&address==1);
 assert(!slot->hcd&&!slot->address);slot_calls++;
 if(slot_error==2)return setup_error; /* No context transferred. */
 if(slot_error==3)return 0; /* Invalid success without token. */
 *slot=(usb_slot_t){.hcd=1,.address=software_address==1?address:software_address==2?2:0};
 return slot_error?setup_error:0;
}
static int setup_mps(usb_hcd_t *h,uint8_t slot,uint16_t mps) {
 int ret=enum_mps(h,slot,mps);return fail_stage==7?setup_error:ret;
}
static int setup_descriptor(usb_hcd_t *h,uint8_t slot,uint8_t type,uint16_t len,void *buf) {
 descriptor_calls++;
 if(descriptor_calls==fail_stage)return setup_error;
 int ret=enum_descriptor(h,slot,type,len,buf);
 if(descriptor_calls==3&&force_allocation)fail_at=allocations+force_allocation;
 if(descriptor_calls==4&&fail_stage==6)((uint8_t *)buf)[4]=0; /* Invalid interface count. */
 return ret;
}
static int setup_control(usb_hcd_t *h,uint8_t slot,const usb_setup_packet_t *p,void *buf) {
 int ret=enum_control(h,slot,p,buf);return fail_stage==5?setup_error:ret;
}
static int setup_abort(usb_hcd_t *h,uint8_t slot) {
 assert(slot==1&&h->port_devices[0]);stop_calls++;
 usb_device_t *u=h->port_devices[0];assert(u->slot==1&&u->hcd==h);
 if(retained)assert(retained==u);
 if(published)assert(published->plat_data==&u->ifaces[0]&&published->drv_priv);
 if(stop_error)return stop_error;
 stopped=1;return 0;
}
static void setup_release(void *p) {assert(stopped);release_calls++;kfree(p);}
static int setup_probe(device_t *dev) {
 assert(!stopped&&dev->parent==active_hcd->hcd_dev);
 void *p=kmalloc(64);assert(p);memset(p,0x4a,64);dev->drv_priv=p;
 assert(!device_add_resource_action(dev,setup_release,p));return 0;
}
static int setup_remove(device_t *dev) {
 assert(stopped&&dev->drv_priv&&((uint8_t *)dev->drv_priv)[0]==0x4a);
 remove_calls++;return child_remove_error?-EBUSY:0;
}
static void setup_publication(device_t *dev,int event) {
 if(event!=BUS_EVENT_ADD||!publication_fault||published)return;
 /* Explicit observer binding stresses a partially published graph; ordinary
  * first probes still wait for complete publication in the production core. */
 assert(!bus_probe_device(dev));published=device_get(dev);
 fail_at=allocations+1;
}
static void setup_check_held(usb_hcd_t *h,unsigned expected_interfaces) {
 retained=h->port_devices[0];assert(retained&&retained->slot==1&&!stopped);
 assert(retained->address==(software_address==1?1:software_address==2?2:0)&&retained->iface_count==expected_interfaces);
 assert(h->port_state[0]&USB_PORT_REMOVE_PENDING);
 unsigned before=usb_resets,slots=slot_calls;
 assert(usb_core_enumerate_port(h,1)==-EBUSY&&usb_resets==before&&slot_calls==slots);
 int address=usb_core_alloc_address();assert(address==(software_address==1?2:1));usb_core_free_address(address);
}
static void usb_setup_cleanup_case(const char *name) {
 static device_t parent={.name="setup-hcd",.references=1,.state=DEV_STATE_PROBED};
 usb_hcd_ops_t ops={.start=setup_start,.port_connected=setup_connected,.reset_port=enum_reset,
  .alloc_slot=setup_slot,.update_ep0_mps=setup_mps,.get_descriptor=setup_descriptor,
  .control=setup_control,.abort_slot=setup_abort};
 usb_hcd_t h={.hcd_dev=&parent,.ops=&ops,.max_ports=1};active_hcd=&h;
 usb_config_length=879;assert(!usb_core_register_hcd(&h));
 int before=allocated;
 int foreign=!strcmp(name,"implicit-other")||!strcmp(name,"implicit-success");
 if(foreign) {
  software_address=2;assert(usb_core_alloc_address()==1&&usb_core_alloc_address()==2);
  usb_core_free_address(1); /* Another HCD still owns reservation 2. */
 }
 if(!strcmp(name,"implicit-success")) {
  fail_stage=0;stop_error=0;
  assert(!usb_core_enumerate_port(&h,1)&&h.port_devices[0]->address==2);
  assert(!(h.port_state[0]&USB_PORT_ADDRESS_OWNED));
  assert(!usb_disconnect_port(&h,1));
 } else if(!strcmp(name,"invalid")) {
  assert(usb_core_enumerate_port(NULL,1)==-EINVAL);
  assert(usb_core_enumerate_port(&h,0)==-EINVAL&&usb_core_enumerate_port(&h,2)==-EINVAL);
  parent.state=DEV_STATE_REMOVING;assert(usb_core_enumerate_port(&h,1)==-ENODEV);
  parent.state=DEV_STATE_PROBE_CLEANUP;assert(usb_core_enumerate_port(&h,1)==-ENODEV);
  parent.state=DEV_STATE_PROBED;
  ops.alloc_slot=NULL;assert(usb_core_enumerate_port(&h,1)==-EINVAL);
  assert(!usb_resets&&!slot_calls&&allocated==before);
 } else if(!strcmp(name,"owner-oom")) {
  fail_at=allocations+1;assert(usb_core_enumerate_port(&h,1)==-ENOMEM);fail_at=0;
  assert(!usb_resets&&!slot_calls&&!stop_calls&&allocated==before);
 } else if(!strcmp(name,"pool")) {
  for(unsigned i=1;i<128;i++)assert(usb_core_alloc_address()==(int)i);
  assert(usb_core_enumerate_port(&h,1)==-ENOMEM&&!slot_calls&&!stop_calls&&allocated==before);
  for(unsigned i=1;i<128;i++)usb_core_free_address(i);
 } else if(!strcmp(name,"empty-slot")||!strcmp(name,"invalid-slot")) {
  slot_error=!strcmp(name,"empty-slot")?2:3;
  assert(usb_core_enumerate_port(&h,1)==-EIO&&!h.port_devices[0]&&!stop_calls&&allocated==before);
 } else if(!strcmp(name,"safe-failure")) {
  stop_error=0;
  assert(usb_core_enumerate_port(&h,1)==-EIO&&!h.port_devices[0]&&stop_calls==1);
  assert(allocated==before);
 } else if(!strcmp(name,"budget")) {
  usb_core_scan();setup_check_held(&h,0);
  assert((h.port_state[0]&USB_PORT_ENUM_ATTEMPTS)==1&&usb_resets==1);
  for(unsigned i=0;i<5;i++)usb_core_scan();
  assert(usb_resets==1&&slot_calls==1&&stop_calls==6);
  stop_error=0;retained=NULL; /* The old owner may be freed after this stop. */
  /* Fail the same first descriptor on every subsequent attempt. */
  descriptor_calls=0;usb_core_scan();assert(usb_resets==2&&!h.port_devices[0]);
  descriptor_calls=0;usb_core_scan();assert(usb_resets==3&&!h.port_devices[0]);
  for(unsigned i=0;i<12;i++)usb_core_scan();
  assert(usb_resets==3&&h.port_state[0]==USB_ENUM_MAX_ATTEMPTS);
  connection=0;usb_core_scan();assert(!h.port_state[0]);
  connection=1;fail_stage=0;usb_core_scan();assert(usb_resets==4&&h.port_devices[0]);
  assert(h.port_state[0]==USB_PORT_ADDRESS_OWNED);
 } else {
  static const device_id_t ids[]={{.vendor=0x0e,.device=DEVICE_ANY,.subvendor=VENDOR_ANY,.subdevice=DEVICE_ANY},{0}};
  driver_t driver={.name="setup-child",.bus=&g_usb_bus,.id_table=ids,.probe=setup_probe,.remove=setup_remove};
  if(!strcmp(name,"slot")||!strcmp(name,"implicit")||!strcmp(name,"positive")||foreign)slot_error=1;
  if(!strcmp(name,"implicit"))software_address=0;
  if(!strcmp(name,"stop-positive"))stop_error=1;
  if(!strcmp(name,"positive"))setup_error=1;
  if(!strncmp(name,"descriptor-",11))fail_stage=name[11]-'0';
  if(!strcmp(name,"control"))fail_stage=5;
  if(!strcmp(name,"parse"))fail_stage=6;
  if(!strcmp(name,"mps"))fail_stage=7;
  if(!strncmp(name,"oom-",4)){fail_stage=0;force_allocation=name[4]-'0';}
  if(!strcmp(name,"no-abort"))ops.abort_slot=NULL;
  int publish=!strcmp(name,"publication");
  if(publish) {
   fail_stage=0;publication_fault=1;g_usb_bus.hotplug=setup_publication;
   assert(!driver_register(&driver));
  }
  int ret=usb_core_enumerate_port(&h,1);fail_at=0;
  assert(ret==(!ops.abort_slot?-EOPNOTSUPP:stop_error>0?-EIO:-ETIMEDOUT));
  unsigned interfaces=(fail_stage==5||publish)?2:0;
  setup_check_held(&h,interfaces);assert(!remove_calls&&!release_calls);
  if(publish)assert(published&&published->drv==&driver&&g_device_count==1&&parent.references==2);
  stop_error=0;ops.abort_slot=setup_abort;
  if(publish) {
   child_remove_error=1;assert(usb_core_unregister_hcd(&h)==-EBUSY);
   assert(stopped&&h.port_devices[0]==retained&&published->state==DEV_STATE_REMOVING);
   assert(h.port_state[0]==(USB_PORT_REMOVE_PENDING|USB_PORT_SLOT_STOPPED|USB_PORT_ADDRESS_OWNED));
   assert(remove_calls==1&&!release_calls);child_remove_error=0;
  }
  unsigned stopped_calls=stop_calls;
  assert(!usb_core_unregister_hcd(&h));retained=NULL;
  assert(stop_calls==stopped_calls+!publish&&!h.port_devices&&!h.port_state&&!g_hcd_count);
  if(publish) {
   assert(remove_calls==2&&release_calls==1&&published->plat_data==NULL);
   assert(!driver_unregister(&driver));device_put(published);published=NULL;
  }
 }
 assert(!usb_core_unregister_hcd(&h)&&!g_hcd_count&&!g_device_count&&parent.references==1);
 int address=usb_core_alloc_address();assert(address==1);
 if(foreign) {int next=usb_core_alloc_address();assert(next==3);usb_core_free_address(next);usb_core_free_address(2);}
 usb_core_free_address(address);
}
