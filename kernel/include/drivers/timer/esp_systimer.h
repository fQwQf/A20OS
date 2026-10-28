#ifndef A20_ESP_SYSTIMER_H
#define A20_ESP_SYSTIMER_H
#include "core/types.h"
void esp_systimer_init(uintptr_t base, uint32_t period);
uint64_t esp_systimer_read(uintptr_t base);
void esp_systimer_enable(uintptr_t base, int enable);
void esp_systimer_ack(uintptr_t base);
#endif
