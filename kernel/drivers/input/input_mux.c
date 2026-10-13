/* Transport-independent evdev. Physical nodes have device-local capabilities
 * and streams; /dev/event0 retains the legacy aggregate stream. Each open file
 * description has its own queue, so udev/probes cannot consume libinput input. */
#include "drivers/input/virtio_input.h"
#include "drivers/input/evdev.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "fs/devfs.h"
#include "fs/readiness.h"
#include "fs/tty.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timekeeping.h"
#include "core/errno.h"
#include "core/poll.h"
#include "proc/proc.h"
#include "sys/usercopy.h"
#include "mm/slab.h"

#define INPUT_QUEUE_EVENTS 256
#define INPUT_KEY_BYTES ((KEY_MAX + 8) / 8)

typedef struct input_client {
    struct input_client *next;
    class_device_t *device;
    struct input_device_info info;
    struct input_event ring[INPUT_QUEUE_EVENTS];
    unsigned head, tail;
    uint8_t keys[INPUT_KEY_BYTES];
    int clock_id, revoked, grabbed, dropping;
} input_client_t;

static mutex_t g_input_lock = MUTEX_INIT;
static input_client_t *g_clients;
/* State belongs to the physical device, including while a client has grabbed
 * it. Keep it for as long as any subscriber can observe that device. */
typedef struct input_state {
    struct input_state *next;
    class_device_t *device;
    uint8_t keys[INPUT_KEY_BYTES];
    int32_t abs[64];
} input_state_t;
static input_state_t *g_states;

static input_state_t *input_state(class_device_t *device) {
    for (input_state_t *s = g_states; s; s = s->next)
        if (s->device == device)
            return s;
    input_state_t *s = kcalloc(1, sizeof(*s));
    if (s) {
        class_device_get(device);
        s->device = device;
        s->next = g_states;
        g_states = s;
    }
    return s;
}
static wait_queue_t g_input_waiters;

void input_mux_wake(void) {
    /* Several devices and independent subscribers share this notification. */
    (void)wait_queue_wake_all(&g_input_waiters, 0, PROC_WAKE_EVENT);
}

int input_device_get_info(class_device_t *device,
                          struct input_device_info *info) {
    if (class_device_call_begin(device) < 0)
        return -ENODEV;
    const input_dev_ops_t *ops = device->dev->drv->class_ops;
    int ret = ops && ops->ioctl
                  ? ops->ioctl(device->dev, INPUT_IOCTL_GET_INFO, info)
                  : -EOPNOTSUPP;
    class_device_call_end(device);
    return ret;
}

static void input_track(input_client_t *client,
                        const struct input_event *event) {
    if (event->type == EV_KEY && event->code <= KEY_MAX) {
        uint8_t mask = 1U << (event->code & 7);
        if (event->value)
            client->keys[event->code >> 3] |= mask;
        else
            client->keys[event->code >> 3] &= ~mask;
    } else if (event->type == EV_ABS && event->code < 64) {
        client->info.abs[event->code].value = event->value;
    }
}

static void input_enqueue(input_client_t *client, struct input_event event) {
    uint64_t ts[2];
    if (client->clock_id == 1)
        timekeeping_get_monotonic(ts);
    else
        timekeeping_get_realtime(ts);
    event.time_sec = ts[0];
    event.time_usec = ts[1] / 1000;
    input_track(client, &event);
    if (client->dropping) {
        if (event.type != EV_SYN || event.code != SYN_REPORT)
            return;
        client->dropping = 0;
    }
    unsigned next = (client->head + 1) % INPUT_QUEUE_EVENTS;
    if (next == client->tail) {
        /* Drop whole unread frames and report loss instead of leaving stuck
         * buttons. The consumer resynchronizes with EVIOCGKEY/EVIOCGABS. */
        client->head = client->tail = 0;
        struct input_event dropped = event;
        dropped.type = EV_SYN;
        dropped.code = SYN_DROPPED;
        dropped.value = 0;
        client->ring[client->head++] = dropped;
        client->dropping = 1;
        if (event.type != EV_SYN || event.code != SYN_REPORT)
            return;
        client->dropping = 0;
        next = client->head + 1;
    }
    client->ring[client->head] = event;
    client->head = next;
}

/* g_input_lock serializes source draining and client fanout. Driver callbacks
 * run with their publication pinned, with no spinlock held by this layer. */
static void input_pump_device(class_device_t *device) {
    if (class_device_call_begin(device) < 0)
        return;
    const input_dev_ops_t *ops = device->dev->drv->class_ops;
    input_state_t *state = input_state(device);
    if (state && ops && ops->read) {
        struct input_event events[32];
        int n;
        /* Bound work under the mux mutex even for a continuously busy source. */
        for (unsigned batch = 0;
             batch < 32 &&
             (n = ops->read(device->dev, events, sizeof(events))) > 0;
             batch++) {
            input_client_t *grabber = NULL;
            for (input_client_t *c = g_clients; c; c = c->next)
                if (c->grabbed && !c->revoked &&
                    (!c->device || c->device == device)) {
                    grabber = c;
                    break;
                }
            for (unsigned i = 0; i < (unsigned)n / sizeof(events[0]); i++) {
                const struct input_event *ev = &events[i];
                if (ev->type == EV_KEY && ev->code <= KEY_MAX) {
                    uint8_t mask = 1U << (ev->code & 7);
                    if (ev->value)
                        state->keys[ev->code >> 3] |= mask;
                    else
                        state->keys[ev->code >> 3] &= ~mask;
                } else if (ev->type == EV_ABS && ev->code < 64) {
                    state->abs[ev->code] = ev->value;
                }
                if (ev->type == EV_REL ||
                    (ev->type == EV_KEY && ev->code >= BTN_MOUSE_FIRST &&
                     ev->code <= BTN_MOUSE_LAST))
                    tty_push_mouse_event(ev->type, ev->code, ev->value);
                for (input_client_t *c = g_clients; c; c = c->next)
                    if (!c->revoked && (!c->device || c->device == device)) {
                        input_track(c, ev);
                        if (!grabber || c == grabber)
                            input_enqueue(c, events[i]);
                    }
            }
        }
    }
    class_device_call_end(device);
}

static void input_pump(input_client_t *client) {
    if (client->device) {
        input_pump_device(client->device);
        return;
    }
    for (unsigned i = 0;; i++) {
        class_device_t *device = class_device_get_by_type(DEV_CLASS_INPUT, i);
        if (!device)
            break;
        input_pump_device(device);
        class_device_put(device);
    }
}

int input_file_open(vfile_t *vf, class_device_t *device) {
    input_client_t *client = kcalloc(1, sizeof(*client));
    if (!client)
        return -ENOMEM;
    client->device = device;
    class_device_get(device);
    int ret = 0;
    if (device) {
        ret = input_device_get_info(device, &client->info);
    } else {
        strcpy(client->info.name, "A20OS evdev mux");
        client->info.id.bustype = BUS_VIRTUAL;
        input_info_bit(&client->info, EV_SYN, SYN_REPORT);
        struct input_device_info *info = kcalloc(1, sizeof(*info));
        if (!info)
            ret = -ENOMEM;
        for (unsigned i = 0; !ret && info; i++) {
            class_device_t *dev = class_device_get_by_type(DEV_CLASS_INPUT, i);
            if (!dev)
                break;
            if (input_device_get_info(dev, info) == 0) {
                for (unsigned t = 0; t < INPUT_EVENT_TYPES; t++)
                    for (unsigned b = 0; b < INPUT_BITMAP_BYTES; b++)
                        client->info.bits[t][b] |= info->bits[t][b];
                for (unsigned a = 0; a < 64; a++)
                    if (info->bits[EV_ABS][a >> 3] & (1U << (a & 7)))
                        client->info.abs[a] = info->abs[a];
            }
            class_device_put(dev);
        }
        kfree(info);
    }
    if (ret < 0) {
        class_device_put(device);
        kfree(client);
        return ret;
    }
    mutex_lock(&g_input_lock);
    /* Consume old source events before subscribing; existing readers still
     * receive them, while a newly opened descriptor starts at the present. */
    input_pump(client);
    for (input_state_t *s = g_states; s; s = s->next) {
        if (device && s->device != device)
            continue;
        for (unsigned b = 0; b < INPUT_KEY_BYTES; b++)
            client->keys[b] |= s->keys[b];
        for (unsigned a = 0; a < 64; a++)
            client->info.abs[a].value = s->abs[a];
    }
    if (device && !input_state(device)) {
        mutex_unlock(&g_input_lock);
        class_device_put(device);
        kfree(client);
        return -ENOMEM;
    }
    client->next = g_clients;
    g_clients = client;
    mutex_unlock(&g_input_lock);
    vf->priv = client;
    vf->ops = &g_devfs_input_ops;
    return 0;
}

static int input_take(input_client_t *client, char *buf, size_t count) {
    if (client->revoked ||
        (client->device &&
         !__atomic_load_n(&client->device->online, __ATOMIC_ACQUIRE)))
        return -ENODEV;
    input_pump(client);
    size_t copied = 0;
    while (client->head != client->tail &&
           copied + sizeof(struct input_event) <= count) {
        memcpy(buf + copied, &client->ring[client->tail],
               sizeof(struct input_event));
        client->tail = (client->tail + 1) % INPUT_QUEUE_EVENTS;
        copied += sizeof(struct input_event);
    }
    return copied ? (int)copied : -EAGAIN;
}

static int input_read(vfile_t *vf, char *buf, size_t count) {
    input_client_t *client = vf->priv;
    if (!client || count < sizeof(struct input_event))
        return -EINVAL;
    while (1) {
        mutex_lock(&g_input_lock);
        int ret = input_take(client, buf, count);
        mutex_unlock(&g_input_lock);
        if (ret != -EAGAIN || (vf->flags & O_NONBLOCK))
            return ret;
        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        if (!token.task)
            return -EAGAIN;
        wait_queue_entry_t entry = {0};
        bool linked = wait_queue_link(&g_input_waiters, &entry, token, 0);
        /* Recheck after subscription to close the enqueue-before-park gap. */
        mutex_lock(&g_input_lock);
        ret = input_take(client, buf, count);
        mutex_unlock(&g_input_lock);
        proc_wake_reason_t reason;
        if (linked && ret == -EAGAIN)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&g_input_waiters, &entry);
        proc_park_finish(token);
        if (ret != -EAGAIN)
            return ret;
        if (proc_wake_reason_is_task_interrupt(reason))
            return -ERESTARTSYS;
    }
}

static int input_poll(vfile_t *vf, short events) {
    input_client_t *client = vf->priv;
    if (!client)
        return POLLERR;
    mutex_lock(&g_input_lock);
    input_pump(client);
    int ready = client->revoked || (client->device &&
                                    !__atomic_load_n(&client->device->online,
                                                     __ATOMIC_ACQUIRE))
                    ? POLLHUP
                : (events & POLLIN) && client->head != client->tail ? POLLIN
                                                                    : 0;
    mutex_unlock(&g_input_lock);
    return ready;
}

static int input_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    input_client_t *client = vf->priv;
    if (!client || A20_IOC_TYPE(req) != 'E')
        return -ENOTTY;
    unsigned nr = A20_IOC_NR(req), len = A20_IOC_SIZE(req);
    uint8_t data[INPUT_BITMAP_BYTES] = {0};
    unsigned size = 0;
    int ret = 0;
    mutex_lock(&g_input_lock);
    input_pump(client);
    if (client->revoked ||
        (client->device &&
         !__atomic_load_n(&client->device->online, __ATOMIC_ACQUIRE))) {
        ret = -ENODEV;
    } else if (req == EVIOCGVERSION) {
        int version = EV_VERSION;
        memcpy(data, &version, sizeof(version));
        size = sizeof(version);
    } else if (req == EVIOCGID) {
        memcpy(data, &client->info.id, sizeof(client->info.id));
        size = sizeof(client->info.id);
    } else if (req == EVIOCGREP) {
        /* The compositor owns key repeat; the evdev stream has no software
         * repeat timer. Zero delay and period report that disabled state. */
        if (!(client->info.bits[0][EV_REP >> 3] & (1U << (EV_REP & 7))))
            ret = -EINVAL;
        else
            size = 2 * sizeof(int);
    } else if (req == EVIOCGRAB) {
        if (arg) {
            for (input_client_t *c = g_clients; c; c = c->next)
                if (c != client && c->grabbed &&
                    (!c->device || !client->device ||
                     c->device == client->device))
                    ret = -EBUSY;
        }
        if (!ret)
            client->grabbed = !!arg;
    } else if (req == EVIOCREVOKE) {
        if (arg)
            ret = -EINVAL;
        else {
            client->revoked = 1;
            client->grabbed = 0;
        }
    } else if (req == EVIOCGCLOCKID) {
        memcpy(data, &client->clock_id, sizeof(int));
        size = sizeof(int);
    } else if (req == EVIOCSCLOCKID) {
        /* User copying can fault, so do it without the client list lock. */
        mutex_unlock(&g_input_lock);
        int clock;
        if (copy_from_user(&clock, arg, sizeof(clock)) < 0)
            return -EFAULT;
        if (clock != 0 && clock != 1)
            return -EINVAL;
        mutex_lock(&g_input_lock);
        client->clock_id = clock;
    } else if (nr == EVIOCGNAME_NR) {
        size = strlen(client->info.name) + 1;
        memcpy(data, client->info.name, size);
    } else if (nr == EVIOCGPHYS_NR || nr == EVIOCGUNIQ_NR) {
        size = 1;
    } else if (nr == EVIOCGPROP_NR) {
        memcpy(data, client->info.properties, sizeof(data));
        size = sizeof(data);
    } else if (nr == EVIOCGKEY_NR) {
        memcpy(data, client->keys, sizeof(client->keys));
        size = sizeof(data);
    } else if (nr >= EVIOCGBIT_NR_BASE &&
               nr < EVIOCGBIT_NR_BASE + INPUT_EVENT_TYPES) {
        memcpy(data, client->info.bits[nr - EVIOCGBIT_NR_BASE], sizeof(data));
        size = sizeof(data);
    } else if (nr >= EVIOCGABS_NR_BASE && nr < EVIOCGABS_NR_BASE + 64) {
        unsigned axis = nr - EVIOCGABS_NR_BASE;
        if (!(client->info.bits[EV_ABS][axis >> 3] & (1U << (axis & 7))))
            ret = -EINVAL;
        else {
            memcpy(data, &client->info.abs[axis], sizeof(struct input_absinfo));
            size = sizeof(struct input_absinfo);
        }
    } else if (nr == 0x19 || nr == 0x1a || nr == 0x1b) {
        size = sizeof(data);
    } else {
        ret = -ENOTTY;
    }
    mutex_unlock(&g_input_lock);
    if (ret)
        return ret;
    if (size) {
        unsigned copied = len < size ? len : size;
        if (copy_to_user(arg, data, copied) < 0)
            return -EFAULT;
        /* Linux variable-length bitmap/name queries return bytes copied. */
        return nr == EVIOCGNAME_NR || nr == EVIOCGPHYS_NR ||
                       nr == EVIOCGUNIQ_NR || nr == EVIOCGPROP_NR ||
                       nr == EVIOCGKEY_NR ||
                       (nr >= EVIOCGBIT_NR_BASE && nr < EVIOCGABS_NR_BASE)
                   ? (int)copied
                   : 0;
    }
    if (req == EVIOCREVOKE)
        input_mux_wake();
    return 0;
}

static int input_close(vfile_t *vf) {
    input_client_t *client = vf->priv;
    mutex_lock(&g_input_lock);
    input_client_t **p = &g_clients;
    while (*p && *p != client)
        p = &(*p)->next;
    if (*p)
        *p = client->next;
    input_state_t **state = &g_states;
    while (*state) {
        int used = 0;
        for (input_client_t *c = g_clients; c; c = c->next)
            if (!c->device || c->device == (*state)->device)
                used = 1;
        if (used) {
            state = &(*state)->next;
        } else {
            input_state_t *old = *state;
            *state = old->next;
            class_device_put(old->device);
            kfree(old);
        }
    }
    mutex_unlock(&g_input_lock);
    if (client) {
        class_device_put(client->device);
        kfree(client);
    }
    return 0;
}

static size_t input_poll_sources(vfile_t *vf, short events,
                                 readiness_source_t *sources, size_t max) {
    (void)vf;
    if (!sources || !max || !(events & POLLIN))
        return 0;
    sources[0] = (readiness_source_t){&g_input_waiters, 0, 0};
    return 1;
}

vfile_ops_t g_devfs_input_ops = {
    .read = input_read,
    .ioctl = input_ioctl,
    .poll = input_poll,
    .poll_sources = input_poll_sources,
    .close = input_close,
};
