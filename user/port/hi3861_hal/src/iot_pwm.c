/* Vendor PWM shim: the A20OS vendor module records duty/period. */
#include "iot_pwm.h"
#include "a20_syscall.h"

#define VENDOR_PWM_START 0x7F05u

hi_u32 IoTPwmInit(hi_u32 port)
{
    (void)port;
    return HI_ERR_SUCCESS;
}

hi_u32 IoTPwmStart(hi_u32 port, hi_u32 duty, hi_u32 freq)
{
    return a20_syscall6(VENDOR_PWM_START, port, duty, freq, 0, 0, 0) == A20_OK
               ? HI_ERR_SUCCESS : HI_ERR_FAILURE;
}

hi_u32 IoTPwmStop(hi_u32 port)
{
    (void)port;
    return HI_ERR_SUCCESS;
}
