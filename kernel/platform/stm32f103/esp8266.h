/*
 * Smart home hub — ESP8266 Wi-Fi (AT firmware) on USART2 (PA2 TX / PA3 RX).
 *
 * USART3 is already taken by the HC-05 bluetooth in this port, so the ESP8266
 * lives on USART2. AT-command driver modelled on docs/pz wifi_function.c.
 *
 * Hardware only: QEMU's stm32vldiscovery models neither USART2 nor a peer, so
 * everything is a safe no-op under CONFIG_STM32_QEMU and init reports absent.
 * NOT yet exercised on real hardware — this is a first cut; the AT exchange is
 * blocking with short timeouts and should become DMA/IRQ driven for production
 * (see docs/stm32-big-exp.md §5.11, §7.3).
 */
#ifndef _STM32F103_ESP8266_H
#define _STM32F103_ESP8266_H

#include "core/types.h"

/* USART2 up, echo off, STA mode, single connection. 0 ok, -1 no module. */
int stm32_esp8266_init(void);

/* Join an AP (AT+CWJAP). 0 ok, -1 fail. */
int stm32_esp8266_join(const char *ssid, const char *pass);

/* Open a TCP connection to the proxy (AT+CIPSTART). 0 ok, -1 fail. */
int stm32_esp8266_connect(const char *ip, int port);

/* Send raw bytes over the open connection (AT+CIPSEND). 0 ok, -1 fail. */
int stm32_esp8266_send(const uint8_t *buf, int len);

/* Receive one +IPD payload into buf (up to max). Returns bytes received,
 * 0 on timeout, -1 on error. */
int stm32_esp8266_recv(uint8_t *buf, int max, uint32_t timeout_ms);

int stm32_esp8266_connected(void);

#endif /* _STM32F103_ESP8266_H */
