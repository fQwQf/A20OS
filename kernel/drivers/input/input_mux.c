/*
 * A20OS — input multiplexer (devfs service for /dev/event0).
 *
 * Split from kernel/drivers/input/virtio_input.c (removed): the input
 * DRIVERS now run as drvmod modules (vinput.drv) and publish input class
 * devices through the unified driver core; this file keeps the
 * transport-independent evdev service — the /dev/event0 vnode, the
 * EVIOCG* ioctl surface, the key/ABS state trackers and the read
 * waiters — and consumes input class devices via their input_dev_ops.
 *
 * Wake path: the mux parks on g_input_waiters; driver ISRs (module code)
 * call input_mux_wake() through the framework export after enqueuing
 * events.  The class poll() hooks keep the GUI responsive on platforms
 * whose virtio IRQ route is masked or unavailable.
 */

#include "drivers/input/virtio_input.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "fs/devfs.h"
#include "fs/vfs.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/timer.h"
#include "core/errno.h"
#include "core/input.h"
#include "core/poll.h"
#include "sys/usercopy.h"

#define EV_MUX_KEY_STATE_BYTES ((KEY_MAX + 8) / 8)

static spinlock_t g_mux_state_lock = SPINLOCK_INIT;
static uint8_t g_mux_key_state[EV_MUX_KEY_STATE_BYTES];
static int32_t g_mux_abs_value[2];

/*
 * Track pressed keys and absolute axis values for EVIOCGKEY/EVIOCGABS.
 * Called for every event consumed from the mux, from both IRQ and
 * read-side paths.
 */
static void evdev_mux_track_event(uint16_t type, uint16_t code, int32_t value) {
    if (type == EV_KEY && code <= KEY_MAX) {
        uint64_t flags = spin_lock_irqsave(&g_mux_state_lock);
        if (value)
            g_mux_key_state[code >> 3] |= (uint8_t)(1U << (code & 7));
        else
            g_mux_key_state[code >> 3] &= (uint8_t)~(1U << (code & 7));
        spin_unlock_irqrestore(&g_mux_state_lock, flags);
    } else if (type == EV_ABS && code <= ABS_Y) {
        uint64_t flags = spin_lock_irqsave(&g_mux_state_lock);
        g_mux_abs_value[code] = value;
        spin_unlock_irqrestore(&g_mux_state_lock, flags);
    }
}

static void evdev_mux_track_buffer(const char *buf, size_t copied) {
    for (size_t off = 0; off + sizeof(struct input_event) <= copied;
         off += sizeof(struct input_event)) {
        const struct input_event *ev = (const struct input_event *)(buf + off);
        evdev_mux_track_event(ev->type, ev->code, ev->value);
    }
}

static wait_queue_t g_input_waiters;

/* Woken by input driver ISRs (module code) after enqueueing events. */
void input_mux_wake(void)
{
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    (void)wait_queue_collect_one(&g_input_waiters, 0,
                                 PROC_WAKE_EVENT, &wake_q);
    (void)proc_wake_q_flush(&wake_q);
}

static int input_read_class_devices(char *buf, size_t count) {
    size_t copied = 0;
    for (int index = 0; copied + sizeof(struct input_event) <= count; index++) {
        device_t *dev = device_find_by_class(DEV_CLASS_INPUT, index);
        if (!dev)
            break;
        const input_dev_ops_t *ops = dev->drv ? dev->drv->class_ops : NULL;
        if (!ops || !ops->read)
            continue;
        int result = ops->read(dev, buf + copied, count - copied);
        if (result > 0)
            copied += (size_t)result;
    }
    return copied ? (int)copied : -EAGAIN;
}

/* Drain every input class device (the poll hooks service their queues
 * directly; used as the polling fallback when IRQ routing is masked). */
static void input_class_poll_all(void) {
    for (int index = 0;; index++) {
        device_t *dev = device_find_by_class(DEV_CLASS_INPUT, index);
        if (!dev)
            break;
        const input_dev_ops_t *ops = dev->drv ? dev->drv->class_ops : NULL;
        if (ops && ops->poll)
            (void)ops->poll(dev, POLLIN);
    }
}

static int input_read(vfile_t *vf, char *buf, size_t count) {
    if (count < sizeof(struct input_event)) return -EINVAL;

    while (1) {
        /* /dev/event0 is a transport-independent evdev multiplexer.  This
         * includes VBox's xHCI HID controller as well as PCI/MMIO
         * virtio-input devices driven by the vinput module. */
        int class_result = input_read_class_devices(buf, count);
        if (class_result > 0) {
            evdev_mux_track_buffer(buf, (size_t)class_result);
            return class_result;
        }

        /* Keep the GUI responsive on QEMU variants whose virtio IRQ route is
         * masked or unavailable even though the queue itself is operational. */
        input_class_poll_all();
        class_result = input_read_class_devices(buf, count);
        if (class_result > 0) {
            evdev_mux_track_buffer(buf, (size_t)class_result);
            return class_result;
        }

        if (vf->flags & O_NONBLOCK)
            return -EAGAIN;

        if (!class_device_get_by_type(DEV_CLASS_INPUT, 0))
            return -EAGAIN;

        /*
         * Park on the mux waiters; driver ISRs wake the mux via
         * input_mux_wake() after events land in their class rings.
         */
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        if (!token.task)
            return -EAGAIN;
        wait_queue_entry_t entry = {0};
        bool linked = wait_queue_link(&g_input_waiters, &entry, token, 0);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&g_input_waiters, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -ERESTARTSYS;
    }
}

static int input_poll(vfile_t *vf, short events) {
    (void)vf;
    if (!(events & POLLIN))
        return 0;

    input_class_poll_all();
    for (int index = 0;; index++) {
        device_t *dev = device_find_by_class(DEV_CLASS_INPUT, index);
        if (!dev)
            break;
        const input_dev_ops_t *ops = dev->drv ? dev->drv->class_ops : NULL;
        if (ops && ops->poll && ops->poll(dev, POLLIN) > 0)
            return POLLIN;
    }
    return 0;
}

static void evdev_bit_set(uint8_t *bits, uint32_t bit) {
    bits[bit >> 3] |= (uint8_t)(1U << (bit & 7));
}

static int evdev_copy_zeros(void *arg, uint32_t len) {
    uint8_t zeros[96];
    memset(zeros, 0, sizeof(zeros));
    while (len) {
        uint32_t chunk = len < sizeof(zeros) ? len : (uint32_t)sizeof(zeros);
        if (copy_to_user(arg, zeros, chunk) < 0)
            return -EFAULT;
        arg = (char *)arg + chunk;
        len -= chunk;
    }
    return 0;
}

static int evdev_bit_ioctl(uint32_t ev, uint32_t len, void *arg) {
    uint8_t bits[EV_MUX_KEY_STATE_BYTES];
    memset(bits, 0, sizeof(bits));
    switch (ev) {
    case 0: /* EVIOCGBIT(0) and EVIOCGBIT(EV_SYN) are the same request:
             * the supported event type bitmap (as on Linux). */
        evdev_bit_set(bits, EV_SYN);
        evdev_bit_set(bits, EV_KEY);
        evdev_bit_set(bits, EV_REL);
        evdev_bit_set(bits, EV_ABS);
        break;
    case EV_KEY:
        /* The mux aggregates keyboards and pointer buttons across
         * transports; advertise a superset. */
        for (uint32_t k = 1; k <= 0xff; k++)
            evdev_bit_set(bits, k);
        for (uint32_t k = BTN_MOUSE_FIRST; k <= BTN_MOUSE_LAST; k++)
            evdev_bit_set(bits, k);
        evdev_bit_set(bits, BTN_TOUCH);
        break;
    case EV_REL:
        evdev_bit_set(bits, REL_X);
        evdev_bit_set(bits, REL_Y);
        evdev_bit_set(bits, REL_WHEEL);
        evdev_bit_set(bits, REL_HWHEEL);
        break;
    case EV_ABS:
        evdev_bit_set(bits, ABS_X);
        evdev_bit_set(bits, ABS_Y);
        break;
    default:
        break; /* empty bitmap */
    }
    uint32_t n = len < sizeof(bits) ? len : (uint32_t)sizeof(bits);
    if (n && copy_to_user(arg, bits, n) < 0)
        return -EFAULT;
    if (len > n)
        return evdev_copy_zeros((char *)arg + n, len - n);
    return 0;
}

static int input_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    (void)vf;
    if (A20_IOC_TYPE(req) != 'E')
        return -EINVAL;
    uint32_t nr = A20_IOC_NR(req);
    uint32_t len = A20_IOC_SIZE(req);

    if (req == EVIOCGVERSION) {
        int version = EV_VERSION;
        return copy_to_user(arg, &version, sizeof(version)) < 0 ? -EFAULT : 0;
    }
    if (req == EVIOCGID) {
        struct input_id id = { BUS_VIRTUAL, 0, 0, 0 };
        return copy_to_user(arg, &id, sizeof(id)) < 0 ? -EFAULT : 0;
    }
    if (req == EVIOCGRAB || req == EVIOCREVOKE) {
        /* The mux has a single implicit client; grabbing is a no-op. */
        return 0;
    }
    if (req == EVIOCSCLOCKID)
        return 0;
    if (req == EVIOCGCLOCKID) {
        int clockid = 1; /* CLOCK_MONOTONIC */
        return copy_to_user(arg, &clockid, sizeof(clockid)) < 0 ? -EFAULT : 0;
    }

    switch (nr) {
    case EVIOCGNAME_NR: {
        static const char name[] = "A20OS evdev mux";
        uint32_t n = len < sizeof(name) ? len : (uint32_t)sizeof(name);
        return n && copy_to_user(arg, name, n) < 0 ? -EFAULT : 0;
    }
    case EVIOCGPHYS_NR:
    case EVIOCGUNIQ_NR: {
        char z = 0;
        return len && copy_to_user(arg, &z, 1) < 0 ? -EFAULT : 0;
    }
    case EVIOCGPROP_NR:
        return evdev_copy_zeros(arg, len);
    case EVIOCGMTSLOTS_NR:
        return -EINVAL;
    case 0x19: /* EVIOCGLED: mux claims no LEDs */
    case 0x1a: /* EVIOCGSND */
    case 0x1b: /* EVIOCGSW */
        return evdev_copy_zeros(arg, len);
    case EVIOCGKEY_NR: {
        uint8_t state[EV_MUX_KEY_STATE_BYTES];
        uint64_t flags = spin_lock_irqsave(&g_mux_state_lock);
        memcpy(state, g_mux_key_state, sizeof(state));
        spin_unlock_irqrestore(&g_mux_state_lock, flags);
        uint32_t n = len < sizeof(state) ? len : (uint32_t)sizeof(state);
        if (n && copy_to_user(arg, state, n) < 0)
            return -EFAULT;
        if (len > n)
            return evdev_copy_zeros((char *)arg + n, len - n);
        return 0;
    }
    default:
        break;
    }

    if (nr >= EVIOCGBIT_NR_BASE && nr < EVIOCGABS_NR_BASE)
        return evdev_bit_ioctl(nr - EVIOCGBIT_NR_BASE, len, arg);

    if (nr >= EVIOCGABS_NR_BASE && nr < EVIOCGABS_NR_BASE + 0x40) {
        uint32_t axis = nr - EVIOCGABS_NR_BASE;
        if (axis > ABS_Y)
            return -EINVAL;
        struct input_absinfo ai;
        memset(&ai, 0, sizeof(ai));
        uint64_t flags = spin_lock_irqsave(&g_mux_state_lock);
        ai.value = g_mux_abs_value[axis];
        spin_unlock_irqrestore(&g_mux_state_lock, flags);
        ai.minimum = 0;
        ai.maximum = 32767; /* virtio-input tablet axis range */
        return copy_to_user(arg, &ai, sizeof(ai)) < 0 ? -EFAULT : 0;
    }

    return -EINVAL;
}

vfile_ops_t g_devfs_input_ops = {
    .read  = input_read,
    .ioctl = input_ioctl,
    .poll  = input_poll,
};
