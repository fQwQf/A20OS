#include "drivers/timer/esp_systimer.h"
#include "core/cpu.h"
#define REG(base, off) (*(volatile uint32_t *)((base) + (off)))
void esp_systimer_enable(uintptr_t base, int enable) {
    if (enable) { REG(base, 0x64) |= 1U; REG(base, 0) |= 1U << 24; }
    else { REG(base, 0) &= ~(1U << 24); REG(base, 0x64) &= ~1U; }
}
void esp_systimer_ack(uintptr_t base) { REG(base, 0x6c) = 1U; }
void esp_systimer_init(uintptr_t base, uint32_t period) {
    esp_systimer_enable(base, 0);
    REG(base, 0) |= (1U << 31) | (1U << 30);
    REG(base, 0) &= ~(1U << 28); /* Counter must keep running during CPU WFI. */
    REG(base, 0x34) = (1U << 30) | (period & 0x3ffffffU);
    REG(base, 0x50) = 1U; /* Apply comparator configuration. */
    esp_systimer_ack(base);
    esp_systimer_enable(base, 1);
}
uint64_t esp_systimer_read(uintptr_t base) {
    /* Snapshot registers are shared; serialize callers with IRQ exclusion. */
    uint32_t flags = arch_irq_save();
    REG(base, 4) |= 1U << 30;
    while (!(REG(base, 4) & (1U << 29))) {}
    uint32_t hi = REG(base, 0x40) & 0xfffffU;
    uint32_t lo = REG(base, 0x44);
    arch_irq_restore(flags);
    return ((uint64_t)hi << 32) | lo;
}
