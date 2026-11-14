/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_PROTOCOL_INTERNAL_H
#define A20_DRIVER_PROTOCOL_INTERNAL_H
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/protocol.h"

struct class_publication {
    class_device_t public;
    volatile int binding_stopped;
    bool has_stop_contract;
    size_t protocol_count;
    struct driver_protocol_descriptor protocols[];
};
/* Lifecycle-serialized core helpers, never module exports. */
static inline uint64_t driver_protocol_class_id(uint32_t type)
{
    switch (type) {
    case DEV_CLASS_CHAR: return DRIVER_PROTOCOL_CHAR;
    case DEV_CLASS_BLOCK: return DRIVER_PROTOCOL_BLOCK;
    case DEV_CLASS_NET: return DRIVER_PROTOCOL_NET;
    case DEV_CLASS_INPUT: return DRIVER_PROTOCOL_INPUT;
    case DEV_CLASS_DISPLAY: return DRIVER_PROTOCOL_DISPLAY;
    case DEV_CLASS_AUDIO: return DRIVER_PROTOCOL_AUDIO;
    default: return 0;
    }
}
size_t device_protocols_copy(const struct device *, struct driver_protocol_descriptor *out);
int driver_publication_create(struct device *, class_device_t **out);
void driver_publication_discard(struct device *, class_device_t *);
#endif
