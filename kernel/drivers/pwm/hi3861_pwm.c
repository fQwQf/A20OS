/*
 * Hi3861 PWM controller (6 channels).
 *
 * Register map and clock gates are taken from the vendor SDK's
 * platform/drivers/pwm (PWM base 0x40040000 + n*0x100; CLDO_CTL_CLKEN1
 * bits 0..4 for PWM0..4, bit 10 for PWM5, plus the PWM bus/main gates).
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/pwm/pwm.h"
#include "core/errno.h"
#include "core/string.h"
#include "mm/slab.h"

#define PWM_EN    0x00U
#define PWM_START 0x04U
#define PWM_FREQ  0x08U
#define PWM_DUTY  0x0CU
#define PWM_STRIDE 0x100U

#define CLKEN1_PWM_MAIN (1U << 5)
#define CLKEN1_PWM_BUS  (1U << 6)
#define CLKEN1_PWM5     (1U << 10)

struct hi3861_pwm {
    void *base;
    void *crg;
    pwm_chip_t chip;
};

static pwm_chip_t *g_pwm;

int pwm_chip_add(pwm_chip_t *chip)
{
    if (!chip || !chip->npwm)
        return -EINVAL;
    if (g_pwm)
        return -EEXIST;
    g_pwm = chip;
    return 0;
}

void pwm_chip_remove(pwm_chip_t *chip)
{
    if (g_pwm == chip)
        g_pwm = NULL;
}

pwm_chip_t *pwm_chip_get(unsigned channel)
{
    if (!g_pwm || channel < (unsigned)g_pwm->base ||
        channel - (unsigned)g_pwm->base >= g_pwm->npwm)
        return NULL;
    return g_pwm;
}

int pwm_start(unsigned channel, uint16_t duty, uint16_t period)
{
    pwm_chip_t *chip = pwm_chip_get(channel);
    if (!chip || !chip->start)
        return -ENODEV;
    return chip->start(chip, channel - (unsigned)chip->base, duty, period);
}

int pwm_stop(unsigned channel)
{
    pwm_chip_t *chip = pwm_chip_get(channel);
    if (!chip || !chip->stop)
        return -ENODEV;
    return chip->stop(chip, channel - (unsigned)chip->base);
}

static void pwm_gate(struct hi3861_pwm *p, unsigned ch, int on)
{
    uint16_t bit = ch == 5 ? CLKEN1_PWM5 : (uint16_t)(1U << ch);
    uint16_t v = *(volatile uint16_t *)((uint8_t *)p->crg + 0x34);
    if (on)
        v |= bit | CLKEN1_PWM_BUS | CLKEN1_PWM_MAIN;
    else
        v &= (uint16_t)~(bit);
    *(volatile uint16_t *)((uint8_t *)p->crg + 0x34) = v;
}

static int hp_start(pwm_chip_t *chip, unsigned ch, uint16_t duty,
                    uint16_t period)
{
    if (ch >= 6 || duty == 0 || period == 0 || duty > period)
        return -EINVAL;
    struct hi3861_pwm *p = chip->priv;
    void *base = (uint8_t *)p->base + ch * PWM_STRIDE;
    pwm_gate(p, ch, 1);
    writew(1, (volatile uint8_t *)base + PWM_EN);
    writew(period, (volatile uint8_t *)base + PWM_FREQ);
    writew(duty, (volatile uint8_t *)base + PWM_DUTY);
    writew(1, (volatile uint8_t *)base + PWM_START);
    return 0;
}

static int hp_stop(pwm_chip_t *chip, unsigned ch)
{
    if (ch >= 6)
        return -EINVAL;
    struct hi3861_pwm *p = chip->priv;
    void *base = (uint8_t *)p->base + ch * PWM_STRIDE;
    writew(0, (volatile uint8_t *)base + PWM_EN);
    writew(1, (volatile uint8_t *)base + PWM_START);
    return 0;
}

static int hi3861_pwm_probe(device_t *dev)
{
    struct hi3861_pwm *p = kcalloc(1, sizeof(*p));
    if (!p)
        return -ENOMEM;
    p->base = device_map_mmio(dev, 0);
    p->crg = device_map_mmio(dev, 1);
    if (!p->base || !p->crg) {
        kfree(p);
        return -ENODEV;
    }
    /* Default to the 160 MHz APB source (CLDO_CTL_CLK_SEL bit 0 = 0). */
    uint16_t sel = *(volatile uint16_t *)((uint8_t *)p->crg + 0x38);
    *(volatile uint16_t *)((uint8_t *)p->crg + 0x38) =
        (uint16_t)(sel & ~1U);
    p->chip.name = "hi3861-pwm";
    p->chip.base = 0;
    p->chip.npwm = 6;
    p->chip.start = hp_start;
    p->chip.stop = hp_stop;
    p->chip.priv = p;
    int ret = pwm_chip_add(&p->chip);
    if (ret) {
        kfree(p);
        return ret;
    }
    dev->drv_priv = p;
    return 0;
}

static int hi3861_pwm_remove(device_t *dev)
{
    struct hi3861_pwm *p = dev->drv_priv;
    if (!p)
        return 0;
    pwm_chip_remove(&p->chip);
    kfree(p);
    dev->drv_priv = NULL;
    return 0;
}

static const firmware_device_id_t hi3861_pwm_ids[] = {
    {.id = "hisilicon,hi3861-pwm"},
    {.id = NULL},
};

static const driver_t hi3861_pwm_driver = {
    .name = "hi3861-pwm",
    .bus = &platform_bus,
    .firmware_ids = hi3861_pwm_ids,
    .probe = hi3861_pwm_probe,
    .remove = hi3861_pwm_remove,
};

DRIVER_REGISTER(hi3861_pwm_driver);
