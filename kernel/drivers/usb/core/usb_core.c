/*
 * A20OS — USB core.
 *
 * Device model (usb_device/usb_interface/usb_endpoint), the "usb" bus that
 * binds class drivers to interfaces by class/subclass/protocol, and the
 * enumeration sequence that turns a freshly reset port into a bound device.
 *
 * The core is host-controller agnostic: it talks only to usb_hcd_ops.
 */
#include "drivers/usb/usb.h"

#include "core/klog.h"
#include "core/string.h"
#include "core/defs.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "mm/slab.h"
#include "core/errno.h"

#define USB_CTRL_BUF_SZ 512

/* ------------------------------------------------------------------ */
/* usb bus                                                            */
/* ------------------------------------------------------------------ */

/* The interface class is stored in hardware_id.vendor and
 * (subclass << 8 | protocol) in hardware_id.device. */
static int usb_match(device_t *dev, const driver_t *drv)
{
    if (!dev->hardware_id || !drv->id_table)
        return 0;
    for (const device_id_t *id = drv->id_table; id->vendor; id++) {
        if (id->vendor != dev->hardware_id->vendor)
            continue;
        if (id->device != DEVICE_ANY &&
            id->device != dev->hardware_id->device)
            continue;
        if (id->subvendor != VENDOR_ANY &&
            id->subvendor != dev->hardware_id->subvendor)
            continue;
        if (id->subdevice != DEVICE_ANY &&
            id->subdevice != dev->hardware_id->subdevice)
            continue;
        dev->matched_id = id;
        return 1;
    }
    return 0;
}

static bus_type_t g_usb_bus = {
    .name = "usb",
    .match = usb_match,
};

/* ------------------------------------------------------------------ */
/* Per-interface device publication                                    */
/* ------------------------------------------------------------------ */

static int usb_publish_interfaces(usb_device_t *udev)
{
    for (uint8_t i = 0; i < udev->iface_count; i++) {
        usb_interface_t *iface = &udev->ifaces[i];

        device_t *d = kcalloc(1, sizeof(*d));
        device_id_t *hwid = kcalloc(1, sizeof(*hwid));
        if (!d || !hwid) {
            if (d) kfree(d);
            if (hwid) kfree(hwid);
            return -ENOMEM;
        }
        hwid->vendor = iface->interface_class;
        hwid->device = (uint32_t)((iface->interface_subclass << 8) |
                                  iface->interface_protocol);
        hwid->subvendor = VENDOR_ANY;
        hwid->subdevice = DEVICE_ANY;
        iface->device = d;

        d->name = "usb-iface";
        d->bus = &g_usb_bus;
        d->hardware_id = hwid;
        d->plat_data = iface;
        d->drv_priv = NULL;

        if (device_register(d) < 0) {
            kfree(hwid);
            kfree(d);
            iface->device = NULL;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */

/* Parse a configuration descriptor buffer into interfaces/endpoints. */
static int usb_parse_config(usb_device_t *udev, const uint8_t *cfg,
                            size_t length)
{
    if (length < 9 || cfg[1] != USB_DT_CONFIG)
        return -EINVAL;
    usb_config_descriptor_t *cd = (usb_config_descriptor_t *)cfg;
    udev->config_value = cd->config_value;
    if (cd->num_interfaces == 0 || cd->num_interfaces > 8)
        return -EINVAL;

    usb_interface_t *ifaces = kcalloc(cd->num_interfaces, sizeof(*ifaces));
    if (!ifaces)
        return -ENOMEM;
    uint8_t iface_count = 0;
    int cur = -1;

    for (size_t off = 0; off + 2 <= length;) {
        uint8_t size = cfg[off];
        uint8_t type = cfg[off + 1];
        if (size < 2 || off + size > length)
            break;
        if (type == USB_DT_INTERFACE && size >= 9) {
            usb_interface_descriptor_t *id = (usb_interface_descriptor_t *)(cfg + off);
            if (iface_count < cd->num_interfaces) {
                cur = iface_count++;
                usb_interface_t *iface = &ifaces[cur];
                memset(iface, 0, sizeof(*iface));
                iface->dev = udev;
                iface->interface_number = id->interface_number;
                iface->alt_setting = id->alt_setting;
                iface->interface_class = id->interface_class;
                iface->interface_subclass = id->interface_subclass;
                iface->interface_protocol = id->interface_protocol;
                iface->ep_count = id->num_endpoints;
                if (iface->ep_count > 8)
                    iface->ep_count = 8;
                if (iface->ep_count) {
                    iface->eps = kcalloc(iface->ep_count, sizeof(*iface->eps));
                    if (!iface->eps) {
                        kfree(ifaces);
                        return -ENOMEM;
                    }
                }
            }
        } else if (type == USB_DT_ENDPOINT && size >= 7 && cur >= 0) {
            usb_endpoint_descriptor_t *ed = (usb_endpoint_descriptor_t *)(cfg + off);
            usb_interface_t *iface = &ifaces[cur];
            for (uint8_t e = 0; e < iface->ep_count; e++) {
                if (iface->eps[e].addr != 0)
                    continue;
                iface->eps[e].addr = ed->bEndpointAddress;
                iface->eps[e].attrs = ed->bmAttributes;
                iface->eps[e].max_packet = ed->wMaxPacketSize & 0x7ffU;
                iface->eps[e].interval = ed->bInterval;
                break;
            }
        }
        off += size;
    }

    udev->ifaces = ifaces;
    udev->iface_count = iface_count;
    return 0;
}

int usb_core_enumerate_port(usb_hcd_t *hcd, unsigned port)
{
    const usb_hcd_ops_t *ops = hcd->ops;
    uint8_t speed;
    int r = ops->reset_port(hcd, port, &speed);
    if (r) {
        kdebug("[USB] port %u reset failed: %d\n", port, r);
        return r;
    }
    mdelay(20);                 /* reset recovery */

    uint8_t slot;
    r = ops->init_slot(hcd, port, speed, &slot);
    if (r) {
        kerr("[USB] port %u init_slot failed: %d\n", port, r);
        return r;
    }
    mdelay(5);

    uint8_t buf[USB_CTRL_BUF_SZ];
    r = ops->get_descriptor(hcd, slot, USB_DT_DEVICE, 8, buf);
    if (r) {
        kdebug("[USB] port %u device descriptor(8) failed: %d\n", port, r);
        return r;
    }
    usb_device_descriptor_t *dd = (usb_device_descriptor_t *)buf;
    uint16_t max_packet = (speed >= USB_SPEED_SUPER) ? (uint16_t)(1U << dd->max_packet0)
                                                     : dd->max_packet0;
    if (max_packet) {
        r = ops->update_ep0_mps(hcd, slot, max_packet);
        if (r)
            kdebug("[USB] port %u EP0 MPS update failed: %d\n", port, r);
    }

    r = ops->get_descriptor(hcd, slot, USB_DT_DEVICE, sizeof(usb_device_descriptor_t), buf);
    if (r)
        return r;
    dd = (usb_device_descriptor_t *)buf;

    usb_device_t *udev = kcalloc(1, sizeof(*udev));
    if (!udev)
        return -ENOMEM;
    udev->hcd = hcd;
    udev->speed = speed;
    udev->slot = slot;
    udev->vendor = dd->vendor;
    udev->product = dd->product;
    udev->address = slot;       /* assigned address == slot id for xHCI */

    r = ops->get_descriptor(hcd, slot, USB_DT_CONFIG, 9, buf);
    if (r) {
        kfree(udev);
        return r;
    }
    uint16_t total = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    if (total < 9 || total > sizeof(buf)) {
        kfree(udev);
        return -EINVAL;
    }
    r = ops->get_descriptor(hcd, slot, USB_DT_CONFIG, total, buf);
    if (r) {
        kfree(udev);
        return r;
    }
    r = usb_parse_config(udev, buf, total);
    if (r) {
        kfree(udev);
        return r;
    }

    kinfo("[USB] device %04x:%04x port=%u speed=%u ifaces=%u\n",
          udev->vendor, udev->product, port, speed, udev->iface_count);

    /* SET_CONFIGURATION (device request). */
    usb_setup_packet_t setup = {
        .request_type = USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = USB_REQ_SET_CONFIGURATION,
        .value = udev->config_value,
        .index = 0,
        .length = 0,
    };
    r = ops->control(hcd, slot, &setup, NULL);
    if (r)
        kdebug("[USB] SET_CONFIGURATION failed: %d\n", r);

    /* Publish interfaces; class drivers bind and finish setup in probe. */
    r = usb_publish_interfaces(udev);
    if (r < 0) {
        for (uint8_t i = 0; i < udev->iface_count; i++)
            if (udev->ifaces[i].eps)
                kfree(udev->ifaces[i].eps);
        kfree(udev->ifaces);
        kfree(udev);
        return r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Generic helpers                                                     */
/* ------------------------------------------------------------------ */

int usb_control_msg(usb_device_t *dev, uint8_t request_type, uint8_t request,
                    uint16_t value, uint16_t index, void *data, uint16_t len)
{
    if (!dev || !dev->hcd || !dev->hcd->ops->control)
        return -EINVAL;
    usb_setup_packet_t setup = {
        .request_type = request_type,
        .request = request,
        .value = value,
        .index = index,
        .length = len,
    };
    return dev->hcd->ops->control(dev->hcd, dev->slot, &setup, data);
}

int usb_submit_urb(usb_urb_t *urb)
{
    if (!urb || !urb->dev || !urb->dev->hcd || !urb->dev->hcd->ops)
        return -EINVAL;
    if (urb->transfer_type == USB_XFER_INTERRUPT) {
        if (!urb->dev->hcd->ops->submit_interrupt)
            return -EINVAL;
        return urb->dev->hcd->ops->submit_interrupt(urb->dev->hcd, urb);
    }
    if (urb->transfer_type == USB_XFER_BULK) {
        if (!urb->dev->hcd->ops->submit_bulk)
            return -EINVAL;
        return urb->dev->hcd->ops->submit_bulk(urb->dev->hcd, urb);
    }
    return -EOPNOTSUPP;
}

usb_interface_t *usb_find_interface(usb_device_t *dev, uint8_t num)
{
    for (uint8_t i = 0; i < dev->iface_count; i++)
        if (dev->ifaces[i].interface_number == num)
            return &dev->ifaces[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

#define USB_MAX_HCDS 4
static usb_hcd_t *g_hcds[USB_MAX_HCDS];
static int g_hcd_count;

int usb_core_register_hcd(usb_hcd_t *hcd)
{
    if (!hcd || !hcd->ops || !hcd->ops->start)
        return -EINVAL;
    hcd->port_state = kcalloc(hcd->max_ports ? hcd->max_ports : 1, 1);
    if (!hcd->port_state)
        return -ENOMEM;
    int r = hcd->ops->start(hcd);
    if (r)
        return r;
    if (g_hcd_count < USB_MAX_HCDS)
        g_hcds[g_hcd_count++] = hcd;
    return 0;
}

void usb_core_unregister_hcd(usb_hcd_t *hcd)
{
    if (!hcd)
        return;
    for (int i = 0; i < g_hcd_count; i++) {
        if (g_hcds[i] == hcd) {
            g_hcds[i] = g_hcds[--g_hcd_count];
            break;
        }
    }
    if (hcd->ops && hcd->ops->abort_slot)
        hcd->ops->abort_slot(hcd, 0);
    kfree(hcd->port_state);
    hcd->port_state = NULL;
}

/*
 * Scan all registered host controllers for connected ports and enumerate
 * them.  Runs after driver_probe_all() so that interface device_register()
 * calls do not re-enter the driver-core registration mutex held during
 * probe.
 */
void usb_core_scan(void)
{
    for (int i = 0; i < g_hcd_count; i++) {
        usb_hcd_t *hcd = g_hcds[i];
        for (unsigned port = 1; port <= hcd->max_ports; port++) {
            if (hcd->port_state && hcd->port_state[port - 1])
                continue;
            /* Port connected?  Ask the HCD (MMIO read). */
            if (!hcd->ops->port_connected || !hcd->ops->port_connected(hcd, port))
                continue;
            hcd->port_state[port - 1] = 1;
            int r = usb_core_enumerate_port(hcd, port);
            if (r)
                kerr("[USB] port %u enumeration failed: %d\n", port, r);
        }
    }
}

void usb_core_init(void)
{
    bus_register(&g_usb_bus);
}
