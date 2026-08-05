#ifdef CONFIG_PS2_INPUT

#include "drivers/input/ps2.h"
#include "drivers/input/virtio_input.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_hwapi.h"
#include "fs/devfs.h"
#include "proc/proc.h"
#include "core/cpu.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "core/errno.h"

#define PS2_DATA_PORT       0x60
#define PS2_STATUS_PORT     0x64
#define PS2_CMD_PORT        0x64

#define PS2_STATUS_OUTPUT   0x01
#define PS2_STATUS_INPUT    0x02
#define PS2_STATUS_AUX      0x20

#define PS2_RING_SIZE       256

typedef struct {
    struct input_event ring[PS2_RING_SIZE];
    uint32_t head;
    uint32_t tail;
    spinlock_t lock;
    wait_queue_t waiters;
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

static int ps2_wait_input_clear(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT))
            return 0;
    }
    return -1;
}

static int ps2_wait_output_full(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT)
            return 0;
    }
    return -1;
}

static int ps2_write_cmd(uint8_t command) {
    if (ps2_wait_input_clear() != 0)
        return -1;
    outb(PS2_CMD_PORT, command);
    return 0;
}

static int ps2_write_data(uint8_t data) {
    if (ps2_wait_input_clear() != 0)
        return -1;
    outb(PS2_DATA_PORT, data);
    return 0;
}

static int ps2_read_data(uint8_t *data) {
    if (ps2_wait_output_full() != 0)
        return -1;
    *data = inb(PS2_DATA_PORT);
    return 0;
}

static int ps2_write_mouse(uint8_t data) {
    return ps2_write_cmd(0xd4) || ps2_write_data(data);
}

static void ps2_flush_output(void) {
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT)
        (void)inb(PS2_DATA_PORT);
}

static void ps2_push_event(uint16_t type, uint16_t code, int32_t value) {
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
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
        (void)wait_queue_collect_one(&g_ps2.waiters, 0,
                                     PROC_WAKE_EVENT, &wake_q);
    }
    spin_unlock_irqrestore(&g_ps2.lock, flags);
    (void)proc_wake_q_flush(&wake_q);
}

static uint16_t ps2_extended_keycode(uint8_t scancode) {
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

static void ps2_handle_keyboard_byte(uint8_t byte) {
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
    if (keycode)
        ps2_push_event(EV_KEY, keycode, released ? 0 : 1);

    if (!released && !extended) {
        char c = (g_ps2.left_shift || g_ps2.right_shift) ?
                 ps2_shift_keymap[scancode] : ps2_keymap[scancode];
        if (c)
            uart_receive_char(c);
    }
}

static void ps2_handle_mouse_byte(uint8_t byte) {
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
}

void ps2_input_handle_irq(void) {
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT) {
        uint8_t status = inb(PS2_STATUS_PORT);
        uint8_t byte = inb(PS2_DATA_PORT);
        if (!g_ps2.initialized)
            continue;
        if (status & PS2_STATUS_AUX)
            ps2_handle_mouse_byte(byte);
        else
            ps2_handle_keyboard_byte(byte);
    }
}

static int ps2_input_read(vfile_t *vf, char *buf, size_t count) {
    if (count < sizeof(struct input_event))
        return -EINVAL;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_ps2.lock);
        if (g_ps2.head != g_ps2.tail) {
            size_t copied = 0;
            while (g_ps2.head != g_ps2.tail &&
                   copied + sizeof(struct input_event) <= count) {
                memcpy(buf + copied, &g_ps2.ring[g_ps2.tail],
                       sizeof(struct input_event));
                g_ps2.tail = (g_ps2.tail + 1) % PS2_RING_SIZE;
                copied += sizeof(struct input_event);
            }
            spin_unlock_irqrestore(&g_ps2.lock, flags);
            return (int)copied;
        }
        if (vf->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&g_ps2.lock, flags);
            return -EAGAIN;
        }
        spin_unlock_irqrestore(&g_ps2.lock, flags);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&g_ps2.lock);
        if (g_ps2.head != g_ps2.tail) {
            spin_unlock_irqrestore(&g_ps2.lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        bool linked = wait_queue_link(&g_ps2.waiters, &entry, token, 0);
        spin_unlock_irqrestore(&g_ps2.lock, flags);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&g_ps2.waiters, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -ERESTARTSYS;
    }
}

static int ps2_input_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    (void)vf;
    (void)req;
    (void)arg;
    return -ENOSYS;
}

/* VirtIO owns /dev/event0 when present.  Keep PS/2's operations private so
 * the two input transports can be linked into the same x86_64 image. */
static vfile_ops_t g_devfs_ps2_input_ops __attribute__((unused)) = {
    .read = ps2_input_read,
    .ioctl = ps2_input_ioctl,
};

int ps2_input_init(void) {
    uint8_t config;
    uint8_t response;

    memset(&g_ps2, 0, sizeof(g_ps2));
    spin_init(&g_ps2.lock);
    wait_queue_init(&g_ps2.waiters);

    if (ps2_write_cmd(0xad) || ps2_write_cmd(0xa7))
        return -1;
    ps2_flush_output();

    if (ps2_write_cmd(0x20) || ps2_read_data(&config))
        return -1;
    config &= (uint8_t)~0x03;
    if (ps2_write_cmd(0x60) || ps2_write_data(config))
        return -1;

    if (ps2_write_cmd(0xaa) || ps2_read_data(&response) || response != 0x55)
        return -1;
    if (ps2_write_cmd(0xab) || ps2_read_data(&response) || response != 0x00)
        return -1;
    if (ps2_write_cmd(0xa9) || ps2_read_data(&response) || response != 0x00)
        return -1;

    if (ps2_write_cmd(0xae) || ps2_write_cmd(0xa8))
        return -1;
    if (ps2_write_data(0xff) || ps2_read_data(&response) || response != 0xfa ||
        ps2_read_data(&response) || response != 0xaa)
        return -1;
    ps2_flush_output(); /* Optional keyboard ID bytes follow the reset result. */
    if (ps2_write_mouse(0xff) || ps2_read_data(&response) || response != 0xfa ||
        ps2_read_data(&response) || response != 0xaa ||
        ps2_read_data(&response))
        return -1;
    if (ps2_write_data(0xf4) || ps2_read_data(&response) || response != 0xfa ||
        ps2_write_mouse(0xf4) || ps2_read_data(&response) || response != 0xfa)
        return -1;

    if (ps2_write_cmd(0x20) || ps2_read_data(&config))
        return -1;
    config |= 0x43;                 /* both IRQs and set-2 to set-1 translation */
    config &= (uint8_t)~0x30;       /* enable both port clocks */
    if (ps2_write_cmd(0x60) || ps2_write_data(config))
        return -1;

    g_ps2.initialized = 1;
    return 0;
}

#endif /* CONFIG_PS2_INPUT */
