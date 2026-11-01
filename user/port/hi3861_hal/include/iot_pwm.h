/*
 * Hi3861 PWM extension (vendor, non-standard).
 */
#ifndef IOT_PWM_H
#define IOT_PWM_H

#include "hi_types_base.h"

hi_u32 IoTPwmInit(hi_u32 port);
hi_u32 IoTPwmStart(hi_u32 port, hi_u32 duty, hi_u32 freq);
hi_u32 IoTPwmStop(hi_u32 port);

#endif /* IOT_PWM_H */
