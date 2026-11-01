/*
 * Vendor IRQ shim for A20OS.
 *
 * Standard CMSIS objects (the dispatcher thread) plus the Vendor Extension
 * syscall range.  Nothing here is declared in cmsis_os2.h, and the standard
 * library never references these symbols.
 */
#include "hi_isr.h"
#include "a20_syscall.h"
#include "a20_sync.h"
#include "cmsis_os2.h"

#define VENDOR_IRQ_SUBSCRIBE 0x7F00u
#define VENDOR_IRQ_ENABLE    0x7F01u
#define VENDOR_IRQ_RAISE     0x7F02u

#define IRQ_DISPATCH_STACK 2048U

static volatile hi_u32 g_irq_word;
static volatile hi_u32 g_critical;
static volatile hi_u32 g_enabled;
static hi_irq_handler g_handler;
static hi_u32 g_handler_arg;
static osThreadId_t g_dispatcher;

static void irq_dispatcher(void *arg)
{
    (void)arg;
    hi_u32 seen = 0;
    for (;;) {
        hi_u32 now = g_irq_word;
        if (now != seen) {
            seen = now;
            /* A critical section defers delivery instead of dropping it. */
            while (g_critical)
                osDelay(1);
            if (g_enabled && g_handler)
                g_handler(HI_IRQ_GPIO, g_handler_arg);
        }
        /* The kernel increments and wakes this word on an edge. */
        (void)a20_futex_wait((hi_u32 *)&g_irq_word, now, ~0ULL);
    }
}

hi_u32 hi_irq_request(hi_u32 irq, hi_irq_handler handler, hi_u32 arg)
{
    if (irq != HI_IRQ_GPIO || !handler)
        return HI_ERR_FAILURE;
    g_handler = handler;
    g_handler_arg = arg;
    g_irq_word = 0;
    g_critical = 0;
    g_enabled = 1;

    hi_s64 status = a20_syscall6(VENDOR_IRQ_SUBSCRIBE, (hi_u32)(hi_size_t)&g_irq_word,
                                 0, 0, 0, 0, 0);
    if (status != A20_OK)
        return HI_ERR_FAILURE;

    if (!g_dispatcher) {
        const osThreadAttr_t attr = {
            .name = "hi_irq",
            .priority = osPriorityAboveNormal,
            .stack_size = IRQ_DISPATCH_STACK,
        };
        g_dispatcher = osThreadNew(irq_dispatcher, NULL, &attr);
        if (!g_dispatcher)
            return HI_ERR_FAILURE;
    }
    return HI_ERR_SUCCESS;
}

hi_u32 hi_irq_enable(hi_u32 irq)
{
    if (irq != HI_IRQ_GPIO)
        return HI_ERR_FAILURE;
    g_enabled = 1;
    (void)a20_syscall6(VENDOR_IRQ_ENABLE, 1, 0, 0, 0, 0, 0);
    return HI_ERR_SUCCESS;
}

hi_u32 hi_irq_disable(hi_u32 irq)
{
    if (irq != HI_IRQ_GPIO)
        return HI_ERR_FAILURE;
    g_enabled = 0;
    (void)a20_syscall6(VENDOR_IRQ_ENABLE, 0, 0, 0, 0, 0, 0);
    return HI_ERR_SUCCESS;
}

hi_u64 hi_int_lock(hi_void)
{
    hi_u64 previous = g_critical;
    g_critical = previous + 1;
    return previous;
}

hi_void hi_int_restore(hi_u64 flags)
{
    g_critical = (hi_u32)flags;
}

hi_u32 hi_irq_trigger(hi_u32 irq)
{
    if (irq != HI_IRQ_GPIO)
        return HI_ERR_FAILURE;
    return a20_syscall6(VENDOR_IRQ_RAISE, 0, 0, 0, 0, 0, 0) == A20_OK
               ? HI_ERR_SUCCESS : HI_ERR_FAILURE;
}
