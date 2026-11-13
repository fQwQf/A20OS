/* Included after the existing PCI/USB hardware fixture and production sources. */
static int remove_error=1,remove_attempts,resource_frees,bus_removes,poll_calls,abort_error;
static device_t *failed_usb;
static void fail_bus_event(device_t *d,int event) {(void)d;if(event==BUS_EVENT_REMOVE)bus_removes++;}
static void fail_resource(void *opaque) {resource_frees++;kfree(opaque);}
static int fail_pci_probe(device_t *dev) {
 void *p=kmalloc(64);assert(p);memset(p,0x5a,64);dev->drv_priv=p;
 return device_add_resource_action(dev,fail_resource,p);
}
static int fail_pci_remove(device_t *dev) {
 remove_attempts++;assert(dev->drv_priv&&((uint8_t *)dev->drv_priv)[0]==0x5a);
 return remove_error?-ETIMEDOUT:0;
}
static int fail_usb_remove(device_t *dev) {
 assert(dev->plat_data);usb_interface_t *iface=dev->plat_data;assert(iface->eps);
 remove_attempts++;
 if(dev==failed_usb&&remove_error)return -ETIMEDOUT;
 removes++;return 0;
}
static int failed_abort(usb_hcd_t *h,uint8_t slot) {
 if(abort_error){aborts++;return -ETIMEDOUT;}return usb_abort(h,slot);
}
static int quiet_poll(usb_hcd_t *h) {(void)h;poll_calls++;return 0;}
static void native_remove_case(const char *name) {
 if(!strncmp(name,"pci-",4)) {
  static device_id_t ids[]={{.vendor=0x1234,.device=DEVICE_ANY,.subvendor=VENDOR_ANY,.subdevice=DEVICE_ANY},{0}};
  driver_t drv={.name="failed-pci",.bus=&pci_bus,.id_table=ids,.probe=fail_pci_probe,.remove=fail_pci_remove};
  pci_bus.hotplug=fail_bus_event;assert(!driver_register(&drv));pci_insert();
  assert(!bus_probe_device(pci_current()));
  device_t *old=device_get(pci_current());assert(old&&old->drv==&drv);void *data=old->plat_data;
  if(!strcmp(name,"pci-replace"))cfg[0]=0x99991234;else present=0;
  pci_rescan();assert(pci_current()==old&&old->state==DEV_STATE_REMOVING&&old->drv==&drv);
  assert(device_is_registered(old)&&old->plat_data==data&&old->drv_priv&&!resource_frees&&!bus_removes);
  assert(remove_attempts==1&&g_device_count==1);
  device_t *found=(void *)1;assert(pci_device_find_get(0,0,&found)==-ENODEV&&!found);
  pci_user_device_info_t info;assert(pci_user_device_find(0x1234,0x5678,&info)<0);
  remove_error=0;pci_rescan();assert(resource_frees==1&&bus_removes==1&&!old->drv);
  if(!strcmp(name,"pci-replace")) {
   device_t *fresh=pci_current();assert(fresh&&fresh!=old);
   assert(!bus_probe_device(fresh)&&fresh->drv==&drv);
   assert(((pci_dev_info_t *)old->plat_data)->device==0x5678);
   pci_remove();assert(resource_frees==2&&bus_removes==2);
  } else assert(!pci_current());
  device_put(old);assert(!driver_unregister(&drv));
  return;
 }
 static device_t parent={.name="hcd",.references=1,.state=DEV_STATE_PROBED};
 static const usb_hcd_ops_t ops={.port_connected=enum_connected,.reset_port=enum_reset,.alloc_slot=enum_slot,
  .update_ep0_mps=enum_mps,.get_descriptor=enum_descriptor,.control=enum_control,.abort_slot=failed_abort,.poll=quiet_poll};
 usb_hcd_t h={.hcd_dev=&parent,.ops=&ops,.max_ports=1};active_hcd=&h;
 h.port_devices=kcalloc(1,sizeof(*h.port_devices));h.port_state=kcalloc(1,sizeof(*h.port_state));
 g_hcds[0]=&h;g_hcd_count=1;g_usb_bus.hotplug=fail_bus_event;
 static device_id_t ids[]={{.vendor=3,.device=DEVICE_ANY,.subvendor=VENDOR_ANY,.subdevice=DEVICE_ANY},{0}};
 driver_t drv={.name="failed-usb",.bus=&g_usb_bus,.id_table=ids,.probe=usb_probe,.remove=fail_usb_remove};
 assert(!driver_register(&drv));expected_interfaces=3;
 usb_device_t *u=usb_create(&h,3);u->address=usb_core_alloc_address();u->slot=1;assert(u->address==1);
 assert(!usb_publish_interfaces(u));h.port_devices[0]=u;
 device_t *held=device_get(u->ifaces[0].device);failed_usb=u->ifaces[1].device;
 int already=!strcmp(name,"usb-already");
 if(already)assert(!device_unregister(held));
 usb_interface_t *interfaces=u->ifaces;usb_endpoint_t *endpoint=u->ifaces[0].eps;
 if(!strcmp(name,"usb-abort")||!strcmp(name,"usb-no-abort")) {
  int no_abort=!strcmp(name,"usb-no-abort");usb_hcd_ops_t missing=ops;missing.abort_slot=NULL;
  if(no_abort)h.ops=&missing;else abort_error=1;
  remove_error=0;assert(usb_core_unregister_hcd(&h)==(no_abort?-EOPNOTSUPP:-ETIMEDOUT));
  assert(!removes&&!bus_removes&&g_device_count==3&&h.port_devices[0]==u);
  assert(h.port_state[0]==USB_PORT_REMOVE_PENDING&&held->plat_data==&u->ifaces[0]);
  assert(held->references==3&&u->ifaces[0].eps==endpoint);
  abort_error=0;h.ops=&ops;assert(!usb_core_unregister_hcd(&h)&&removes==3&&aborts==2-no_abort);
  assert(!g_hcd_count&&!g_device_count&&!h.port_devices&&!h.port_state&&held->plat_data==NULL);
  device_put(held);assert(parent.references==1);
  int address=usb_core_alloc_address();assert(address==1);usb_core_free_address(address);
  assert(!driver_unregister(&drv));return;
 }
 int ret=!strcmp(name,"usb-hcd")?usb_core_unregister_hcd(&h):usb_disconnect_port(&h,1);
 assert(ret==-ETIMEDOUT&&g_hcd_count==1&&h.port_devices[0]==u&&h.port_state[0]==(USB_PORT_REMOVE_PENDING|USB_PORT_SLOT_STOPPED));
 assert(u->ifaces==interfaces&&u->ifaces[0].eps==endpoint&&u->ifaces[0].eps[0].addr==0);
 assert(!u->ifaces[0].device&&held->plat_data==NULL&&held->references==1);
 assert(failed_usb->state==DEV_STATE_REMOVING&&failed_usb->plat_data==&u->ifaces[1]);
 assert(g_device_count==2&&removes==1&&bus_removes==1-already&&aborts==1);
 int address=usb_core_alloc_address();assert(address==2);usb_core_free_address(address);
 if(!strcmp(name,"usb-hcd")) {
  parent.state=DEV_STATE_REMOVING;usb_core_scan();assert(!poll_calls&&aborts==1);
  remove_error=0;assert(!usb_core_unregister_hcd(&h)&&!g_hcd_count&&!h.port_devices&&!h.port_state&&aborts==1);
  assert(!usb_core_unregister_hcd(&h));
 } else {
  /* Pending removal must be retried even if a replacement is now connected. */
  usb_present=1;usb_config_length=879;usb_core_scan();assert(!usb_resets&&h.port_devices[0]==u);
  remove_error=0;usb_core_scan();assert(usb_resets==1&&h.port_devices[0]&&h.port_devices[0]->iface_count==2);
  assert(h.port_devices[0]->ifaces[0].device!=held&&held->plat_data==NULL);
  usb_present=0;usb_core_scan();assert(!h.port_devices[0]);
  assert(!usb_core_unregister_hcd(&h));
 }
 assert(removes==3&&g_device_count==0);device_put(held);assert(parent.references==1);
 address=usb_core_alloc_address();assert(address==1);usb_core_free_address(address);
 assert(!driver_unregister(&drv));
}
