/*
 * Hi3861 GPIO + pin-mux controller.
 *
 * Register layout comes from the vendor SDK's flashboot GPIO/IO drivers
 * (boot/flashboot/drivers/gpio and .../io) and the platform base map:
 *
 *   GPIO  0x50006000: SWPORT_DR 0x00, SWPORT_DDR 0x04, EXT_PORT 0x50
 *   IOCFG 0x5000A000: pin mux +0x604 + id*4, control +0x904 + id*4
 *
 * The driver binds through the A20OS platform bus; it never includes an arch
 * header and uses only the driver hardware API for register access.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/gpio/gpio.h"
#include "drivers/bus/platform_bus.h"
#include "core/errno.h"
#include "mm/slab.h"

#define HI3861_GPIO_BASE   0x50006000UL
#define HI3861_IOCFG_BASE  0x5000A000UL
#define HI3861_GPIO_PINS   15U

#define GPIO_SWPORT_DR     0x00U
#define GPIO_SWPORT_DDR    0x04U
#define GPIO_EXT_PORT      0x50U

#define IOCFG_MUX(id)      (0x604U + (id) * 4U)
#define IOCFG_CTRL(id)     (0x904U + (id) * 4U)
#define IOCFG_PULL_SHIFT   8U
#define IOCFG_PULL_MASK    0x3U
#define IOCFG_IE           (1U << 10)

struct hi3861_gpio {
    void *gpio;
    void *iocfg;
    gpio_chip_t chip;
};

static gpio_chip_t *g_gpio_chips[2];

int gpio_chip_add(gpio_chip_t *chip)
{
    if (!chip || !chip->ngpio || chip->base < 0)
        return -EINVAL;
    for (unsigned i = 0; i < 2; i++) {
        if (g_gpio_chips[i] == chip)
            return -EEXIST;
        if (!g_gpio_chips[i]) {
            g_gpio_chips[i] = chip;
            return 0;
        }
    }
    return -ENOSPC;
}

void gpio_chip_remove(gpio_chip_t *chip)
{
    for (unsigned i = 0; i < 2; i++)
        if (g_gpio_chips[i] == chip)
            g_gpio_chips[i] = NULL;
}

gpio_chip_t *gpio_chip_get(unsigned gpio)
{
    for (unsigned i = 0; i < 2; i++) {
        gpio_chip_t *chip = g_gpio_chips[i];
        if (chip && gpio >= (unsigned)chip->base &&
            gpio - (unsigned)chip->base < chip->ngpio)
            return chip;
    }
    return NULL;
}

int gpio_set_direction(unsigned gpio, int out)
{
    gpio_chip_t *chip = gpio_chip_get(gpio);
    if (!chip || !chip->set_direction)
        return -ENODEV;
    return chip->set_direction(chip, gpio - (unsigned)chip->base, out);
}

int gpio_get_direction(unsigned gpio)
{
    gpio_chip_t *chip = gpio_chip_get(gpio);
    if (!chip || !chip->get_direction)
        return -ENODEV;
    return chip->get_direction(chip, gpio - (unsigned)chip->base);
}

int gpio_set_value(unsigned gpio, int value)
{
    gpio_chip_t *chip = gpio_chip_get(gpio);
    if (!chip || !chip->set_value)
        return -ENODEV;
    return chip->set_value(chip, gpio - (unsigned)chip->base, value);
}

int gpio_get_value(unsigned gpio)
{
    gpio_chip_t *chip = gpio_chip_get(gpio);
    if (!chip || !chip->get_value)
        return -ENODEV;
    return chip->get_value(chip, gpio - (unsigned)chip->base);
}

int gpio_set_pull(unsigned gpio, unsigned pull)
{
    gpio_chip_t *chip = gpio_chip_get(gpio);
    if (!chip || !chip->set_pull)
        return -ENODEV;
    return chip->set_pull(chip, gpio - (unsigned)chip->base, pull);
}

int gpio_set_function(unsigned gpio, unsigned func)
{
    gpio_chip_t *chip = gpio_chip_get(gpio);
    if (!chip || !chip->set_function)
        return -ENODEV;
    return chip->set_function(chip, gpio - (unsigned)chip->base, func);
}

int gpio_set_input_enable(unsigned gpio, int enable)
{
    gpio_chip_t *chip = gpio_chip_get(gpio);
    if (!chip || !chip->set_input_enable)
        return -ENODEV;
    return chip->set_input_enable(chip, gpio - (unsigned)chip->base, enable);
}

static int hg_set_direction(gpio_chip_t *chip, unsigned off, int out)
{
    struct hi3861_gpio *g = chip->priv;
    uint16_t v = readw((volatile uint8_t *)g->gpio + GPIO_SWPORT_DDR);
    if (out)
        v |= (uint16_t)(1U << off);
    else
        v &= (uint16_t)~(1U << off);
    writew(v, (volatile uint8_t *)g->gpio + GPIO_SWPORT_DDR);
    return 0;
}

static int hg_get_direction(gpio_chip_t *chip, unsigned off)
{
    struct hi3861_gpio *g = chip->priv;
    uint16_t v = readw((volatile uint8_t *)g->gpio + GPIO_SWPORT_DDR);
    return (v & (1U << off)) ? GPIO_DIR_OUT : GPIO_DIR_IN;
}

static int hg_set_value(gpio_chip_t *chip, unsigned off, int value)
{
    struct hi3861_gpio *g = chip->priv;
    uint16_t v = readw((volatile uint8_t *)g->gpio + GPIO_SWPORT_DR);
    if (value)
        v |= (uint16_t)(1U << off);
    else
        v &= (uint16_t)~(1U << off);
    writew(v, (volatile uint8_t *)g->gpio + GPIO_SWPORT_DR);
    return 0;
}

static int hg_get_value(gpio_chip_t *chip, unsigned off)
{
    struct hi3861_gpio *g = chip->priv;
    uint16_t v = readw((volatile uint8_t *)g->gpio + GPIO_EXT_PORT);
    return (v & (1U << off)) ? 1 : 0;
}

static int hg_set_pull(gpio_chip_t *chip, unsigned off, unsigned pull)
{
    struct hi3861_gpio *g = chip->priv;
    if (pull > GPIO_PULL_DOWN)
        return -EINVAL;
    uint32_t v = readl((volatile uint8_t *)g->iocfg + IOCFG_CTRL(off));
    v &= ~(IOCFG_PULL_MASK << IOCFG_PULL_SHIFT);
    v |= (pull & IOCFG_PULL_MASK) << IOCFG_PULL_SHIFT;
    writel(v, (volatile uint8_t *)g->iocfg + IOCFG_CTRL(off));
    return 0;
}

static int hg_set_function(gpio_chip_t *chip, unsigned off, unsigned func)
{
    struct hi3861_gpio *g = chip->priv;
    if (func > 0xF)
        return -EINVAL;
    writel(func, (volatile uint8_t *)g->iocfg + IOCFG_MUX(off));
    return 0;
}

static int hg_set_input_enable(gpio_chip_t *chip, unsigned off, int enable)
{
    struct hi3861_gpio *g = chip->priv;
    uint32_t v = readl((volatile uint8_t *)g->iocfg + IOCFG_CTRL(off));
    if (enable)
        v |= IOCFG_IE;
    else
        v &= ~IOCFG_IE;
    writel(v, (volatile uint8_t *)g->iocfg + IOCFG_CTRL(off));
    return 0;
}

static int hi3861_gpio_probe(device_t *dev)
{
    struct hi3861_gpio *g = kcalloc(1, sizeof(*g));
    if (!g)
        return -ENOMEM;
    g->gpio = device_map_mmio(dev, 0);
    g->iocfg = device_map_mmio(dev, 1);
    if (!g->gpio || !g->iocfg) {
        kfree(g);
        return -ENODEV;
    }
    gpio_chip_t *chip = &g->chip;
    chip->name = "hi3861-gpio";
    chip->base = 0;
    chip->ngpio = HI3861_GPIO_PINS;
    chip->set_direction = hg_set_direction;
    chip->get_direction = hg_get_direction;
    chip->set_value = hg_set_value;
    chip->get_value = hg_get_value;
    chip->set_pull = hg_set_pull;
    chip->set_function = hg_set_function;
    chip->set_input_enable = hg_set_input_enable;
    chip->priv = g;
    int ret = gpio_chip_add(chip);
    if (ret) {
        kfree(g);
        return ret;
    }
    dev->drv_priv = g;
    return 0;
}

static int hi3861_gpio_remove(device_t *dev)
{
    struct hi3861_gpio *g = dev->drv_priv;
    if (!g)
        return 0;
    gpio_chip_remove(&g->chip);
    kfree(g);
    dev->drv_priv = NULL;
    return 0;
}

static const firmware_device_id_t hi3861_gpio_ids[] = {
    {.id = "hisilicon,hi3861-gpio"},
    {.id = NULL},
};

static const driver_t hi3861_gpio_driver = {
    .name = "hi3861-gpio",
    .bus = &platform_bus,
    .firmware_ids = hi3861_gpio_ids,
    .probe = hi3861_gpio_probe,
    .remove = hi3861_gpio_remove,
};

DRIVER_REGISTER(hi3861_gpio_driver);
