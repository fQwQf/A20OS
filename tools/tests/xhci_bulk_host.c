static int bulk_start(usb_hcd_t *h) {(void)h;return 0;}
static int bulk_match(device_t *d,const driver_t *r) {(void)d;(void)r;return 1;}
static int bulk_probe(device_t *d) {
 assert(!device_probe_use_remove(d)&&!device_add_remove_prepare(d,xhci_prepare_remove,d));
 xhci_controller_t *x=kcalloc(1,sizeof(*x));assert(x);stop_controller=x;x->alloc_ptr=x;
 spin_init(&x->lock);d->drv_priv=x;x->op=0x1000;x->runtime=0x2000;x->doorbell=0x3000;x->max_slots=2;
 static const usb_hcd_ops_t ops={.start=bulk_start,.abort_slot=xhci_op_abort_slot,.submit_bulk=xhci_op_submit_bulk};
 x->hcd=(usb_hcd_t){.hcd_dev=d,.ops=&ops,.priv=x,.max_ports=1};allocation_owner=d;
 int ret=driver_dma_buffer_create(d,65536,65536,&x->bulk_buffer);if(ret)return ret;
 x->hardware_owned=x->running=x->irq_registered=1;x->irq=12;
 assert(!usb_core_register_hcd(&x->hcd));
 usb_device_t *u=usb_create(&x->hcd,1);u->slot=1;u->address=usb_core_alloc_address();assert(u->address==1);
 u->ifaces[0].ep_count=1;u->ifaces[0].eps[0].addr=0x81;
 x->hcd.port_devices[0]=u;x->hcd.port_state[0]=USB_PORT_ADDRESS_OWNED;x->dcbaa[1]=0xabc;
 xhci_ep_t *ep=kcalloc(1,sizeof(*ep));assert(ep);ep->alloc_ptr=ep;ep->slot=1;ep->dci=3;ep->addr=0x81;
 ep->ring.cycle=1;x->eps=ep;x->ep_count=1;return 0;
}
static void bulk_case(const char *name) {
 pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0x8800b000,.end=0x8900b000,.start_pfn=0,.end_pfn=4096};
 bus_type_t bus={.name="bulk",.match=bulk_match};assert(!bus_register(&bus));
 driver_t driver={.name="bulk",.bus=&bus,.probe=bulk_probe,.remove=xhci_remove};assert(!driver_register(&driver));
 device_t d={.name="bulk",.bus=&bus,.references=1};
 if(!strcmp(name,"probe-failure"))page_fail=1;
 assert(!device_register(&d));
 if(!strcmp(name,"probe-failure")) {
  assert(!d.drv&&!d.drv_priv&&d.probe_error==-ENOMEM&&controller_frees==1&&!page_frees&&!irq_frees);
  assert(!device_unregister(&d)&&!driver_unregister(&driver));bus_unregister(&bus);return;
 }
 assert(d.drv==&driver&&d.drv_priv);xhci_controller_t *x=d.drv_priv;
 usb_device_t *u=x->hcd.port_devices[0];usb_endpoint_t *ep=u->ifaces[0].eps;
 uint8_t *caller=kmalloc(65536);assert(caller);memset(caller,0x55,65536);bulk_caller=caller;
 usb_urb_t urb={.dev=u,.ep=ep,.transfer_type=USB_XFER_BULK,.buf=caller,.len=64};
 if(!strcmp(name,"copies")) {
  assert(!xhci_op_submit_bulk(&x->hcd,&urb)&&caller[0]==0x3c&&caller[63]==0x3c&&urb.actual_length==64);
  assert(x->eps->ring.trbs[0].parameter==driver_dma_buffer_address(x->bulk_buffer));
  assert(x->eps->ring.trbs[0].parameter!=(uintptr_t)caller);
  ep->addr=1;x->eps->dci=2;memset(caller,0x55,64);
  assert(!xhci_op_submit_bulk(&x->hcd,&urb)&&caller[0]==0x55);
  urb.len=0;urb.buf=NULL;assert(!xhci_op_submit_bulk(&x->hcd,&urb));
  ep->addr=0x81;x->eps->dci=3;urb.len=65536;urb.buf=caller;
  assert(!xhci_op_submit_bulk(&x->hcd,&urb)&&caller[65535]==0x3c&&bulk_bells==4);
 } else if(!strcmp(name,"timeout")) {
  wait_rc=-ETIMEDOUT;stop_status=0;
  assert(xhci_op_submit_bulk(&x->hcd,&urb)==-ETIMEDOUT&&caller[0]==0x55&&x->fatal&&!x->halted);
  struct driver_dma_buffer *held_buffer=x->bulk_buffer;driver_dma_buffer_get(held_buffer);
  kfree(caller);caller=NULL; /* A late DMA must not retain this caller storage. */
  small_memory[0]=0xe1;assert(!page_frees&&driver_dma_buffer_data(held_buffer)==small_memory);
  assert(driver_unregister(&driver)==-ETIMEDOUT&&d.drv_priv==x&&!page_frees&&!controller_frees);
  assert(x->bulk_buffer==held_buffer&&refcount_read(&held_buffer->references)==2);
  small_memory[63]=0xe2;assert(!page_frees);
  stop_status=1;assert(!driver_unregister(&driver)&&!d.drv_priv&&controller_frees==1&&!page_frees);
  assert(small_memory[0]==0xe1&&small_memory[63]==0xe2);
  assert(!device_unregister(&d));driver_dma_buffer_put(held_buffer);assert(page_frees==1&&freed_order==4);
  bus_unregister(&bus);return;
 } else if(!strcmp(name,"short")) {
  wait_remaining=8;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-EMSGSIZE&&urb.actual_length==56&&urb.len==64);
  for(unsigned i=0;i<64;i++)assert(caller[i]==(i<56?0x3c:0x55));
  memset(caller,0x55,64);
  wait_remaining=64;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-EMSGSIZE&&!urb.actual_length&&caller[0]==0x55);
  wait_remaining=65;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-EIO&&!urb.actual_length&&caller[0]==0x55);
  wait_remaining=0;wait_rc=-EIO;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-EIO&&!urb.actual_length&&caller[0]==0x55);
  wait_rc=-6;wait_cc=6;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-EPIPE&&!urb.actual_length&&caller[0]==0x55);
 } else if(!strcmp(name,"admission")) {
  urb.len=65537;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-EMSGSIZE);urb.len=64;
  urb.buf=NULL;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-EINVAL);urb.buf=caller;
  u->slot=3;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-EINVAL);u->slot=1;
  x->dcbaa[1]=0;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-ESHUTDOWN);x->dcbaa[1]=0xabc;
  x->eps->closing=1;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-ESHUTDOWN);x->eps->closing=0;
  x->eps->pending=&urb;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-EBUSY);x->eps->pending=NULL;
  xhci_ep_t *saved=x->eps;x->eps=NULL;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-ENODEV);x->eps=saved;
  struct driver_dma_buffer *buffer=x->bulk_buffer;x->bulk_buffer=NULL;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-ENODEV);x->bulk_buffer=buffer;
  assert(!bulk_bells&&!bulk_waits&&caller[0]==0x55&&!x->xfer_busy);
 } else if(!strcmp(name,"copy-race")) {
  sync_race=1;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-ESHUTDOWN&&!bulk_bells&&caller[0]==0x55);
  x->fatal=0;sync_race=2;assert(xhci_op_submit_bulk(&x->hcd,&urb)==-ESHUTDOWN&&!bulk_bells&&caller[0]==0x55);
  x->eps->closing=0;sync_race=0;
 } else {
  assert(!strcmp(name,"busy"));small_memory[0]=0x77;x->xfer_busy=1;x->xfer_deadline=timer_get_ticks()+2;
  assert(xhci_op_submit_bulk(&x->hcd,&urb)==-EBUSY&&!bulk_bells&&small_memory[0]==0x77);x->xfer_busy=0;
 }
 kfree(caller);assert(!device_unregister(&d));assert(!driver_unregister(&driver));bus_unregister(&bus);
 assert(page_frees==1&&controller_frees==1);
}
