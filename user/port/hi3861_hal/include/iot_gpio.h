/*
 * Hi3861 GPIO extension (vendor, non-standard).
 *
 * The A20OS vendor module keeps the configuration and output state; the
 * experiment uses it to show that the vendor layer composes with CMSIS
 * without either header knowing about the other.
 */
#ifndef IOT_GPIO_H
#define IOT_GPIO_H

#include "hi_types_base.h"

#define IOT_GPIO_DIR_IN  0U
#define IOT_GPIO_DIR_OUT 1U

hi_u32 IoTGpioInit(hi_u32 id);
hi_u32 IoTGpioSetDir(hi_u32 id, hi_u32 dir);
hi_u32 IoTGpioSetOutputVal(hi_u32 id, hi_u32 value);
hi_u32 IoSetFunc(hi_u32 id, hi_u32 func);

#endif /* IOT_GPIO_H */
