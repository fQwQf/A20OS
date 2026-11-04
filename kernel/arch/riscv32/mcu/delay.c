/*
 * Hi3861 microsecond delay.
 *
 * Timer 2 is the scheduler tick.  Timer 1 is unused by A20OS and its 24 MHz
 * free-running counter gives drivers a real microsecond reference for the
 * polled I2C/SPI/ADC timeouts, without touching the tick or the system clock.
 *
 * The SDK gates timer 1 through CLDO_CTL_CLKEN bit 7; timer 0 has no gate in
 * the SDK and is why the first A20OS bring-up used timer 2 for the tick.
 */
#include "core/cpu.h"
#include "core/types.h"

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define TIMER1_BASE       0x40050014UL
#define TIMER_LOAD        0x00UL
#define TIMER_CURRENT     0x04UL
#define TIMER_CONTROL     0x08UL
#define TIMER_CTL_FREE_RUN 0x1UL

#define CLDO_CTL_CLKEN    0x40010030UL
#define CLDO_TIMER1_CLK   (1U << 7)
#define TIMER1_HZ         24000000UL

static int timer1_ready;

static void timer1_init(void)
{
    if (timer1_ready)
        return;
    uint16_t gate = *(volatile uint16_t *)(uintptr_t)CLDO_CTL_CLKEN;
    *(volatile uint16_t *)(uintptr_t)CLDO_CTL_CLKEN =
        (uint16_t)(gate | CLDO_TIMER1_CLK);
    REG(TIMER1_BASE + TIMER_CONTROL) = 0;
    REG(TIMER1_BASE + TIMER_LOAD) = 0;
    REG(TIMER1_BASE + TIMER_CONTROL) = TIMER_CTL_FREE_RUN;
    timer1_ready = 1;
}

static uint32_t timer1_read(void)
{
    return REG(TIMER1_BASE + TIMER_CURRENT);
}

void udelay(unsigned usecs)
{
    if (!usecs)
        return;
    timer1_init();
    uint32_t start = timer1_read();
    /* 32-bit counter at 24 MHz wraps every ~179 s; unsigned subtraction keeps
     * a wait short enough to stay correct across one wrap. */
    uint32_t wait = usecs > (0xFFFFFFFFUL / TIMER1_HZ)
                        ? 0xFFFFFFFFUL : usecs * TIMER1_HZ / 1000000UL;
    if (!wait)
        wait = 1;
    while ((uint32_t)(timer1_read() - start) < wait)
        arch_cpu_relax();
}

void mdelay(unsigned msecs)
{
    while (msecs--)
        udelay(1000);
}
