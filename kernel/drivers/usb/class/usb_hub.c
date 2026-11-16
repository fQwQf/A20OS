/*
 * A20OS — USB hub class driver.
 *
 * Hub class requests manage port power, status and resets. The root host
 * controller owns every device slot and sends transfers directly to the
 * addressed endpoint, including EP0 on devices behind a hub.
 */
#include "drivers/usb/usb.h"

#include "core/defs.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/timer.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "mm/slab.h"

#define HUB_STATUS_BYTES(nports)  (((nports) + 1U + 7U) / 8U)
#define HUB_PORT_CONNECTION       (1U << 0)
#define HUB_PORT_ENABLE           (1U << 1)
#define HUB_PORT_OVERCURRENT      (1U << 3)
#define HUB_PORT_RESET            (1U << 4)
#define HUB_PORT_LOW_SPEED        (1U << 9)
#define HUB_PORT_HIGH_SPEED       (1U << 10)
#define HUB_RESET_POLL_MS         500U
#define HUB_RESET_STEP_MS         2U
#define HUB_MAX_DEPTH             5U
#define HUB_SS_DESCRIPTOR_SIZE    12U

typedef struct usb_hub {
    usb_hcd_t hcd;
    usb_device_t *dev;
    uint16_t status[USB_HUB_MAX_PORTS];
    uint8_t status_bytes;

    usb_endpoint_t *sc_ep;
    usb_urb_t sc_urb;
    uint8_t sc_buf[HUB_STATUS_BYTES(USB_HUB_MAX_PORTS)];
    uint8_t sc_armed;
    uint8_t stopping;
    spinlock_t sc_lock;
} usb_hub_t;

static int hub_port_feature(usb_hub_t *hub, unsigned port,
                            uint8_t request, uint16_t feature)
{
    return usb_control_msg(hub->dev, USB_TYPE_CLASS | USB_RECIP_OTHER,
                           request, feature, (uint16_t)port, NULL, 0);
}

static int hub_read_port(usb_hub_t *hub, unsigned port, uint16_t *change)
{
    uint8_t status[4];
    int r = usb_control_msg(hub->dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
                            USB_REQ_GET_STATUS, 0, (uint16_t)port, status,
                            sizeof(status));
    if (r)
        return r;
    hub->status[port - 1U] = (uint16_t)status[0] | ((uint16_t)status[1] << 8);
    *change = (uint16_t)status[2] | ((uint16_t)status[3] << 8);
    return 0;
}

static void hub_ack_port_changes(usb_hub_t *hub, unsigned port, uint16_t change)
{
    static const uint8_t features[] = {
        USB_FEAT_C_PORT_CONNECTION,
        USB_FEAT_C_PORT_ENABLE,
        USB_FEAT_C_PORT_SUSPEND,
        USB_FEAT_C_PORT_OVER_CURRENT,
        USB_FEAT_C_PORT_RESET,
        USB_FEAT_C_BH_PORT_RESET,
        USB_FEAT_C_PORT_LINK_STATE,
        USB_FEAT_C_PORT_CONFIG_ERROR,
    };
    unsigned bits = hub->dev->speed >= USB_SPEED_SUPER ? sizeof(features) : 5U;
    for (unsigned bit = 0; bit < bits; bit++)
        if (change & (1U << bit))
            (void)hub_port_feature(hub, port, USB_REQ_CLEAR_FEATURE, features[bit]);
}

static void hub_refresh_ports(usb_hub_t *hub)
{
    for (unsigned port = 1; port <= hub->hcd.max_ports; port++) {
        uint16_t change;
        if (hub_read_port(hub, port, &change) == 0)
            hub_ack_port_changes(hub, port, change);
    }
}

static int hub_op_start(usb_hcd_t *hcd)
{
    (void)hcd;
    return 0;
}

static int hub_op_port_connected(usb_hcd_t *hcd, unsigned port)
{
    usb_hub_t *hub = hcd->priv;
    return port && port <= hcd->max_ports &&
           (hub->status[port - 1U] & HUB_PORT_CONNECTION);
}

static int hub_op_reset_port(usb_hcd_t *hcd, unsigned port, uint8_t *speed)
{
    usb_hub_t *hub = hcd->priv;
    if (!hub_op_port_connected(hcd, port))
        return -ENODEV;

    int r = hub_port_feature(hub, port, USB_REQ_SET_FEATURE, USB_FEAT_PORT_RESET);
    if (r)
        return r;
    for (unsigned ms = 0; ms < HUB_RESET_POLL_MS; ms += HUB_RESET_STEP_MS) {
        mdelay(HUB_RESET_STEP_MS);
        uint16_t change;
        r = hub_read_port(hub, port, &change);
        if (r)
            return r;
        uint16_t status = hub->status[port - 1U];
        if (!(status & HUB_PORT_CONNECTION))
            return -ENODEV;
        if (status & HUB_PORT_OVERCURRENT)
            return -EIO;
        if ((status & HUB_PORT_RESET) || !(status & HUB_PORT_ENABLE))
            continue;
        if (hub->dev->speed >= USB_SPEED_SUPER)
            *speed = USB_SPEED_SUPER;
        else if (status & HUB_PORT_HIGH_SPEED)
            *speed = USB_SPEED_HIGH;
        else if (status & HUB_PORT_LOW_SPEED)
            *speed = USB_SPEED_LOW;
        else
            *speed = USB_SPEED_FULL;
        hub_ack_port_changes(hub, port, change);
        return 0;
    }
    return -ETIMEDOUT;
}

static int hub_op_alloc_slot(usb_hcd_t *hcd, unsigned port, uint8_t speed,
                             uint8_t hub_slot, uint8_t address, usb_slot_t *out)
{
    usb_hcd_t *root = hcd->parent_hcd;
    return root->ops->alloc_slot(root, port, speed, hub_slot, address, out);
}

static int hub_op_control(usb_hcd_t *hcd, uint8_t slot,
                          const usb_setup_packet_t *setup, void *data)
{
    usb_hcd_t *root = hcd->parent_hcd;
    return root->ops->control(root, slot, setup, data);
}

static int hub_op_get_descriptor(usb_hcd_t *hcd, uint8_t slot, uint8_t type,
                                 uint16_t len, void *buf)
{
    usb_hcd_t *root = hcd->parent_hcd;
    return root->ops->get_descriptor(root, slot, type, len, buf);
}

static int hub_op_update_ep0_mps(usb_hcd_t *hcd, uint8_t slot, uint16_t max_packet)
{
    usb_hcd_t *root = hcd->parent_hcd;
    return root->ops->update_ep0_mps(root, slot, max_packet);
}

static int hub_op_configure_endpoint(usb_hcd_t *hcd, usb_device_t *dev,
                                     uint8_t addr, uint8_t ep_type,
                                     uint16_t max_packet, uint8_t interval)
{
    usb_hcd_t *root = hcd->parent_hcd;
    return root->ops->configure_endpoint(root, dev, addr, ep_type, max_packet,
                                         interval);
}

static int hub_op_submit_interrupt(usb_hcd_t *hcd, usb_urb_t *urb)
{
    usb_hcd_t *root = hcd->parent_hcd;
    return root->ops->submit_interrupt(root, urb);
}

static int hub_op_submit_bulk(usb_hcd_t *hcd, usb_urb_t *urb)
{
    usb_hcd_t *root = hcd->parent_hcd;
    return root->ops->submit_bulk(root, urb);
}

static int hub_op_abort_slot(usb_hcd_t *hcd, uint8_t slot)
{
    usb_hcd_t *root = hcd->parent_hcd;
    return root->ops->abort_slot(root, slot);
}

static void hub_sc_complete(usb_urb_t *urb)
{
    usb_hub_t *hub = urb->ctx;
    uint64_t flags = spin_lock_irqsave(&hub->sc_lock);
    hub->sc_armed = 0;
    spin_unlock_irqrestore(&hub->sc_lock, flags);
}

static int hub_op_poll(usb_hcd_t *hcd)
{
    usb_hub_t *hub = hcd->priv;
    uint64_t flags = spin_lock_irqsave(&hub->sc_lock);
    if (hub->stopping || hub->sc_armed) {
        spin_unlock_irqrestore(&hub->sc_lock, flags);
        return 0;
    }
    /* Claim the URB before submit so an immediate callback can disarm it. */
    hub->sc_armed = hub->sc_ep != NULL;
    spin_unlock_irqrestore(&hub->sc_lock, flags);

    hub_refresh_ports(hub);
    if (!hub->sc_ep)
        return 0;
    int r = usb_submit_urb(&hub->sc_urb);
    if (r) {
        flags = spin_lock_irqsave(&hub->sc_lock);
        hub->sc_armed = 0;
        spin_unlock_irqrestore(&hub->sc_lock, flags);
    }
    return r;
}

static int hub_op_release_endpoint(usb_hcd_t *hcd, usb_device_t *dev, uint8_t addr)
{
    usb_hcd_t *root = hcd->parent_hcd;
    return root->ops->release_endpoint ? root->ops->release_endpoint(root, dev, addr) : -EOPNOTSUPP;
}

static const usb_hcd_ops_t hub_hcd_ops = {
    .start = hub_op_start,
    .poll = hub_op_poll,
    .port_connected = hub_op_port_connected,
    .reset_port = hub_op_reset_port,
    .alloc_slot = hub_op_alloc_slot,
    .update_ep0_mps = hub_op_update_ep0_mps,
    .control = hub_op_control,
    .get_descriptor = hub_op_get_descriptor,
    .configure_endpoint = hub_op_configure_endpoint,
    .release_endpoint = hub_op_release_endpoint,
    .submit_interrupt = hub_op_submit_interrupt,
    .submit_bulk = hub_op_submit_bulk,
    .abort_slot = hub_op_abort_slot,
};

/* Close the hub before consumer-first unbind starts. If a child cannot stop,
 * the retained hub must not enumerate replacements or rearm its status URB.
 * This is admission closure only; endpoint release still proves DMA stop. */
static int usb_hub_prepare(void *context)
{
    usb_hub_t *hub = context;
    uint64_t flags = spin_lock_irqsave(&hub->sc_lock);
    hub->stopping = 1;
    spin_unlock_irqrestore(&hub->sc_lock, flags);
    return 0;
}

static int usb_hub_probe(device_t *dev)
{
    usb_interface_t *iface = dev->plat_data;
    if (!iface || iface->interface_class != USB_CLASS_HUB)
        return -EINVAL;
    usb_device_t *hub_dev = iface->dev;
    if (!hub_dev || !hub_dev->hcd)
        return -EINVAL;
    usb_hcd_t *bus = hub_dev->hcd;
    usb_hcd_t *root = bus->parent_hcd ? bus->parent_hcd : bus;
    if (!root->ops->configure_hub || bus->hub_depth >= HUB_MAX_DEPTH)
        return -EOPNOTSUPP;

    int superspeed = hub_dev->speed >= USB_SPEED_SUPER;
    uint8_t type = superspeed ? USB_DT_SS_HUB : USB_DT_HUB;
    uint8_t descriptor[HUB_SS_DESCRIPTOR_SIZE] = {0};
    uint16_t len = superspeed ? sizeof(descriptor) : sizeof(usb_hub_descriptor_t);
    int r = usb_control_msg(hub_dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                            USB_REQ_GET_DESCRIPTOR, (uint16_t)type << 8, 0,
                            descriptor, len);
    if (r)
        return r;
    const usb_hub_descriptor_t *hd = (const usb_hub_descriptor_t *)descriptor;
    if (hd->type != type || hd->length < len || !hd->ports ||
        hd->ports > USB_HUB_MAX_PORTS)
        return -EINVAL;
    uint8_t think_time = (hd->characteristics >> USB_HUB_CHAR_TTT_SHIFT) & 3U;
    if (!root->ops->release_endpoint) return -EOPNOTSUPP;
    r = device_probe_use_remove(dev);
    if (r) return r;
    r = root->ops->configure_hub(root, hub_dev, hd->ports, think_time);
    if (r)
        return r;
    if (superspeed) {
        r = usb_control_msg(hub_dev, USB_TYPE_CLASS | USB_RECIP_DEVICE,
                            USB_REQ_SET_HUB_DEPTH, bus->hub_depth, 0, NULL, 0);
        if (r)
            return r;
    }

    usb_hub_t *hub = kcalloc(1, sizeof(*hub));
    if (!hub)
        return -ENOMEM;
    dev->drv_priv = hub;
    hub->dev = hub_dev;
    hub->status_bytes = HUB_STATUS_BYTES(hd->ports);
    spin_init(&hub->sc_lock);
    hub->hcd = (usb_hcd_t){
        .ops = &hub_hcd_ops,
        .hcd_dev = dev,
        .parent_hcd = root,
        .hub_slot = hub_dev->slot,
        .max_ports = hd->ports,
        .priv = hub,
        .hub_depth = bus->hub_depth + 1U,
    };
    r = device_add_remove_prepare(dev, usb_hub_prepare, hub);
    if (r) return r;
    kinfo("[USB-HUB] hub %04x:%04x: downstream ports=%u status_bytes=%u ss=%u\n",
          hub_dev->vendor, hub_dev->product, hd->ports, hub->status_bytes, superspeed);

    for (unsigned port = 1; port <= hd->ports; port++) {
        r = hub_port_feature(hub, port, USB_REQ_SET_FEATURE, USB_FEAT_PORT_POWER);
        if (r)
            goto fail;
    }
    unsigned power_good_ms = 2U * hd->pwr_on2pwr_good;
    mdelay(power_good_ms > 100U ? power_good_ms : 100U);

    for (uint8_t i = 0; i < iface->ep_count; i++) {
        usb_endpoint_t *ep = &iface->eps[i];
        if ((ep->attrs & 3U) != USB_XFER_INTERRUPT || !(ep->addr & USB_DIR_IN) ||
            ep->max_packet < hub->status_bytes)
            continue;
        r = usb_control_msg(hub_dev, USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
                            USB_REQ_SET_INTERFACE, iface->alt_setting,
                            iface->interface_number, NULL, 0);
        if (r) goto fail;
        r = root->ops->configure_endpoint(root, hub_dev, ep->addr,
                                          USB_XFER_INTERRUPT, ep->max_packet,
                                          ep->interval);
        if (r)
            goto fail;
        hub->sc_ep = ep;
        hub->sc_urb = (usb_urb_t){
            .dev = hub_dev, .ep = ep,
            .transfer_type = USB_XFER_INTERRUPT, .direction = USB_DIR_IN,
            .buf = hub->sc_buf, .len = hub->status_bytes,
            .complete = hub_sc_complete, .ctx = hub,
        };
        break;
    }
    r = usb_core_register_hcd(&hub->hcd);
    if (r)
        goto fail;
    dev->drv_priv = hub;
    kinfo("[USB-HUB] downstream bus live: %u ports behind %04x:%04x\n",
          hd->ports, hub_dev->vendor, hub_dev->product);
    return 0;

fail:
    return r; /* Core owns partial downstream registration and endpoint state. */
}

static int usb_hub_remove(device_t *dev)
{
    usb_hub_t *hub = dev->drv_priv;
    if (hub) {
        usb_hub_prepare(hub);
        int ret = usb_core_unregister_hcd(&hub->hcd);
        if (ret) return ret;
        ret = usb_interface_release_endpoints(dev->plat_data);
        if (ret) return ret;
        dev->drv_priv = NULL;
        kfree(hub);
    }
    return 0;
}

static const device_id_t usb_hub_ids[] = {
    { .vendor = USB_CLASS_HUB, .device = DEVICE_ANY,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t usb_hub_driver = {
    .name = "usb-hub",
    .id_table = usb_hub_ids,
    .probe = usb_hub_probe,
    .remove = usb_hub_remove,
    .class_type = DEV_CLASS_NONE,
};

DRIVER_REGISTER(usb_hub_driver);
