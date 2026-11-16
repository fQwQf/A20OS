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
#include "core/timer.h"
#include "mm/slab.h"
#include "core/errno.h"

#define USB_PORT_REMOVE_PENDING 0x80u
#define USB_PORT_SLOT_STOPPED   0x40u
#define USB_PORT_ADDRESS_OWNED  0x20u
#define USB_PORT_ENUM_ATTEMPTS  0x1fu

#define USB_ENUM_MAX_ATTEMPTS 3U

/* Serialized by the native lifecycle coordinator. Frames reject same-HCD
 * callback reentry; nested operations on a different (e.g. hub) HCD are valid. */
struct usb_operation { usb_hcd_t *hcd; struct usb_operation *previous; };
static struct usb_operation *usb_operations;
static bool usb_scanning;
static bool usb_controller_has_pending(usb_hcd_t *hcd);
static bool usb_hcd_stopping(usb_hcd_t *hcd);
static bool usb_operation_active(usb_hcd_t *hcd) {
    for (struct usb_operation *at = usb_operations; at; at = at->previous)
        if (at->hcd == hcd) return true;
    return false;
}

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

static void usb_interface_device_release(device_t *dev)
{
    kfree((void *)dev->hardware_id);
    kfree(dev);
}

static int usb_unpublish_interface(usb_interface_t *iface)
{
    device_t *dev = iface->device;
    if (!dev)
        return 0;
    int ret = device_is_registered(dev) ? device_hotplug(dev, BUS_EVENT_REMOVE) : 0;
    if (ret) return ret;
    /* Class removal has drained operations. An open class handle can retain
     * the device shell, but must never retain freed interface storage. */
    dev->plat_data = NULL;
    iface->device = NULL;
    device_put(dev); /* Drop the publisher's creator reference. */
    return 0;
}

static int usb_publish_interfaces(usb_device_t *udev)
{
    int ret = 0;
    driver_discovery_begin();
    for (uint8_t i = 0; i < udev->iface_count; i++) {
        usb_interface_t *iface = &udev->ifaces[i];

        device_t *d = kcalloc(1, sizeof(*d));
        device_id_t *hwid = kcalloc(1, sizeof(*hwid));
        if (!d || !hwid) {
            if (d) kfree(d);
            if (hwid) kfree(hwid);
            ret = -ENOMEM;
            goto rollback;
        }
        hwid->vendor = iface->interface_class;
        hwid->device = (uint32_t)((iface->interface_subclass << 8) |
                                  iface->interface_protocol);
        hwid->subvendor = VENDOR_ANY;
        hwid->subdevice = DEVICE_ANY;
        d->name = "usb-iface";
        d->bus = &g_usb_bus;
        d->parent = udev->hcd->hcd_dev;
        d->references = 1;
        d->release = usb_interface_device_release;
        d->hardware_id = hwid;
        d->plat_data = iface;
        d->drv_priv = NULL;

        ret = device_hotplug(d, BUS_EVENT_ADD);
        if (ret < 0) {
            d->plat_data = NULL;
            device_put(d);
            goto rollback;
        }
        iface->device = d;
    }
    driver_discovery_end();
    /* Deferred retry only visits devices that already returned EPROBE_DEFER;
     * these newly published interfaces still need their first probe. */
    for (uint8_t i = 0; i < udev->iface_count; i++)
        bus_probe_device(udev->ifaces[i].device);
    driver_retry_deferred();
    return 0;

rollback:
    /* The port owns even a partially published device. Stop its slot before
     * removing any bound interfaces; the caller uses disconnect_port(), whose
     * fallible cleanup retains this graph when the controller cannot stop. */
    driver_discovery_end();
    return ret;
}

static int usb_disconnect_port(usb_hcd_t *hcd, unsigned port)
{
    if (!hcd || port == 0 || port > hcd->max_ports || !hcd->port_devices)
        return -EINVAL;
    usb_device_t *udev = hcd->port_devices[port - 1];
    if (!udev) {
        hcd->port_state[port - 1] = 0;
        return 0;
    }
    hcd->port_state[port - 1] |= USB_PORT_REMOVE_PENDING;
    for (uint8_t i = 0; i < udev->iface_count; i++)
        if (udev->ifaces[i].device)
            class_device_mark_offline(udev->ifaces[i].device);

    /* Tear the controller side down FIRST.  The transfer rings this slot owns
     * outlive the class devices underneath it, and the controller's interrupt
     * handler may still drain them: xhci_collect_interrupt() reads
     * ep->pending and then urb->complete on the way to deciding whether there is
     * anything to run.  Unlinking the endpoints here makes ep->pending
     * unreachable, so a drain racing this teardown finds no endpoint rather
     * than one pointing at a class driver's freed URB.
     *
     * The stronger slot proof precedes interface removal. Migrated HID/hub
     * removes reuse it without issuing commands to this stopped slot; their
     * independent driver withdrawal instead releases their own endpoints.
     * Callback and class storage remain owned until both proofs/drains finish. */
    uint8_t address = udev->address;
    bool address_owned = hcd->port_state[port - 1] & USB_PORT_ADDRESS_OWNED;
    uint8_t slot = udev->slot;
    if (!(hcd->port_state[port - 1] & USB_PORT_SLOT_STOPPED)) {
        if (!hcd->ops || !hcd->ops->abort_slot) return -EOPNOTSUPP;
        int ret = hcd->ops->abort_slot(hcd, udev->slot);
        if (ret) return ret < 0 ? ret : -EIO;
        hcd->port_state[port - 1] |= USB_PORT_SLOT_STOPPED;
    }

    /* Remove class devices before their interface and endpoint storage. */
    for (uint8_t i = 0; i < udev->iface_count; i++) {
        int ret = usb_unpublish_interface(&udev->ifaces[i]);
        if (ret) return ret; /* Keep every endpoint and the port/address for retry. */
    }
    for (uint8_t i = 0; i < udev->iface_count; i++)
        kfree(udev->ifaces[i].eps);
    kfree(udev->ifaces);
    kfree(udev);
    hcd->port_devices[port - 1] = NULL;
    hcd->port_state[port - 1] &= USB_PORT_ENUM_ATTEMPTS;
    /* Only release our retained software reservation. An implicit controller
     * address can equal a number reserved by another HCD; observing that
     * number does not transfer ownership of its pool entry. */
    if (address_owned) usb_core_free_address(address);
    kinfo("[USB] port %u disconnected: address=%u reclaimed, slot=%u released\n",
          port, address, slot);
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
            const usb_interface_descriptor_t *id =
                (const usb_interface_descriptor_t *)(cfg + off);
            /* SET_CONFIGURATION activates alternate setting zero. Endpoints
             * in another setting must not be attached to the active one. */
            cur = -1;
            if (id->alt_setting == 0 && iface_count < cd->num_interfaces) {
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
                        for (uint8_t i = 0; i < cur; i++)
                            kfree(ifaces[i].eps);
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

static int usb_enumerate_port_locked(usb_hcd_t *hcd, unsigned port)
{
    if (!hcd || !hcd->port_devices || !hcd->port_state ||
        port == 0 || port > hcd->max_ports || !hcd->ops ||
        !hcd->ops->reset_port || !hcd->ops->alloc_slot ||
        !hcd->ops->get_descriptor || !hcd->ops->control)
        return -EINVAL;
    if (usb_hcd_stopping(hcd))
        return -ENODEV;
    if (hcd->port_devices[port - 1] ||
        (hcd->port_state[port - 1] & USB_PORT_REMOVE_PENDING))
        return -EBUSY;
    if (usb_controller_has_pending(hcd)) return -EAGAIN;
    /* Allocate the cleanup owner before the HCD can publish a slot. No
     * allocation may be required merely to retain a failed hardware setup. */
    usb_device_t *udev = kcalloc(1, sizeof(*udev));
    if (!udev) return -ENOMEM;
    udev->hcd = hcd;
    const usb_hcd_ops_t *ops = hcd->ops;
    uint8_t speed;
    int r = ops->reset_port(hcd, port, &speed);
    if (r) {
        kdebug("[USB] port %u reset failed: %d\n", port, r);
        kfree(udev);
        return r < 0 ? r : -EIO;
    }
    mdelay(20);                 /* reset recovery */

    /* Reset callbacks can change lifecycle or begin another retirement. */
    if (usb_hcd_stopping(hcd)) { kfree(udev); return -ENODEV; }
    if (usb_controller_has_pending(hcd)) { kfree(udev); return -EAGAIN; }
    /* Provide a reservation for HCDs that assign addresses in software. */
    int address = usb_core_alloc_address();
    if (address < 0) {
        kerr("[USB] port %u address pool exhausted\n", port);
        kfree(udev);
        return address;
    }

    usb_slot_t slot = {0};
    r = ops->alloc_slot(hcd, port, speed, hcd->hub_slot,
                        (uint8_t)address, &slot);
    if (!slot.hcd) {
        /* No token was transferred: the HCD owns any uncertain private
         * controller state. A successful allocation must return a token. */
        usb_core_free_address((uint8_t)address);
        kfree(udev);
        return r ? (r < 0 ? r : -EIO) : -EIO;
    }
    /* An HCD that addresses devices implicitly never echoes the number, so
     * the reservation goes straight back.  One that spoke SET_ADDRESS owns
     * it now. */
    if (slot.address != (uint8_t)address)
        usb_core_free_address((uint8_t)address);
    else
        hcd->port_state[port - 1] |= USB_PORT_ADDRESS_OWNED;
    slot.port = (uint8_t)port;
    udev->speed = speed;
    udev->slot = slot.hcd;
    udev->address = slot.address;
    hcd->port_devices[port - 1] = udev;
    if (r) goto fail; /* Nonzero result can still transfer a cleanup token. */
    /* Reported here rather than after descriptors are read: address reuse is a
     * property of the pool and the controller's slot, and a later failure must
     * still leave the assignment visible in the log. */
    kinfo("[USB] port %u slot=%u address=%u\n", port, slot.hcd, slot.address);
    mdelay(5);

    uint8_t buf[sizeof(usb_device_descriptor_t)];
    r = ops->get_descriptor(hcd, slot.hcd, USB_DT_DEVICE, 8, buf);
    if (r) {
        kdebug("[USB] port %u device descriptor(8) failed: %d\n", port, r);
        goto fail;
    }
    usb_device_descriptor_t *dd = (usb_device_descriptor_t *)buf;
    uint16_t max_packet = (speed >= USB_SPEED_SUPER) ? (uint16_t)(1U << dd->max_packet0)
                                                     : dd->max_packet0;
    if (max_packet && ops->update_ep0_mps) {
        r = ops->update_ep0_mps(hcd, slot.hcd, max_packet);
        if (r) {
            kdebug("[USB] port %u EP0 MPS update failed: %d\n", port, r);
            goto fail;
        }
    }

    r = ops->get_descriptor(hcd, slot.hcd, USB_DT_DEVICE,
                            sizeof(usb_device_descriptor_t), buf);
    if (r)
        goto fail;
    dd = (usb_device_descriptor_t *)buf;
    uint16_t vendor = dd->vendor;
    uint16_t product = dd->product;

    r = ops->get_descriptor(hcd, slot.hcd, USB_DT_CONFIG,
                            sizeof(usb_config_descriptor_t), buf);
    if (r)
        goto fail;
    uint16_t total = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    if (buf[1] != USB_DT_CONFIG || buf[0] < sizeof(usb_config_descriptor_t) ||
        total < buf[0] || total > USB_CONTROL_MAX_LENGTH) {
        kwarn("[USB] %04x:%04x port %u invalid config header: type=%u length=%u total=%u (limit=%u)\n",
              vendor, product, port, buf[1], buf[0], total, USB_CONTROL_MAX_LENGTH);
        r = total > USB_CONTROL_MAX_LENGTH ? -EMSGSIZE : -EINVAL;
        goto fail;
    }
    uint8_t *config = kmalloc(total);
    if (!config) {
        r = -ENOMEM;
        goto fail;
    }
    r = ops->get_descriptor(hcd, slot.hcd, USB_DT_CONFIG, total, config);
    if (r) {
        kfree(config);
        goto fail;
    }

    udev->vendor = vendor;
    udev->product = product;
    r = usb_parse_config(udev, config, total);
    kfree(config);
    if (r)
        goto fail;

    /* SET_CONFIGURATION (device request). */
    usb_setup_packet_t setup = {
        .request_type = USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = USB_REQ_SET_CONFIGURATION,
        .value = udev->config_value,
        .index = 0,
        .length = 0,
    };
    r = ops->control(hcd, slot.hcd, &setup, NULL);
    if (r)
        goto fail;

    kinfo("[USB] device %04x:%04x port=%u speed=%u ifaces=%u config_bytes=%u\n",
          vendor, product, port, speed, udev->iface_count, total);

    /* Publish interfaces; class drivers bind and finish setup in probe. */
    r = usb_publish_interfaces(udev);
    if (r < 0)
        goto fail;
    hcd->port_state[port - 1] &= USB_PORT_ADDRESS_OWNED;
    return 0;

fail: {
    int original = r < 0 ? r : -EIO;
    int cleanup = usb_disconnect_port(hcd, port);
    if (cleanup) {
        kwarn("[USB] port %u setup failed: %d; cleanup failed: %d; context retained\n",
              port, original, cleanup);
        return cleanup;
    }
    return original;
}
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
    if (urb) urb->actual_length = 0;
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

#define USB_MAX_HCDS 32
static usb_hcd_t *g_hcds[USB_MAX_HCDS];
static int g_hcd_count;
static unsigned usb_hcd_starting;

/* ------------------------------------------------------------------ */
/* Bus address pool                                                    */
/* ------------------------------------------------------------------ */

/* Bit 0 is the USB default address and is never handed out. */
static uint8_t g_usb_addresses[16];
static spinlock_t g_usb_address_lock = SPINLOCK_INIT;

int usb_core_alloc_address(void)
{
    int address = -1;
    uint64_t flags = spin_lock_irqsave(&g_usb_address_lock);
    for (int i = 1; i < 128; i++) {
        if (!(g_usb_addresses[i >> 3] & (1U << (i & 7)))) {
            g_usb_addresses[i >> 3] |= (uint8_t)(1U << (i & 7));
            address = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_usb_address_lock, flags);
    return (address < 0) ? -ENOMEM : address;
}

void usb_core_free_address(uint8_t address)
{
    if (!address || address > 127)
        return;
    uint64_t flags = spin_lock_irqsave(&g_usb_address_lock);
    g_usb_addresses[address >> 3] &= (uint8_t)~(1U << (address & 7));
    spin_unlock_irqrestore(&g_usb_address_lock, flags);
}

static int usb_register_hcd_locked(usb_hcd_t *hcd)
{
    if (!hcd || !hcd->ops || !hcd->ops->start)
        return -EINVAL;
    if (g_hcd_count == USB_MAX_HCDS)
        return -ENOSPC;
    hcd->port_state = kcalloc(hcd->max_ports ? hcd->max_ports : 1, 1);
    if (!hcd->port_state)
        return -ENOMEM;
    hcd->port_devices = kcalloc(hcd->max_ports ? hcd->max_ports : 1,
                                sizeof(*hcd->port_devices));
    if (!hcd->port_devices) {
        kfree(hcd->port_state);
        hcd->port_state = NULL;
        return -ENOMEM;
    }
    int r = hcd->ops->start(hcd);
    if (r) {
        kfree(hcd->port_devices);
        kfree(hcd->port_state);
        hcd->port_devices = NULL;
        hcd->port_state = NULL;
        return r;
    }
    g_hcds[g_hcd_count++] = hcd;
    return 0;
}

static int usb_unregister_hcd_locked(usb_hcd_t *hcd)
{
    if (!hcd) return -EINVAL;
    if (!hcd->port_devices) return 0;
    for (unsigned port = 1; port <= hcd->max_ports; port++) {
        int ret = usb_disconnect_port(hcd, port);
        if (ret) return ret;
    }
    /* A downstream bus owns its parent transport even if no published port
     * currently points to it. Never leave a registered dangling parent. */
    for (int i = 0; i < g_hcd_count; i++)
        if (g_hcds[i]->parent_hcd == hcd) return -EBUSY;
    for (int i = 0; i < g_hcd_count; i++) {
        if (g_hcds[i] == hcd) {
            g_hcds[i] = g_hcds[--g_hcd_count];
            break;
        }
    }
    kfree(hcd->port_devices);
    hcd->port_devices = NULL;
    kfree(hcd->port_state);
    hcd->port_state = NULL;
    return 0;
}

/*
 * Scan all registered host controllers for connected ports and enumerate
 * them under the native lifecycle coordinator. Nested scans are skipped;
 * interface publication may synchronously probe and register downstream buses.
 */
static void usb_scan_locked(void)
{
    /* Callbacks may remove another HCD and compact the registry, or add a
     * hub bus. Re-select from current membership; never keep stale slots.
     * At most 32 distinct addresses per pass, no allocation or borrowed list. */
    uintptr_t visited[USB_MAX_HCDS];
    unsigned visited_count = 0;
    while (visited_count < USB_MAX_HCDS) {
        usb_hcd_t *hcd = NULL;
        for (int i = 0; i < g_hcd_count && !hcd; i++) {
            bool seen = false;
            for (unsigned j = 0; j < visited_count; j++)
                if (visited[j] == (uintptr_t)g_hcds[i]) { seen = true; break; }
            if (!seen) hcd = g_hcds[i];
        }
        if (!hcd) break;
        visited[visited_count++] = (uintptr_t)hcd;
        if (usb_hcd_stopping(hcd))
            continue;
        struct usb_operation operation = {hcd, usb_operations};
        usb_operations = &operation;
        if (hcd->ops->poll)
            hcd->ops->poll(hcd);
        if (usb_hcd_stopping(hcd)) {
            usb_operations = operation.previous;
            continue;
        }
        for (unsigned port = 1; port <= hcd->max_ports; port++) {
            if ((hcd->port_state[port - 1] & USB_PORT_REMOVE_PENDING) && usb_disconnect_port(hcd, port))
                continue;
            /* Port connected?  Ask the HCD (MMIO read). */
            int connected = hcd->ops->port_connected &&
                            hcd->ops->port_connected(hcd, port);
            if (!connected) {
                (void)usb_disconnect_port(hcd, port);
                continue;
            }
            uint8_t *state = &hcd->port_state[port - 1];
            unsigned failures = *state & USB_PORT_ENUM_ATTEMPTS;
            if (hcd->port_devices[port - 1] || failures >= USB_ENUM_MAX_ATTEMPTS)
                continue;
            /* Disabled hardware slot IDs may be reused by Enable Slot. Keep
             * allocation closed across this physical controller until every
             * retiring port has drained callbacks/classes and released its
             * protocol owner. This deferral is not an enumeration attempt. */
            if (usb_controller_has_pending(hcd)) continue;
            int r = usb_enumerate_port_locked(hcd, port);
            if (r == -EAGAIN) continue; /* Reentrant retirement is deferral. */
            if (r) {
                ++failures;
                *state = (*state & ~USB_PORT_ENUM_ATTEMPTS) | failures;
                kwarn("[USB] port %u enumeration failed: %d (attempt %u/%u)%s\n",
                      port, r, failures, USB_ENUM_MAX_ATTEMPTS,
                      failures == USB_ENUM_MAX_ATTEMPTS ? "; waiting for disconnect" : "");
            }
        }
        usb_operations = operation.previous;
    }
}

static bool usb_hcd_registered(usb_hcd_t *hcd) {
    for (int i = 0; i < g_hcd_count; i++) if (g_hcds[i] == hcd) return true;
    return false;
}

/* Active descendants also borrow their upstream transport. In particular a
 * child's start frame precedes its registry publication. */
static bool usb_operation_uses_hcd(usb_hcd_t *hcd) {
    for (struct usb_operation *at = usb_operations; at; at = at->previous) {
        usb_hcd_t *bus = at->hcd;
        for (unsigned depth = 0; bus && depth < USB_MAX_HCDS; depth++) {
            if (bus == hcd) return true;
            if (!bus->parent_hcd || !usb_hcd_registered(bus->parent_hcd)) break;
            bus = bus->parent_hcd;
        }
    }
    return false;
}

static usb_hcd_t *usb_root_hcd(usb_hcd_t *hcd) {
    for (unsigned depth = 0; hcd && depth < USB_MAX_HCDS; depth++) {
        if (!hcd->parent_hcd) return hcd;
        if (!usb_hcd_registered(hcd->parent_hcd)) return NULL;
        hcd = hcd->parent_hcd;
    }
    return NULL;
}
static bool usb_controller_has_pending(usb_hcd_t *hcd) {
    usb_hcd_t *root = usb_root_hcd(hcd);
    if (!root) return true;
    for (int i = 0; i < g_hcd_count; i++) {
        usb_hcd_t *peer = g_hcds[i];
        if (usb_root_hcd(peer) != root) continue;
        for (unsigned port = 0; port < peer->max_ports; port++)
            if (peer->port_state[port] & USB_PORT_REMOVE_PENDING) return true;
    }
    return false;
}
static bool usb_hcd_stopping(usb_hcd_t *hcd) {
    for (unsigned depth = 0; hcd && depth < USB_MAX_HCDS; depth++) {
        if (hcd->hcd_dev && device_is_stopping(hcd->hcd_dev)) return true;
        if (!hcd->parent_hcd) return false;
        if (!usb_hcd_registered(hcd->parent_hcd)) return true;
        hcd = hcd->parent_hcd;
    }
    return true;
}

int usb_core_register_hcd(usb_hcd_t *hcd) {
    if (!hcd) return -EINVAL;
    driver_lifecycle_begin();
    int ret = -EBUSY;
    if (!usb_operation_active(hcd)) {
        struct usb_operation operation = {hcd, usb_operations};
        usb_operations = &operation;
        if (usb_hcd_registered(hcd)) ret = -EEXIST;
        else if (!usb_root_hcd(hcd)) ret = -ENODEV;
        else if (usb_hcd_stopping(hcd)) ret = -ENODEV;
        else if ((unsigned)g_hcd_count + usb_hcd_starting >= USB_MAX_HCDS) ret = -ENOSPC;
        else {
            /* Reserve capacity before start can register a different HCD. */
            usb_hcd_starting++;
            ret = usb_register_hcd_locked(hcd);
            usb_hcd_starting--;
        }
        usb_operations = operation.previous;
    }
    driver_lifecycle_end();
    return ret;
}

int usb_core_unregister_hcd(usb_hcd_t *hcd) {
    if (!hcd) return -EINVAL;
    driver_lifecycle_begin();
    int ret = -EBUSY;
    if (!usb_operation_uses_hcd(hcd)) {
        struct usb_operation operation = {hcd, usb_operations};
        usb_operations = &operation;
        ret = usb_unregister_hcd_locked(hcd);
        usb_operations = operation.previous;
    }
    driver_lifecycle_end();
    return ret;
}

int usb_core_enumerate_port(usb_hcd_t *hcd, unsigned port) {
    if (!hcd) return -EINVAL;
    driver_lifecycle_begin();
    int ret = -EBUSY;
    if (!usb_operation_active(hcd)) {
        struct usb_operation operation = {hcd, usb_operations};
        usb_operations = &operation;
        ret = usb_hcd_registered(hcd) ? usb_enumerate_port_locked(hcd, port) : -ENODEV;
        usb_operations = operation.previous;
    }
    driver_lifecycle_end();
    return ret;
}

void usb_core_scan(void) {
    driver_lifecycle_begin();
    if (!usb_scanning && !usb_operations) {
        usb_scanning = true;
        usb_scan_locked();
        usb_scanning = false;
    }
    driver_lifecycle_end();
}

int usb_device_poll(usb_device_t *dev) {
    if (!dev || !dev->hcd) return -EINVAL;
    int ret = driver_lifecycle_try_begin();
    if (ret) return ret; /* Never wait on teardown holding an active class call. */
    usb_hcd_t *hcd = dev->hcd;
    ret = -ENODEV;
    if (usb_hcd_registered(hcd) && !usb_hcd_stopping(hcd)) {
        for (unsigned port = 0; port < hcd->max_ports; port++) {
            if (hcd->port_devices[port] != dev ||
                (hcd->port_state[port] & USB_PORT_REMOVE_PENDING)) continue;
            ret = -EBUSY;
            if (!usb_operation_active(hcd)) {
                struct usb_operation operation = {hcd, usb_operations};
                usb_operations = &operation;
                ret = hcd->ops->poll ? hcd->ops->poll(hcd) : 0;
                usb_operations = operation.previous;
            }
            break;
        }
    }
    driver_lifecycle_end();
    return ret;
}

int usb_interface_release_endpoints(usb_interface_t *iface) {
    if (!iface || !iface->dev || !iface->device || (iface->ep_count && !iface->eps)) return -EINVAL;
    driver_lifecycle_begin();
    usb_device_t *dev = iface->dev;
    usb_hcd_t *hcd = dev->hcd;
    int ret = -ENODEV;
    if (!device_is_stopping(iface->device) || iface->device->plat_data != iface) {
        ret = -EINVAL;
        goto out;
    }
    if (!usb_hcd_registered(hcd)) goto out;
    bool owned = false, stopped = false;
    for (unsigned port = 0; port < hcd->max_ports; port++) {
        if (hcd->port_devices[port] != dev) continue;
        owned = true;
        stopped = hcd->port_state[port] & USB_PORT_SLOT_STOPPED;
        break;
    }
    if (!owned) goto out;
    ret = 0;
    if (stopped) goto out; /* Slot abort already proved the stronger condition. */
    if (iface->ep_count && !hcd->ops->release_endpoint) { ret = -EOPNOTSUPP; goto out; }
    for (uint8_t i = 0; i < iface->ep_count; i++) {
        if (!(iface->eps[i].addr & 0x0fU)) { ret = -EINVAL; break; }
        ret = hcd->ops->release_endpoint(hcd, dev, iface->eps[i].addr);
        if (ret) { if (ret > 0) ret = -EIO; break; }
    }
out:
    driver_lifecycle_end();
    return ret;
}

void usb_core_poll(void)
{
    static uint64_t last_poll;
    static bool polling;
    /* A scheduler pass on another CPU can overlap a slow port reset. Keep
     * enumeration and its per-port retry accounting single-owner. */
    if (__atomic_exchange_n(&polling, true, __ATOMIC_ACQUIRE))
        return;
    if (driver_lifecycle_try_begin()) {
        __atomic_store_n(&polling, false, __ATOMIC_RELEASE);
        return;
    }
    uint64_t now = timer_get_ticks();
    /* 50 Hz, not 4 Hz.  HID interrupt transfers fall back to this loop when a
     * platform does not route the xHCI line; at 4 Hz a boot keyboard drops
     * every keystroke that is pressed and released between two sweeps.  The
     * port sweep is a handful of MMIO reads and the IRQ path remains the
     * normal completion source. */
    uint64_t interval = clock_ticks_per_sec() / 50;
    if (!interval)
        interval = 1;
    if (now - last_poll >= interval && !usb_scanning && !usb_operations) {
        last_poll = now;
        usb_scanning = true;
        usb_scan_locked();
        usb_scanning = false;
    }
    driver_lifecycle_end();
    __atomic_store_n(&polling, false, __ATOMIC_RELEASE);
}

void usb_core_init(void)
{
    bus_register(&g_usb_bus);
}
