/*
 * Hi3861 I2C controller.
 *
 * Polled port of the vendor SDK platform/drivers/i2c core.  The SDK uses a
 * semaphore plus a task-context API; here the adapter owns a mutex and exposes
 * the generic i2c_msg transfer shape.  7-bit addressing is implemented; a
 * 10-bit address is rejected instead of being silently mistranslated.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/i2c/i2c.h"
#include "core/errno.h"
#include "core/sync.h"
#include "mm/slab.h"

#define I2C_CTRL      0x000U
#define I2C_COM       0x004U
#define I2C_ICR       0x008U
#define I2C_SR        0x00CU
#define I2C_SCL_H     0x010U
#define I2C_SCL_L     0x014U
#define I2C_TXR       0x018U
#define I2C_RXR       0x01CU

#define I2C_IP_ENABLE       (1U << 8)
#define I2C_UNMASK_ALL      (1U << 7)
#define I2C_UNMASK_ACK      (1U << 2)
#define I2C_UNMASK_ARBITRATE (1U << 1)
#define I2C_UNMASK_OVER     (1U << 0)

#define I2C_START  (1U << 3)
#define I2C_READ   (1U << 2)
#define I2C_WRITE  (1U << 1)
#define I2C_STOP   (1U << 0)
#define I2C_NACK    (1U << 4)

#define I2C_INT_DONE (1U << 0)
#define I2C_ACK_INTR (1U << 2)
#define I2C_CLEAR_ALL 0x1FFU
#define I2C_CLEAR_OVER (1U << 0)
#define I2C_START_END ((1U << 6) | I2C_INT_DONE)

#define I2C_CLK_HZ 80000000UL
#define I2C_TIMEOUT_US 10000U

struct hi3861_i2c {
    void *base;
    void *crg;
    uint32_t rate;
    mutex_t lock;
    i2c_adapter_t adapter;
};

static void i2c_fail(struct hi3861_i2c *i2c)
{
    writel(I2C_CLEAR_ALL, (volatile uint8_t *)i2c->base + I2C_ICR);
}

static int i2c_wait_done(struct hi3861_i2c *i2c)
{
    uint32_t sr = 0;
    for (uint32_t t = 0; t < I2C_TIMEOUT_US; t++) {
        sr = readl((volatile uint8_t *)i2c->base + I2C_SR);
        if (sr & I2C_INT_DONE)
            break;
        udelay(1);
    }
    if (!(sr & I2C_INT_DONE))
        return -ETIMEDOUT;
    if (sr & I2C_ACK_INTR)
        return -EIO;
    writel(readl((volatile uint8_t *)i2c->base + I2C_ICR) | I2C_CLEAR_OVER,
           (volatile uint8_t *)i2c->base + I2C_ICR);
    return 0;
}

static int i2c_start(struct hi3861_i2c *i2c)
{
    writel(I2C_CLEAR_ALL, (volatile uint8_t *)i2c->base + I2C_ICR);
    writel(I2C_START | I2C_WRITE, (volatile uint8_t *)i2c->base + I2C_COM);
    uint32_t sr = 0;
    for (uint32_t t = 0; t < I2C_TIMEOUT_US; t++) {
        sr = readl((volatile uint8_t *)i2c->base + I2C_SR);
        if (sr & I2C_INT_DONE)
            break;
        udelay(1);
    }
    writel(I2C_START_END, (volatile uint8_t *)i2c->base + I2C_ICR);
    if (!(sr & I2C_INT_DONE))
        return -ETIMEDOUT;
    if (sr & I2C_ACK_INTR)
        return -EIO;
    writel(I2C_CLEAR_OVER, (volatile uint8_t *)i2c->base + I2C_ICR);
    return 0;
}

static int i2c_stop(struct hi3861_i2c *i2c)
{
    writel(I2C_STOP, (volatile uint8_t *)i2c->base + I2C_COM);
    uint32_t sr = 0;
    for (uint32_t t = 0; t < I2C_TIMEOUT_US; t++) {
        sr = readl((volatile uint8_t *)i2c->base + I2C_SR);
        if (sr & I2C_INT_DONE)
            break;
        udelay(1);
    }
    writel(I2C_CLEAR_ALL, (volatile uint8_t *)i2c->base + I2C_ICR);
    return (sr & I2C_INT_DONE) ? 0 : -ETIMEDOUT;
}

static int i2c_send_byte(struct hi3861_i2c *i2c, uint8_t v)
{
    writel(v, (volatile uint8_t *)i2c->base + I2C_TXR);
    writel(I2C_CLEAR_ALL, (volatile uint8_t *)i2c->base + I2C_ICR);
    writel(I2C_WRITE, (volatile uint8_t *)i2c->base + I2C_COM);
    return i2c_wait_done(i2c);
}

static int i2c_recv_byte(struct hi3861_i2c *i2c, uint8_t *v, int last)
{
    writel(I2C_CLEAR_ALL, (volatile uint8_t *)i2c->base + I2C_ICR);
    writel(last ? (I2C_READ | I2C_NACK) : I2C_READ,
           (volatile uint8_t *)i2c->base + I2C_COM);
    uint32_t sr = 0;
    for (uint32_t t = 0; t < I2C_TIMEOUT_US; t++) {
        sr = readl((volatile uint8_t *)i2c->base + I2C_SR);
        if (sr & I2C_INT_DONE)
            break;
        udelay(1);
    }
    if (!(sr & I2C_INT_DONE))
        return -ETIMEDOUT;
    *v = (uint8_t)readl((volatile uint8_t *)i2c->base + I2C_RXR);
    int rc = i2c_wait_done(i2c);
    writel((1U << 3) | I2C_INT_DONE,
           (volatile uint8_t *)i2c->base + I2C_ICR);
    return rc;
}

static int i2c_send(struct hi3861_i2c *i2c, uint16_t addr, const uint8_t *buf,
                    uint16_t len)
{
    writel(I2C_IP_ENABLE | I2C_UNMASK_ALL | I2C_UNMASK_ACK |
               I2C_UNMASK_ARBITRATE | I2C_UNMASK_OVER,
           (volatile uint8_t *)i2c->base + I2C_CTRL);
    writel(I2C_CLEAR_ALL, (volatile uint8_t *)i2c->base + I2C_ICR);
    writel((uint32_t)(addr << 1) & ~1U,
           (volatile uint8_t *)i2c->base + I2C_TXR);
    int rc = i2c_start(i2c);
    if (rc) {
        i2c_fail(i2c);
        return rc;
    }
    for (uint16_t i = 0; i < len; i++) {
        rc = i2c_send_byte(i2c, buf[i]);
        if (rc) {
            i2c_fail(i2c);
            return rc;
        }
    }
    return i2c_stop(i2c);
}

static int i2c_receive(struct hi3861_i2c *i2c, uint16_t addr, uint8_t *buf,
                       uint16_t len)
{
    writel(I2C_IP_ENABLE | I2C_UNMASK_ALL | I2C_UNMASK_ACK |
               I2C_UNMASK_ARBITRATE | I2C_UNMASK_OVER,
           (volatile uint8_t *)i2c->base + I2C_CTRL);
    writel(I2C_CLEAR_ALL, (volatile uint8_t *)i2c->base + I2C_ICR);
    writel(((uint32_t)addr << 1) | 1U,
           (volatile uint8_t *)i2c->base + I2C_TXR);
    int rc = i2c_start(i2c);
    if (rc)
        return rc;
    for (uint16_t i = 0; i < len; i++) {
        rc = i2c_recv_byte(i2c, &buf[i], i == len - 1);
        if (rc) {
            i2c_fail(i2c);
            return rc;
        }
    }
    return i2c_stop(i2c);
}

static int i2c_writeread(struct hi3861_i2c *i2c, uint16_t addr,
                         const uint8_t *wbuf, uint16_t wlen, uint8_t *rbuf,
                         uint16_t rlen)
{
    writel(I2C_IP_ENABLE | I2C_UNMASK_ALL | I2C_UNMASK_ACK |
               I2C_UNMASK_ARBITRATE | I2C_UNMASK_OVER,
           (volatile uint8_t *)i2c->base + I2C_CTRL);
    writel(I2C_CLEAR_ALL, (volatile uint8_t *)i2c->base + I2C_ICR);
    writel((uint32_t)(addr << 1) & ~1U,
           (volatile uint8_t *)i2c->base + I2C_TXR);
    int rc = i2c_start(i2c);
    if (rc)
        return rc;
    for (uint16_t i = 0; i < wlen; i++) {
        rc = i2c_send_byte(i2c, wbuf[i]);
        if (rc) {
            i2c_fail(i2c);
            return rc;
        }
    }
    writel(((uint32_t)addr << 1) | 1U,
           (volatile uint8_t *)i2c->base + I2C_TXR);
    rc = i2c_start(i2c);
    if (rc)
        return rc;
    for (uint16_t i = 0; i < rlen; i++) {
        rc = i2c_recv_byte(i2c, &rbuf[i], i == rlen - 1);
        if (rc) {
            i2c_fail(i2c);
            return rc;
        }
    }
    return i2c_stop(i2c);
}

static int hi3861_i2c_transfer(i2c_adapter_t *a, i2c_msg_t *msgs, int n)
{
    struct hi3861_i2c *i2c = a->priv;
    if (!msgs || n < 1 || n > 2 || !msgs[0].buf || !msgs[0].len)
        return -EINVAL;
    if (msgs[0].addr & 0xF000)
        return -EOPNOTSUPP; /* 10-bit addressing not ported yet */

    mutex_lock(&i2c->lock);
    int rc;
    if (n == 1 && !(msgs[0].flags & I2C_M_READ))
        rc = i2c_send(i2c, msgs[0].addr, msgs[0].buf, msgs[0].len);
    else if (n == 1)
        rc = i2c_receive(i2c, msgs[0].addr, msgs[0].buf, msgs[0].len);
    else if (!(msgs[0].flags & I2C_M_READ) &&
             (msgs[1].flags & I2C_M_READ) && msgs[1].buf && msgs[1].len)
        rc = i2c_writeread(i2c, msgs[0].addr, msgs[0].buf, msgs[0].len,
                           msgs[1].buf, msgs[1].len);
    else
        rc = -EINVAL;
    mutex_unlock(&i2c->lock);
    return rc;
}

static void i2c_configure(struct hi3861_i2c *i2c, uint32_t rate)
{
    if (!rate)
        rate = 100000;
    i2c->rate = rate;
    /* SCL high/low are equal; I2C_CLK is the 80 MHz WDT/UART clock in the
     * vendor SDK. */
    uint32_t div = (I2C_CLK_HZ / (rate * 2U)) / 2U - 1U;
    if (div > 0xFFFF)
        div = 0xFFFF;
    uint32_t ctrl = readl((volatile uint8_t *)i2c->base + I2C_CTRL);
    writel(ctrl & ~I2C_UNMASK_ALL, (volatile uint8_t *)i2c->base + I2C_CTRL);
    writel(div, (volatile uint8_t *)i2c->base + I2C_SCL_H);
    writel(div, (volatile uint8_t *)i2c->base + I2C_SCL_L);
    writel(ctrl, (volatile uint8_t *)i2c->base + I2C_CTRL);
    writel(I2C_CLEAR_ALL, (volatile uint8_t *)i2c->base + I2C_ICR);
}

static int hi3861_i2c_probe(device_t *dev)
{
    struct hi3861_i2c *i2c = kcalloc(1, sizeof(*i2c));
    if (!i2c)
        return -ENOMEM;
    i2c->base = device_map_mmio(dev, 0);
    i2c->crg = device_map_mmio(dev, 1);
    if (!i2c->base || !i2c->crg) {
        kfree(i2c);
        return -ENODEV;
    }
    /* CLKEN bit 14 for I2C0, 15 for I2C1.  The bus number is in the device
     * name suffix to keep the driver data-driven. */
    int nr = dev->firmware_match
                 ? (int)dev->firmware_match->driver_data : 0;
    if (nr < 0 || nr > 1)
        nr = 0;
    uint16_t gate = *(volatile uint16_t *)((uint8_t *)i2c->crg + 0x30);
    *(volatile uint16_t *)((uint8_t *)i2c->crg + 0x30) =
        (uint16_t)(gate | (1U << (14 + nr)));
    writel(~I2C_IP_ENABLE, (volatile uint8_t *)i2c->base + I2C_CTRL);
    i2c_configure(i2c, 100000);
    mutex_init(&i2c->lock);
    i2c->adapter.name = nr ? "hi3861-i2c1" : "hi3861-i2c0";
    i2c->adapter.nr = nr;
    i2c->adapter.transfer = hi3861_i2c_transfer;
    i2c->adapter.priv = i2c;
    int ret = i2c_adapter_add(&i2c->adapter);
    if (ret) {
        kfree(i2c);
        return ret;
    }
    dev->drv_priv = i2c;
    return 0;
}

static int hi3861_i2c_remove(device_t *dev)
{
    struct hi3861_i2c *i2c = dev->drv_priv;
    if (!i2c)
        return 0;
    int ret = i2c_adapter_remove(&i2c->adapter);
    if (ret)
        return ret;
    kfree(i2c);
    dev->drv_priv = NULL;
    return 0;
}

static const firmware_device_id_t hi3861_i2c_ids[] = {
    {.id = "hisilicon,hi3861-i2c0", .driver_data = 0},
    {.id = "hisilicon,hi3861-i2c1", .driver_data = 1},
    {.id = NULL},
};

static const driver_t hi3861_i2c_driver = {
    .name = "hi3861-i2c",
    .bus = &platform_bus,
    .firmware_ids = hi3861_i2c_ids,
    .probe = hi3861_i2c_probe,
    .remove = hi3861_i2c_remove,
};

DRIVER_REGISTER(hi3861_i2c_driver);
