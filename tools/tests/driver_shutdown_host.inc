/* Included by the existing production core/class removal fixture. */
static unsigned legacy_shutdown_calls;
static void legacy_shutdown(device_t *dev) {(void)dev; legacy_shutdown_calls++;}
static void shutdown_case(const char *name)
{
 bus_type_t bus={.name="terminal-test",.match=remove_matches};
 char_dev_ops_t ops={.read=dummy_read};
 driver_t driver={.name="terminal-test",.bus=&bus,.probe=remove_probe,
  .remove=remove_callback,.quiesce=remove_quiesce,.shutdown=legacy_shutdown,
  .class_type=DEV_CLASS_CHAR,.class_ops=&ops};
 struct remove_state state={.target=&driver};
 resource_t range={.type=RES_MMIO,.start=0x91000000,.end=0x91000fff};
 device_t dev={.name="terminal-device",.bus=&bus,.references=1,
  .plat_data=&state,.res=&range,.res_count=1};
 assert(!bus_register(&bus)&&!driver_register(&driver)&&!device_register(&dev));
 state.stale=class_device_ref_for_device(&dev);assert(state.stale);
 assert(!class_device_call_begin(state.stale));
 if(!strcmp(name,"missing")) {
  driver.remove=NULL;
  assert(driver_core_kexec_prepare()==-EOPNOTSUPP);
  assert(driver_core_shutdown_all()==-EOPNOTSUPP);
  assert(!state.quiesces&&!state.released&&!legacy_shutdown_calls);
  assert(dev.drv==&driver&&dev.class_dev==state.stale&&state.buffer);
  driver.remove=remove_callback;
 } else {
  assert(!driver_core_kexec_prepare());
  state.error=-ETIMEDOUT;
  assert(driver_core_shutdown_all()==-ETIMEDOUT);
  assert(dev.drv==&driver&&dev.drv_priv==&state&&state.buffer&&!state.released);
  assert(state.quiesces==1&&state.attempts==1&&!dev.class_dev);
  assert(class_device_call_begin(state.stale)==-ENODEV);
  device_t late={.name="late",.references=1};
  driver_t late_driver={.name="late"};bus_type_t late_bus={.name="late"};
  assert(device_register(&late)==-ESHUTDOWN);
  assert(driver_register(&late_driver)==-ESHUTDOWN);
  assert(bus_register(&late_bus)==-ESHUTDOWN);
  assert(bus_probe_device(&dev)==-ESHUTDOWN);
  driver_resource_claim_t *claim=NULL;
  assert(driver_resource_request(&late,&range,&claim)==-EBUSY&&!claim);
  state.error=0;
 }
 assert(!driver_core_shutdown_all());
 assert(!dev.drv&&!dev.drv_priv&&state.released==1&&state.removed==1);
 assert(state.quiesces==1&&!legacy_shutdown_calls);
 assert(!driver_core_shutdown_all()&&state.removed==1);
 class_device_put(state.stale);
 assert(!device_unregister(&dev)&&!driver_unregister(&driver));bus_unregister(&bus);
}
