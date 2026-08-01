/*
 * A20OS — USB HID boot-protocol class driver.
 *
 * Binds to HID interfaces with boot subclass (keyboard/mouse) and the
 * QEMU/VBox USB tablet quirk, configures the interrupt IN endpoint through
 * the HCD, and turns boot-protocol reports into input events.
 */
#include "drivers/usb/usb.h"

#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/input/virtio_input.h"
#include "mm/slab.h"
#include "abi/linux/errno.h"

#define USB_HID_REPORT_MAX  64U
#define USB_HID_EVENTS      64U

#define USB_HID_KBD   1U
#define USB_HID_MOUSE 2U
#define USB_HID_TABLET 3U

#define VBOX_USB_VENDOR 0x80eeU
#define VBOX_USB_TABLET 0x0021U

#define SYN_REPORT  0x00U
#define REL_X       0x00U
#define REL_Y       0x01U
#define REL_WHEEL   0x08U
#define ABS_X       0x00U
#define ABS_Y       0x01U
#define BTN_LEFT    0x110U
#define BTN_RIGHT   0x111U
#define BTN_MIDDLE  0x112U

typedef struct usb_hid_dev {
    usb_interface_t *iface;
    usb_endpoint_t  *ep;
    usb_urb_t        urb;
    uint8_t          report[USB_HID_REPORT_MAX] ALIGNED(64);
    uint8_t          previous[USB_HID_REPORT_MAX];
    uint8_t          kind;
    uint8_t          report_size;
    int              running;
    struct input_event events[USB_HID_EVENTS];
    uint16_t         head;
    uint16_t         tail;
    spinlock_t       lock;
} usb_hid_dev_t;

static const uint16_t hid_usage_to_linux[0x74] = {
    [0x04] = 30, [0x05] = 48, [0x06] = 46, [0x07] = 32,
    [0x08] = 18, [0x09] = 33, [0x0a] = 34, [0x0b] = 35,
    [0x0c] = 23, [0x0d] = 36, [0x0e] = 37, [0x0f] = 38,
    [0x10] = 50, [0x11] = 49, [0x12] = 24, [0x13] = 25,
    [0x14] = 16, [0x15] = 19, [0x16] = 31, [0x17] = 20,
    [0x18] = 22, [0x19] = 47, [0x1a] = 17, [0x1b] = 45,
    [0x1c] = 21, [0x1d] = 44,
    [0x1e] = 2, [0x1f] = 3, [0x20] = 4, [0x21] = 5,
    [0x22] = 6, [0x23] = 7, [0x24] = 8, [0x25] = 9,
    [0x26] = 10, [0x27] = 11,
    [0x28] = 28, [0x29] = 1, [0x2a] = 14, [0x2b] = 15,
    [0x2c] = 57, [0x2d] = 12, [0x2e] = 13, [0x2f] = 26,
    [0x30] = 27, [0x31] = 43, [0x32] = 43, [0x33] = 39,
    [0x34] = 40, [0x35] = 41, [0x36] = 51, [0x37] = 52,
    [0x38] = 53, [0x39] = 58,
    [0x3a] = 59, [0x3b] = 60, [0x3c] = 61, [0x3d] = 62,
    [0x3e] = 63, [0x3f] = 64, [0x40] = 65, [0x41] = 66,
    [0x42] = 67, [0x43] = 68, [0x44] = 87, [0x45] = 88,
    [0x46] = 99, [0x47] = 70, [0x48] = 119, [0x49] = 110,
    [0x4a] = 102, [0x4b] = 104, [0x4c] = 111, [0x4d] = 107,
    [0x4e] = 109, [0x4f] = 106, [0x50] = 105, [0x51] = 108,
    [0x52] = 103, [0x53] = 69, [0x54] = 98, [0x55] = 55,
    [0x56] = 74, [0x57] = 78, [0x58] = 96, [0x59] = 79,
    [0x5a] = 80, [0x5b] = 81, [0x5c] = 75, [0x5d] = 76,
    [0x5e] = 77, [0x5f] = 71, [0x60] = 72, [0x61] = 73,
    [0x62] = 82, [0x63] = 83, [0x65] = 127,
};

static void usb_hid_emit(usb_hid_dev_t *h, uint16_t type, uint16_t code,
                         int32_t value)
{
    uint16_t next = (uint16_t)((h->head + 1U) % USB_HID_EVENTS);
    if (next == h->tail)
        return;
    struct input_event *e = &h->events[h->head];
    e->time_sec = 0;
    e->time_usec = 0;
    e->type = type;
    e->code = code;
    e->value = value;
    h->head = next;
}

static int usb_hid_key_present(const uint8_t report[8], uint8_t usage)
{
    for (unsigned i = 2; i < 8; i++)
        if (report[i] == usage)
            return 1;
    return 0;
}

static void usb_hid_parse_keyboard(usb_hid_dev_t *h)
{
    static const uint16_t mods[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };
    const uint8_t *now = h->report;
    const uint8_t *old = h->previous;
    int changed = 0;
    uint8_t mod = now[0] ^ old[0];
    for (unsigned bit = 0; bit < 8; bit++) {
        if (mod & (1U << bit)) {
            usb_hid_emit(h, EV_KEY, mods[bit], !!(now[0] & (1U << bit)));
            changed = 1;
        }
    }
    for (unsigned i = 2; i < 8; i++) {
        uint8_t usage = old[i];
        if (usage > 3 && !usb_hid_key_present(now, usage) &&
            usage < ARRAY_SIZE(hid_usage_to_linux) && hid_usage_to_linux[usage]) {
            usb_hid_emit(h, EV_KEY, hid_usage_to_linux[usage], 0);
            changed = 1;
        }
    }
    for (unsigned i = 2; i < 8; i++) {
        uint8_t usage = now[i];
        if (usage > 3 && !usb_hid_key_present(old, usage) &&
            usage < ARRAY_SIZE(hid_usage_to_linux) && hid_usage_to_linux[usage]) {
            usb_hid_emit(h, EV_KEY, hid_usage_to_linux[usage], 1);
            changed = 1;
        }
    }
    if (changed)
        usb_hid_emit(h, EV_SYN, SYN_REPORT, 0);
    memcpy(h->previous, now, 8);
}

static void usb_hid_parse_mouse(usb_hid_dev_t *h)
{
    static const uint16_t buttons[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    const uint8_t *now = h->report;
    const uint8_t *old = h->previous;
    int changed = 0;
    for (unsigned bit = 0; bit < 3; bit++) {
        if ((now[0] ^ old[0]) & (1U << bit)) {
            usb_hid_emit(h, EV_KEY, buttons[bit], !!(now[0] & (1U << bit)));
            changed = 1;
        }
    }
    if ((int8_t)now[1]) {
        usb_hid_emit(h, EV_REL, REL_X, (int8_t)now[1]);
        changed = 1;
    }
    if ((int8_t)now[2]) {
        usb_hid_emit(h, EV_REL, REL_Y, (int8_t)now[2]);
        changed = 1;
    }
    if (h->report_size >= 4 && (int8_t)now[3]) {
        usb_hid_emit(h, EV_REL, REL_WHEEL, (int8_t)now[3]);
        changed = 1;
    }
    if (changed)
        usb_hid_emit(h, EV_SYN, SYN_REPORT, 0);
    memcpy(h->previous, now, h->report_size);
}

static void usb_hid_parse_tablet(usb_hid_dev_t *h)
{
    static const uint16_t buttons[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    const uint8_t *now = h->report;
    const uint8_t *old = h->previous;
    for (unsigned bit = 0; bit < 3; bit++) {
        if ((now[0] ^ old[0]) & (1U << bit))
            usb_hid_emit(h, EV_KEY, buttons[bit], !!(now[0] & (1U << bit)));
    }
    uint16_t x = (uint16_t)now[4] | ((uint16_t)now[5] << 8);
    uint16_t y = (uint16_t)now[6] | ((uint16_t)now[7] << 8);
    usb_hid_emit(h, EV_ABS, ABS_X, x);
    usb_hid_emit(h, EV_ABS, ABS_Y, y);
    if ((int8_t)now[1])
        usb_hid_emit(h, EV_REL, REL_WHEEL, (int8_t)now[1]);
    usb_hid_emit(h, EV_SYN, SYN_REPORT, 0);
    memcpy(h->previous, now, h->report_size);
}

/* Called from the HCD poll context while the class driver's lock is held. */
static void usb_hid_complete(usb_urb_t *urb)
{
    usb_hid_dev_t *h = (usb_hid_dev_t *)urb->ctx;
    if (!h || !h->running)
        return;
    if (urb->status == 0) {
        if (h->kind == USB_HID_KBD)
            usb_hid_parse_keyboard(h);
        else if (h->kind == USB_HID_MOUSE)
            usb_hid_parse_mouse(h);
        else if (h->kind == USB_HID_TABLET)
            usb_hid_parse_tablet(h);
    }
    /* Re-arm the periodic transfer. */
    (void)usb_submit_urb(urb);
}

static int usb_hid_read(device_t *dev, void *buffer, size_t count)
{
    usb_hid_dev_t *h = (usb_hid_dev_t *)dev->drv_priv;
    if (!h || count < sizeof(struct input_event))
        return -EINVAL;
    usb_hcd_t *hcd = h->iface->dev->hcd;

    uint64_t flags = spin_lock_irqsave(&h->lock);
    if (hcd->ops->poll)
        hcd->ops->poll(hcd);
    size_t copied = 0;
    while (h->tail != h->head && copied + sizeof(struct input_event) <= count) {
        *(struct input_event *)((uint8_t *)buffer + copied) = h->events[h->tail];
        h->tail = (uint16_t)((h->tail + 1U) % USB_HID_EVENTS);
        copied += sizeof(struct input_event);
    }
    spin_unlock_irqrestore(&h->lock, flags);
    return copied ? (int)copied : -EAGAIN;
}

static int usb_hid_poll(device_t *dev, short events)
{
    (void)events;
    usb_hid_dev_t *h = (usb_hid_dev_t *)dev->drv_priv;
    if (!h)
        return 0;
    usb_hcd_t *hcd = h->iface->dev->hcd;
    uint64_t flags = spin_lock_irqsave(&h->lock);
    if (hcd->ops->poll)
        hcd->ops->poll(hcd);
    int ready = h->tail != h->head;
    spin_unlock_irqrestore(&h->lock, flags);
    return ready;
}

static int usb_hid_ioctl(device_t *dev, unsigned long request, void *argument)
{
    (void)dev;
    (void)request;
    (void)argument;
    return -ENOSYS;
}

static const input_dev_ops_t usb_hid_ops = {
    .read = usb_hid_read,
    .ioctl = usb_hid_ioctl,
    .poll = usb_hid_poll,
};

static int usb_hid_probe(device_t *dev)
{
    usb_interface_t *iface = (usb_interface_t *)dev->plat_data;
    if (!iface || !iface->dev || !iface->dev->hcd)
        return -EINVAL;
    usb_device_t *udev = iface->dev;

    /* Determine device kind. */
    uint8_t kind = 0;
    if (iface->interface_subclass == 1 && iface->interface_protocol == 1)
        kind = USB_HID_KBD;
    else if (iface->interface_subclass == 1 && iface->interface_protocol == 2)
        kind = USB_HID_MOUSE;
    else if (udev->vendor == VBOX_USB_VENDOR && udev->product == VBOX_USB_TABLET)
        kind = USB_HID_TABLET;
    if (!kind)
        return -ENODEV;         /* non-boot HID: not supported */

    /* Find the interrupt IN endpoint. */
    usb_endpoint_t *ep = NULL;
    for (uint8_t i = 0; i < iface->ep_count; i++) {
        if ((iface->eps[i].addr & USB_DIR_IN) &&
            (iface->eps[i].attrs & 3U) == USB_XFER_INTERRUPT) {
            ep = &iface->eps[i];
            break;
        }
    }
    if (!ep)
        return -ENODEV;

    usb_hid_dev_t *h = kcalloc(1, sizeof(*h));
    if (!h)
        return -ENOMEM;
    spin_init(&h->lock);
    h->iface = iface;
    h->ep = ep;
    h->kind = kind;
    h->report_size = (kind == USB_HID_MOUSE) ? 4 : 8;
    if (h->report_size > ep->max_packet)
        h->report_size = (uint8_t)ep->max_packet;
    h->urb.dev = udev;
    h->urb.ep = ep;
    h->urb.transfer_type = USB_XFER_INTERRUPT;
    h->urb.direction = USB_DIR_IN;
    h->urb.buf = h->report;
    h->urb.len = h->report_size;
    h->urb.complete = usb_hid_complete;
    h->urb.ctx = h;

    /* Configure the endpoint through the HCD. */
    int r = udev->hcd->ops->configure_endpoint(udev->hcd, udev, ep->addr,
                                               USB_XFER_INTERRUPT,
                                               ep->max_packet, ep->interval);
    if (r) {
        kfree(h);
        return r;
    }

    /* Boot protocol + idle for keyboard/mouse. */
    if (kind == USB_HID_KBD || kind == USB_HID_MOUSE) {
        (void)usb_control_msg(udev, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                              USB_REQ_SET_PROTOCOL, 0,
                              iface->interface_number, NULL, 0);
    }
    (void)usb_control_msg(udev, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                          USB_REQ_SET_IDLE, 0, iface->interface_number, NULL, 0);

    dev->drv_priv = h;
    h->running = 1;
    r = usb_submit_urb(&h->urb);
    if (r) {
        h->running = 0;
        dev->drv_priv = NULL;
        kfree(h);
        return r;
    }

    kinfo("[USB-HID] %s ready: if=%u ep=%02x\n",
          kind == USB_HID_KBD ? "keyboard" :
          kind == USB_HID_MOUSE ? "mouse" : "tablet",
          iface->interface_number, ep->addr);
    return 0;
}

static int usb_hid_remove(device_t *dev)
{
    usb_hid_dev_t *h = (usb_hid_dev_t *)dev->drv_priv;
    if (h) {
        h->running = 0;
        dev->drv_priv = NULL;
        kfree(h);
    }
    return 0;
}

static const device_id_t usb_hid_ids[] = {
    { .vendor = USB_CLASS_HID, .device = DEVICE_ANY,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t usb_hid_driver = {
    .name = "usb-hid",
    .id_table = usb_hid_ids,
    .bus = NULL,
    .probe = usb_hid_probe,
    .remove = usb_hid_remove,
    .class_ops = &usb_hid_ops,
    .class_type = DEV_CLASS_INPUT,
};

DRIVER_REGISTER(usb_hid_driver);
