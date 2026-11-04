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
void i2c_adapter_remove(struct i2c_adapter *adapter);
struct i2c_adapter *i2c_adapter_get(int nr);
int i2c_transfer(int nr, struct i2c_msg *msgs, int n);
int i2c_write(int nr, uint16_t addr, const void *buf, uint16_t len);
int i2c_read(int nr, uint16_t addr, void *buf, uint16_t len);
int i2c_write_read(int nr, uint16_t addr, const void *wbuf, uint16_t wlen,
                   void *rbuf, uint16_t rlen);

#endif /* A20_DRIVERS_I2C_I2C_H */
