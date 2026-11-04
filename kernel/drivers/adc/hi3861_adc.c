/*
 * Hi3861 LSADC.
 *
 * Ported from platform/drivers/adc/hi_adc.c: the SDK configures the clock
 * divider, powers the block on, programs a channel bitmap plus averaging and
 * bias fields, starts a scan and drains the FIFO.  The A20OS version polls the
 * same registers through driver_hwapi and returns the raw 12-bit code.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/adc/adc.h"
#include "core/errno.h"
#include "core/sync.h"
#include "mm/slab.h"

#define ADC_CFG       0x00U
#define ADC_IMSC      0x08U
#define ADC_SR        0x10U
#define ADC_START     0x1CU
#define ADC_STOP      0x20U
#define ADC_DR        0x24U
#define ADC_EN        0x2CU

#define ADC_SR_RNE    (1U << 0)
#define ADC_DATA_MASK 0xFFFU
#define ADC_CH_SHIFT  12U
#define ADC_TIMEOUT   500U

struct hi3861_adc {
    void *base;
    void *crg;
    mutex_t lock;
    adc_controller_t ctrl;
};

static adc_controller_t *g_adc;

int adc_controller_add(adc_controller_t *ctrl)
{
    if (!ctrl || !ctrl->read || !ctrl->nchannels)
        return -EINVAL;
    if (g_adc)
        return -EEXIST;
    g_adc = ctrl;
    return 0;
}

void adc_controller_remove(adc_controller_t *ctrl)
{
    if (g_adc == ctrl)
        g_adc = NULL;
}

adc_controller_t *adc_controller_get(void)
{
    return g_adc;
}

int adc_read_raw(unsigned channel, uint16_t *raw)
{
    if (!g_adc || !g_adc->read)
        return -ENODEV;
    return g_adc->read(g_adc, channel, raw);
}

uint32_t adc_raw_to_mv(uint16_t raw)
{
    /* code = voltage / 4 / 1.8 V * 4096 -> voltage = code * 7200 / 4096 */
    return ((uint32_t)raw * 7200U) / 4096U;
}

static void adc_clock(struct hi3861_adc *a)
{
    /* LS_ADC_CLK_DIV1: CLDO_CTL_CLK_DIV1_REG bits 9..12; 7 for a 24 MHz
     * crystal, 0xC for 40 MHz.  The Hi3861 boards in tree are 24 MHz. */
    uint16_t div = *(volatile uint16_t *)((uint8_t *)a->crg + 0x78);
    div &= (uint16_t)~(0xFU << 9);
    div |= (uint16_t)(0x7U << 9);
    *(volatile uint16_t *)((uint8_t *)a->crg + 0x78) = div;
}

static int ha_read(adc_controller_t *ctrl, unsigned channel, uint16_t *raw)
{
    struct hi3861_adc *a = ctrl->priv;
    if (!raw || channel > 6)
        return -EINVAL;

    adc_clock(a);
    mutex_lock(&a->lock);
    writel(0, (volatile uint8_t *)a->base + ADC_EN);      /* power on */
    uint32_t cfg = (1U << channel) | (0U << 8) | (0xFU << 12) | (0U << 24);
    writel(cfg, (volatile uint8_t *)a->base + ADC_CFG);
    writel(1, (volatile uint8_t *)a->base + ADC_START);

    int rc = -ETIMEDOUT;
    for (uint32_t t = 0; t < ADC_TIMEOUT; t++) {
        uint32_t sr = readl((volatile uint8_t *)a->base + ADC_SR);
        if (sr & ADC_SR_RNE) {
            uint32_t v = readl((volatile uint8_t *)a->base + ADC_DR);
            if (((v >> ADC_CH_SHIFT) & 0x7U) == channel) {
                *raw = (uint16_t)(v & ADC_DATA_MASK);
                rc = 0;
                break;
            }
        }
        udelay(5);
    }

    writel(1, (volatile uint8_t *)a->base + ADC_STOP);
    writel(0, (volatile uint8_t *)a->base + ADC_IMSC);
    for (unsigned i = 0; i < 32; i++)
        if (!(readl((volatile uint8_t *)a->base + ADC_SR) & ADC_SR_RNE))
            break;
        else
            (void)readl((volatile uint8_t *)a->base + ADC_DR);
    writel(1, (volatile uint8_t *)a->base + ADC_EN);      /* power off */
    mutex_unlock(&a->lock);
    return rc;
}

static int hi3861_adc_probe(device_t *dev)
{
    struct hi3861_adc *a = kcalloc(1, sizeof(*a));
    if (!a)
        return -ENOMEM;
    a->base = device_map_mmio(dev, 0);
    a->crg = device_map_mmio(dev, 1);
    if (!a->base || !a->crg) {
        kfree(a);
        return -ENODEV;
    }
    mutex_init(&a->lock);
    a->ctrl.name = "hi3861-adc";
    a->ctrl.nchannels = 7;
    a->ctrl.read = ha_read;
    a->ctrl.priv = a;
    int ret = adc_controller_add(&a->ctrl);
    if (ret) {
        kfree(a);
        return ret;
    }
    dev->drv_priv = a;
    return 0;
}

static int hi3861_adc_remove(device_t *dev)
{
    struct hi3861_adc *a = dev->drv_priv;
    if (!a)
        return 0;
    adc_controller_remove(&a->ctrl);
    kfree(a);
    dev->drv_priv = NULL;
    return 0;
}

static const firmware_device_id_t hi3861_adc_ids[] = {
    {.id = "hisilicon,hi3861-adc"},
    {.id = NULL},
};

static const driver_t hi3861_adc_driver = {
    .name = "hi3861-adc",
    .bus = &platform_bus,
    .firmware_ids = hi3861_adc_ids,
    .probe = hi3861_adc_probe,
    .remove = hi3861_adc_remove,
};

DRIVER_REGISTER(hi3861_adc_driver);
