/*
 * A20OS — USB hub class driver.
 *
 * A class-9 hub turns its downstream ports into a second tier of the bus.
 * The core only knows how to enumerate the ports of an usb_hcd_t, so this
 * driver publishes the hub's ports as a second usb_hcd_t whose ops are all
 * control transfers to the hub device itself.  A device behind a hub is
 * therefore enumerated by exactly the same code that enumerates a device on
 * a root port, and a second hub just nests.
 *
 * The hub does not own the controller: the endpoint contexts a downstream
 * device needs live in the root controller, so every data path here is
 * forwarded to the parent HCD with the token that HCD handed out.  Only the
 * bus address is the hub's, which is why addresses for hub-attached devices
 * come from the core-wide pool rather than from the hub itself.
 */
#include "drivers/usb/usb.h"

#include "core/defs.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/timer.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "mm/slab.h"

/* A USB 3.x hub asks for a downstream reset with SET_FEATURE on the port's
 * SuperSpeed Reset bit, which is bit 27 of the status word; it is not one of
 * the class feature selectors. */
#define HUB_FEAT_SS_RESET        (1U << 27)

/* Port status bitmaps: ceil(bNbrPorts / 8) bytes of port status, then the
 * same number of change bits, plus one byte of hub-wide status. */
#define HUB_STATUS_BYTES(nports)  (((nports) + 7U) / 8U + 1U)
#define HUB_CTRL_BUF_SZ           256

/* Hub port status, USB 2.0 half, as a bit position inside one byte of the
 * port bitmap. */
#define HUB_PORT_BIT_CONNECTION    0U
#define HUB_PORT_BIT_ENABLE        1U
#define HUB_PORT_BIT_OVERCURRENT   8U
#define HUB_PORT_BIT_RESET         4U
#define HUB_PORT_BIT_LOW_SPEED     10U

/* Reset recovery budget.  A device has 10 ms to become ready after a reset;
 * the budget above that is generous, but a wedged port must not hold up the
 * rest of the bus. */
#define HUB_RESET_POLL_MS          500U
#define HUB_RESET_STEP_MS          2U

typedef struct usb_hub_port {
    uint8_t token;              /* parent-controller context, 0 = unused */
    uint8_t address;            /* USB address the hub assigned */
} usb_hub_port_t;

typedef struct usb_hub {
    usb_hcd_t      hcd;          /* the downstream bus */
    usb_hcd_t     *parent_hcd;
    usb_device_t  *hub;          /* the hub device itself, on parent_hcd */

    device_t      *dev;          /* the class-9 interface we bound to */
    usb_interface_t *iface;

    uint8_t        ports;
    uint8_t        status_bytes;
    uint16_t       characteristics;
    uint8_t        superspeed;   /* downstream ports are SuperSpeed ports */
    uint8_t        pwr_on2pwr_good;  /* bPwrOn2PwrGood, in 2 ms units */
    uint8_t        registered;

    uint8_t       *status;       /* port status, then hub status */
    uint8_t       *status_change;

    usb_hub_port_t child[USB_HUB_MAX_PORTS];

    usb_endpoint_t *sc_ep;        /* status-change endpoint on the hub */
    usb_urb_t       sc_urb;
    uint8_t         sc_buf[HUB_CTRL_BUF_SZ];
    /* The parent HCD completes this URB from its own interrupt handler as
     * well as from its process-context poll, while hub_op_poll() reads and
     * writes the same two flags, so they carry their own lock.  It is the
     * outermost lock here: hub_sc_arm() reaches the parent HCD under it, and
     * the parent never takes it back. */
    uint8_t         sc_armed;
    uint8_t         sc_changed;
    spinlock_t      sc_lock;
} usb_hub_t;

/* ------------------------------------------------------------------ */
/* Port bitmap access                                                  */
/* ------------------------------------------------------------------ */

static int hub_port_bit(const uint8_t *map, unsigned port, unsigned bit)
{
    uint8_t index = (uint8_t)(port - 1U);
    return (map[index >> 3] >> (bit & 7U)) & 1U;
}

static int hub_port_connected_bit(const usb_hub_t *hub, unsigned port)
{
    uint8_t index = (uint8_t)(port - 1U);
    if (hub->superspeed)
        /* SuperSpeed state occupies bits 24..31, which is nibble
         * (index & 7) of the byte. */
        return (hub->status[index >> 3] >> ((index & 7U) * 4U)) & 0x0fU;
    return hub_port_bit(hub->status, port, HUB_PORT_BIT_CONNECTION);
}

static int hub_port_reset_bit(const usb_hub_t *hub, unsigned port)
{
    uint8_t index = (uint8_t)(port - 1U);
    if (hub->superspeed)
        return (hub->status[index >> 3] >> ((index & 7U) * 4U)) & 0x08U;
    return hub_port_bit(hub->status, port, HUB_PORT_BIT_RESET);
}

static int hub_port_enable_bit(const usb_hub_t *hub, unsigned port)
{
    uint8_t index = (uint8_t)(port - 1U);
    if (hub->superspeed)
        return (hub->status[index >> 3] >> ((index & 7U) * 4U)) & 0x10U;
    return hub_port_bit(hub->status, port, HUB_PORT_BIT_ENABLE);
}

static int hub_port_overcurrent_bit(const usb_hub_t *hub, unsigned port)
{
    uint8_t index = (uint8_t)(port - 1U);
    if (hub->superspeed)
        return (hub->status[index >> 3] >> ((index & 7U) * 4U)) & 0x20U;
    return hub_port_bit(hub->status, port, HUB_PORT_BIT_OVERCURRENT);
}

static int hub_port_low_speed_bit(const usb_hub_t *hub, unsigned port)
{
    uint8_t index = (uint8_t)(port - 1U);
    return (hub->status[index >> 3] >> HUB_PORT_BIT_LOW_SPEED) & 1U;
}

/* SuperSpeed link speed, which lives in the top two bits of the nibble. */
static unsigned hub_port_ss_speed(const usb_hub_t *hub, unsigned port)
{
    uint8_t index = (uint8_t)(port - 1U);
    return (unsigned)((hub->status[index >> 3] >> ((index & 7U) * 4U)) >> 2);
}

/* ------------------------------------------------------------------ */
/* Requests aimed at the hub itself                                   */
/* ------------------------------------------------------------------ */

static int hub_class_req(usb_hub_t *hub, uint8_t request_type,
                         uint8_t request, uint16_t value, uint16_t index,
                         void *data, uint16_t len)
{
    return usb_control_msg(hub->hub, request_type, request, value, index,
                           data, len);
}

static int hub_get_status(usb_hub_t *hub)
{
    uint8_t bytes = hub->status_bytes;
    if ((uint16_t)bytes * 2U > sizeof(hub->sc_buf))
        return -EINVAL;
    int r = hub_class_req(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                          USB_REQ_GET_DESCRIPTOR, 0, 0, hub->sc_buf, bytes);
    if (r)
        return r;
    memcpy(hub->status, hub->sc_buf, bytes);
    memcpy(hub->status_change, hub->sc_buf + bytes, bytes);
    return 0;
}

static int hub_set_feature(usb_hub_t *hub, uint16_t feature, uint16_t port)
{
    return hub_class_req(hub, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                         USB_REQ_SET_FEATURE, feature, port, NULL, 0);
}

static int hub_clear_feature(usb_hub_t *hub, uint16_t feature, uint16_t port)
{
    return hub_class_req(hub, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                         USB_REQ_CLEAR_FEATURE, feature, port, NULL, 0);
}

/* Acknowledge latched changes so the hub stops reporting a port the core has
 * already acted on. */
static void hub_ack_changes(usb_hub_t *hub)
{
    for (unsigned port = 1; port <= hub->ports; port++) {
        if (!hub_port_bit(hub->status_change, port, HUB_PORT_BIT_CONNECTION))
            continue;
        (void)hub_clear_feature(hub, USB_FEAT_C_PORT_CONNECTION,
                                (uint16_t)port);
        (void)hub_clear_feature(hub, USB_FEAT_C_PORT_ENABLE, (uint16_t)port);
    }
}

/* ------------------------------------------------------------------ */
/* The downstream bus                                                  */
/* ------------------------------------------------------------------ */

static int hub_op_start(usb_hcd_t *hcd)
{
    /* Everything the downstream bus needs was established in probe, before
     * the core sized its port arrays from max_ports. */
    (void)hcd;
    return 0;
}

static int hub_op_port_connected(usb_hcd_t *hcd, unsigned port)
{
    usb_hub_t *hub = hcd->priv;
    if (port == 0 || port > hub->ports)
        return 0;
    return hub_port_connected_bit(hub, port);
}

static int hub_op_reset_port(usb_hcd_t *hcd, unsigned port, uint8_t *speed)
{
    usb_hub_t *hub = hcd->priv;
    if (port == 0 || port > hub->ports)
        return -EINVAL;

    /* Acknowledge whatever reset-change the hub latched before asking for a
     * reset: a stale change bit would be read as this reset having finished
     * on the very first poll. */
    (void)hub_clear_feature(hub, USB_FEAT_C_PORT_RESET, (uint16_t)port);
    int r = hub_set_feature(hub, hub->superspeed ? HUB_FEAT_SS_RESET
                                                 : USB_FEAT_PORT_RESET,
                            (uint16_t)port);
    if (r)
        return r;

    for (uint32_t waited = 0; waited < HUB_RESET_POLL_MS;
         waited += HUB_RESET_STEP_MS) {
        mdelay(HUB_RESET_STEP_MS);
        if (hub_get_status(hub))
            continue;
        if (hub_port_reset_bit(hub, port))
            continue;
        if (hub_port_overcurrent_bit(hub, port))
            return -EIO;
        if (!hub_port_enable_bit(hub, port))
            return -ETIMEDOUT;
        if (hub->superspeed) {
            switch (hub_port_ss_speed(hub, port)) {
            case 1:  *speed = USB_SPEED_LOW;   break;
            case 2:  *speed = USB_SPEED_FULL;  break;
            case 3:  *speed = USB_SPEED_HIGH;  break;
            default: *speed = USB_SPEED_SUPER; break;
            }
        } else {
            /* A USB 2.0 hub only ever negotiates full or low speed: a high
             * speed device behind one drops to full speed by design, so the
             * absence of the low-speed bit means full. */
            *speed = hub_port_low_speed_bit(hub, port)
                     ? USB_SPEED_LOW : USB_SPEED_FULL;
        }
        (void)hub_clear_feature(hub, USB_FEAT_C_PORT_RESET, (uint16_t)port);
        (void)hub_clear_feature(hub, USB_FEAT_PORT_RESET, (uint16_t)port);
        return 0;
    }
    return -ETIMEDOUT;
}

/* Find the downstream port holding the controller token @token.  Token 0 is
 * the parent controller's "no such slot" value and never matches. */
static usb_hub_port_t *hub_port_of(usb_hub_t *hub, uint8_t token)
{
    if (!token)
        return NULL;
    for (unsigned port = 1; port <= hub->ports; port++) {
        if (hub->child[port - 1].token == token)
            return &hub->child[port - 1];
    }
    return NULL;
}

static int hub_op_alloc_slot(usb_hcd_t *hcd, unsigned port, uint8_t speed,
                             uint8_t hub_address, uint8_t address,
                             usb_slot_t *out)
{
    usb_hub_t *hub = hcd->priv;
    (void)hub_address;           /* it is this hub, by construction */
    if (port == 0 || port > hub->ports)
        return -EINVAL;

    usb_hub_port_t *child = &hub->child[port - 1];
    if (child->token) {
        /* The core asked for a second context on an occupied port.  Reusing
         * the first would leave two usb_device_t objects sharing one. */
        return -EBUSY;
    }

    /* The parent controller builds the child's context, naming this hub as
     * the port's owner.  A root controller that addresses devices implicitly
     * never gave the hub a USB address, so the slot context says "root hub",
     * which is the same statement that controller already made about every
     * other device on it. */
    usb_slot_t ctx;
    int r = hcd->parent_hcd->ops->alloc_slot(hcd->parent_hcd, port, speed,
                                             hub->hub->address, address,
                                             &ctx);
    if (r)
        return r;

    /* The device is still unaddressed, so every transfer must still go
     * through the hub.  SET_ADDRESS is the first: a hub addresses the device
     * on the port the reset just cleared, which is why wIndex carries the
     * port number rather than an address. */
    r = hub_class_req(hub, USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                      USB_REQ_SET_ADDRESS, address, (uint16_t)port, NULL, 0);
    if (r) {
        hcd->parent_hcd->ops->abort_slot(hcd->parent_hcd, ctx.hcd);
        return r;
    }
    /* The device only accepts traffic again once it has been reset a second
     * time; the hub states how long that takes as bPwrOn2PwrGood. */
    (void)hub_set_feature(hub, USB_FEAT_PORT_RESET, (uint16_t)port);
    mdelay(2U * (uint32_t)hub->pwr_on2pwr_good);

    child->token = ctx.hcd;
    child->address = address;
    *out = ctx;
    out->address = address;     /* the hub took the number */
    return 0;
}

/* A transfer that must reach a downstream device is issued to the hub with
 * wIndex naming the device, because that is how a hub is told which of its
 * ports a control transfer is for. */
static int hub_child_control(usb_hub_t *hub, uint8_t token,
                             const usb_setup_packet_t *setup, void *data)
{
    usb_hub_port_t *child = hub_port_of(hub, token);
    if (!child)
        return -ENODEV;
    usb_setup_packet_t forwarded = *setup;
    forwarded.index = child->address;
    return usb_control_msg(hub->hub, forwarded.request_type,
                           forwarded.request, forwarded.value,
                           forwarded.index, data, forwarded.length);
}

static int hub_op_control(usb_hcd_t *hcd, uint8_t token,
                          const usb_setup_packet_t *setup, void *data)
{
    return hub_child_control(hcd->priv, token, setup, data);
}

static int hub_op_get_descriptor(usb_hcd_t *hcd, uint8_t token, uint8_t type,
                                 uint16_t len, void *buf)
{
    usb_hub_t *hub = hcd->priv;
    usb_hub_port_t *child = hub_port_of(hub, token);
    if (!child)
        return -ENODEV;
    usb_setup_packet_t setup = {
        .request_type = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (uint16_t)type << 8,
        .index = child->address,
        .length = len,
    };
    return usb_control_msg(hub->hub, setup.request_type, setup.request,
                           setup.value, setup.index, buf, len);
}

static int hub_op_update_ep0_mps(usb_hcd_t *hcd, uint8_t token,
                                 uint16_t max_packet)
{
    /* A hub enforces 8 bytes on a downstream control pipe whatever the
     * device's own wMaxPacketSize0 says, so there is nothing to reprogram. */
    (void)hcd;
    (void)token;
    (void)max_packet;
    return 0;
}

static int hub_op_configure_endpoint(usb_hcd_t *hcd, usb_device_t *dev,
                                     uint8_t addr, uint8_t ep_type,
                                     uint16_t max_packet, uint8_t interval)
{
    /* The transfer context belongs to the root controller, which is the only
     * thing that can program a ring for it. */
    return hcd->parent_hcd->ops->configure_endpoint(hcd->parent_hcd, dev, addr,
                                                   ep_type, max_packet,
                                                   interval);
}

static int hub_op_submit_interrupt(usb_hcd_t *hcd, usb_urb_t *urb)
{
    return hcd->parent_hcd->ops->submit_interrupt(hcd->parent_hcd, urb);
}

static int hub_op_submit_bulk(usb_hcd_t *hcd, usb_urb_t *urb)
{
    return hcd->parent_hcd->ops->submit_bulk(hcd->parent_hcd, urb);
}

static int hub_op_abort_slot(usb_hcd_t *hcd, uint8_t token)
{
    usb_hub_t *hub = hcd->priv;
    usb_hub_port_t *child = hub_port_of(hub, token);
    if (!child)
        return 0;
    usb_core_free_address(child->address);
    child->token = 0;
    child->address = 0;
    return hcd->parent_hcd->ops->abort_slot(hcd->parent_hcd, token);
}

/* ------------------------------------------------------------------ */
/* Status-change endpoint                                             */
/* ------------------------------------------------------------------ */

static void hub_sc_complete(usb_urb_t *urb)
{
    usb_hub_t *hub = (usb_hub_t *)urb->ctx;
    if (!hub)
        return;
    /* Completion runs from inside the parent controller's event handling --
     * its interrupt handler, its poll, or a synchronous wait on another
     * endpoint -- so it only records that something moved; our own poll does
     * the work. */
    uint64_t flags = spin_lock_irqsave(&hub->sc_lock);
    if (urb->status == 0)
        hub->sc_changed = 1;
    hub->sc_armed = 0;
    spin_unlock_irqrestore(&hub->sc_lock, flags);
}

static int hub_sc_arm(usb_hub_t *hub)
{
    uint64_t flags = spin_lock_irqsave(&hub->sc_lock);
    if (hub->sc_armed || !hub->sc_ep) {
        spin_unlock_irqrestore(&hub->sc_lock, flags);
        return 0;
    }
    /* Claim the slot before the submit, not after it.  The completion arrives
     * from the parent controller -- its IRQ handler or its poll, i.e. another
     * CPU -- and it clears sc_armed; storing 1 after the submit would overwrite
     * that clear, leaving hub_op_poll() returning early forever and this hub
     * permanently deaf to attach/detach.  Going up first makes an early
     * completion the last writer, which is the order that re-arms. */
    hub->sc_armed = 1;
    spin_unlock_irqrestore(&hub->sc_lock, flags);
    hub->sc_urb.buf = hub->sc_buf;
    hub->sc_urb.len = hub->status_bytes;
    /* Submitted without sc_lock: usb_submit_urb() reaches the parent
     * controller's lock, and the parent never takes sc_lock back, so the
     * order is one-way. */
    int r = usb_submit_urb(&hub->sc_urb);
    if (r) {
        /* Nothing was armed, so give the slot back. */
        flags = spin_lock_irqsave(&hub->sc_lock);
        hub->sc_armed = 0;
        spin_unlock_irqrestore(&hub->sc_lock, flags);
    }
    return r;
}

static int hub_op_poll(usb_hcd_t *hcd)
{
    usb_hub_t *hub = hcd->priv;

    /* The endpoint completes only when the hub has something to report, so
     * re-arming is what turns a status change into a bit we can look at. */
    uint64_t flags = spin_lock_irqsave(&hub->sc_lock);
    if (hub->sc_armed) {
        spin_unlock_irqrestore(&hub->sc_lock, flags);
        return 0;
    }
    int changed = hub->sc_changed;
    hub->sc_changed = 0;
    spin_unlock_irqrestore(&hub->sc_lock, flags);

    if (changed) {
        if (hub_get_status(hub) == 0)
            hub_ack_changes(hub);
    } else if (hub_get_status(hub) != 0) {
        return 0;
    }
    return hub_sc_arm(hub);
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
    .submit_interrupt = hub_op_submit_interrupt,
    .submit_bulk = hub_op_submit_bulk,
    .abort_slot = hub_op_abort_slot,
};

/* ------------------------------------------------------------------ */
/* Probe                                                              */
/* ------------------------------------------------------------------ */

static void hub_free(usb_hub_t *hub)
{
    if (!hub)
        return;
    /* The parent controller may still have this URB queued when we are torn
     * down.  Clearing the callback narrows that window to a completion that
     * has already read the pointer; it does not close it, because aborting a
     * queued URB is the parent's job and the parent runs it from the same
     * event path this completion runs in.  Written under sc_lock so it is a
     * single store the completion either sees or does not. */
    uint64_t flags = spin_lock_irqsave(&hub->sc_lock);
    hub->sc_urb.complete = NULL;
    hub->sc_urb.ctx = NULL;
    spin_unlock_irqrestore(&hub->sc_lock, flags);
    if (hub->registered)
        usb_core_unregister_hcd(&hub->hcd);
    kfree(hub->status);
    kfree(hub);
}

static int usb_hub_probe(device_t *dev)
{
    usb_interface_t *iface = (usb_interface_t *)dev->plat_data;
    if (!iface || iface->interface_class != USB_CLASS_HUB)
        return -EINVAL;
    usb_device_t *hub_dev = iface->dev;
    if (!hub_dev || !hub_dev->hcd || !hub_dev->hcd->ops->control)
        return -EINVAL;

    usb_hub_t *hub = kcalloc(1, sizeof(*hub));
    if (!hub)
        return -ENOMEM;
    hub->hub = hub_dev;
    hub->parent_hcd = hub_dev->hcd;
    hub->dev = dev;
    hub->iface = iface;
    spin_init(&hub->sc_lock);

    /* A hub descriptor is a class request, not a standard one: the hub is
     * asked for it with bmRequestType 0xA0 and wValue 0.  Answering it as a
     * standard GET_DESCRIPTOR, which is the obvious thing to do, gets a
     * stall from every real hub.
     *
     * Its length is also variable — the fixed part, then DeviceRemovable[]
     * and PortPwrCtrlMask[], both ceil(bNbrPorts / 8) bytes — so read the
     * fixed part first, because that is what says how long the rest is. */
    usb_hub_descriptor_t hd;
    int r = usb_control_msg(hub_dev, USB_DIR_IN | USB_TYPE_CLASS |
                            USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                            0, 0, &hd, sizeof(hd));
    if (r) {
        kerr("[USB-HUB] hub descriptor read failed: %d\n", r);
        hub_free(hub);
        return r;
    }
    if (hd.type != USB_DT_HUB || hd.length < sizeof(hd) ||
        hd.ports == 0 || hd.ports > USB_HUB_MAX_PORTS) {
        kerr("[USB-HUB] malformed hub descriptor (type=%u len=%u ports=%u)\n",
             hd.type, hd.length, hd.ports);
        hub_free(hub);
        return -EINVAL;
    }

    hub->ports = hd.ports;
    hub->characteristics = hd.characteristics;
    hub->pwr_on2pwr_good = hd.pwr_on2pwr_good;
    hub->status_bytes = (uint8_t)HUB_STATUS_BYTES(hd.ports);
    /* wHubCharacteristics bit 2 means the hub itself runs at SuperSpeed or
     * above, which is what makes its downstream ports SuperSpeed ports. */
    hub->superspeed = (hd.characteristics & USB_HUB_CHAR_SUPERSPEED) ? 1 : 0;

    hub->status = kcalloc((size_t)hub->status_bytes * 2U, 1);
    if (!hub->status) {
        hub_free(hub);
        return -ENOMEM;
    }
    hub->status_change = hub->status + hub->status_bytes;

    kinfo("[USB-HUB] hub %04x:%04x: downstream ports=%u status_bytes=%u ss=%u\n",
          hub_dev->vendor, hub_dev->product, hub->ports, hub->status_bytes,
          hub->superspeed);

    /* Arm the status-change endpoint before publishing the bus, so a device
     * plugged the moment the ports go live is already noticed. */
    for (uint8_t i = 0; i < iface->ep_count; i++) {
        usb_endpoint_t *ep = &iface->eps[i];
        if ((ep->attrs & 3U) != USB_XFER_INTERRUPT || !(ep->addr & USB_DIR_IN))
            continue;
        hub->sc_ep = ep;
        break;
    }
    if (hub->sc_ep) {
        r = hub->parent_hcd->ops->configure_endpoint(hub->parent_hcd, hub_dev,
                                                     hub->sc_ep->addr,
                                                     USB_XFER_INTERRUPT,
                                                     hub->sc_ep->max_packet,
                                                     hub->sc_ep->interval);
        if (r) {
            kerr("[USB-HUB] status-change endpoint %02x setup failed: %d\n",
                 hub->sc_ep->addr, r);
            hub->sc_ep = NULL;
        } else {
            hub->sc_urb.dev = hub_dev;
            hub->sc_urb.ep = hub->sc_ep;
            hub->sc_urb.transfer_type = USB_XFER_INTERRUPT;
            hub->sc_urb.direction = USB_DIR_IN;
            hub->sc_urb.complete = hub_sc_complete;
            hub->sc_urb.ctx = hub;
            kinfo("[USB-HUB] status-change endpoint %02x armed: mps=%u interval=%u\n",
                  hub->sc_ep->addr, hub->sc_ep->max_packet,
                  hub->sc_ep->interval);
        }
    } else {
        kinfo("[USB-HUB] no interrupt IN endpoint; ports are polled only\n");
    }

    hub->hcd.ops = &hub_hcd_ops;
    hub->hcd.hcd_dev = dev;
    hub->hcd.parent_hcd = hub->parent_hcd;
    hub->hcd.hub_address = hub_dev->address;
    hub->hcd.max_ports = hub->ports;
    hub->hcd.priv = hub;

    /* Registering is what makes the core scan these ports, and descend into
     * anything already plugged in behind the hub. */
    r = usb_core_register_hcd(&hub->hcd);
    if (r) {
        kerr("[USB-HUB] downstream bus registration failed: %d\n", r);
        hub_free(hub);
        return r;
    }
    hub->registered = 1;
    dev->drv_priv = hub;

    /* Seed the bitmap so the first scan does not read a stable port as one
     * that just changed. */
    if (hub_get_status(hub) == 0)
        hub_ack_changes(hub);
    (void)hub_sc_arm(hub);

    kinfo("[USB-HUB] downstream bus live: %u ports behind %04x:%04x\n",
          hub->ports, hub_dev->vendor, hub_dev->product);
    return 0;
}

static int usb_hub_remove(device_t *dev)
{
    usb_hub_t *hub = (usb_hub_t *)dev->drv_priv;
    dev->drv_priv = NULL;
    hub_free(hub);
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
    .bus = NULL,               /* bound via the usb bus match on interface */
    .probe = usb_hub_probe,
    .remove = usb_hub_remove,
    .class_type = DEV_CLASS_NONE,
};

DRIVER_REGISTER(usb_hub_driver);
