#ifndef _VIRTIO_INPUT_H
#define _VIRTIO_INPUT_H

#include "core/types.h"
#include "drivers/core/driver_class.h"

#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03

struct input_event {
    uint32_t time_sec;
    uint32_t time_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

int virtio_input_init(void);

#endif
