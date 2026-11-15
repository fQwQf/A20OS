/* Real bus/core/hub retirement; only the HCD stop/transfer backend is fake. */
#define HUB_STATUS_BYTES(nports) (((nports)+1U+7U)/8U)
/* HUB_SHAPE */
/* HUB_REMOVE */
struct tree_transport { usb_urb_t *pending; unsigned aborts, late; int fail; };
static int tree_start(usb_hcd_t *h) {(void)h;return 0;}
static int tree_abort(usb_hcd_t *h,uint8_t slot) {
 struct tree_transport *t=h->priv;assert(slot==1);t->aborts++;
 if(t->fail)return -EBUSY;
 if(t->pending){t->pending->status=-ESHUTDOWN;t->pending=NULL;}
 return 0;
}
static const usb_hcd_ops_t tree_ops={.start=tree_start,.abort_slot=tree_abort};
static void tree_late_event(struct tree_transport *t) {
 /* A retired fake slot cannot dispatch the prior URB or touch its freed dev. */
 assert(!t->pending);t->late++;
}
static void tree_complete(usb_urb_t *urb) {(void)urb;assert(0);}
static usb_device_t *tree_device(usb_hcd_t *h,int hub_class) {
 usb_device_t *u=usb_create(h,1);u->slot=1;
 u->address=(uint8_t)usb_core_alloc_address();assert(u->address>0);
 h->port_devices[0]=u;h->port_state[0]=USB_PORT_ADDRESS_OWNED;
 u->ifaces[0].interface_class=hub_class ? USB_CLASS_HUB : USB_CLASS_HID;
 assert(!usb_publish_interfaces(u));return u;
}
static void tree_case(void) {
 for(unsigned round=0;round<64;round++) {
  struct tree_transport parent={0},child={.fail=1};
  device_t physical={.name="fake-root",.references=1};
  usb_hcd_t root={.ops=&tree_ops,.max_ports=1,.hcd_dev=&physical,.priv=&parent};
  assert(!usb_core_register_hcd(&root));
  usb_device_t *hub_device=tree_device(&root,1);
  usb_interface_t *hub_iface=&hub_device->ifaces[0];
  usb_hub_t *hub=kcalloc(1,sizeof(*hub));assert(hub);spin_init(&hub->sc_lock);
  hub->hcd=(usb_hcd_t){.ops=&tree_ops,.max_ports=1,.hcd_dev=hub_iface->device,.parent_hcd=&root,.priv=&child};
  assert(!usb_core_register_hcd(&hub->hcd));
  usb_device_t *leaf=tree_device(&hub->hcd,0);
  uint8_t address_parent=hub_device->address,address_child=leaf->address;
  assert(address_parent!=address_child);
  usb_urb_t pu={.dev=hub_device,.ep=hub_iface->eps,.complete=tree_complete};
  usb_urb_t cu={.dev=leaf,.ep=leaf->ifaces[0].eps,.complete=tree_complete};
  parent.pending=&pu;child.pending=&cu;
  driver_t hub_driver={.name="fake-hub-binding",.remove=usb_hub_remove};
  hub_iface->device->drv=&hub_driver;hub_iface->device->drv_priv=hub;
  hub_iface->device->state=DEV_STATE_PROBED;
  uint8_t reserved[125];for(unsigned i=0;i<125;i++){int a=usb_core_alloc_address();assert(a>0);reserved[i]=(uint8_t)a;}
  assert(usb_core_alloc_address()==-ENOMEM);
  /* Upstream stop succeeds; downstream stop fails. Neither address may recycle. */
  assert(usb_disconnect_port(&root,1)==-EBUSY);
  assert(root.port_devices[0]==hub_device&&hub->hcd.port_devices[0]==leaf);
  assert(!parent.pending&&child.pending==&cu&&parent.aborts==1&&child.aborts==1);
  assert(usb_core_alloc_address()==-ENOMEM&&g_hcd_count==2);
  child.fail=0;assert(!usb_disconnect_port(&root,1));
  assert(!parent.pending&&!child.pending&&pu.status==-ESHUTDOWN&&cu.status==-ESHUTDOWN);
  assert(parent.aborts==1&&child.aborts==2&&!root.port_devices[0]);
  assert(g_hcd_count==1&&!g_device_count&&physical.references==1);
  tree_late_event(&parent);tree_late_event(&child);
  assert(parent.late==1&&child.late==1);
  int a=usb_core_alloc_address(),b=usb_core_alloc_address();
  assert(a==address_parent&&b==address_child);
  assert(usb_core_alloc_address()==-ENOMEM);
  usb_core_free_address((uint8_t)a);usb_core_free_address((uint8_t)b);
  for(unsigned i=0;i<125;i++)usb_core_free_address(reserved[i]);
  assert(!usb_core_unregister_hcd(&root)&&!g_hcd_count);
  assert(allocated==(int)baseline);
 }
 for(unsigned i=0;i<sizeof(g_usb_addresses);i++)assert(!g_usb_addresses[i]);
}
