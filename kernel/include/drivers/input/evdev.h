#ifndef _DRIVERS_INPUT_EVDEV_H
#define _DRIVERS_INPUT_EVDEV_H

#include "core/types.h"
#include "core/input.h"

/* Internal driver query; argument is a kernel buffer, never a user pointer. */
#define INPUT_IOCTL_GET_INFO 0xa200U
#define INPUT_IOCTL_LED_EVENT 0xa201U /* kernel struct input_event */
#define INPUT_IOCTL_GET_LEDS 0xa202U  /* kernel uint32_t bitmap */
#define INPUT_EVENT_TYPES 32
#define INPUT_BITMAP_BYTES 128
struct input_device_info {
    char name[128];
    struct input_id id;
    uint8_t properties[INPUT_BITMAP_BYTES];
    uint8_t bits[INPUT_EVENT_TYPES][INPUT_BITMAP_BYTES];
    struct input_absinfo abs[64];
};

static inline void input_info_bit(struct input_device_info *info, unsigned type,
                                  unsigned code) {
    if (type >= INPUT_EVENT_TYPES || code >= INPUT_BITMAP_BYTES * 8)
        return;
    info->bits[0][type >> 3] |= 1U << (type & 7);
    if (type)
        info->bits[type][code >> 3] |= 1U << (code & 7);
}

struct vfile;
struct class_device;
int input_device_get_info(struct class_device *device,
                          struct input_device_info *info);
int input_file_open(struct vfile *vf, struct class_device *device);
#endif
