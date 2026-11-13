/*
 * Generic I2C adapter registry.
 *
 * This is the transport-independent half of the I2C support: controller
 * drivers register an i2c_adapter_t here and clients reach them through the
 * adapter number.  It used to live inside the Hi3861 controller driver, which
 * meant no other architecture had an I2C core at all; the registry is now a
 * kernel service so platform controllers (Hi3861, and the AMD DesignWare
 * controller a laptop's HID-over-I2C touchpad hangs off) share one namespace.
 *
 * The table is deliberately small and static: adapters are registered by
 * probe and unregistered by remove, and the number is theirs to choose.  A
 * lookup retains an adapter until i2c_adapter_put(). Removal returns -EBUSY
 * while a client or transfer owns a reference; controllers must propagate
 * that result instead of freeing live transport storage.
 */
#include "drivers/i2c/i2c.h"
#include "drivers/core/driver_core.h"
#include "core/errno.h"
#include "core/lock.h"

#define I2C_MAX_ADAPTERS 8

static i2c_adapter_t *g_i2c_adapters[I2C_MAX_ADAPTERS];
/* Controller device that owns each adapter, for child-node lookup. */
static struct device *g_i2c_adapter_owner[I2C_MAX_ADAPTERS];

static unsigned g_i2c_adapter_users[I2C_MAX_ADAPTERS];
static spinlock_t g_i2c_lock = SPINLOCK_INIT;

int i2c_adapter_add_for_device(struct device *dev, i2c_adapter_t *adapter)
{
    if (!adapter || adapter->nr < 0 || adapter->nr >= I2C_MAX_ADAPTERS ||
        !adapter->transfer)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_i2c_lock);
    int ret = -EEXIST;
    if (!g_i2c_adapters[adapter->nr]) {
        g_i2c_adapters[adapter->nr] = adapter;
        g_i2c_adapter_owner[adapter->nr] = dev;
        g_i2c_adapter_users[adapter->nr] = 0;
        ret = 0;
    }
    spin_unlock_irqrestore(&g_i2c_lock, flags);
    return ret;
}

int i2c_adapter_add(i2c_adapter_t *adapter)
{
    return i2c_adapter_add_for_device(NULL, adapter);
}

int i2c_adapter_remove(i2c_adapter_t *adapter)
{
    if (!adapter || adapter->nr < 0 || adapter->nr >= I2C_MAX_ADAPTERS)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_i2c_lock);
    int ret = 0;
    if (g_i2c_adapters[adapter->nr] == adapter) {
        if (g_i2c_adapter_users[adapter->nr]) {
            ret = -EBUSY;
        } else {
            g_i2c_adapters[adapter->nr] = NULL;
            g_i2c_adapter_owner[adapter->nr] = NULL;
        }
    }
    spin_unlock_irqrestore(&g_i2c_lock, flags);
    return ret;
}

i2c_adapter_t *i2c_adapter_get_for_device(struct device *dev)
{
    if (!dev)
        return NULL;
    i2c_adapter_t *found = NULL;
    uint64_t flags = spin_lock_irqsave(&g_i2c_lock);
    for (int nr = 0; nr < I2C_MAX_ADAPTERS; nr++) {
        if (g_i2c_adapters[nr] && g_i2c_adapter_owner[nr] == dev) {
            found = g_i2c_adapters[nr];
            g_i2c_adapter_users[nr]++;
            break;
        }
    }
    spin_unlock_irqrestore(&g_i2c_lock, flags);
    return found;
}

i2c_adapter_t *i2c_adapter_get(int nr)
{
    if (nr < 0 || nr >= I2C_MAX_ADAPTERS)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&g_i2c_lock);
    i2c_adapter_t *found = g_i2c_adapters[nr];
    if (found)
        g_i2c_adapter_users[nr]++;
    spin_unlock_irqrestore(&g_i2c_lock, flags);
    return found;
}

void i2c_adapter_put(i2c_adapter_t *adapter)
{
    if (!adapter || adapter->nr < 0 || adapter->nr >= I2C_MAX_ADAPTERS)
        return;
    uint64_t flags = spin_lock_irqsave(&g_i2c_lock);
    if (g_i2c_adapters[adapter->nr] == adapter && g_i2c_adapter_users[adapter->nr])
        g_i2c_adapter_users[adapter->nr]--;
    spin_unlock_irqrestore(&g_i2c_lock, flags);
}

int i2c_adapter_alloc_nr(void)
{
    uint64_t flags = spin_lock_irqsave(&g_i2c_lock);
    int found = -ENOSPC;
    for (int nr = 0; nr < I2C_MAX_ADAPTERS; nr++)
        if (!g_i2c_adapters[nr]) { found = nr; break; }
    spin_unlock_irqrestore(&g_i2c_lock, flags);
    return found; /* add still validates uniqueness atomically. */
}

int i2c_transfer(int nr, i2c_msg_t *msgs, int n)
{
    if (!msgs || n <= 0)
        return -EINVAL;
    i2c_adapter_t *a = i2c_adapter_get(nr);
    if (!a)
        return -ENODEV;
    int ret = a->transfer(a, msgs, n);
    i2c_adapter_put(a);
    return ret;
}

int i2c_write(int nr, uint16_t addr, const void *buf, uint16_t len)
{
    i2c_msg_t m = {.addr = addr, .flags = I2C_M_WRITE, .len = len,
                   .buf = (uint8_t *)(uintptr_t)buf};
    return i2c_transfer(nr, &m, 1);
}

int i2c_read(int nr, uint16_t addr, void *buf, uint16_t len)
{
    i2c_msg_t m = {.addr = addr, .flags = I2C_M_READ, .len = len,
                   .buf = buf};
    return i2c_transfer(nr, &m, 1);
}

int i2c_write_read(int nr, uint16_t addr, const void *wbuf, uint16_t wlen,
                   void *rbuf, uint16_t rlen)
{
    i2c_msg_t m[2] = {
        {.addr = addr, .flags = I2C_M_WRITE, .len = wlen,
         .buf = (uint8_t *)(uintptr_t)wbuf},
        {.addr = addr, .flags = I2C_M_READ, .len = rlen, .buf = rbuf},
    };
    return i2c_transfer(nr, m, 2);
}
