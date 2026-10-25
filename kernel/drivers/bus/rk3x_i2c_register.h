/* SPDX-License-Identifier: Apache-2.0
 * Bounded polling register transfers for RK3x/RK356x I2C, preserving firmware
 * clock divisors/timing. Register facts: RK3568 TRM and U-Boot rk_i2c.c.
 * Caller serializes the bus; no concurrent IRQ-driven owner is permitted.
 */
#ifndef A20_RK3X_I2C_REGISTER_H
#define A20_RK3X_I2C_REGISTER_H
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
static inline int rk3x_i2c_wait(volatile uint8_t *io, uint32_t wanted) {
    for (unsigned i = 0; i < 10000; i++) {
        uint32_t pending = readl(io + 0x1c);
        if (pending & 0x40)
            return -ENXIO;
        if (pending & wanted) {
            writel(wanted, io + 0x1c);
            return 0;
        }
        udelay(10);
    }
    return -ETIMEDOUT;
}
static inline int rk3x_i2c_register(volatile uint8_t *io, unsigned address,
                                    uint8_t reg, uint8_t *value, int write) {
    if (!io || !value || address > 0x7f)
        return -EINVAL;
    uint32_t old = readl(io), interrupts = readl(io + 0x18);
    if (old & 1)
        return -EBUSY;
    uint32_t timing = old & ~0x7fU;
    writel(0, io + 0x18);
    writel(0x7f, io + 0x1c);
    writel(timing | 9, io); /* EN, START */
    int result = rk3x_i2c_wait(io, 0x10);
    if (!result) {
        if (write) {
            writel((address << 1) | ((uint32_t)reg << 8) |
                       ((uint32_t)*value << 16),
                   io + 0x100);
            writel(timing | 1, io);
            writel(3, io + 0x10);
            result = rk3x_i2c_wait(io, 4);
        } else {
            writel((1U << 24) | (address << 1) | 1, io + 0x08);
            writel((1U << 24) | reg, io + 0x0c);
            writel(timing | 0x23, io); /* TRX, LASTACK */
            writel(1, io + 0x14);
            result = rk3x_i2c_wait(io, 8);
            if (!result)
                *value = readl(io + 0x200) & 255;
        }
    }
    writel(0x7f, io + 0x1c);
    writel(timing | 0x11, io);
    int stopped = rk3x_i2c_wait(io, 0x20);
    writel(0x7f, io + 0x1c);
    writel(old, io);
    writel(interrupts, io + 0x18);
    return result ? result : stopped;
}
#endif
