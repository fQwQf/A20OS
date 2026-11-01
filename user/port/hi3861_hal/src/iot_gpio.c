/* Vendor GPIO shim: the A20OS vendor module owns the pin state. */
#include "iot_gpio.h"
#include "a20_syscall.h"

#define VENDOR_GPIO_CONFIG 0x7F03u
#define VENDOR_GPIO_WRITE  0x7F04u

hi_u32 IoTGpioInit(hi_u32 id)
{
    (void)id;
    return HI_ERR_SUCCESS;
}

hi_u32 IoSetFunc(hi_u32 id, hi_u32 func)
{
    return a20_syscall6(VENDOR_GPIO_CONFIG, id, func, 0, 0, 0, 0) == A20_OK
               ? HI_ERR_SUCCESS : HI_ERR_FAILURE;
}

hi_u32 IoTGpioSetDir(hi_u32 id, hi_u32 dir)
{
    return a20_syscall6(VENDOR_GPIO_CONFIG, id, dir, 0, 0, 0, 0) == A20_OK
               ? HI_ERR_SUCCESS : HI_ERR_FAILURE;
}

hi_u32 IoTGpioSetOutputVal(hi_u32 id, hi_u32 value)
{
    return a20_syscall6(VENDOR_GPIO_WRITE, id, value, 0, 0, 0, 0) == A20_OK
               ? HI_ERR_SUCCESS : HI_ERR_FAILURE;
}
