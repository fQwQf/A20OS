/*
 * Hi3861 platform devices.
 *
 * The board publishes resources only; register offsets and sequencing live in
 * kernel/drivers/<class>/hi3861_<device>.c.  This keeps the standard split:
 * topology, driver = programming, driver core = matching/probe.
 *
 * All peripherals share the CRG/clock block (0x40010000) as a second resource
 * so the drivers can gate their own clock without an arch header.
 */
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "core/stdio.h"

#define HI3861_GPIO_BASE  0x50006000ULL
#define HI3861_GPIO_SIZE  0x100ULL
#define HI3861_IOCFG_BASE 0x5000A000ULL
#define HI3861_IOCFG_SIZE 0x1000ULL
#define HI3861_CRG_BASE   0x40010000ULL
#define HI3861_CRG_SIZE   0x100ULL

#define HI3861_I2C0_BASE  0x40018000ULL
#define HI3861_I2C1_BASE  0x40019000ULL
#define HI3861_I2C_SIZE   0x100ULL
#define HI3861_SPI0_BASE  0x40058000ULL
#define HI3861_SPI1_BASE  0x40059000ULL
#define HI3861_SPI_SIZE   0x100ULL
#define HI3861_PWM_BASE   0x40040000ULL
#define HI3861_PWM_SIZE   0x600ULL
#define HI3861_ADC_BASE   0x40070000ULL
#define HI3861_ADC_SIZE   0x100ULL

static resource_t gpio_res[] = {
    {.type = RES_MMIO, .start = HI3861_GPIO_BASE,
     .end = HI3861_GPIO_BASE + HI3861_GPIO_SIZE - 1, .name = "gpio"},
    {.type = RES_MMIO, .start = HI3861_IOCFG_BASE,
     .end = HI3861_IOCFG_BASE + HI3861_IOCFG_SIZE - 1, .name = "iocfg"},
};
static resource_t i2c0_res[] = {
    {.type = RES_MMIO, .start = HI3861_I2C0_BASE,
     .end = HI3861_I2C0_BASE + HI3861_I2C_SIZE - 1, .name = "i2c0"},
    {.type = RES_MMIO, .start = HI3861_CRG_BASE,
     .end = HI3861_CRG_BASE + HI3861_CRG_SIZE - 1, .name = "crg"},
};
static resource_t i2c1_res[] = {
    {.type = RES_MMIO, .start = HI3861_I2C1_BASE,
     .end = HI3861_I2C1_BASE + HI3861_I2C_SIZE - 1, .name = "i2c1"},
    {.type = RES_MMIO, .start = HI3861_CRG_BASE,
     .end = HI3861_CRG_BASE + HI3861_CRG_SIZE - 1, .name = "crg"},
};
static resource_t spi0_res[] = {
    {.type = RES_MMIO, .start = HI3861_SPI0_BASE,
     .end = HI3861_SPI0_BASE + HI3861_SPI_SIZE - 1, .name = "spi0"},
    {.type = RES_MMIO, .start = HI3861_CRG_BASE,
     .end = HI3861_CRG_BASE + HI3861_CRG_SIZE - 1, .name = "crg"},
};
static resource_t spi1_res[] = {
    {.type = RES_MMIO, .start = HI3861_SPI1_BASE,
     .end = HI3861_SPI1_BASE + HI3861_SPI_SIZE - 1, .name = "spi1"},
    {.type = RES_MMIO, .start = HI3861_CRG_BASE,
     .end = HI3861_CRG_BASE + HI3861_CRG_SIZE - 1, .name = "crg"},
};
static resource_t pwm_res[] = {
    {.type = RES_MMIO, .start = HI3861_PWM_BASE,
     .end = HI3861_PWM_BASE + HI3861_PWM_SIZE - 1, .name = "pwm"},
    {.type = RES_MMIO, .start = HI3861_CRG_BASE,
     .end = HI3861_CRG_BASE + HI3861_CRG_SIZE - 1, .name = "crg"},
};
static resource_t adc_res[] = {
    {.type = RES_MMIO, .start = HI3861_ADC_BASE,
     .end = HI3861_ADC_BASE + HI3861_ADC_SIZE - 1, .name = "adc"},
    {.type = RES_MMIO, .start = HI3861_CRG_BASE,
     .end = HI3861_CRG_BASE + HI3861_CRG_SIZE - 1, .name = "crg"},
};

#define HI3861_PDEV(name_, id_, res_)                                        \
    static platform_device_t name_ = {                                       \
        .dev = {.name = #name_,                                             \
                .software_id = id_,                                         \
                .res = res_,                                                \
                .res_count = (int)(sizeof(res_) / sizeof((res_)[0]))}}

HI3861_PDEV(hi3861_gpio, "hisilicon,hi3861-gpio", gpio_res);
HI3861_PDEV(hi3861_i2c0, "hisilicon,hi3861-i2c0", i2c0_res);
HI3861_PDEV(hi3861_i2c1, "hisilicon,hi3861-i2c1", i2c1_res);
HI3861_PDEV(hi3861_spi0, "hisilicon,hi3861-spi0", spi0_res);
HI3861_PDEV(hi3861_spi1, "hisilicon,hi3861-spi1", spi1_res);
HI3861_PDEV(hi3861_pwm, "hisilicon,hi3861-pwm", pwm_res);
HI3861_PDEV(hi3861_adc, "hisilicon,hi3861-adc", adc_res);

static platform_device_t *const devices[] = {
    &hi3861_gpio, &hi3861_i2c0, &hi3861_i2c1, &hi3861_spi0,
    &hi3861_spi1, &hi3861_pwm, &hi3861_adc,
};

void hi3861_enumerate_devices(void)
{
    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        int ret = platform_device_register(devices[i]);
        if (ret)
            printf("[DRV] %s register failed: %d\n", devices[i]->dev.name,
                   ret);
    }
}
