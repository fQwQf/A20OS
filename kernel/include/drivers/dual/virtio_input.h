/*
 * virtio-input — shared device protocol (dual-placement).
 *
 * Config-space queries (non-destructive) shared by the kernel probe
 * shell and the user driver shell.  Virtqueue setup/event delivery
 * is intentionally not in the shared layer yet: it is destructive
 * (device status transitions) and therefore single-owner; it arrives
 * with the ownership arbitration work in 04-dual-placement.md.
 *
 * Config layout (virtio spec 5.8):
 *   u8 select (0=unset, 1=dev-name...), u8 subsel, u8 size, u8 data[128]
 */
#ifndef _DRIVERS_DUAL_VIRTIO_INPUT_H
#define _DRIVERS_DUAL_VIRTIO_INPUT_H

#include "drivers/dual/virtio_mmio.h"

#define VIRTIO_INPUT_DEVICE_ID      18u

#define VIRTIO_INPUT_CFG_UNSET      0x00u
#define VIRTIO_INPUT_CFG_ID_NAME    0x01u
#define VIRTIO_INPUT_CFG_ID_SERIAL  0x02u
#define VIRTIO_INPUT_CFG_ID_DEVIDS  0x03u
#define VIRTIO_INPUT_CFG_PROP_BITS  0x04u
#define VIRTIO_INPUT_CFG_EV_BITS    0x05u
#define VIRTIO_INPUT_CFG_ABS_INFO   0x06u

#define VINPUT_CFG_SELECT   0x00u
#define VINPUT_CFG_SUBSEL   0x01u
#define VINPUT_CFG_SIZE     0x02u
#define VINPUT_CFG_DATA     0x08u
#define VINPUT_CFG_DATA_MAX 128u

typedef struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} virtio_input_event_t;

/* Read a config string (name/serial).  Returns length, 0 if absent.
 * Non-destructive: select/subsel writes only steer the config latch. */
static inline uint32_t vinput_cfg_string(uint64_t base, uint8_t select,
                                         char *out, uint32_t out_max)
{
    if (!out || out_max == 0)
        return 0;
    vmmio_cfg_write8(base, VINPUT_CFG_SELECT, select);
    vmmio_cfg_write8(base, VINPUT_CFG_SUBSEL, 0);
    uint8_t size = vmmio_cfg_read8(base, VINPUT_CFG_SIZE);
    if (size == 0 || size > VINPUT_CFG_DATA_MAX) {
        out[0] = '\0';
        return 0;
    }
    uint32_t n = size < out_max - 1 ? size : out_max - 1;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (char)vmmio_cfg_read8(base, VINPUT_CFG_DATA + i);
    out[n] = '\0';
    /* leave the latch parked at UNSET for the next reader */
    vmmio_cfg_write8(base, VINPUT_CFG_SELECT, VIRTIO_INPUT_CFG_UNSET);
    return n;
}

#endif
