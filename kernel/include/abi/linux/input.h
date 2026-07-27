#ifndef _ABI_LINUX_INPUT_H
#define _ABI_LINUX_INPUT_H

/*
 * Linux evdev (input event device) ABI constants, mirroring
 * linux/input.h and asm-generic/ioctl.h.  All A20OS target
 * architectures use the asm-generic _IOC encoding.
 */

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

#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_REL   0x02
#define EV_ABS   0x03
#define EV_MSC   0x04

#define SYN_REPORT  0
#define SYN_CONFIG  1
#define SYN_DROPPED 3

#define REL_X      0x00
#define REL_Y      0x01
#define REL_HWHEEL 0x06
#define REL_WHEEL  0x08

#define ABS_X 0x00
#define ABS_Y 0x01

#define KEY_MAX    0x2ff
#define BTN_TOUCH  0x14a
#define BTN_MOUSE_FIRST 0x110
#define BTN_MOUSE_LAST  0x117

#define BUS_VIRTUAL 0x06

#define A20_IOC_NRBITS   8
#define A20_IOC_TYPEBITS 8
#define A20_IOC_SIZEBITS 14
#define A20_IOC_NRSHIFT   0
#define A20_IOC_TYPESHIFT (A20_IOC_NRSHIFT + A20_IOC_NRBITS)
#define A20_IOC_SIZESHIFT (A20_IOC_TYPESHIFT + A20_IOC_TYPEBITS)
#define A20_IOC_DIRSHIFT  (A20_IOC_SIZESHIFT + A20_IOC_SIZEBITS)

#define A20_IOC_WRITE 1U
#define A20_IOC_READ  2U

#define A20_IOC(dir, type, nr, size) \
    (((dir) << A20_IOC_DIRSHIFT) | ((type) << A20_IOC_TYPESHIFT) | \
     ((nr) << A20_IOC_NRSHIFT) | ((size) << A20_IOC_SIZESHIFT))
#define A20_IOR(type, nr, size) A20_IOC(A20_IOC_READ, type, nr, size)
#define A20_IOW(type, nr, size) A20_IOC(A20_IOC_WRITE, type, nr, size)

#define A20_IOC_NR(req)  (((req) >> A20_IOC_NRSHIFT) & ((1U << A20_IOC_NRBITS) - 1))
#define A20_IOC_TYPE(req) (((req) >> A20_IOC_TYPESHIFT) & ((1U << A20_IOC_TYPEBITS) - 1))
#define A20_IOC_SIZE(req) (((req) >> A20_IOC_SIZESHIFT) & ((1U << A20_IOC_SIZEBITS) - 1))

#define EVIOCGVERSION A20_IOR('E', 0x00, 4)  /* int: EV_VERSION */
#define EVIOCGID      A20_IOR('E', 0x02, 8)  /* struct input_id */
#define EVIOCGRAB     A20_IOW('E', 0x90, 4)
#define EVIOCREVOKE   A20_IOW('E', 0x91, 4)
#define EVIOCSCLOCKID A20_IOW('E', 0xa0, 4)
#define EVIOCGCLOCKID A20_IOR('E', 0xa0, 4)

/* Variable-length ioctls: nr numbers, size carried in the request. */
#define EVIOCGNAME_NR   0x06
#define EVIOCGPHYS_NR   0x07
#define EVIOCGUNIQ_NR   0x08
#define EVIOCGPROP_NR   0x09
#define EVIOCGMTSLOTS_NR 0x0a
#define EVIOCGKEY_NR    0x18
#define EVIOCGBIT_NR_BASE 0x20 /* + event type */
#define EVIOCGABS_NR_BASE 0x40 /* + abs axis */

#endif /* _ABI_LINUX_INPUT_H */
