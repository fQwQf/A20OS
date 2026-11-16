/* Production core/USB/HID retirement, with a deterministic xHC backend. */
#define XHCI_RING_TRBS 64U
#define XHCI_MAX_EPS 32U
#define XHCI_IRQ_BATCH 8U
#define XHCI_POLL_SPINS 1U
#define XHCI_XFER_HOLD_BUDGET_MS 500U
#define XHCI_BULK_HOLD_BUDGET_MS 5000U
#define XHCI_TRB_TYPE(t) ((uint32_t)(t)<<10)
#define XHCI_TRB_CYCLE 1U
#define XHCI_TRB_CHAIN 16U
#define XHCI_TRB_ENT 2U
#define XHCI_TRB_LINK 6U
#define XHCI_TRB_NORMAL 1U
#define XHCI_TRB_ISP 4U
#define XHCI_TRB_IOC 32U
#define XHCI_TRB_TRANSFER_EVENT 32U
#define XHCI_TRB_CONFIGURE_ENDPOINT 12U
#define XHCI_TRB_RESET_ENDPOINT 14U
#define XHCI_TRB_STOP_ENDPOINT 15U
#define XHCI_TRB_SET_TR_DEQUEUE 16U
#define XHCI_CC_SUCCESS 1U
#define XHCI_CC_SHORT_PACKET 13U
typedef struct {uint64_t parameter;uint32_t status,control;} xhci_trb_t;
typedef struct {xhci_trb_t trbs[XHCI_RING_TRBS];uint16_t enqueue;uint8_t cycle;} xhci_ring_t;
typedef struct xhci_ep {
 struct xhci_ep *next;void *alloc_ptr;xhci_ring_t ring;
 uint8_t slot,dci,addr;usb_urb_t *pending;unsigned callbacks_active;uint8_t closing,stopped,disabled;
} xhci_ep_t;
typedef struct {
 usb_hcd_t hcd;spinlock_t lock;unsigned fatal,halted,xfer_busy,ep_count,max_slots,context_size;
 unsigned callbacks_active,slot_callbacks[3];uint64_t xfer_deadline,dcbaa[3];uintptr_t doorbell;
 uint32_t input_context[33][16],output_context[3][32][16];xhci_ep_t *eps;
} xhci_controller_t;
static unsigned interface_count;static int control_error,expect_device_reset;
static unsigned configure_calls,stop_count,drop_count,reset_count,dequeue_count,bells;
static int configure_error,stop_error,drop_error,stop_readback_error,drop_readback_error,hid_submit_error,drop_fail_dci;
static void no_relax(void) {}
#define arch_cpu_relax no_relax
uint64_t timer_get_ticks(void) {static uint64_t ticks;return ticks++;}
uint64_t clock_ticks_per_sec(void) {return 1000;}
static uintptr_t va_to_pa(const void *p) {return (uintptr_t)p;}
static void sync_noop(const void *p,size_t n) {assert(p&&n);}
#define arch_dma_sync_for_cpu sync_noop
#define arch_dma_sync_for_device sync_noop
#undef wmb
#define wmb() ((void)0)
static void *xhci_input_context(xhci_controller_t *x,unsigned i) {assert(i<33);return x->input_context[i];}
static void *xhci_output_context(xhci_controller_t *x,unsigned s,unsigned i) {assert(s<3&&i<32);return x->output_context[s][i];}
static void xhci_ack_event_irq(xhci_controller_t *x) {(void)x;}
static void xhci_write32(uintptr_t b,uint32_t off,uint32_t v) {(void)b;assert(off==4&&v);bells++;}
static uint32_t xhci_read32(uintptr_t b,uint32_t off) {(void)b;assert(off==4);return 0;}
static int xhci_alloc_ep(device_t *d,xhci_ep_t **out) {(void)d;*out=kcalloc(1,sizeof(**out));if(!*out)return -ENOMEM;(*out)->alloc_ptr=*out;return 0;}
static uint8_t xhci_ep_type(uint8_t type,uint8_t addr) {(void)addr;return type;}
static uint8_t xhci_interval(uint8_t speed,uint8_t n) {(void)speed;return n;}
static void xhci_fill_ep_context(xhci_controller_t *x,uint8_t dci,xhci_ring_t *r,uint8_t type,uint16_t mps,uint8_t interval) {
 (void)r;(void)type;(void)mps;(void)interval;x->input_context[dci+1][0]=1;
}
static void xhci_slot_add_context(uint32_t *slot,unsigned dci) {if((slot[0]>>27)<dci)slot[0]=(slot[0]&0x07ffffffU)|(dci<<27);}
static int xhci_command_locked(xhci_controller_t *x,uint64_t p,uint32_t status,uint32_t control,void *event) {
 assert(x->xfer_busy&&!status&&!event);unsigned type=(control>>10)&63,slot=control>>24,dci=(control>>16)&31;
 assert(slot==1);
 if(type==12) {
  assert(p==va_to_pa(x->input_context));uint32_t drop=x->input_context[0][0],add=x->input_context[0][1];
  if(!drop) {
   configure_calls++;if(expect_device_reset)assert(interface_count==configure_calls);unsigned index=2;while(!(add&(1U<<index)))index++;
   assert(index<32&&x->eps&&x->eps->closing&&x->eps->ring.cycle==1);
   if(configure_error){if(configure_error==-ETIMEDOUT){x->fatal=1;x->output_context[1][index][0]=1;}return configure_error;}
   x->output_context[1][index][0]=1;x->output_context[1][0][0]=x->input_context[1][0];return 0;
  }
  assert(add==1&&!(drop&3));unsigned index=2;while(!(drop&(1U<<index)))index++;
  assert(drop==(1U<<index)&&x->output_context[1][index][0]==3);drop_count++;
  if(drop_error&&(!drop_fail_dci||drop_fail_dci==(int)index))return drop_error;
  if(!drop_readback_error)x->output_context[1][index][0]=0;
  x->output_context[1][0][0]=x->input_context[1][0];return 0;
 }
 assert(dci>=2&&dci<32);
 if(type==15){assert(!p);stop_count++;if(stop_error)return stop_error;if(!stop_readback_error)x->output_context[1][dci][0]=3;return 0;}
 if(type==14){assert(!p&&x->output_context[1][dci][0]==2);reset_count++;x->output_context[1][dci][0]=3;return 0;}
 assert(type==16&&p&&x->output_context[1][dci][0]==4);dequeue_count++;x->output_context[1][dci][0]=3;return 0;
}
/* PRODUCTION_FUNCTIONS */
/* HID_TYPES */
static void usb_hid_select_protocol(usb_hid_dev_t *h) {if(hid_submit_error)((xhci_controller_t *)h->iface->dev->hcd->priv)->dcbaa[1]=0;}
static void usb_hid_complete(usb_urb_t *urb) {(void)urb;}
/* HID_FUNCTIONS */
typedef struct {usb_hcd_t hcd;spinlock_t sc_lock;uint8_t stopping;} usb_hub_t;
/* HUB_FUNCTIONS */
static int release_start(usb_hcd_t *h) {(void)h;return 0;}
static int release_abort(usb_hcd_t *h,uint8_t slot) {
 xhci_controller_t *x=h->priv;assert(slot==1);x->halted=1;
 while(x->eps){assert(!xhci_op_release_endpoint(h,h->port_devices[0],x->eps->addr));}return 0;
}
static int release_control(usb_hcd_t *h,uint8_t slot,const usb_setup_packet_t *setup,void *buffer) {
 (void)h;assert(slot==1&&!buffer&&setup->request_type==1&&setup->request==USB_REQ_SET_INTERFACE&&setup->value==0&&setup->index==0&&!setup->length);
 if(control_error)return control_error;
 interface_count++;return 0;
}
static const usb_hcd_ops_t release_ops={.start=release_start,.configure_endpoint=xhci_op_configure_endpoint,
 .control=release_control,.submit_interrupt=xhci_op_submit_interrupt,.release_endpoint=xhci_op_release_endpoint,.abort_slot=release_abort};
static pthread_barrier_t cb_entered,cb_release;
static void release_barrier(pthread_barrier_t *b) {int r=pthread_barrier_wait(b);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
static void blocking_complete(usb_urb_t *urb) {release_barrier(&cb_entered);release_barrier(&cb_release);assert(urb->ctx);}
struct callback_args {xhci_controller_t *x;usb_urb_t *urb;};
static void *release_callback_worker(void *arg) {
 struct callback_args *a=arg;usb_urb_t *ready[1]={a->urb};uint64_t flags=spin_lock_irqsave(&a->x->lock);
 xhci_run_completions(ready,1,a->x,&flags);spin_unlock_irqrestore(&a->x->lock,flags);return NULL;
}
static int endpoint_match(device_t *d,const driver_t *r) {(void)d;(void)r;return 1;}
static void endpoint_case(const char *name) {
 xhci_controller_t x={.max_slots=2,.context_size=32};spin_init(&x.lock);x.dcbaa[1]=123;
 x.hcd=(usb_hcd_t){.ops=&release_ops,.priv=&x,.max_ports=1};assert(!usb_core_register_hcd(&x.hcd));
 usb_device_t *u=usb_create(&x.hcd,1);u->slot=1;x.hcd.port_devices[0]=u;
 usb_interface_t *iface=&u->ifaces[0];iface->ep_count=1;iface->interface_subclass=1;iface->interface_protocol=1;
 iface->eps[0]=(usb_endpoint_t){.iface=iface,.addr=0x81,.attrs=USB_XFER_INTERRUPT,.max_packet=8,.interval=1};
 if(!strncmp(name,"probe-",6)) {
  expect_device_reset=1;
  bus_type_t bus={.name="hid-release",.match=endpoint_match};assert(!bus_register(&bus));
  driver_t driver={.name="hid",.bus=&bus,.probe=usb_hid_probe,.remove=usb_hid_remove,.quiesce=usb_hid_quiesce};
  device_t d={.name="hid",.bus=&bus,.plat_data=iface,.references=1};iface->device=&d;
  assert(!device_register(&d));g_drivers[g_driver_count++]=&driver;
  if(!strcmp(name,"probe-configure"))configure_error=-ETIMEDOUT;
  if(!strcmp(name,"probe-interface"))control_error=-EPIPE;
  if(!strcmp(name,"probe-submit"))hid_submit_error=1;
  int ret=bus_probe_device(&d);
  if(!strcmp(name,"probe-configure")) {
   assert(ret==-ESHUTDOWN&&d.probe_error==-ETIMEDOUT&&d.drv==&driver&&d.drv_priv&&d.state==DEV_STATE_REMOVING&&x.eps);
   assert(device_unregister(&d)==-ESHUTDOWN&&d.drv_priv&&x.ep_count==1);
   x.halted=1;assert(!device_unregister(&d)&&!d.drv_priv&&!x.eps);
  } else if(!strcmp(name,"probe-interface")){assert(ret==-EPIPE&&!d.drv&&!d.drv_priv&&!x.eps&&!configure_calls);assert(!device_unregister(&d));}
  else if(!strcmp(name,"probe-submit")){assert(ret==-ESHUTDOWN&&!d.drv&&!d.drv_priv&&!x.eps);assert(!device_unregister(&d));}
  else {
   assert(!ret&&d.drv_priv&&x.eps->pending);usb_hid_dev_t *private=d.drv_priv;void *saved=private->urb.ctx;
   stop_error=-EBUSY;assert(driver_unregister(&driver)==-EBUSY&&d.drv_priv==private&&!private->running);
   assert(private->urb.ctx==saved&&private->urb.complete&&x.eps->pending==&private->urb);
   stop_error=0;assert(!driver_unregister(&driver)&&!d.drv_priv&&!x.eps);
   assert(!driver_register(&driver));assert(d.drv==&driver&&d.drv_priv&&x.eps->pending);
   assert(!device_unregister(&d));
  }
  assert(!driver_unregister(&driver));bus_unregister(&bus);iface->device=NULL;
  x.fatal=0;assert(!usb_core_unregister_hcd(&x.hcd));return;
 }
 assert(!xhci_op_configure_endpoint(&x.hcd,u,0x81,USB_XFER_INTERRUPT,8,1));
 xhci_ep_t *ep=x.eps;usb_urb_t urb={.dev=u,.ep=iface->eps,.buf=(uint8_t[8]){0},.len=8,.ctx=&x,.complete=blocking_complete};
 assert(!xhci_op_submit_interrupt(&x.hcd,&urb));
 if(!strcmp(name,"retry")) {
  drop_error=-EBUSY;assert(xhci_op_release_endpoint(&x.hcd,u,0x81)==-EBUSY&&x.eps==ep&&ep->stopped);
  assert(stop_count==1&&drop_count==1&&ep->pending==&urb);
  xhci_trb_t event={.control=XHCI_TRB_TYPE(XHCI_TRB_TRANSFER_EVENT)|(1U<<24)|(3U<<16)};
  usb_urb_t *ready[1];assert(!xhci_collect_interrupt(&x,&event,ready)&&ep->pending==&urb);
  memset(urb.buf,0x77,urb.len);assert(xhci_op_submit_interrupt(&x.hcd,&urb)==-ESHUTDOWN&&((uint8_t *)urb.buf)[0]==0x77);
  drop_error=0;assert(!xhci_op_release_endpoint(&x.hcd,u,0x81)&&!x.eps&&stop_count==1&&drop_count==2);
  assert(!xhci_op_release_endpoint(&x.hcd,u,0x81)&&drop_count==2);
 } else if(!strcmp(name,"callback")) {
  assert(!pthread_barrier_init(&cb_entered,NULL,2)&&!pthread_barrier_init(&cb_release,NULL,2));
  pthread_t worker;struct callback_args args={&x,&urb};assert(!pthread_create(&worker,NULL,release_callback_worker,&args));release_barrier(&cb_entered);
  assert(xhci_op_release_endpoint(&x.hcd,u,0x81)==-EBUSY&&ep->closing&&!stop_count&&ep->callbacks_active==1);
  release_barrier(&cb_release);assert(!pthread_join(worker,NULL));assert(!ep->callbacks_active);
  assert(!xhci_op_release_endpoint(&x.hcd,u,0x81));assert(!pthread_barrier_destroy(&cb_entered)&&!pthread_barrier_destroy(&cb_release));
 } else if(!strcmp(name,"siblings")) {
  assert(!xhci_op_configure_endpoint(&x.hcd,u,0x82,USB_XFER_INTERRUPT,8,1));
  xhci_ep_t *other=x.eps;other->callbacks_active=1;
  assert(!xhci_op_release_endpoint(&x.hcd,u,0x81)&&x.eps==other&&x.ep_count==1);
  assert(x.output_context[1][5][0]==1&&(x.output_context[1][0][0]>>27)==5);
  other->callbacks_active=0;assert(!xhci_op_release_endpoint(&x.hcd,u,0x82));
 } else if(!strcmp(name,"states")) {
  x.output_context[1][3][0]=2;assert(!xhci_op_release_endpoint(&x.hcd,u,0x81)&&reset_count==1&&!stop_count);
  assert(!xhci_op_configure_endpoint(&x.hcd,u,0x81,USB_XFER_INTERRUPT,8,1));x.output_context[1][3][0]=4;
  assert(!xhci_op_release_endpoint(&x.hcd,u,0x81)&&dequeue_count==1);
 } else if(!strcmp(name,"readback")) {
  stop_readback_error=1;assert(xhci_op_release_endpoint(&x.hcd,u,0x81)==-EIO&&x.eps==ep&&!ep->stopped&&!drop_count);
  stop_readback_error=0;drop_readback_error=1;assert(xhci_op_release_endpoint(&x.hcd,u,0x81)==-EIO&&ep->stopped&&x.eps==ep);
  drop_readback_error=0;assert(!xhci_op_release_endpoint(&x.hcd,u,0x81)&&stop_count==2&&drop_count==2);
 } else if(!strcmp(name,"configure")) {
  assert(xhci_op_configure_endpoint(&x.hcd,u,0x81,USB_XFER_INTERRUPT,8,1)==-EBUSY&&x.eps==ep&&x.ep_count==1);
  assert(!xhci_op_release_endpoint(&x.hcd,u,0x81));configure_error=-22;
  assert(xhci_op_configure_endpoint(&x.hcd,u,0x81,USB_XFER_INTERRUPT,8,1)==-22&&x.eps&&x.eps->closing);
  assert(!xhci_op_release_endpoint(&x.hcd,u,0x81)&&stop_count==1);
 } else if(!strcmp(name,"busy-fatal")) {
  x.xfer_busy=1;assert(xhci_op_release_endpoint(&x.hcd,u,0x81)==-EBUSY&&x.eps==ep&&!stop_count);
  x.xfer_busy=0;x.fatal=1;assert(xhci_op_release_endpoint(&x.hcd,u,0x81)==-ESHUTDOWN&&x.eps==ep);
  x.halted=1;assert(!xhci_op_release_endpoint(&x.hcd,u,0x81)&&!x.eps&&!stop_count&&!drop_count);
 } else if(!strcmp(name,"partial-interface")) {
  iface->eps=krealloc(iface->eps,2*sizeof(*iface->eps));assert(iface->eps);iface->eps[1]=iface->eps[0];iface->eps[1].addr=0x82;iface->ep_count=2;
  assert(!xhci_op_configure_endpoint(&x.hcd,u,0x82,USB_XFER_INTERRUPT,8,1));
  device_t d={.plat_data=iface,.state=DEV_STATE_REMOVING};iface->device=&d;
  drop_error=-EBUSY;drop_fail_dci=5;assert(usb_interface_release_endpoints(iface)==-EBUSY&&x.ep_count==1&&x.eps->dci==5);
  assert(stop_count==2&&drop_count==2);drop_error=0;
  assert(!usb_interface_release_endpoints(iface)&&!x.eps&&stop_count==2&&drop_count==3);iface->device=NULL;
 } else if(!strcmp(name,"hub-partial")) {
  usb_hub_t *hub=kcalloc(1,sizeof(*hub));assert(hub);spin_init(&hub->sc_lock);
  device_t d={.plat_data=iface,.state=DEV_STATE_PROBED,.drv_priv=hub};iface->device=&d;
  hub->hcd=(usb_hcd_t){.ops=&release_ops,.parent_hcd=&x.hcd,.hcd_dev=&d,.priv=&x};
  assert(!usb_core_register_hcd(&hub->hcd));d.state=DEV_STATE_REMOVING;
  stop_error=-EBUSY;assert(usb_hub_remove(&d)==-EBUSY&&d.drv_priv==hub&&hub->stopping);
  assert(!hub->hcd.port_devices&&!usb_hcd_registered(&hub->hcd)&&x.eps==ep);
  stop_error=0;assert(!usb_hub_remove(&d)&&!d.drv_priv&&!x.eps&&g_hcd_count==1);iface->device=NULL;
 } else if(!strcmp(name,"admission")) {
  device_t d={.plat_data=iface,.state=DEV_STATE_PROBED};iface->device=&d;
  assert(usb_interface_release_endpoints(iface)==-EINVAL&&x.eps==ep);
  d.state=DEV_STATE_REMOVING;usb_hcd_ops_t missing=release_ops;missing.release_endpoint=NULL;x.hcd.ops=&missing;
  assert(usb_interface_release_endpoints(iface)==-EOPNOTSUPP&&x.eps==ep);x.hcd.ops=&release_ops;
  x.hcd.port_devices[0]=NULL;assert(usb_interface_release_endpoints(iface)==-ENODEV&&x.eps==ep);x.hcd.port_devices[0]=u;
  assert(!usb_interface_release_endpoints(iface)&&!x.eps);iface->device=NULL;
 } else assert(0);
 assert(!usb_core_unregister_hcd(&x.hcd));
}
