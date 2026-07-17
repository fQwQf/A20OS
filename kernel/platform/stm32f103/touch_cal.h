#ifndef _STM32F103_TOUCH_CAL_H
#define _STM32F103_TOUCH_CAL_H

/*
 * Touch calibration solver + persistence (hardware-independent).
 *
 * The XPT2046 driver (touch.c) applies a stm32_touch_calibration_t inside
 * stm32_touch_poll(): optional swap of the raw axes, a linear scale of each
 * raw axis onto [0, pixels-1] via (x/y)_(min/max), then an optional invert.
 * This module derives that struct from a handful of "touch a known screen
 * point, record the raw ADC reading" samples (the classic four-corner
 * calibration, §5.6 of the hub manual) and (de)serialises it to a fixed blob
 * for /cfg/touch.cal on the TF card.
 *
 * All of this is pure integer math with no register access, so it is covered
 * by the QEMU self-test and the host unit test.
 */

#include "core/types.h"
#include "drivers/stm32f1/touch.h"

/* One calibration sample: the raw ADC pair recorded while the user touched a
 * known target pixel (screen_x, screen_y). */
typedef struct touch_cal_point {
    uint16_t raw_x;
    uint16_t raw_y;
    uint16_t screen_x;
    uint16_t screen_y;
} touch_cal_point_t;

/*
 * Solve for a calibration from >=2 sample points that span the screen in both
 * axes (four corners is the recommended set). screen_w/screen_h are the panel
 * dimensions in pixels (e.g. 320x480). Detects axis swap and per-axis invert
 * automatically from the samples, then fits the raw min/max range.
 *
 * Returns 0 on success (out filled), -1 on bad arguments or degenerate input
 * (e.g. all samples collapse onto one raw value so a range can't be found).
 */
int touch_cal_solve(const touch_cal_point_t *points, unsigned count,
                    uint16_t screen_w, uint16_t screen_h,
                    stm32_touch_calibration_t *out);

/* Serialised on-card size of a calibration blob (magic+version+fields+crc). */
#define TOUCH_CAL_BLOB_SIZE 20U

/*
 * Serialise cal into a TOUCH_CAL_BLOB_SIZE-byte little-endian blob suitable
 * for /cfg/touch.cal. Returns the number of bytes written (TOUCH_CAL_BLOB_SIZE)
 * or -1 if cal is NULL or buf is too small.
 */
int touch_cal_serialize(const stm32_touch_calibration_t *cal, uint8_t *buf,
                        unsigned buf_size);

/*
 * Parse a blob written by touch_cal_serialize back into cal. Returns 0 on
 * success, -1 on bad size/magic/version/CRC (caller should then fall back to
 * the driver default and re-run calibration).
 */
int touch_cal_deserialize(const uint8_t *buf, unsigned buf_size,
                          stm32_touch_calibration_t *cal);

#endif /* _STM32F103_TOUCH_CAL_H */
