/*
 * Hi3861 interrupt-controller extension (vendor, non-standard).
 *
 * The API shape follows the Hi3861 SDK's hi_isr.h.  On A20OS the handler is
 * not run in exception context: a subscriber thread owns the futex word that
 * the kernel vendor module raises, so the handler runs in a CMSIS thread and
 * may call osDelay/osSemaphore.  A real ISR may do neither, which is exactly
 * why a user-space driver on this NOMMU part must defer.
 */
#ifndef HI_ISR_H
#define HI_ISR_H

#include "hi_types_base.h"

typedef hi_void (*hi_irq_handler)(hi_u32 irq, hi_u32 arg);

/* One vendor IRQ line is enough for the teaching profile. */
#define HI_IRQ_GPIO 0U

hi_u32 hi_irq_request(hi_u32 irq, hi_irq_handler handler, hi_u32 arg);
hi_u32 hi_irq_enable(hi_u32 irq);
hi_u32 hi_irq_disable(hi_u32 irq);
hi_u64 hi_int_lock(hi_void);
hi_void hi_int_restore(hi_u64 flags);

/* Vendor test hook: produce the edge that a wired pin would generate.
 * Kept out of CMSIS on purpose; the experiment uses it on QEMU where the
 * machine has no GPIO block. */
hi_u32 hi_irq_trigger(hi_u32 irq);

#endif /* HI_ISR_H */
