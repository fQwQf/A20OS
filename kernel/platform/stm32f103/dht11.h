/*
 * Smart home hub — DHT11 temperature/humidity sensor (single-wire, PG11).
 *
 * Hardware driver. QEMU's stm32vldiscovery does not model GPIOG or the
 * one-wire timing, so under CONFIG_STM32_QEMU these are safe no-ops that
 * report "no reading" — the app then falls back to synthetic input. On the
 * real board the bit-bang timing may need a small calibration (see dht11.c).
 * Reference: docs/pz/3-DHT11温湿度实验.
 */
#ifndef _STM32F103_DHT11_H
#define _STM32F103_DHT11_H

#include "core/types.h"

int stm32_dht11_init(void);

/* Read one sample. Returns 0 on success (temp_c/humidity written),
 * -1 on timeout/checksum error. Call no more than ~1 Hz. */
int stm32_dht11_read(int16_t *temp_c, uint8_t *humidity);

#endif /* _STM32F103_DHT11_H */
