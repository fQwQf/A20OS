/* Real completion dispatch and stop paths; the worker models an unlocked
 * process/poll callback, so free_irq deliberately does not wait on it. */
static pthread_barrier_t callback_entered,callback_release;
static int callback_self_free;
struct callback_payload {usb_urb_t urb;usb_device_t device;unsigned sentinel;};
static void callback_barrier(pthread_barrier_t *b) {int r=pthread_barrier_wait(b);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
static void callback_wait(usb_urb_t *urb) {
 struct callback_payload *p=urb->ctx;callback_barrier(&callback_entered);callback_barrier(&callback_release);
 assert(p->sentinel==0x76543210);if(callback_self_free)kfree(p);
}
static void callback_reentrant(usb_urb_t *urb) {
 struct callback_payload *p=urb->ctx;
 assert(xhci_prepare_remove(stop_controller->hcd.hcd_dev)==-EBUSY);
 assert(!stop_controller->halted&&p->sentinel==0x76543210);
}
static void *callback_worker(void *opaque) {
 usb_urb_t *ready[1]={opaque};uint64_t flags=spin_lock_irqsave(&stop_controller->lock);
 xhci_run_completions(ready,1,stop_controller,&flags);
 spin_unlock_irqrestore(&stop_controller->lock,flags);return NULL;
}
static void callback_case(const char *name) {
 xhci_controller_t x={.op=0x1000,.runtime=0x2000,.hardware_owned=1,.running=1,.irq_registered=1,.irq=12,.max_slots=2};
 stop_controller=&x;spin_init(&x.lock);device_t parent={.drv_priv=&x};
 x.hcd.hcd_dev=&parent;x.hcd.priv=&x;x.dcbaa[1]=x.dcbaa[2]=0xabc;
 xhci_ep_t *ep=kcalloc(1,sizeof(*ep));assert(ep);ep->alloc_ptr=ep;ep->slot=1;x.eps=ep;x.ep_count=1;
 if(!strcmp(name,"control")) {
  usb_setup_packet_t setup={0};assert(!xhci_control(&x,1,&setup,NULL)&&control_posts==1);
  x.dcbaa[1]=0;assert(xhci_control(&x,1,&setup,NULL)==-ESHUTDOWN&&control_posts==1&&!x.xfer_busy);
  assert(!xhci_op_abort_slot(&x.hcd,1)&&!commands_issued);return;
 }
 struct callback_payload *p=kcalloc(1,sizeof(*p));assert(p);
 p->sentinel=0x76543210;p->device.slot=!strcmp(name,"unrelated")?2:1;
 p->urb=(usb_urb_t){.dev=&p->device,.ctx=p,.complete=callback_wait};
 if(!strcmp(name,"reentrant")) {
  p->urb.complete=callback_reentrant;callback_worker(&p->urb);
  assert(!x.callbacks_active&&!x.slot_callbacks[1]);assert(!xhci_prepare_remove(&parent));
  assert(!xhci_op_abort_slot(&x.hcd,1));kfree(p);return;
 }
 callback_self_free=!strcmp(name,"self-free");
 assert(!pthread_barrier_init(&callback_entered,NULL,2)&&!pthread_barrier_init(&callback_release,NULL,2));
 pthread_t worker;assert(!pthread_create(&worker,NULL,callback_worker,&p->urb));callback_barrier(&callback_entered);
 uint64_t flags=spin_lock_irqsave(&x.lock);assert(x.callbacks_active==1&&x.slot_callbacks[p->device.slot]==1);
 spin_unlock_irqrestore(&x.lock,flags);
 if(!strcmp(name,"abort")||!strcmp(name,"unrelated")) {
  int ret=xhci_op_abort_slot(&x.hcd,1);
  assert(ret==(!strcmp(name,"abort")?-EBUSY:0));
  assert(!x.dcbaa[1]&&!x.eps&&commands_issued==1&&endpoint_frees==1);
  assert(xhci_op_abort_slot(&x.hcd,1)==ret&&commands_issued==1&&endpoint_frees==1);
 } else {
  assert(xhci_prepare_remove(&parent)==-EBUSY&&!x.halted&&x.eps==ep);
  assert(irq_frees==1&&x.fatal);
 }
 callback_barrier(&callback_release);assert(!pthread_join(worker,NULL));
 flags=spin_lock_irqsave(&x.lock);assert(!x.callbacks_active&&!x.slot_callbacks[1]&&!x.slot_callbacks[2]);
 spin_unlock_irqrestore(&x.lock,flags);
 if(!strcmp(name,"abort")||!strcmp(name,"unrelated")) {
  assert(!xhci_op_abort_slot(&x.hcd,1)&&commands_issued==1&&endpoint_frees==1);
 } else {
  assert(!xhci_prepare_remove(&parent)&&x.halted&&irq_frees==1);
  assert(!xhci_op_abort_slot(&x.hcd,1)&&!commands_issued&&endpoint_frees==1);
 }
 if(!callback_self_free)kfree(p);
 assert(!pthread_barrier_destroy(&callback_entered)&&!pthread_barrier_destroy(&callback_release));
}
