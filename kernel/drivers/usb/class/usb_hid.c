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
#include "core/sync.h"
#include "core/string.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/input/virtio_input.h"
#include "drivers/char/fbcon.h"
#include "drivers/char/uart.h"
#include "mm/slab.h"
#include "core/errno.h"
#include "drivers/input/evdev.h"

/* IRQ-safe readiness notification; fanout/draining remains in evdev process
 * context and must never acquire the mux's sleeping mutex from this driver. */
extern void input_mux_wake(void);

#define USB_HID_REPORT_MAX  64U
#define USB_HID_EVENTS      64U

#define USB_HID_KBD   1U
#define USB_HID_MOUSE 2U
#define USB_HID_TABLET 3U

#define VBOX_USB_VENDOR 0x80eeU
#define VBOX_USB_TABLET 0x0021U
#define ITE_USB_VENDOR  0x048dU
#define ITE_8910_KBD    0xc102U
#define ITE_8910_REPORT_ID   1U
#define ITE_8910_ARRAY      3U
#define ITE_8910_BITMAP     9U
#define ITE_8910_REPORT_LEN 37U

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
    uint8_t          ite_keyboard;
    uint8_t          report_logged;
    uint8_t          transfer_error_logged;
    uint8_t          console_input_logged;
    int              running;
    struct input_event events[USB_HID_EVENTS];
    uint16_t         head;
    uint16_t         tail;
    /* First decoded key transition, published once.  It is what the x86_64
     * xHCI gate matches: "the interrupt path still carries keystrokes" is a
     * claim about decoded reports, not about enumeration. */
    uint16_t         first_key_code;
    int32_t          first_key_value;
    uint8_t          key_pending;
    uint8_t          key_logged;
    spinlock_t       lock;
    mutex_t         led_lock;
    uint32_t        leds;
    uint8_t         leds_valid;
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
    if (type == EV_KEY && !h->key_logged && !h->key_pending) {
        h->first_key_code = code;
        h->first_key_value = value;
        h->key_pending = 1;
    }
}

static int usb_hid_key_present(const uint8_t report[8], uint8_t usage)
{
    for (unsigned i = 2; i < 8; i++)
        if (report[i] == usage)
            return 1;
    return 0;
}

/* The text shell has no compositor reading evdev, so a USB keyboard must also
 * feed the console byte stream.  PS/2 already does this from its IRQ; do the
 * same for boot-protocol HID reports while fbcon owns the scanout.  The ASCII
 * map is the US layout, matching the PS/2 console keymap.  The GUI path still
 * consumes the evdev events; fbcon_active() is false once DRM presents. */
static char usb_hid_usage_ascii(uint8_t usage, int shift) {
    if (usage >= 0x04 && usage <= 0x1d)
        return (char)((shift ? 'A' : 'a') + (usage - 0x04));
    switch (usage) {
    case 0x1e: return shift ? '!' : '1';
    case 0x1f: return shift ? '@' : '2';
    case 0x20: return shift ? '#' : '3';
    case 0x21: return shift ? '$' : '4';
    case 0x22: return shift ? '%' : '5';
    case 0x23: return shift ? '^' : '6';
    case 0x24: return shift ? '&' : '7';
    case 0x25: return shift ? '*' : '8';
    case 0x26: return shift ? '(' : '9';
    case 0x27: return shift ? ')' : '0';
    case 0x28: return '\n';
    case 0x29: return 0x1b;
    case 0x2a: return 0x7f;
    case 0x2b: return '\t';
    case 0x2c: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x32: return shift ? '~' : '#';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return 0;
    }
}

static int usb_hid_console_key(uint8_t usage, uint8_t mods) {
    if (!fbcon_active())
        return 0;
    int shift = (mods & ((1U << 1) | (1U << 5))) != 0;
    int ctrl = (mods & ((1U << 0) | (1U << 4))) != 0;
    char c = usb_hid_usage_ascii(usage, shift);
    if (!c)
        return 0;
    if (ctrl) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
        else if (c == '[')
            c = 0x1b;
        else if (c == '\\')
            c = 0x1c;
        else if (c == ']')
            c = 0x1d;
        else if (c == '^')
            c = 0x1e;
        else if (c == '_')
            c = 0x1f;
    }
    uart_receive_char(c);
    return 1;
}

static int usb_hid_keyboard_modifiers(usb_hid_dev_t *h, uint8_t old, uint8_t now)
{
    static const uint16_t mods[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };
    uint8_t changed = now ^ old;
    for (unsigned bit = 0; bit < 8; bit++)
        if (changed & (1U << bit))
            usb_hid_emit(h, EV_KEY, mods[bit], !!(now & (1U << bit)));
    return changed != 0;
}

static void usb_hid_parse_keyboard(usb_hid_dev_t *h)
{
    const uint8_t *now = h->report;
    const uint8_t *old = h->previous;
    int changed = usb_hid_keyboard_modifiers(h, old[0], now[0]);
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
            usb_hid_console_key(usage, now[0]);
            changed = 1;
        }
    }
    if (changed)
        usb_hid_emit(h, EV_SYN, SYN_REPORT, 0);
    memcpy(h->previous, now, 8);
}

/* 048d:c102 report 1 contains modifiers, one reserved byte, six key usages
 * and a keyboard-usage bitmap. A key can appear in either representation.
 * The other report IDs carry consumer controls and are not keyboard state. */
static int usb_hid_ite_key_present(const uint8_t *report, unsigned usage)
{
    if (report[ITE_8910_BITMAP + usage / 8U] & (1U << (usage % 8U)))
        return 1;
    for (unsigned i = ITE_8910_ARRAY; i < ITE_8910_BITMAP; i++)
        if (report[i] == usage)
            return 1;
    return 0;
}

static int usb_hid_parse_ite_keyboard(usb_hid_dev_t *h)
{
    const uint8_t *now = h->report;
    const uint8_t *old = h->previous;
    if (now[0] != ITE_8910_REPORT_ID)
        return 0;
    int forwarded = 0;
    int changed = usb_hid_keyboard_modifiers(h, old[1], now[1]);
    for (unsigned usage = 4; usage < ARRAY_SIZE(hid_usage_to_linux); usage++) {
        if (!hid_usage_to_linux[usage])
            continue;
        int pressed = usb_hid_ite_key_present(now, usage);
        if (pressed == usb_hid_ite_key_present(old, usage))
            continue;
        usb_hid_emit(h, EV_KEY, hid_usage_to_linux[usage], pressed);
        if (pressed)
            forwarded |= usb_hid_console_key((uint8_t)usage, now[1]);
        changed = 1;
    }
    if (changed)
        usb_hid_emit(h, EV_SYN, SYN_REPORT, 0);
    memcpy(h->previous, now, ITE_8910_REPORT_LEN);
    return forwarded;
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

/* Completion callback.  It takes h->lock itself: the HCD calls it from its
 * interrupt handler, from the core's process-context poll, and from inside a
 * synchronous control/bulk wait on another endpoint, so no single caller can
 * be the one that already holds the lock.
 *
 * The documented order is h->lock -> xhci->lock (the re-arm below reaches the
 * HCD), and the HCD never holds its own lock while calling in here -- it drops
 * it before every callback.  See docs/drivers/guide/lock-order.md. */
static void usb_hid_complete(usb_urb_t *urb)
{
    usb_hid_dev_t *h = (usb_hid_dev_t *)urb->ctx;
    if (!h)
        return;
    uint64_t flags = spin_lock_irqsave(&h->lock);
    int running = h->running;
    int status = urb->status;
    int report_seen = running && h->ite_keyboard && !status && !h->report_logged;
    int transfer_failed = running && h->ite_keyboard && status &&
                          !h->transfer_error_logged;
    uint8_t report_id = h->report[0];
    if (report_seen)
        h->report_logged = 1;
    if (transfer_failed)
        h->transfer_error_logged = 1;
    uint16_t old_head = h->head;
    int first_console_input = 0;
    if (running && !status) {
        if (h->ite_keyboard) {
            int forwarded = usb_hid_parse_ite_keyboard(h);
            first_console_input = forwarded && !h->console_input_logged;
            if (first_console_input)
                h->console_input_logged = 1;
        } else if (h->kind == USB_HID_KBD)
            usb_hid_parse_keyboard(h);
        else if (h->kind == USB_HID_MOUSE)
            usb_hid_parse_mouse(h);
        else if (h->kind == USB_HID_TABLET)
            usb_hid_parse_tablet(h);
    }
    uint16_t code = h->first_key_code;
    int32_t value = h->first_key_value;
    int announce = h->key_pending;
    int queued = h->head != old_head;
    if (announce)
        h->key_logged = 1;
    h->key_pending = 0;
    spin_unlock_irqrestore(&h->lock, flags);
    /* Producers publish their queue before waking read/poll/epoll waiters.
     * Without this edge libinput can sleep forever after its initial probe:
     * an xHCI IRQ alone never wakes the evdev readiness source. */
    if (queued)
        input_mux_wake();
    if (report_seen)
        kinfo("[USB-HID] ITE first interrupt report: id=%u fbcon=%d\n",
              report_id, fbcon_active());
    if (transfer_failed)
        kwarn("[USB-HID] ITE interrupt transfer failed: %d\n", status);
    if (announce)
        kinfo("[USB-HID] key event: code=%u value=%d\n", code, value);
    if (first_console_input)
        kinfo("[USB-HID] ITE first console input forwarded\n");
    if (!running)
        return;
    /* Re-arm the periodic transfer outside h->lock: usb_submit_urb() reaches
     * the HCD, and nesting the controller lock inside the interface lock is
     * the one order this driver is allowed to take. */
    int r = usb_submit_urb(urb);
    if (r)
        kwarn("[USB-HID] interrupt rearm failed: %d\n", r);
}

static int usb_hid_read(device_t *dev, void *buffer, size_t count)
{
    usb_hid_dev_t *h = (usb_hid_dev_t *)dev->drv_priv;
    if (!h || count < sizeof(struct input_event))
        return -EINVAL;

    /* Drain the controller with h->lock released.  The HCD runs completion
     * callbacks, which take h->lock themselves; calling it under that lock
     * would be the reverse of the documented h->lock -> xhci->lock order. */
    (void)usb_device_poll(h->iface->dev);
    uint64_t flags = spin_lock_irqsave(&h->lock);
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
    (void)usb_device_poll(h->iface->dev);
    uint64_t flags = spin_lock_irqsave(&h->lock);
    int ready = h->tail != h->head;
    spin_unlock_irqrestore(&h->lock, flags);
    return ready;
}

static int usb_hid_led_event(usb_hid_dev_t *h, const struct input_event *event)
{
    if (h->kind != USB_HID_KBD || event->type != EV_LED || event->code > LED_SCROLLL)
        return -EOPNOTSUPP;
    mutex_lock(&h->led_lock);
    uint32_t leds = h->leds;
    if (event->value)
        leds |= 1U << event->code;
    else
        leds &= ~(1U << event->code);
    int result = 0;
    if (!h->leds_valid || leds != h->leds) {
        /* Boot protocol has no report ID; ITE's recorded descriptor declares
         * output report 1, five LED bits and three padding bits. */
        uint8_t report[2] = {h->ite_keyboard ? ITE_8910_REPORT_ID : (uint8_t)leds,
                             (uint8_t)leds};
        result = usb_control_msg(h->iface->dev, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                  0x09 /* SET_REPORT */, 0x0200 | (h->ite_keyboard ? 1 : 0),
                                  h->iface->interface_number, report,
                                  h->ite_keyboard ? 2 : 1);
        if (!result) {
            __atomic_store_n(&h->leds, leds, __ATOMIC_RELEASE);
            h->leds_valid = 1;
            kinfo("[USB-HID] %04x:%04x keyboard LEDs=%x report=%u\n",
                  h->iface->dev->vendor, h->iface->dev->product, (unsigned)leds,
                  h->ite_keyboard ? 1 : 0);
        } else {
            kwarn("[USB-HID] keyboard LED SET_REPORT failed: %d\n", result);
        }
    }
    mutex_unlock(&h->led_lock);
    return result;
}

static int usb_hid_ioctl(device_t *dev, unsigned long request, void *argument)
{
    if (!argument)
        return -ENOTTY;
    usb_hid_dev_t *h = dev ? dev->drv_priv : NULL;
    if (!h)
        return -ENODEV;
    if (request == INPUT_IOCTL_LED_EVENT)
        return usb_hid_led_event(h, argument);
    if (request == INPUT_IOCTL_GET_LEDS) {
        *(uint32_t *)argument = __atomic_load_n(&h->leds, __ATOMIC_ACQUIRE);
        return h->kind == USB_HID_KBD ? 0 : -EOPNOTSUPP;
    }
    if (request != INPUT_IOCTL_GET_INFO)
        return -ENOTTY;
    struct input_device_info *info = argument;
    memset(info, 0, sizeof(*info));
    strncpy(info->name, h->kind == USB_HID_KBD ? "USB HID keyboard" :
                       h->kind == USB_HID_MOUSE ? "USB HID mouse" : "USB HID tablet", sizeof(info->name) - 1);
    info->id.bustype = 3; /* BUS_USB */
    info->id.vendor = h->iface->dev->vendor;
    info->id.product = h->iface->dev->product;
    input_info_bit(info, EV_SYN, SYN_REPORT);
    if (h->kind == USB_HID_KBD) {
        for (unsigned i = LED_NUML; i <= LED_SCROLLL; i++)
            input_info_bit(info, EV_LED, i);
        for (unsigned i = 0; i < ARRAY_SIZE(hid_usage_to_linux); i++)
            if (hid_usage_to_linux[i])
                input_info_bit(info, EV_KEY, hid_usage_to_linux[i]);
        static const uint16_t modifiers[] = {29, 42, 56, 125, 97, 54, 100, 126};
        for (unsigned i = 0; i < ARRAY_SIZE(modifiers); i++)
            input_info_bit(info, EV_KEY, modifiers[i]);
    } else {
        for (unsigned button = BTN_LEFT; button <= BTN_MIDDLE; button++)
            input_info_bit(info, EV_KEY, button);
        input_info_bit(info, EV_REL, REL_WHEEL);
        if (h->kind == USB_HID_MOUSE) {
            input_info_bit(info, EV_REL, REL_X);
            input_info_bit(info, EV_REL, REL_Y);
        } else {
            info->properties[0] = 1; /* INPUT_PROP_POINTER */
            for (unsigned axis = ABS_X; axis <= ABS_Y; axis++) {
                input_info_bit(info, EV_ABS, axis);
                info->abs[axis].maximum = 32767;
            }
        }
    }
    return 0;
}

static const input_dev_ops_t usb_hid_ops = {
    .read = usb_hid_read,
    .ioctl = usb_hid_ioctl,
    .poll = usb_hid_poll,
};

static void usb_hid_select_protocol(usb_hid_dev_t *h)
{
    usb_device_t *udev = h->iface->dev;
    usb_interface_t *iface = h->iface;
    uint8_t kind = h->kind;
    int r;
    /* Firmware can leave a boot keyboard in boot protocol. ITE input and
     * its report-ID-prefixed LED output both require report protocol; select
     * it explicitly instead of inferring state from an interrupt packet. */
    if (kind == USB_HID_KBD || kind == USB_HID_MOUSE) {
        uint16_t protocol = h->ite_keyboard ? 1 : 0;
        r = usb_control_msg(udev, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                            USB_REQ_SET_PROTOCOL, protocol,
                            iface->interface_number, NULL, 0);
        if (r)
            kwarn("[USB-HID] %04x:%04x SET_PROTOCOL(%u) failed: %d\n",
                  udev->vendor, udev->product, protocol, r);
        if (h->ite_keyboard && !r) {
            uint8_t actual = 0xff;
            int check = usb_control_msg(udev,
                USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                0x03 /* GET_PROTOCOL */, 0, iface->interface_number,
                &actual, sizeof(actual));
            kinfo("[USB-HID] %04x:%04x report protocol requested=%u actual=%u rc=%d\n",
                  udev->vendor, udev->product, protocol, actual, check);
        }
    }
    if (!h->ite_keyboard)
        (void)usb_control_msg(udev, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                              USB_REQ_SET_IDLE, 0, iface->interface_number,
                              NULL, 0);
}

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
    mutex_init(&h->led_lock);
    h->ep = ep;
    h->kind = kind;
    h->ite_keyboard = kind == USB_HID_KBD &&
                      udev->vendor == ITE_USB_VENDOR && udev->product == ITE_8910_KBD &&
                      iface->interface_number == 0 && ep->max_packet >= ITE_8910_REPORT_LEN;
    h->report_size = (kind == USB_HID_MOUSE) ? 4 : 8;
    if (h->ite_keyboard)
        h->report_size = USB_HID_REPORT_MAX;
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
        kwarn("[USB-HID] %04x:%04x endpoint %02x configure failed: %d\n",
              udev->vendor, udev->product, ep->addr, r);
        kfree(h);
        return r;
    }

    usb_hid_select_protocol(h);

    dev->drv_priv = h;
    h->running = 1;
    r = usb_submit_urb(&h->urb);
    if (r) {
        kwarn("[USB-HID] %04x:%04x first interrupt submit failed: %d\n",
              udev->vendor, udev->product, r);
        h->running = 0;
        dev->drv_priv = NULL;
        kfree(h);
        return r;
    }

    kinfo("[USB-HID] %s ready: if=%u ep=%02x id=%04x:%04x mode=%s len=%u\n",
          kind == USB_HID_KBD ? "keyboard" :
          kind == USB_HID_MOUSE ? "mouse" : "tablet",
          iface->interface_number, ep->addr, udev->vendor, udev->product,
          h->ite_keyboard ? "report" : kind == USB_HID_TABLET ? "report" : "boot",
          h->report_size);
    return 0;
}

static int usb_hid_remove(device_t *dev)
{
    usb_hid_dev_t *h = (usb_hid_dev_t *)dev->drv_priv;
    if (h) {
        /* The HCD's transfer ring outlives this interface until abort_slot()
         * runs, and its interrupt handler may still be draining the ring in
         * that window.  Clearing complete/ctx first is what makes the stale
         * URB a no-op there instead of a callback into freed memory. */
        h->urb.complete = NULL;
        h->urb.ctx = NULL;
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
