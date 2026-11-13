/* Real driver core + USB graph. Host threads replace scheduler/hardware. */
#include <time.h>
static _Thread_local char task_identity;
static _Thread_local int boot_context;
static unsigned start_calls,poll_calls,reset_calls,stop_calls;
static int stop_failure,mode,gate_poll,gate_reset;
static usb_hcd_t *current_hcd,*remove_hcd,*add_hcd,*earlier_hcd;
static usb_device_t *current_device;
static pthread_barrier_t entered,release_gate,remove_started;
static void barrier(pthread_barrier_t *b) {int r=pthread_barrier_wait(b);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
static uint64_t host_clock;
uint64_t timer_get_ticks(void) {return __atomic_add_fetch(&host_clock,1000,__ATOMIC_RELAXED);}
uint64_t clock_ticks_per_sec(void) {return 1000;}
static int concurrent_start(usb_hcd_t *h) {
 start_calls++;
 if(mode==1&&h==current_hcd) {
  assert(usb_core_register_hcd(h)==-EBUSY&&usb_core_unregister_hcd(h)==-EBUSY);
  assert(usb_core_enumerate_port(h,1)==-EBUSY);usb_core_scan();usb_core_poll();
 }
 if(mode==5&&h->parent_hcd)assert(usb_core_unregister_hcd(h->parent_hcd)==-EBUSY);
 if(mode==2&&h==current_hcd)assert(usb_core_register_hcd(add_hcd)==-ENOSPC);
 return 0;
}
static int concurrent_stop(usb_hcd_t *h,uint8_t slot) {(void)h;assert(slot==1);stop_calls++;return stop_failure?-EBUSY:0;}
static int concurrent_poll(usb_hcd_t *h) {
 poll_calls++;
 if(gate_poll){barrier(&entered);barrier(&release_gate);assert(h->port_devices);}
 if(mode==3&&h==current_hcd) {
  assert(usb_core_unregister_hcd(h)==-EBUSY&&usb_core_register_hcd(h)==-EBUSY);
  assert(usb_core_enumerate_port(h,1)==-EBUSY);usb_core_scan();usb_core_poll();
  assert(!usb_core_unregister_hcd(remove_hcd));assert(!usb_core_register_hcd(add_hcd));
 } else if(mode==3&&h==add_hcd)assert(!usb_core_unregister_hcd(earlier_hcd));
 return 0;
}
static int concurrent_connected(usb_hcd_t *h,unsigned port) {
 assert(port==1);return mode==4?h!=current_hcd:1;
}
static int concurrent_reset(usb_hcd_t *h,unsigned port,uint8_t *speed) {
 assert(port==1);reset_calls++;*speed=USB_SPEED_HIGH;
 if(mode==6)h->hcd_dev->state=DEV_STATE_PREPARING;
 if(mode==7)current_device->hcd->port_state[0]|=USB_PORT_REMOVE_PENDING;
 if(gate_reset){barrier(&entered);barrier(&release_gate);assert(h->port_devices);}
 return 0;
}
static const usb_hcd_ops_t concurrent_ops={.start=concurrent_start,.poll=concurrent_poll,
 .port_connected=concurrent_connected,.reset_port=concurrent_reset,.alloc_slot=enum_slot,
 .update_ep0_mps=enum_mps,.get_descriptor=enum_descriptor,.control=enum_control,.abort_slot=concurrent_stop};
static usb_device_t *empty_port(usb_hcd_t *h) {
 usb_device_t *u=kcalloc(1,sizeof(*u));assert(u);u->hcd=h;u->slot=1;u->address=usb_core_alloc_address();assert(u->address==1);
 h->port_devices[0]=u;h->port_state[0]=USB_PORT_ADDRESS_OWNED;return u;
}
static void *try_poll(void *unused) {
 (void)unused;assert(driver_lifecycle_try_begin()==-EAGAIN);
 assert(usb_device_poll(current_device)==-EAGAIN);usb_core_poll();return NULL;
}
static void *scan_worker(void *unused) {(void)unused;usb_core_scan();return NULL;}
static void *enumerate_worker(void *unused) {(void)unused;assert(!usb_core_enumerate_port(current_hcd,1));return NULL;}
static void *remove_worker(void *unused) {
 (void)unused;barrier(&remove_started);assert(!usb_core_unregister_hcd(current_hcd));return NULL;
}
static void timed_join(pthread_t worker,int expected) {
 struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));
 deadline.tv_nsec+=100000000;if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
 assert(pthread_timedjoin_np(worker,NULL,&deadline)==expected);
}
static int probe_calls;
static int coordinator_match(device_t *d,const driver_t *r) {(void)d;(void)r;return 1;}
static int coordinator_probe(device_t *d) {(void)d;probe_calls++;return 0;}
static void concurrency_case(const char *name) {
 if(!strcmp(name,"lifecycle")||!strcmp(name,"boot")) {
  boot_context=!strcmp(name,"boot");assert(!driver_lifecycle_try_begin());
  assert(driver_lifecycle_try_begin()==-EAGAIN);driver_lifecycle_begin();
  assert(driver_lifecycle_try_begin()==-EAGAIN);driver_lifecycle_end();driver_lifecycle_end();
  assert(!g_driver_core_ops.depth&&!boot_ops_depth[0]&&!discovery_depth);
  bus_type_t bus={.name="coord",.match=coordinator_match};assert(!bus_register(&bus));
  driver_t driver={.name="coord",.bus=&bus,.probe=coordinator_probe};assert(!driver_register(&driver));
  device_t a={.name="a",.bus=&bus,.references=1},b=a;
  driver_lifecycle_begin();assert(!device_register(&a)&&probe_calls==1);driver_lifecycle_end();
  driver_discovery_begin();assert(!device_register(&b)&&probe_calls==1);driver_discovery_end();
  driver_probe_all();assert(probe_calls==2);assert(!device_unregister(&a)&&!device_unregister(&b));
  assert(!driver_unregister(&driver));bus_unregister(&bus);boot_context=0;return;
 }
 usb_config_length=879;
 usb_hcd_t h={.ops=&concurrent_ops,.max_ports=1};current_hcd=&h;
 if(!strcmp(name,"busy")) {
  assert(!usb_core_register_hcd(&h));current_device=empty_port(&h);
  driver_lifecycle_begin();pthread_t worker;assert(!pthread_create(&worker,NULL,try_poll,NULL));
  timed_join(worker,0);assert(!poll_calls);driver_lifecycle_end();
  assert(!usb_device_poll(current_device)&&poll_calls==1);usb_core_poll();assert(poll_calls==2);
  h.port_state[0]|=USB_PORT_REMOVE_PENDING;assert(usb_device_poll(current_device)==-ENODEV&&poll_calls==2);
  assert(!usb_core_unregister_hcd(&h));return;
 }
 if(!strcmp(name,"scan")||!strcmp(name,"enumerate")) {
  assert(!usb_core_register_hcd(&h));
  gate_poll=!strcmp(name,"scan");gate_reset=!gate_poll;
  if(gate_poll)current_device=empty_port(&h);
  assert(!pthread_barrier_init(&entered,NULL,2)&&!pthread_barrier_init(&release_gate,NULL,2));
  assert(!pthread_barrier_init(&remove_started,NULL,2));
  pthread_t a,b;assert(!pthread_create(&a,NULL,gate_poll?scan_worker:enumerate_worker,NULL));barrier(&entered);
  assert(!pthread_create(&b,NULL,remove_worker,NULL));barrier(&remove_started);
  timed_join(b,ETIMEDOUT);assert(!stop_calls);barrier(&release_gate);
  assert(!pthread_join(a,NULL)&&!pthread_join(b,NULL));
  assert(!h.port_devices&&!h.port_state&&stop_calls==1&&!g_hcd_count);
  assert(!pthread_barrier_destroy(&entered)&&!pthread_barrier_destroy(&release_gate)&&!pthread_barrier_destroy(&remove_started));return;
 }
 if(!strcmp(name,"registry")) {
  mode=1;assert(!usb_core_register_hcd(&h));void *ports=h.port_devices;
  assert(usb_core_register_hcd(&h)==-EEXIST&&h.port_devices==ports&&start_calls==1&&!poll_calls);
  mode=5;usb_hcd_t child={.ops=&concurrent_ops,.parent_hcd=&h};assert(!usb_core_register_hcd(&child));
  assert(usb_core_unregister_hcd(&h)==-EBUSY&&h.port_devices==ports);
  assert(!usb_core_unregister_hcd(&child)&&!usb_core_unregister_hcd(&h));
  usb_hcd_t invalid={.ops=&concurrent_ops};invalid.parent_hcd=&invalid;
  assert(usb_core_register_hcd(&invalid)==-ENODEV);
  invalid.parent_hcd=(void *)0x1234;assert(usb_core_register_hcd(&invalid)==-ENODEV);return;
 }
 if(!strcmp(name,"reset-retiring")) {
  assert(!usb_core_register_hcd(&h));current_device=empty_port(&h);
  usb_hcd_t child={.ops=&concurrent_ops,.max_ports=1,.parent_hcd=&h};assert(!usb_core_register_hcd(&child));
  mode=7;usb_core_scan();assert(reset_calls==1&&!child.port_devices[0]&&child.port_state[0]==0);
  assert(h.port_state[0]&USB_PORT_REMOVE_PENDING);
  assert(!usb_core_unregister_hcd(&child)&&!usb_core_unregister_hcd(&h));return;
 }
 if(!strcmp(name,"reset-stopping")) {
  device_t owner={.state=DEV_STATE_PROBED,.references=1};h.hcd_dev=&owner;
  assert(!usb_core_register_hcd(&h));mode=6;
  assert(usb_core_enumerate_port(&h,1)==-ENODEV&&reset_calls==1&&!h.port_devices[0]);
  assert(!stop_calls&&usb_core_alloc_address()==1);usb_core_free_address(1);
  assert(!usb_core_unregister_hcd(&h));return;
 }
 if(!strcmp(name,"stopping-parent")) {
  device_t owner={.state=DEV_STATE_PROBED,.references=1};h.hcd_dev=&owner;h.max_ports=0;
  assert(!usb_core_register_hcd(&h));
  usb_hcd_t child={.ops=&concurrent_ops,.max_ports=1,.parent_hcd=&h};assert(!usb_core_register_hcd(&child));
  usb_device_t *u=empty_port(&child);owner.state=DEV_STATE_PREPARING;
  assert(usb_device_poll(u)==-ENODEV&&usb_core_enumerate_port(&child,1)==-ENODEV);
  usb_hcd_t later={.ops=&concurrent_ops,.parent_hcd=&child};assert(usb_core_register_hcd(&later)==-ENODEV);
  usb_core_scan();assert(!poll_calls&&!reset_calls);
  assert(!usb_core_unregister_hcd(&child)&&!usb_core_unregister_hcd(&h));return;
 }
 if(!strcmp(name,"capacity")) {
  usb_hcd_t controllers[USB_MAX_HCDS+1];memset(controllers,0,sizeof(controllers));
  for(unsigned i=0;i<USB_MAX_HCDS+1;i++)controllers[i].ops=&concurrent_ops;
  for(unsigned i=0;i<USB_MAX_HCDS-1;i++)assert(!usb_core_register_hcd(&controllers[i]));
  mode=2;current_hcd=&controllers[USB_MAX_HCDS-1];add_hcd=&controllers[USB_MAX_HCDS];
  assert(!usb_core_register_hcd(current_hcd)&&g_hcd_count==USB_MAX_HCDS&&start_calls==USB_MAX_HCDS);
  for(unsigned i=0;i<USB_MAX_HCDS;i++){assert(!usb_core_unregister_hcd(&controllers[i]));}
  return;
 }
 if(!strcmp(name,"compaction")) {
  usb_hcd_t a={.ops=&concurrent_ops},b=a,c=a,d=a;
  current_hcd=&a;remove_hcd=&b;add_hcd=&c;earlier_hcd=&a;
  assert(!usb_core_register_hcd(&a)&&!usb_core_register_hcd(&b)&&!usb_core_register_hcd(&d));
  mode=3;usb_core_scan();assert(poll_calls==3&&g_hcd_count==2);
  assert(usb_hcd_registered(&c)&&usb_hcd_registered(&d)&&!usb_hcd_registered(&a)&&!usb_hcd_registered(&b));
  assert(!usb_core_unregister_hcd(&c)&&!usb_core_unregister_hcd(&d));return;
 }
 assert(!strcmp(name,"retire"));
 mode=4;assert(!usb_core_register_hcd(&h));current_device=empty_port(&h);stop_failure=1;
 usb_hcd_t child={.ops=&concurrent_ops,.max_ports=1,.parent_hcd=&h};assert(!usb_core_register_hcd(&child));
 for(unsigned i=0;i<4;i++)usb_core_scan();
 assert(!reset_calls&&child.port_state[0]==0&&h.port_devices[0]==current_device);
 assert(usb_core_enumerate_port(&child,1)==-EAGAIN&&!reset_calls);
 stop_failure=0;usb_core_scan();assert(reset_calls==1&&child.port_devices[0]&&!h.port_devices[0]);
 assert(!usb_core_unregister_hcd(&child)&&!usb_core_unregister_hcd(&h));
}
