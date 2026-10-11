#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <poll.h>
#include <stdlib.h>

#define EV_VERSION 0x010001

struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

struct input_absinfo {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_REP 0x14

#define REL_X 0x00
#define REL_Y 0x01
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_Z 0x02

#define KEY_A 30
#define BTN_LEFT 0x110
#define KEY_MAX 0x2ff

#define BUS_VIRTUAL 0x06

#define IOC_READ 2U
#define IOC(dir, type, nr, size)                                               \
    (((dir) << 30) | ((type) << 8) | (nr) | ((size) << 16))

#define EVIOCGVERSION IOC(IOC_READ, 'E', 0x01, 4)
#define EVIOCGREP IOC(IOC_READ, 'E', 0x03, 8)
#define EVIOCGID IOC(IOC_READ, 'E', 0x02, 8)
#define EVIOCGRAB IOC(1U, 'E', 0x90, 4)
#define EVIOCGNAME(len) IOC(IOC_READ, 'E', 0x06, len)
#define EVIOCGBIT(ev, len) IOC(IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGABS(abs) IOC(IOC_READ, 'E', 0x40 + (abs), 24)

static int bit_test(const uint8_t *bits, int bit) {
    return (bits[bit >> 3] >> (bit & 7)) & 1;
}

static int fail(const char *what) {
    printf("EVDEV_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int basic(void) {
    int fd = open("/dev/event0", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return fail("open /dev/event0");

    int version = 0;
    if (ioctl(fd, EVIOCGVERSION, &version) < 0)
        return fail("EVIOCGVERSION");
    if (version != EV_VERSION)
        return fail("EVIOCGVERSION value");

    struct input_id id;
    memset(&id, 0, sizeof(id));
    if (ioctl(fd, EVIOCGID, &id) < 0)
        return fail("EVIOCGID");
    if (id.bustype != BUS_VIRTUAL)
        return fail("EVIOCGID bustype");

    char name[64];
    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
        return fail("EVIOCGNAME");
    if (strcmp(name, "A20OS evdev mux") != 0)
        return fail("EVIOCGNAME value");

    uint8_t evbits[16];
    memset(evbits, 0, sizeof(evbits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
        return fail("EVIOCGBIT(0)");
    if (!bit_test(evbits, EV_SYN))
        return fail("event type bitmap");

    uint8_t keybits[(KEY_MAX + 8) / 8];
    memset(keybits, 0, sizeof(keybits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
        return fail("EVIOCGBIT(EV_KEY)");
    if (bit_test(evbits, EV_KEY) && !bit_test(keybits, KEY_A) &&
        !bit_test(keybits, BTN_LEFT))
        return fail("key bitmap");

    uint8_t relbits[4];
    memset(relbits, 0, sizeof(relbits));
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) < 0)
        return fail("EVIOCGBIT(EV_REL)");
    if (bit_test(evbits, EV_REL) &&
        (!bit_test(relbits, REL_X) || !bit_test(relbits, REL_Y)))
        return fail("rel bitmap");

    struct input_absinfo ai;
    memset(&ai, 0, sizeof(ai));
    if (bit_test(evbits, EV_ABS)) {
        if (ioctl(fd, EVIOCGABS(ABS_X), &ai) < 0 || ai.maximum <= ai.minimum)
            return fail("EVIOCGABS advertised range");
    } else if (ioctl(fd, EVIOCGABS(ABS_X), &ai) >= 0 || errno != EINVAL) {
        return fail("unadvertised ABS axis");
    }
    errno = 0;
    if (ioctl(fd, EVIOCGABS(ABS_Z), &ai) >= 0 || errno != EINVAL)
        return fail("EVIOCGABS unknown axis must EINVAL");

    if (ioctl(fd, EVIOCGRAB, 1) < 0)
        return fail("EVIOCGRAB");
    if (ioctl(fd, EVIOCGRAB, 0) < 0)
        return fail("EVIOCGRAB release");

    /* O_NONBLOCK read with no events must report EAGAIN. */
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n >= 0 || errno != EAGAIN)
        return fail("nonblock empty read");

    close(fd);
    printf("EVDEV_STRESS: PASS\n");
    return 0;
}

struct event {
    int64_t sec, usec;
    uint16_t type, code;
    int32_t value;
};
#define EVIOCGKEY(len) IOC(IOC_READ, 'E', 0x18, len)
#define EVIOCREVOKE IOC(1U, 'E', 0x91, 4)

static int empty(int fd) {
    struct event e;
    return read(fd, &e, sizeof(e)) == -1 && errno == EAGAIN;
}

static int clicks(int fd, int want_release) {
    int down = 0, up = 0;
    for (int retry = 0; retry < 100; retry++) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 100) < 0)
            return fail("poll stream");
        struct event e[32];
        int n = read(fd, e, sizeof(e));
        if (n < 0 && errno != EAGAIN)
            return fail("read stream");
        for (int i = 0; i < n / (int)sizeof(e[0]); i++) {
            if (e[i].type == EV_KEY && e[i].code == BTN_LEFT) {
                down |= e[i].value == 1;
                up |= e[i].value == 0;
            }
        }
        if (down && (!want_release || up))
            return 0;
    }
    return fail("missing button transition in subscriber");
}

static int sysfs_bitmap(const char *path, const uint8_t *expected,
                        unsigned bytes) {
    char line[512];
    FILE *f = fopen(path, "r");
    if (!f || !fgets(line, sizeof(line), f)) {
        printf("EVDEV_ATTR: %s errno=%d\n", path, errno);
        return fail("sysfs bitmap read");
    }
    fclose(f);
    unsigned long words[16] = {0};
    unsigned n = 0;
    char *p = line;
    while (*p && n < 16) {
        char *end;
        unsigned long value = strtoul(p, &end, 16);
        if (p == end)
            break;
        words[n++] = value;
        p = end;
        while (*p == ' ' || *p == '\n')
            p++;
    }
    for (unsigned bit = 0; bit < bytes * 8; bit++) {
        unsigned group = bit / (sizeof(unsigned long) * 8);
        int value = group < n ? !!(words[n - 1 - group] &
                                   (1UL << (bit % (sizeof(unsigned long) * 8))))
                              : 0;
        if (value != bit_test(expected, bit))
            return fail("sysfs/ioctl bitmap mismatch");
    }
    return 0;
}

static int stream(void) {
    char path[64], name[128];
    int a = -1;
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        memset(name, 0, sizeof(name));
        int n = ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        printf("EVDEV_DEVICE: %s %s bytes=%d\n", path, name, n);
        if (strstr(name, "Virtio Keyboard")) {
            uint8_t bits[96] = {0};
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) <= 0 ||
                !bit_test(bits, KEY_A) || bit_test(bits, BTN_LEFT))
                return fail("keyboard device-local keys");
            memset(bits, 0, sizeof(bits));
            if (ioctl(fd, EVIOCGBIT(0, sizeof(bits)), bits) <= 0 ||
                bit_test(bits, EV_REL) || bit_test(bits, EV_ABS))
                return fail("keyboard device-local event types");
            if (bit_test(bits, EV_REP)) {
                int repeat[2] = {-1, -1};
                if (ioctl(fd, EVIOCGREP, repeat) != 0 || repeat[0] || repeat[1])
                    return fail("keyboard repeat query");
            }
        }
        if (strstr(name, "Virtio Mouse")) {
            uint8_t bits[96] = {0};
            if (n != (int)strlen(name) + 1 ||
                ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) <= 0 ||
                bit_test(bits, KEY_A) || !bit_test(bits, BTN_LEFT))
                return fail("mouse device-local keys/name");
            char attr[96];
            snprintf(attr, sizeof(attr),
                     "/sys/class/input/event%d/capabilities/key", i);
            if (sysfs_bitmap(attr, bits, sizeof(bits)))
                return 1;
            snprintf(attr, sizeof(attr),
                     "/sys/devices/virtual/input/event%d/capabilities/key", i);
            if (sysfs_bitmap(attr, bits, sizeof(bits)))
                return 1;
            snprintf(attr, sizeof(attr), "/sys/dev/char/29:%d/capabilities/key",
                     i + 1);
            if (sysfs_bitmap(attr, bits, sizeof(bits)))
                return 1;
            memset(bits, 0, sizeof(bits));
            if (ioctl(fd, EVIOCGBIT(0, sizeof(bits)), bits) <= 0 ||
                !bit_test(bits, EV_REL) || bit_test(bits, EV_ABS))
                return fail("relative mouse device-local event types");
            a = fd;
            break;
        }
        close(fd);
    }
    if (a < 0)
        return fail("find Virtio Mouse");
    int b = open(path, O_RDONLY | O_NONBLOCK);
    int mux = open("/dev/event0", O_RDONLY | O_NONBLOCK);
    if (b < 0 || mux < 0)
        return fail("independent opens");
    /* Use the same path after opening a reader with a button held below. */
    printf("EVDEV_STREAM: READY normal\n");
    if (clicks(a, 1) || clicks(b, 1) || clicks(mux, 1))
        return 1;
    printf("EVDEV_STREAM: READY held\n");
    if (clicks(a, 0))
        return 1;
    int c = open(path, O_RDONLY | O_NONBLOCK);
    uint8_t keys[96] = {0};
    if (c < 0 || ioctl(c, EVIOCGKEY(sizeof(keys)), keys) <= 0 ||
        !bit_test(keys, BTN_LEFT))
        return fail("new reader sees held state");
    printf("EVDEV_STREAM: READY release\n");
    for (int i = 0; i < 100; i++) {
        ioctl(a, EVIOCGKEY(sizeof(keys)), keys);
        if (!bit_test(keys, BTN_LEFT))
            break;
        usleep(10000);
    }
    if (bit_test(keys, BTN_LEFT))
        return fail("release state");
    struct event discard[256];
    while (read(a, discard, sizeof(discard)) > 0) {
    }
    while (read(b, discard, sizeof(discard)) > 0) {
    }
    while (read(c, discard, sizeof(discard)) > 0) {
    }
    while (read(mux, discard, sizeof(discard)) > 0) {
    }
    if (ioctl(a, EVIOCGRAB, 1) < 0 || ioctl(b, EVIOCGRAB, 1) != -1 ||
        errno != EBUSY)
        return fail("exclusive grab");
    printf("EVDEV_STREAM: READY grabbed\n");
    if (clicks(a, 1))
        return 1;
    if (!empty(b) || !empty(c) || !empty(mux))
        return fail("grab isolation");
    if (ioctl(a, EVIOCGRAB, 0) < 0 || ioctl(b, EVIOCREVOKE, 0) < 0)
        return fail("ungrab/revoke");
    if (read(b, discard, sizeof(discard)) != -1 || errno != ENODEV)
        return fail("revoked read");
    struct pollfd p = {b, POLLIN, 0};
    if (poll(&p, 1, 0) != 1 || !(p.revents & POLLHUP))
        return fail("revoked poll");
    printf("EVDEV_STREAM: READY final\n");
    if (clicks(a, 1) || clicks(c, 1) || clicks(mux, 1))
        return 1;
    printf("EVDEV_STREAM: READY overflow\n");
    unsigned motions = 0;
    for (unsigned retry = 0; retry < 1000 && motions < 300; retry++) {
        struct pollfd ready = {a, POLLIN, 0};
        poll(&ready, 1, 20);
        struct event events[32];
        int n = read(a, events, sizeof(events));
        for (int i = 0; i < n / (int)sizeof(events[0]); i++)
            motions += events[i].type == EV_REL && events[i].code == REL_X;
    }
    if (motions < 300)
        return fail("overflow motion delivery");
    int n = read(c, discard, sizeof(discard));
    int dropped = 0;
    for (int i = 0; i < n / (int)sizeof(discard[0]); i++)
        dropped |= discard[i].type == EV_SYN && discard[i].code == 3;
    if (!dropped)
        return fail("slow reader SYN_DROPPED");
    memset(keys, 0xff, sizeof(keys));
    if (ioctl(c, EVIOCGKEY(sizeof(keys)), keys) <= 0 ||
        bit_test(keys, BTN_LEFT))
        return fail("overflow current button state");
    close(a);
    close(b);
    close(c);
    close(mux);
    printf("EVDEV_STREAM: PASS\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--stream"))
        return stream();
    return basic();
}
