/*
 * LiteOS textbook experiment 4.4: interrupts.
 *
 * This is the only experiment that includes vendor headers.  It uses the
 * Hi3861 SDK surface (hi_isr.h / iot_gpio.h / iot_pwm.h); the implementation
 * of those lives in user/port/hi3861_hal/ and the standard CMSIS library in
 * user/cmsis_rtos2/ never sees them.
 *
 * The handler runs in the vendor dispatcher thread, not in exception
 * context, because a user program on this NOMMU part cannot execute an ISR.
 * Delivery is still edge-driven: the kernel vendor module increments and
 * wakes the subscriber word, and the handler observes the interrupt count.
 */
#include "cmsis_os2.h"
#include "hi_isr.h"
#include "iot_gpio.h"
#include "iot_pwm.h"

void demo_log(const char *fmt, ...);
int demo_check(int condition, const char *what);

static volatile unsigned g_isr_count;

static void gpio_isr(hi_u32 irq, hi_u32 arg)
{
    (void)arg;
    if (irq == HI_IRQ_GPIO)
        g_isr_count++;
}

int experiment_44(void)
{
    demo_log("[4.4] vendor IRQ, GPIO and PWM");
    g_isr_count = 0;
    int ok = demo_check(hi_irq_request(HI_IRQ_GPIO, gpio_isr, 0) ==
                            HI_ERR_SUCCESS,
                        "hi_irq_request");
    ok &= demo_check(hi_irq_enable(HI_IRQ_GPIO) == HI_ERR_SUCCESS,
                     "hi_irq_enable");

    hi_u32 gpio = IoTGpioInit(5);
    gpio |= IoSetFunc(5, 1);
    gpio |= IoTGpioSetDir(5, IOT_GPIO_DIR_OUT);
    gpio |= IoTGpioSetOutputVal(5, 1);
    ok &= demo_check(gpio == HI_ERR_SUCCESS, "GPIO init/dir/output");

    hi_u32 pwm = IoTPwmInit(0);
    pwm |= IoTPwmStart(0, 50, 1000);
    ok &= demo_check(pwm == HI_ERR_SUCCESS, "PWM init/start");

    for (unsigned i = 0; i < 3; i++) {
        ok &= demo_check(hi_irq_trigger(HI_IRQ_GPIO) == HI_ERR_SUCCESS,
                         "hi_irq_trigger");
        osDelay(10);
    }
    osDelay(20);
    ok &= demo_check(g_isr_count == 3, "three edges reach the handler");

    /* A critical section defers delivery instead of losing the edge. */
    hi_u64 flags = hi_int_lock();
    unsigned before = g_isr_count;
    (void)hi_irq_trigger(HI_IRQ_GPIO);
    osDelay(10);
    ok &= demo_check(g_isr_count == before, "handler deferred while locked");
    hi_int_restore(flags);
    osDelay(20);
    ok &= demo_check(g_isr_count == before + 1, "deferred edge delivered");

    (void)hi_irq_disable(HI_IRQ_GPIO);
    (void)hi_irq_trigger(HI_IRQ_GPIO);
    osDelay(20);
    ok &= demo_check(g_isr_count == before + 1, "disabled line is silent");
    (void)hi_irq_enable(HI_IRQ_GPIO);
    (void)hi_irq_trigger(HI_IRQ_GPIO);
    osDelay(20);
    ok &= demo_check(g_isr_count == before + 2, "re-enabled line delivers");

    demo_log("[4.4] %s irq=%u", ok ? "PASS" : "FAIL", (unsigned)g_isr_count);
    return ok;
}
