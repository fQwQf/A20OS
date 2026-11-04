/*
 * Hi3861 SPI (PL022-compatible) polled controller.
 *
 * Ported from the vendor SDK's block-transfer path (spi_transfer_8bits_block /
 * spi_check_write_timeout).  The SDK's event/DMA engine is intentionally not
 * ported yet; a polled transfer is enough for register-style devices and is
 * the part that can be checked with a MOSI-MISO loopback.
 *
 * SPI0 uses GPIO5..8 (CSN/CK/RXD/TXD), SPI1 uses GPIO0..3.  Pin mux is a
 * pinctrl decision and is left to the board/consumer, as in the SDK demo.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/spi/spi.h"
#include "core/errno.h"
#include "core/sync.h"
#include "mm/slab.h"

#define SPI_CR0      0x00U
#define SPI_CR1      0x04U
#define SPI_DR       0x08U
#define SPI_SR       0x0CU
#define SPI_CPSR     0x10U
#define SPI_TXFIFOCR 0x28U
#define SPI_RXFIFOCR 0x2CU

#define SPI_SR_TFE (1U << 0)
#define SPI_SR_TNF (1U << 1)
#define SPI_SR_RNE (1U << 2)
#define SPI_SR_BSY (1U << 4)

#define SPI_CR1_SSE (1U << 1)

#define SPI_CLK_HZ 160000000UL
#define SPI_TIMEOUT_US 1000000U
#define SPI_CHUNK 32U

struct hi3861_spi {
    void *base;
    void *crg;
    int bus;
    mutex_t lock;
    spi_controller_t ctrl;
};

static spi_controller_t *g_spi[2];

int spi_controller_add(spi_controller_t *ctrl)
{
    if (!ctrl || ctrl->bus < 0 || ctrl->bus >= 2 || !ctrl->transfer)
        return -EINVAL;
    if (g_spi[ctrl->bus])
        return -EEXIST;
    g_spi[ctrl->bus] = ctrl;
    return 0;
}

void spi_controller_remove(spi_controller_t *ctrl)
{
    if (ctrl && ctrl->bus >= 0 && ctrl->bus < 2 &&
        g_spi[ctrl->bus] == ctrl)
        g_spi[ctrl->bus] = NULL;
}

spi_controller_t *spi_controller_get(int bus)
{
    return (bus >= 0 && bus < 2) ? g_spi[bus] : NULL;
}

int spi_transfer(int bus, const void *tx, void *rx, size_t len, uint32_t hz,
                 uint8_t mode)
{
    spi_controller_t *c = spi_controller_get(bus);
    if (!c || !len || (!tx && !rx))
        return -ENODEV;
    return c->transfer(c, tx, rx, len, hz, mode);
}

static int spi_wait(struct hi3861_spi *s, uint32_t mask, uint32_t want)
{
    for (uint32_t t = 0; t < SPI_TIMEOUT_US; t++) {
        uint32_t sr = readw((volatile uint8_t *)s->base + SPI_SR);
        if ((sr & mask) == want)
            return 0;
        udelay(1);
    }
    return -ETIMEDOUT;
}

static void spi_flush(struct hi3861_spi *s)
{
    for (unsigned i = 0; i < 256; i++) {
        uint32_t sr = readw((volatile uint8_t *)s->base + SPI_SR);
        if (!(sr & SPI_SR_RNE))
            break;
        (void)readw((volatile uint8_t *)s->base + SPI_DR);
    }
}

static int spi_configure(struct hi3861_spi *s, uint32_t hz, uint8_t mode)
{
    if (!hz)
        hz = 1000000;
    if (hz > 40000000)
        hz = 40000000;
    if (hz < 2469)
        hz = 2469;

    uint32_t div = SPI_CLK_HZ / hz;
    uint32_t cpsdvsr = div & ~1U;
    if (cpsdvsr < 4)
        cpsdvsr = 4;
    if (cpsdvsr > 254)
        cpsdvsr = 254;
    uint32_t scr = div / cpsdvsr;
    if (scr)
        scr--;
    if (scr > 255)
        scr = 255;

    /* 8-bit frame: DSS = 7, Motorola frame, CPOL/CPHA from mode. */
    uint16_t cr0 = (uint16_t)(7U | ((mode & SPI_MODE_CPOL) ? (1U << 6) : 0) |
                              ((mode & SPI_MODE_CPHA) ? (1U << 7) : 0) |
                              (scr << 8));
    writew(cr0, (volatile uint8_t *)s->base + SPI_CR0);
    writew(SPI_CR1_SSE, (volatile uint8_t *)s->base + SPI_CR1);
    writew((uint16_t)cpsdvsr, (volatile uint8_t *)s->base + SPI_CPSR);
    writew(0, (volatile uint8_t *)s->base + SPI_TXFIFOCR);
    writew(0, (volatile uint8_t *)s->base + SPI_RXFIFOCR);
    return 0;
}

static int hi3861_spi_transfer(spi_controller_t *ctrl, const void *tx,
                               void *rx, size_t len, uint32_t hz, uint8_t mode)
{
    struct hi3861_spi *s = ctrl->priv;
    const uint8_t *txb = tx;
    uint8_t *rxb = rx;

    mutex_lock(&s->lock);
    spi_configure(s, hz, mode);
    spi_flush(s);

    size_t left = len;
    while (left) {
        size_t chunk = left > SPI_CHUNK ? SPI_CHUNK : left;
        for (size_t i = 0; i < chunk; i++) {
            uint8_t v = txb ? txb[i] : 0xFF;
            writew(v, (volatile uint8_t *)s->base + SPI_DR);
        }
        int rc = spi_wait(s, SPI_SR_TFE | SPI_SR_BSY, SPI_SR_TFE);
        if (rc) {
            mutex_unlock(&s->lock);
            return rc;
        }
        for (size_t i = 0; i < chunk; i++) {
            uint8_t v = (uint8_t)readw((volatile uint8_t *)s->base + SPI_DR);
            if (rxb)
                rxb[i] = v;
        }
        if (txb)
            txb += chunk;
        if (rxb)
            rxb += chunk;
        left -= chunk;
    }
    mutex_unlock(&s->lock);
    return 0;
}

static int hi3861_spi_probe(device_t *dev)
{
    struct hi3861_spi *s = kcalloc(1, sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->base = device_map_mmio(dev, 0);
    s->crg = device_map_mmio(dev, 1);
    if (!s->base || !s->crg) {
        kfree(s);
        return -ENODEV;
    }
    s->bus = dev->firmware_match
                 ? (int)dev->firmware_match->driver_data : 0;
    if (s->bus < 0 || s->bus > 1)
        s->bus = 0;

    uint16_t gate = *(volatile uint16_t *)((uint8_t *)s->crg + 0x30);
    *(volatile uint16_t *)((uint8_t *)s->crg + 0x30) =
        (uint16_t)(gate | (1U << (s->bus ? 0 : 5)));
    /* SSP0 reset bit 7, SPI1 (SSP2) reset bit 5. */
    uint16_t reset_bit = s->bus ? (1U << 5) : (1U << 7);
    uint16_t rst = *(volatile uint16_t *)((uint8_t *)s->crg + 0x20);
    *(volatile uint16_t *)((uint8_t *)s->crg + 0x20) =
        (uint16_t)(rst & ~reset_bit);
    *(volatile uint16_t *)((uint8_t *)s->crg + 0x20) =
        (uint16_t)(rst | reset_bit);

    mutex_init(&s->lock);
    spi_configure(s, 1000000, 0);
    s->ctrl.name = s->bus ? "hi3861-spi1" : "hi3861-spi0";
    s->ctrl.bus = s->bus;
    s->ctrl.transfer = hi3861_spi_transfer;
    s->ctrl.priv = s;
    int ret = spi_controller_add(&s->ctrl);
    if (ret) {
        kfree(s);
        return ret;
    }
    dev->drv_priv = s;
    return 0;
}

static int hi3861_spi_remove(device_t *dev)
{
    struct hi3861_spi *s = dev->drv_priv;
    if (!s)
        return 0;
    spi_controller_remove(&s->ctrl);
    kfree(s);
    dev->drv_priv = NULL;
    return 0;
}

static const firmware_device_id_t hi3861_spi_ids[] = {
    {.id = "hisilicon,hi3861-spi0", .driver_data = 0},
    {.id = "hisilicon,hi3861-spi1", .driver_data = 1},
    {.id = NULL},
};

static const driver_t hi3861_spi_driver = {
    .name = "hi3861-spi",
    .bus = &platform_bus,
    .firmware_ids = hi3861_spi_ids,
    .probe = hi3861_spi_probe,
    .remove = hi3861_spi_remove,
};

DRIVER_REGISTER(hi3861_spi_driver);
