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

/* Diagnostic read: like the above but reports where a failure happened, so the
 * bit-bang timing / wiring can be tuned on-board. */
typedef struct stm32_dht11_debug {
    int rest_level;   /* PG11 level at idle (1 = pulled-up/idle as expected)   */
    int bytes_read;   /* how many of the 5 bytes were read before failing      */
    int failed_stage; /* -1 ok; 1/2/3 response phase; 10+i byte i; 20 checksum */
    int resp_us[3];   /* measured us of the 3 response-phase edges (or -1)     */
    int bit_us[8];    /* measured high-pulse us of the first byte's 8 bits     */
    uint8_t raw[5];
    uint8_t checksum_calc;
    uint8_t checksum_recv;
    int16_t temp_c;
    uint8_t humidity;
} stm32_dht11_debug_t;

int stm32_dht11_read_debug(stm32_dht11_debug_t *dbg);

#endif /* _STM32F103_DHT11_H */
