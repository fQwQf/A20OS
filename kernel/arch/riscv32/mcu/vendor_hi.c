/*
 * Hi3861 vendor-extension surface (non-standard).
 *
 * This file is deliberately outside both the Native syscall table and the
 * CMSIS personality.  The standard ABI (0x0000..0x0FFF) is generated from
 * the manifest; the vendor calls live in their own range (0x7F00..) and are
 * only reachable by vendor code in user/port/hi3861_hal/.
 *
 * The interrupt path is the useful part: a user program subscribes a futex
 * word, and a raised vendor IRQ increments and wakes that word.  The user
 * side dispatcher (user/port/hi3861_hal/src/hi_isr.c) then runs the handler
 * in a normal CMSIS thread, which is the deferral model a user driver has to
 * use on an NOMMU part.  On QEMU the "pin edge" is produced by the vendor
 * trigger call; on a wired Hi3861 the same raise path is driven by the real
 * GPIO interrupt handler.
 */
#ifdef CONFIG_MCU_NATIVE_PROGRAM
#include "core/types.h"
#include "core/errno.h"
#include "core/arch.h"
#include "proc/proc.h"
#include "trap_frame.h"
#include "program.h"
#include "sys/futex.h"
#include "abi/native/errno.h"

#define HI_VENDOR_IRQ_SUBSCRIBE 0x7F00
#define HI_VENDOR_IRQ_ENABLE    0x7F01
#define HI_VENDOR_IRQ_RAISE     0x7F02
#define HI_VENDOR_GPIO_CONFIG   0x7F03
#define HI_VENDOR_GPIO_WRITE    0x7F04
#define HI_VENDOR_PWM_START     0x7F05

static uintptr_t g_irq_word;
static uint32_t g_irq_enabled;
static uint32_t g_gpio_dir[32];
static uint32_t g_gpio_value[32];
static uint32_t g_pwm_duty[16];
static uint32_t g_pwm_freq[16];

static int64_t vendor_subscribe(uintptr_t word)
{
    if (!word || (word & 3U) || !mcu_program_user_range((void *)word, 4))
        return -A20_ERR_FAULT;
    task_t *task = proc_current();
    if (!task || !task->flat_user)
        return -A20_ERR_BAD_HANDLE;
    g_irq_word = word;
    g_irq_enabled = 1;
    return A20_OK;
}

static int64_t vendor_raise(void)
{
    if (!g_irq_word || !g_irq_enabled)
        return A20_OK;
    if (!mcu_program_user_range((void *)g_irq_word, 4))
        return -A20_ERR_FAULT;
    __atomic_add_fetch((uint32_t *)g_irq_word, 1, __ATOMIC_SEQ_CST);
    (void)futex_wake_user((int *)g_irq_word, 0x7FFFFFFF);
    return A20_OK;
}

int64_t riscv_hi_vendor_call(uint64_t nr, trap_context_t *ctx)
{
    uint64_t a0 = TRAP_CTX_ARG0(ctx);
    uint64_t a1 = TRAP_CTX_ARG1(ctx);
    uint64_t a2 = TRAP_CTX_ARG2(ctx);
    switch (nr) {
    case HI_VENDOR_IRQ_SUBSCRIBE:
        return vendor_subscribe((uintptr_t)a0);
    case HI_VENDOR_IRQ_ENABLE:
        g_irq_enabled = a0 ? 1U : 0U;
        return A20_OK;
    case HI_VENDOR_IRQ_RAISE:
        return vendor_raise();
    case HI_VENDOR_GPIO_CONFIG:
        if (a0 >= 32)
            return -A20_ERR_INVALID_ARGUMENT;
        g_gpio_dir[a0] = (uint32_t)a1;
        return A20_OK;
    case HI_VENDOR_GPIO_WRITE:
        if (a0 >= 32)
            return -A20_ERR_INVALID_ARGUMENT;
        g_gpio_value[a0] = a1 ? 1U : 0U;
        return A20_OK;
    case HI_VENDOR_PWM_START:
        if (a0 >= 16)
            return -A20_ERR_INVALID_ARGUMENT;
        g_pwm_duty[a0] = (uint32_t)a1;
        g_pwm_freq[a0] = (uint32_t)a2;
        return A20_OK;
    default:
        return -A20_ERR_NOT_SUPPORTED;
    }
}

int riscv_hi_vendor_is_call(uint64_t nr)
{
    return nr >= 0x7F00 && nr < 0x7F10;
}
#endif
