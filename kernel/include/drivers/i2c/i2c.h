#ifndef A20_DRIVERS_I2C_I2C_H
#define A20_DRIVERS_I2C_I2C_H

#include "core/types.h"

#define I2C_M_WRITE 0
#define I2C_M_READ  1

/* One transfer segment.  A two-message transfer is write-then-read (the usual
 * register access); a single message is a plain write or read. */
typedef struct i2c_msg {
    uint16_t addr;   /* 7-bit address, without the R/W bit */
    uint16_t flags;  /* I2C_M_* */
    uint16_t len;
    uint8_t *buf;
} i2c_msg_t;

typedef struct i2c_adapter i2c_adapter_t;
struct i2c_adapter {
    const char *name;
    int         nr;
    int (*transfer)(struct i2c_adapter *adapter, struct i2c_msg *msgs, int n);
    void *priv;
};

int i2c_adapter_add(struct i2c_adapter *adapter);
/* Register an adapter on behalf of the device that implements it, so a child
 * node (an I2C-HID touchpad on an ACPI I2C controller) can find its bus
 * through the firmware parent instead of guessing a number. */
struct device;
int i2c_adapter_add_for_device(struct device *dev, struct i2c_adapter *adapter);
struct i2c_adapter *i2c_adapter_get_for_device(struct device *dev);
/* First free adapter number, or -ENOSPC.  For controllers with no
 * firmware-assigned number. */
int i2c_adapter_alloc_nr(void);
int i2c_adapter_remove(struct i2c_adapter *adapter);
/* Both getters retain a transport reference. Pair every success with put;
 * controller remove must honor -EBUSY while references remain. */
struct i2c_adapter *i2c_adapter_get(int nr);
void i2c_adapter_put(struct i2c_adapter *adapter);
int i2c_transfer(int nr, struct i2c_msg *msgs, int n);
int i2c_write(int nr, uint16_t addr, const void *buf, uint16_t len);
int i2c_read(int nr, uint16_t addr, void *buf, uint16_t len);
int i2c_write_read(int nr, uint16_t addr, const void *wbuf, uint16_t wlen,
                   void *rbuf, uint16_t rlen);

#endif /* A20_DRIVERS_I2C_I2C_H */
