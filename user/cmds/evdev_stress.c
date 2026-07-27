#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

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

#define REL_X 0x00
#define REL_Y 0x01
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_Z 0x02

#define KEY_A     30
#define BTN_LEFT  0x110
#define KEY_MAX   0x2ff

#define BUS_VIRTUAL 0x06

#define IOC_READ 2U
#define IOC(dir, type, nr, size) \
    (((dir) << 30) | ((type) << 8) | (nr) | ((size) << 16))

#define EVIOCGVERSION IOC(IOC_READ, 'E', 0x00, 4)
#define EVIOCGID      IOC(IOC_READ, 'E', 0x02, 8)
#define EVIOCGRAB     IOC(1U, 'E', 0x90, 4)
#define EVIOCGNAME(len)   IOC(IOC_READ, 'E', 0x06, len)
#define EVIOCGBIT(ev, len) IOC(IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGABS(abs)    IOC(IOC_READ, 'E', 0x40 + (abs), 24)

static int bit_test(const uint8_t *bits, int bit)
{
    return (bits[bit >> 3] >> (bit & 7)) & 1;
}

static int fail(const char *what)
{
    printf("EVDEV_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

int main(void)
{
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
    if (!bit_test(evbits, EV_KEY) || !bit_test(evbits, EV_REL) ||
        !bit_test(evbits, EV_ABS))
        return fail("event type bitmap");

    uint8_t keybits[(KEY_MAX + 8) / 8];
    memset(keybits, 0, sizeof(keybits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
        return fail("EVIOCGBIT(EV_KEY)");
    if (!bit_test(keybits, KEY_A) || !bit_test(keybits, BTN_LEFT))
        return fail("key bitmap");

    uint8_t relbits[4];
    memset(relbits, 0, sizeof(relbits));
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) < 0)
        return fail("EVIOCGBIT(EV_REL)");
    if (!bit_test(relbits, REL_X) || !bit_test(relbits, REL_Y))
        return fail("rel bitmap");

    struct input_absinfo ai;
    memset(&ai, 0, sizeof(ai));
    if (ioctl(fd, EVIOCGABS(ABS_X), &ai) < 0)
        return fail("EVIOCGABS(ABS_X)");
    if (ai.maximum != 32767 || ai.minimum != 0)
        return fail("EVIOCGABS range");
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
