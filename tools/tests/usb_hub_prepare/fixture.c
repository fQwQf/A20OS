/* Production hub probe/prepare/remove, USB graph, devres and unbind ordering. */
#include "drivers/usb/class/usb_hub.c"

static usb_urb_t *hub_pending;
static unsigned hub_submits, hub_controls, hub_endpoint_releases, hub_child_aborts;
static int hub_child_error, hub_abort_error, hub_endpoint_error;

static int hub_backend_start(usb_hcd_t *h) { (void)h; return 0; }
static int hub_backend_connected(usb_hcd_t *h, unsigned port) {
    assert(port == 1);
    return h->port_devices[0] != NULL;
}
static int hub_backend_control(usb_hcd_t *h, uint8_t slot,
                               const usb_setup_packet_t *setup, void *data) {
    (void)h;
    assert(slot == 1);
    hub_controls++;
    if (setup->request == USB_REQ_GET_DESCRIPTOR) {
        assert(setup->value == (USB_DT_HUB << 8));
        assert(setup->length == sizeof(usb_hub_descriptor_t));
        usb_hub_descriptor_t descriptor = { .length = sizeof(descriptor),
            .type = USB_DT_HUB, .ports = 1 };
        memcpy(data, &descriptor, sizeof(descriptor));
    } else if (setup->request == USB_REQ_GET_STATUS) {
        assert(setup->index == 1 && setup->length == 4 && data);
        memset(data, 0, 4);
    } else assert(setup->request == USB_REQ_SET_FEATURE ||
                   setup->request == USB_REQ_SET_INTERFACE);
    return 0;
}
static int hub_backend_configure_hub(usb_hcd_t *h, usb_device_t *u,
                                     uint8_t ports, uint8_t think) {
    (void)h; assert(u->slot == 1 && ports == 1 && !think); return 0;
}
static int hub_backend_configure_endpoint(usb_hcd_t *h, usb_device_t *u,
                                          uint8_t addr, uint8_t type,
                                          uint16_t mps, uint8_t interval) {
    (void)h;
    assert(u->slot == 1 && addr == 0x81 && type == USB_XFER_INTERRUPT);
    assert(mps == 8 && interval == 1);
    return 0;
}
static int hub_backend_interrupt(usb_hcd_t *h, usb_urb_t *urb) {
    (void)h;
    assert(!hub_pending && urb->ctx && urb->complete);
    hub_pending = urb; hub_submits++;
    return 0;
}
static int hub_backend_abort(usb_hcd_t *h, uint8_t slot) {
    (void)h;
    if (slot == 2) { hub_child_aborts++; return hub_abort_error; }
    assert(slot == 1 && !hub_pending);
    return 0;
}
static int hub_backend_release(usb_hcd_t *h, usb_device_t *u, uint8_t addr) {
    (void)h;
    assert(u->slot == 1 && addr == 0x81 && hub_pending);
    usb_hub_t *hub = hub_pending->ctx;
    assert(hub->stopping && !hub->hcd.port_devices);
    hub_endpoint_releases++;
    if (hub_endpoint_error) return hub_endpoint_error;
    hub_pending->complete(hub_pending);
    hub_pending = NULL;
    return 0;
}
static int hub_leaf_probe(device_t *d) { (void)d; return 0; }
static int hub_leaf_remove(device_t *d) { (void)d; return hub_child_error; }

static void hub_prepare_case(void) {
    device_t parent = { .name = "hcd", .references = 1 };
    static const usb_hcd_ops_t ops = { .start = hub_backend_start,
        .port_connected = hub_backend_connected, .control = hub_backend_control,
        .configure_hub = hub_backend_configure_hub,
        .configure_endpoint = hub_backend_configure_endpoint,
        .release_endpoint = hub_backend_release, .abort_slot = hub_backend_abort,
        .submit_interrupt = hub_backend_interrupt };
    usb_hcd_t root = { .hcd_dev = &parent, .ops = &ops, .max_ports = 1 };
    assert(!usb_core_register_hcd(&root));
    static const device_id_t leaf_ids[] = { { .vendor = USB_CLASS_HID,
        .device = DEVICE_ANY, .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY }, {0} };
    driver_t leaf_driver = { .name = "leaf", .id_table = leaf_ids,
        .probe = hub_leaf_probe, .remove = hub_leaf_remove };
    g_drivers[0] = &usb_hub_driver; g_drivers[1] = &leaf_driver; g_driver_count = 2;
    usb_device_t *u = usb_create(&root, 1); u->slot = 1; u->speed = USB_SPEED_HIGH;
    usb_interface_t *iface = u->ifaces;
    iface->interface_class = USB_CLASS_HUB;
    iface->ep_count = 1;
    iface->eps[0] = (usb_endpoint_t){ .iface = iface, .addr = 0x81,
        .attrs = USB_XFER_INTERRUPT, .max_packet = 8, .interval = 1 };
    root.port_devices[0] = u;
    assert(!usb_publish_interfaces(u));
    device_t *dev = iface->device;
    usb_hub_t *hub = dev->drv_priv;
    assert(hub && dev->drv == &usb_hub_driver && g_hcd_count == 2);
    usb_device_t *leaf = usb_create(&hub->hcd, 1); leaf->slot = 2;
    hub->hcd.port_devices[0] = leaf;
    assert(!usb_publish_interfaces(leaf) && leaf->ifaces[0].device->drv == &leaf_driver);
    assert(!hub_op_poll(&hub->hcd) && hub_pending && hub_submits == 1);

    /* A failing child must leave an already-closed hub admission gate. */
    hub_child_error = -EBUSY;
    driver_lifecycle_begin();
    assert(device_unbind(dev) == -EBUSY);
    driver_lifecycle_end();
    assert(hub->stopping && dev->state == DEV_STATE_PREPARING && dev->drv_priv == hub);
    assert(hub_pending && hub->hcd.port_devices[0] == leaf && !hub_child_aborts);
    unsigned controls = hub_controls;
    assert(!hub_op_poll(&hub->hcd) && hub_controls == controls && hub_submits == 1);
    assert(usb_core_enumerate_port(&hub->hcd, 1) == -ENODEV);
    usb_core_scan();
    assert(hub_controls == controls && hub->hcd.port_devices[0] == leaf);

    /* Child slot failure retains its graph; upstream failure retains the hub. */
    hub_child_error = 0; hub_abort_error = -ETIMEDOUT;
    driver_lifecycle_begin();
    assert(device_unbind(dev) == -ETIMEDOUT);
    driver_lifecycle_end();
    assert(dev->drv_priv == hub && hub->hcd.port_devices[0] == leaf);
    assert(usb_hcd_registered(&hub->hcd) && hub_child_aborts == 1 && !hub_endpoint_releases);
    hub_abort_error = 0; hub_endpoint_error = -ETIMEDOUT;
    driver_lifecycle_begin();
    assert(device_unbind(dev) == -ETIMEDOUT);
    driver_lifecycle_end();
    assert(dev->drv_priv == hub && !hub->hcd.port_devices && hub_pending);
    assert(!usb_hcd_registered(&hub->hcd) && hub_child_aborts == 2 && hub_endpoint_releases == 1);
    assert(!hub_op_poll(&hub->hcd) && hub_controls == controls);
    hub_endpoint_error = 0;
    driver_lifecycle_begin();
    assert(!device_unbind(dev));
    driver_lifecycle_end();
    assert(!dev->drv_priv && !hub_pending && hub_endpoint_releases == 2 && g_hcd_count == 1);
    assert(!usb_core_unregister_hcd(&root) && parent.references == 1);
}
