/* Production stop/remove/slot cleanup, real driver core and USB graph. */
#define XHCI_WAIT_LOOPS 4U
#define XHCI_POLL_SPINS 1U
#define XHCI_XFER_HOLD_BUDGET_MS 500U
#define XHCI_BULK_HOLD_BUDGET_MS 5000U
#define XHCI_USBCMD 0U
#define XHCI_USBSTS 4U
#define XHCI_CMD_RUN 1U
#define XHCI_CMD_RESET 2U
#define XHCI_CMD_INTE 4U
#define XHCI_STS_HALTED 1U
#define XHCI_STS_CNR (1U<<11)
#define XHCI_INTERRUPTER_OFFSET 0x20U
#define XHCI_TRB_DISABLE_SLOT 10U
#define XHCI_TRB_TYPE(t) ((t)<<10)
typedef struct {unsigned route;} xhci_slot_path_t;
typedef struct xhci_ep {struct xhci_ep *next;void *alloc_ptr;uint8_t slot;usb_urb_t *pending;} xhci_ep_t;
typedef struct {
 void *alloc_ptr;usb_hcd_t hcd;uintptr_t op,runtime;spinlock_t lock;
 unsigned fatal,running,hardware_owned,halted,irq_registered,max_slots,xfer_busy,ep_count;
 unsigned callbacks_active,slot_callbacks[3];
 uint64_t xfer_deadline,dcbaa[3];xhci_slot_path_t slot_path[3];xhci_ep_t *eps;int irq;
} xhci_controller_t;
static xhci_controller_t *stop_controller;
static unsigned stop_command=5,stop_status=1,stop_pci=7;
static unsigned register_reads,register_writes,pci_writes,irq_frees,controller_frees,endpoint_frees,child_removes,commands_issued;
static int stop_fault,start_error,child_error,command_error;
static void stop_relax(void) {}
#define arch_cpu_relax stop_relax
uint64_t timer_get_ticks(void) {static uint64_t ticks;return ticks++;}
uint64_t clock_ticks_per_sec(void) {return 1000;}
static uint32_t xhci_read32(uintptr_t base,uint32_t reg) {
 register_reads++;
 if(base==0x2020){assert(reg==0);return 0;}
 assert(base==0x1000&&(reg==0||reg==4));
 if(reg==0)return stop_fault==1?0xffffffffU:stop_command;
 return stop_fault==2?0xffffffffU:stop_status;
}
static void xhci_write32(uintptr_t base,uint32_t reg,uint32_t value) {
 register_writes++;
 if(base==0x2020){assert(reg==0&&value==0);return;}
 assert(base==0x1000&&reg==0);
 if(stop_fault!=3)stop_command=value;
}
static uint16_t stop_pci_read(device_t *d,uint32_t reg) {
 assert(d==stop_controller->hcd.hcd_dev&&reg==4&&!stop_controller->lock.held);
 return stop_fault==6?0xffffU:stop_pci;
}
static void stop_pci_write(device_t *d,uint32_t reg,uint16_t value) {
 assert(d==stop_controller->hcd.hcd_dev&&reg==4&&!stop_controller->lock.held);pci_writes++;
 if(stop_fault!=7)stop_pci=value;
}
void free_irq(uint32_t irq,void *context) {
 xhci_controller_t *x=context;assert(x==stop_controller&&irq==12&&!x->lock.held&&x->fatal&&!x->running);irq_frees++;
}
static void stop_free(void *p) {
 if(p==stop_controller){assert(!stop_controller->hardware_owned||stop_controller->halted);controller_frees++;}
 else if(p&&stop_controller->eps==NULL){endpoint_frees++;}
 kfree(p);
}
static int xhci_command_locked(xhci_controller_t *x,uint64_t p,uint32_t status,uint32_t control,void *event) {
 assert(x==stop_controller&&!p&&!status&&!event&&!x->fatal&&x->xfer_busy);
 assert(control==(XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT)|(1U<<24)));commands_issued++;return command_error;
}
static void stop_sync(const void *p,size_t n) {assert(p&&n);}
#define arch_dma_sync_for_device stop_sync
static void xhci_ack_event_irq(xhci_controller_t *x) {(void)x;}
#define pci_cfg_read16 stop_pci_read
#define pci_cfg_write16 stop_pci_write
#define kfree stop_free
/* PRODUCTION_FUNCTIONS */
#undef kfree
#undef pci_cfg_read16
#undef pci_cfg_write16
static int stop_start(usb_hcd_t *h) {(void)h;return start_error;}
static int stop_child_probe(device_t *d) {d->drv_priv=kmalloc(64);assert(d->drv_priv);return 0;}
static int stop_child_remove(device_t *d) {
 assert(stop_controller->halted&&irq_frees==1&&d->drv_priv);child_removes++;
 if(child_error)return -EBUSY;
 kfree(d->drv_priv);d->drv_priv=NULL;return 0;
}
static int stop_probe(device_t *dev) {
 assert(!device_probe_use_remove(dev));
 int ret=device_add_remove_prepare(dev,xhci_prepare_remove,dev);if(ret)return ret;
 xhci_controller_t *x=kcalloc(1,sizeof(*x));assert(x);stop_controller=x;
 x->alloc_ptr=x;x->op=0x1000;x->runtime=0x2000;x->hardware_owned=stop_fault!=8;
 x->running=1;x->irq_registered=1;x->irq=12;x->max_slots=2;
 spin_init(&x->lock);dev->drv_priv=x;
 static const usb_hcd_ops_t ops={.start=stop_start,.abort_slot=xhci_op_abort_slot};
 x->hcd=(usb_hcd_t){.hcd_dev=dev,.ops=&ops,.priv=x,.max_ports=1};
 if(stop_fault==9)x->xfer_busy=1;
 ret=usb_core_register_hcd(&x->hcd);if(ret)return ret;
 usb_device_t *u=usb_create(&x->hcd,1);u->slot=1;u->address=usb_core_alloc_address();assert(u->address==1);
 x->hcd.port_devices[0]=u;x->hcd.port_state[0]=USB_PORT_ADDRESS_OWNED;x->dcbaa[1]=0xabc;
 xhci_ep_t *ep=kcalloc(1,sizeof(*ep));assert(ep);ep->alloc_ptr=ep;ep->slot=1;x->eps=ep;x->ep_count=1;
 assert(!usb_publish_interfaces(u));return 0;
}
static int stop_match(device_t *dev,const driver_t *driver) {return dev->bus==driver->bus;}
static void stop_case(const char *name) {
 bus_type_t bus={.name="stop-hcd",.match=stop_match};assert(!bus_register(&bus));
 driver_t driver={.name="stop-hcd",.bus=&bus,.probe=stop_probe,.remove=xhci_remove};
 static device_id_t ids[]={{.vendor=3,.device=DEVICE_ANY,.subvendor=VENDOR_ANY,.subdevice=DEVICE_ANY},{0}};
 driver_t child_driver={.name="stop-child",.bus=&g_usb_bus,.id_table=ids,.probe=stop_child_probe,.remove=stop_child_remove};
 device_t dev={.name="stop-hcd",.bus=&bus,.references=1};
 if(!strcmp(name,"command-read"))stop_fault=1;
 if(!strcmp(name,"status-read"))stop_fault=2;
 if(!strcmp(name,"command-write"))stop_fault=3;
 if(!strcmp(name,"halt"))stop_status=0;
 if(!strcmp(name,"reset"))stop_command|=2;
 if(!strcmp(name,"not-ready"))stop_status|=XHCI_STS_CNR;
 if(!strcmp(name,"pci-read"))stop_fault=6;
 if(!strcmp(name,"pci-write"))stop_fault=7;
 if(!strcmp(name,"unowned")){stop_fault=8;start_error=-EIO;}
 if(!strcmp(name,"busy"))stop_fault=9;
 if(!strcmp(name,"failed-probe")){start_error=-EIO;stop_status=0;}
 assert(!driver_register(&driver)&&!driver_register(&child_driver)&&!device_register(&dev));
 if(stop_fault==8) {
  assert(!dev.drv);assert(dev.probe_error==-EIO);assert(controller_frees==1);assert(!register_reads&&!register_writes&&!pci_writes);
 } else {
  unsigned failed_probe=!!start_error;
  if(!failed_probe) {
   assert(dev.drv==&driver&&g_hcd_count==1&&stop_controller->hcd.port_devices[0]);
   assert(xhci_remove(&dev)==-EBUSY&&!controller_frees&&!child_removes);
  }
  int ret=failed_probe?-ETIMEDOUT:device_unregister(&dev);
  int failure=stop_fault||!(stop_status&1)||(stop_status&XHCI_STS_CNR)||(stop_command&2);
  if(failed_probe||failure) {
   assert(ret<0&&dev.drv==&driver&&!controller_frees&&!child_removes&&!stop_controller->halted);
   assert(dev.state==(failed_probe?DEV_STATE_PROBE_CLEANUP:DEV_STATE_PREPARING)&&irq_frees==1);
   assert(!stop_controller->running&&stop_controller->fatal&&!commands_issued);
   if(failed_probe)assert(dev.probe_error==-EIO&&!g_hcd_count);
   else assert(g_hcd_count==1&&stop_controller->hcd.port_devices[0]->ifaces[0].device->drv==&child_driver);
   assert(xhci_xfer_acquire(stop_controller,500)==-ESHUTDOWN);
   stop_fault=0;stop_status=1;stop_command=0;stop_controller->xfer_busy=0;
   if(!failed_probe)child_error=1;
   ret=device_unregister(&dev);
   if(!failed_probe) {
    assert(ret==-EBUSY&&dev.state==DEV_STATE_PREPARING&&stop_controller->halted&&child_removes==1);
    unsigned reads=register_reads;child_error=0;
    assert(!device_unregister(&dev)&&register_reads==reads);
   } else assert(!ret);
  } else assert(!ret);
  assert(controller_frees==1&&irq_frees==1&&!g_hcd_count&&!commands_issued);
 }
 if(device_is_registered(&dev)){assert(!device_unregister(&dev));}
 assert(dev.references==1);assert(!g_device_count);
 assert(!driver_unregister(&child_driver)&&!driver_unregister(&driver));bus_unregister(&bus);
 int address=usb_core_alloc_address();assert(address==1);usb_core_free_address(address);
}
