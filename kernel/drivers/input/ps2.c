/* Shared PS2 device implementation; module packaging is separate. */
#include "drivers/core/driver_register.h"
#include "drivers/input/virtio_input.h"
#include "drivers/char/uart.h"
#include "drivers/bus/platform_bus.h"
#include "firmware/irq.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "core/input.h"
#include "drivers/input/evdev.h"
#include "proc/park.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/errno.h"

extern void input_mux_wake(void);

#define PS2_DATA_PORT       0x60
#define PS2_STATUS_PORT     0x64
#define PS2_CMD_PORT        0x64

#define PS2_STATUS_OUTPUT   0x01
#define PS2_STATUS_INPUT    0x02
#define PS2_STATUS_AUX      0x20

#define PS2_RING_SIZE       256

#define IRQ_VECTOR_KEYBOARD  0x21
#define PS2_MOUSE_IRQ_VECTOR 0x2c

typedef struct {
    struct input_event ring[PS2_RING_SIZE];
    uint32_t head;
    uint32_t tail;
    spinlock_t lock;
    uint8_t initialized;
    uint8_t extended;
    uint8_t pause_bytes;
    uint8_t left_shift;
    uint8_t right_shift;
    uint8_t mouse_packet[3];
    uint8_t mouse_count;
    uint8_t mouse_buttons;
} ps2_input_t;

static ps2_input_t g_ps2;
static int g_ps2_kbd_irq = -1, g_ps2_mouse_irq = -1;
static device_t *g_ps2_owner;
static int g_ps2_closing, g_ps2_hardware_owned, g_ps2_stop_complete;

static const char ps2_keymap[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0a] = '9', [0x0b] = '0', [0x0c] = '-', [0x0d] = '=',
    [0x0e] = 0x7f, [0x0f] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1a] = '[', [0x1b] = ']',
    [0x1c] = '\n',
    [0x1e] = 'a', [0x1f] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2b] = '\\',
    [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x39] = ' ',
};

static const char ps2_shift_keymap[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0a] = '(', [0x0b] = ')', [0x0c] = '_', [0x0d] = '+',
    [0x0e] = 0x7f, [0x0f] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1a] = '{', [0x1b] = '}',
    [0x1c] = '\n',
    [0x1e] = 'A', [0x1f] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2b] = '|',
    [0x2c] = 'Z', [0x2d] = 'X', [0x2e] = 'C', [0x2f] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
    [0x34] = '>', [0x35] = '?', [0x39] = ' ',
};

/* Both boot draining and IRQ dispatch must terminate on a stuck/lost port. */
static int ps2_flush_output(void)
{
    for (unsigned i = 0; i < PS2_RING_SIZE; i++) {
        uint8_t status = ioport_read8(PS2_STATUS_PORT);
        if (status == 0xff)
            return -ENODEV;
        if (!(status & PS2_STATUS_OUTPUT))
            return 0;
        (void)ioport_read8(PS2_DATA_PORT);
    }
    return -ETIMEDOUT;
}

static void ps2_push_event(uint16_t type, uint16_t code, int32_t value)
{
    uint64_t flags = spin_lock_irqsave(&g_ps2.lock);
    uint32_t next = (g_ps2.head + 1) % PS2_RING_SIZE;
    if (next != g_ps2.tail) {
        struct input_event *event = &g_ps2.ring[g_ps2.head];
        event->time_sec = 0;
        event->time_usec = 0;
        event->type = type;
        event->code = code;
        event->value = value;
        g_ps2.head = next;
    }
    spin_unlock_irqrestore(&g_ps2.lock, flags);
    input_mux_wake();
}

static uint16_t ps2_extended_keycode(uint8_t scancode)
{
    switch (scancode) {
    case 0x1c: return 96;  /* KEY_KPENTER */
    case 0x1d: return 97;  /* KEY_RIGHTCTRL */
    case 0x35: return 98;  /* KEY_KPSLASH */
    case 0x38: return 100; /* KEY_RIGHTALT */
    case 0x47: return 102; /* KEY_HOME */
    case 0x48: return 103; /* KEY_UP */
    case 0x49: return 104; /* KEY_PAGEUP */
    case 0x4b: return 105; /* KEY_LEFT */
    case 0x4d: return 106; /* KEY_RIGHT */
    case 0x4f: return 107; /* KEY_END */
    case 0x50: return 108; /* KEY_DOWN */
    case 0x51: return 109; /* KEY_PAGEDOWN */
    case 0x52: return 110; /* KEY_INSERT */
    case 0x53: return 111; /* KEY_DELETE */
    default: return 0;
    }
}

static void ps2_handle_keyboard_byte(uint8_t byte)
{
    if (g_ps2.pause_bytes) {
        g_ps2.pause_bytes--;
        return;
    }
    if (byte == 0xe1) {
        g_ps2.pause_bytes = 5;
        return;
    }
    if (byte == 0xe0) {
        g_ps2.extended = 1;
        return;
    }

    uint8_t released = byte & 0x80;
    uint8_t scancode = byte & 0x7f;
    uint8_t extended = g_ps2.extended;
    g_ps2.extended = 0;

    if (!extended && scancode == 0x2a)
        g_ps2.left_shift = !released;
    else if (!extended && scancode == 0x36)
        g_ps2.right_shift = !released;

    uint16_t keycode = extended ? ps2_extended_keycode(scancode) : scancode;
    if (keycode) {
        ps2_push_event(EV_KEY, keycode, released ? 0 : 1);
        ps2_push_event(EV_SYN, SYN_REPORT, 0);
    }

    if (!released && !extended) {
        char c = (g_ps2.left_shift || g_ps2.right_shift) ?
                 ps2_shift_keymap[scancode] : ps2_keymap[scancode];
        if (c)
            uart_receive_char(c);
    }
}

static void ps2_handle_mouse_byte(uint8_t byte)
{
    if (g_ps2.mouse_count == 0 && !(byte & 0x08))
        return;
    g_ps2.mouse_packet[g_ps2.mouse_count++] = byte;
    if (g_ps2.mouse_count != 3)
        return;

    uint8_t status = g_ps2.mouse_packet[0];
    g_ps2.mouse_count = 0;
    if (status & 0xc0)
        return;

    int32_t dx = (status & 0x10) ? (int32_t)g_ps2.mouse_packet[1] - 256 :
                                  g_ps2.mouse_packet[1];
    int32_t dy = (status & 0x20) ? (int32_t)g_ps2.mouse_packet[2] - 256 :
                                  g_ps2.mouse_packet[2];
    if (dx)
        ps2_push_event(EV_REL, 0, dx);       /* REL_X */
    if (dy)
        ps2_push_event(EV_REL, 1, -dy);      /* REL_Y is screen-down */

    uint8_t buttons = status & 0x07;
    uint8_t changed = buttons ^ g_ps2.mouse_buttons;
    if (changed & 0x01)
        ps2_push_event(EV_KEY, 0x110, buttons & 0x01); /* BTN_LEFT */
    if (changed & 0x02)
        ps2_push_event(EV_KEY, 0x111, (buttons >> 1) & 0x01); /* BTN_RIGHT */
    if (changed & 0x04)
        ps2_push_event(EV_KEY, 0x112, (buttons >> 2) & 0x01); /* BTN_MIDDLE */
    g_ps2.mouse_buttons = buttons;
    ps2_push_event(EV_SYN, SYN_REPORT, 0);
}

static void ps2_handle_irq(void)
{
    for (unsigned i = 0; i < PS2_RING_SIZE; i++) {
        if (__atomic_load_n(&g_ps2_closing, __ATOMIC_ACQUIRE))
            break;
        uint8_t status = ioport_read8(PS2_STATUS_PORT);
        if (status == 0xff || !(status & PS2_STATUS_OUTPUT))
            break;
        uint8_t byte = ioport_read8(PS2_DATA_PORT);
        if (!g_ps2.initialized)
            continue;
        if (status & PS2_STATUS_AUX)
            ps2_handle_mouse_byte(byte);
        else
            ps2_handle_keyboard_byte(byte);
    }
}

static int ps2_isr(int irq, void *ctx)
{
    (void)irq;
    (void)ctx;
    ps2_handle_irq();
    return 1;
}

static int ps2_write_cmd(uint8_t cmd)
{
    int tries = 10000;
    while (tries-- > 0) {
        uint8_t status = ioport_read8(PS2_STATUS_PORT);
        if (status == 0xff)
            return -ENODEV;
        if (!(status & PS2_STATUS_INPUT)) {
            ioport_write8(PS2_CMD_PORT, cmd);
            return 0;
        }
        udelay(20);
    }
    return -ETIMEDOUT;
}

static int ps2_write_data(uint8_t data)
{
    int tries = 10000;
    while (tries-- > 0) {
        uint8_t status = ioport_read8(PS2_STATUS_PORT);
        if (status == 0xff)
            return -ENODEV;
        if (!(status & PS2_STATUS_INPUT)) {
            ioport_write8(PS2_DATA_PORT, data);
            return 0;
        }
        udelay(20);
    }
    return -ETIMEDOUT;
}

static int ps2_write_mouse(uint8_t data)
{
    int ret = ps2_write_cmd(0xd4);
    return ret ? ret : ps2_write_data(data);
}

static int ps2_read_data(uint8_t *out)
{
    int tries = 10000;
    while (tries-- > 0) {
        uint8_t status = ioport_read8(PS2_STATUS_PORT);
        if (status == 0xff)
            return -ENODEV;
        if (status & PS2_STATUS_OUTPUT) {
            *out = ioport_read8(PS2_DATA_PORT);
            return 0;
        }
        udelay(20);
    }
    return -ETIMEDOUT;
}

static int __attribute__((unused)) ps2_wait_input_clear(void)
{
    int tries = 10000;
    while (tries-- > 0) {
        if (!(ioport_read8(PS2_STATUS_PORT) & PS2_STATUS_INPUT))
            return 0;
        udelay(20);
    }
    return -1;
}

static int ps2_init_hw(void)
{
    uint8_t config = 0x45;
    uint8_t response;
    int have_config = 0;
    int selftest = -1;
    int kbd_reset = -1;
    int mouse_reset = -1;
    int ret;

    memset(&g_ps2, 0, sizeof(g_ps2));
    spin_init(&g_ps2.lock);

    /* A machine without an i8042 decodes 0xff on both ports.  Do not spend
     * the per-command timeout on every step just to discover that the
     * keyboard is I2C-HID. */
    if (ioport_read8(PS2_STATUS_PORT) == 0xff)
        return -ENODEV;

    (void)ps2_write_cmd(0xad);          /* keyboard off */
    (void)ps2_write_cmd(0xa7);          /* mouse off */
    if ((ret = ps2_flush_output()))
        return ret;

    if (ps2_write_cmd(0x20) == 0 && ps2_read_data(&config) == 0)
        have_config = 1;
    else
        config = 0x45;                  /* translation on, both IRQs off */

    config &= (uint8_t)~0x03;           /* IRQs off while programming */
    config |= 0x30;                     /* both ports disabled */
    if ((ret = ps2_write_cmd(0x60)) || (ret = ps2_write_data(config)))
        return ret;

    /* The self-test is advisory.  Laptop ECs are known to return 0xfc/0xfd
     * or drop the response; that does not mean the keyboard cannot deliver
     * keys.  The interface tests are likewise best effort. */
    if (ps2_write_cmd(0xaa) == 0 && ps2_read_data(&response) == 0)
        selftest = (response == 0x55) ? 1 : 0;
    if ((ret = ps2_flush_output()))
        return ret;
    (void)ps2_write_cmd(0xab);
    (void)ps2_read_data(&response);
    if ((ret = ps2_flush_output()))
        return ret;
    (void)ps2_write_cmd(0xa9);
    (void)ps2_read_data(&response);
    if ((ret = ps2_flush_output()))
        return ret;

    (void)ps2_write_cmd(0xae);          /* keyboard port on */
    (void)ps2_write_cmd(0xa8);          /* mouse port on (may be absent) */

    /* Final controller config before scanning is enabled: IRQs on,
     * translation on, both clocks and ports enabled.  Clear the disable bits
     * explicitly: 0xae/0xa8 are not enough on controllers that keep the old
     * config byte.  Doing this before 0xf4 means a key press cannot splice a
     * scancode into the config response. */
    if (ps2_write_cmd(0x20) == 0 && ps2_read_data(&config) == 0) {
        config &= (uint8_t)~0x30;       /* KBDDIS/AUXDIS; preserve other control bits */
        config |= 0x43;                 /* KBDINT/AUXINT, set-2 -> set-1 */
        (void)ps2_write_cmd(0x60);
        (void)ps2_write_data(config);
    }
    if ((ret = ps2_flush_output()))
        return ret;

    /* Keyboard reset and enable scanning.  This is the device the driver
     * exists for.  A failed reset is recorded, but 0xf4 is still attempted
     * because several ECs answer 0xf4 even when they drop the 0xff batch. */
    if (ps2_write_data(0xff) == 0) {
        uint8_t ack = 0, bat = 0;
        if (ps2_read_data(&ack) == 0 && ps2_read_data(&bat) == 0 &&
            ack == 0xfa && bat == 0xaa)
            kbd_reset = 1;
        else
            kbd_reset = 0;
    }
    if ((ret = ps2_flush_output()))
        return ret;
    if (ps2_write_data(0xf4) == 0) {
        if (ps2_read_data(&response) == 0 && response == 0xfa)
            kbd_reset = 1;
    }
    if ((ret = ps2_flush_output()))
        return ret;

    /* Optional mouse.  A missing AUX device used to fail the whole probe --
     * exactly wrong on a laptop whose pointing device is I2C-HID: the
     * keyboard was fully functional but never registered. */
    if (ps2_write_mouse(0xff) == 0) {
        uint8_t ack = 0, bat = 0;
        if (ps2_read_data(&ack) == 0 && ps2_read_data(&bat) == 0 &&
            ack == 0xfa && bat == 0xaa)
            mouse_reset = 1;
        else
            mouse_reset = 0;
        if ((ret = ps2_flush_output()))
            return ret;
        (void)ps2_write_mouse(0xf4);
        (void)ps2_read_data(&response);
    }
    if ((ret = ps2_flush_output()))
        return ret;

    g_ps2.initialized = 1;
    kinfo("[PS2] init: cfg=%s0x%02x selftest=%d kbd_reset=%d mouse=%d\n",
            have_config ? "" : "default ", config, selftest, kbd_reset,
            mouse_reset);
    return 0;
}

static int ps2_stop(void *context)
{
    device_t *dev = context;
    if (g_ps2_owner != dev)
        return -ENODEV;
    __atomic_store_n(&g_ps2_closing, 1, __ATOMIC_RELEASE);
    /* Handlers stop reading immediately; drain accepted dispatches before
     * using the shared command/data ports for controller replies. */
    if (g_ps2_kbd_irq >= 0)
        irq_disable((uint32_t)g_ps2_kbd_irq);
    if (g_ps2_mouse_irq >= 0)
        irq_disable((uint32_t)g_ps2_mouse_irq);
    int ret;
    if (g_ps2_kbd_irq >= 0 && (ret = synchronize_irq((uint32_t)g_ps2_kbd_irq)))
        return ret;
    if (g_ps2_mouse_irq >= 0 && (ret = synchronize_irq((uint32_t)g_ps2_mouse_irq)))
        return ret;
    if (g_ps2_hardware_owned) {
        /* Stop clocks before flushing so a scancode cannot become the
         * configuration reply. Preserve translation/system/lock policy. */
        uint8_t config, verify;
        if ((ret = ps2_write_cmd(0xad)) || (ret = ps2_write_cmd(0xa7)) ||
            (ret = ps2_flush_output()) || (ret = ps2_write_cmd(0x20)) ||
            (ret = ps2_read_data(&config)))
            return ret;
        config = (config & (uint8_t)~0x03) | 0x30;
        if ((ret = ps2_write_cmd(0x60)) || (ret = ps2_write_data(config)) ||
            (ret = ps2_write_cmd(0x20)) || (ret = ps2_read_data(&verify)))
            return ret;
        if ((verify & 0x33) != 0x30)
            return -EIO;
        if ((ret = ps2_flush_output()))
            return ret;
        kinfo("[PS2] controller stopped: config=0x%02x\n", verify);
    }
    g_ps2_stop_complete = 1;
    return 0;
}

static void ps2_release(void *context)
{
    if (g_ps2_owner != context)
        return;
    memset(&g_ps2, 0, sizeof(g_ps2));
    g_ps2_owner = NULL;
    g_ps2_hardware_owned = g_ps2_stop_complete = 0;
}

static int ps2_probe(device_t *dev)
{
    if (g_ps2_owner)
        return -EBUSY;
    /* Use the declared fixed ports, never claim an undeclared ISA range. */
    resource_t *data = NULL, *command = NULL, *res;
    for (int i = 0; (res = device_get_resource(dev, RES_IOPORT, i)); i++) {
        if (res->start <= PS2_DATA_PORT && res->end >= PS2_DATA_PORT)
            data = res;
        if (res->start <= PS2_CMD_PORT && res->end >= PS2_CMD_PORT)
            command = res;
    }
    if (!data || !command)
        return -ENODEV;
    int ret = device_probe_use_remove(dev);
    if (ret)
        return ret;
    g_ps2_owner = dev;
    g_ps2_hardware_owned = g_ps2_stop_complete = 0;
    __atomic_store_n(&g_ps2_closing, 0, __ATOMIC_RELEASE);
    dev->drv_priv = &g_ps2;
    ret = device_add_execution_action(dev, ps2_stop, ps2_release, dev);
    if (ret) {
        g_ps2_owner = NULL;
        dev->drv_priv = NULL;
        return ret;
    }
    if ((ret = device_claim_resource(dev, data)) ||
        (command != data && (ret = device_claim_resource(dev, command))))
        return ret;
    if (ioport_read8(PS2_STATUS_PORT) == 0xff)
        return -ENODEV;
    g_ps2_hardware_owned = 1;
    ret = ps2_init_hw();
    if (ret) {
        kinfo("[PS2] controller init failed; no PS/2 keyboard\n");
        return ret;
    }

    int kbd_vector = firmware_legacy_irq_map(1, IRQ_VECTOR_KEYBOARD);
    int mouse_vector = firmware_legacy_irq_map(12, PS2_MOUSE_IRQ_VECTOR);
    if (kbd_vector < 0)
        return kbd_vector;
    ret = request_irq((uint32_t)kbd_vector, ps2_isr, 0, dev);
    if (ret)
        return ret;
    g_ps2_kbd_irq = kbd_vector;
    if (mouse_vector >= 0 && !request_irq((uint32_t)mouse_vector, ps2_isr, 0, dev))
        g_ps2_mouse_irq = mouse_vector;
    kinfo("[PS2] input ready (kbd irq=%d mouse irq=%d)\n",
          g_ps2_kbd_irq, g_ps2_mouse_irq);
    return 0;
}

static int ps2_remove(device_t *dev)
{
    if (g_ps2_owner != dev)
        return 0;
    if (!g_ps2_stop_complete)
        return -EBUSY;
    if (g_ps2_kbd_irq >= 0)
        free_irq((uint32_t)g_ps2_kbd_irq, dev);
    if (g_ps2_mouse_irq >= 0)
        free_irq((uint32_t)g_ps2_mouse_irq, dev);
    g_ps2_kbd_irq = g_ps2_mouse_irq = -1;
    dev->drv_priv = NULL;
    return 0;
}

static void ps2_driver_shutdown(device_t *dev)
{
    int ret = ps2_stop(dev);
    if (ret)
        kerr("[PS2] legacy shutdown failed: %d\n", ret);
}


/* Publish the ring as an input class device so /dev/event0 (input_mux)
 * drains it: QEMU's monitor/VNC input targets the PS/2 controller by
 * default, which is the reliable injection path on x86_64. */
static int ps2_class_read(device_t *dev, void *buf, size_t count)
{
    (void)dev;
    if (!buf || count < sizeof(struct input_event))
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_ps2.lock);
    size_t copied = 0;
    while (g_ps2.head != g_ps2.tail &&
           copied + sizeof(struct input_event) <= count) {
        *(struct input_event *)((char *)buf + copied) = g_ps2.ring[g_ps2.tail];
        g_ps2.tail = (g_ps2.tail + 1) % PS2_RING_SIZE;
        copied += sizeof(struct input_event);
    }
    spin_unlock_irqrestore(&g_ps2.lock, flags);
    return copied ? (int)copied : -EAGAIN;
}

static int ps2_class_poll(device_t *dev, short events)
{
    (void)dev;
    (void)events;
    uint64_t flags = spin_lock_irqsave(&g_ps2.lock);
    int ready = g_ps2.head != g_ps2.tail;
    spin_unlock_irqrestore(&g_ps2.lock, flags);
    return ready;
}

static int ps2_class_ioctl(device_t *dev, unsigned long req, void *arg)
{
    (void)dev;
    if (req != INPUT_IOCTL_GET_INFO || !arg)
        return -ENOTTY;
    struct input_device_info *info = arg;
    memset(info, 0, sizeof(*info));
    strncpy(info->name, "PS/2 keyboard and mouse", sizeof(info->name) - 1);
    info->id.bustype = 0x11; /* BUS_I8042 */
    input_info_bit(info, EV_SYN, SYN_REPORT);
    for (unsigned key = 1; key < 128; key++)
        input_info_bit(info, EV_KEY, key);
    for (unsigned key = 0x110; key <= 0x112; key++)
        input_info_bit(info, EV_KEY, key);
    input_info_bit(info, EV_REL, REL_X);
    input_info_bit(info, EV_REL, REL_Y);
    return 0;
}

static const input_dev_ops_t ps2_class_ops = {
    .read  = ps2_class_read,
    .poll  = ps2_class_poll,
    .ioctl = ps2_class_ioctl,
};

static const firmware_device_id_t firmware_ids[] = {
    {.id = "PNP0303"}, {0}
};
static driver_t ps2_driver = {
    .name = "ps2",
    .firmware_ids = firmware_ids,
    .bus = &platform_bus,
    .probe = ps2_probe,
    .remove = ps2_remove,
    .shutdown = ps2_driver_shutdown,
    .class_ops = &ps2_class_ops,
    .class_type = DEV_CLASS_INPUT,
};

DRIVER_REGISTER(ps2_driver);
