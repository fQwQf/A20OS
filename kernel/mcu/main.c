#include "core/arch.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_core.h"
#include "mm/slab.h"
#include "peripherals.h"

void mcu_heap_init(void);
size_t mcu_heap_available(void);

void kernel_main(void) {
    arch_local_irq_disable();
    if (current_board && current_board->early_init)
        current_board->early_init();
    uart_init();
    mcu_heap_init();

    printf("\n======================================\n");
    printf(" A20OS ARMv7-M STM32F103 bringup\n");
    printf("======================================\n");
    printf("[BOOT] board=%s arch=%s\n",
           current_board ? current_board->name : "unknown", ARCH_NAME);
    printf("[BOOT] heap=%u bytes\n", (unsigned)mcu_heap_available());

    void *probe = kmalloc(64);
    printf("[BOOT] allocator=%s ptr=%p\n", probe ? "ok" : "failed", probe);
    kfree(probe);

    stm32_peripherals_init();

    timer_init();
    arch_local_irq_enable();
    printf("[BOOT] SysTick=1000Hz, entering WFI loop\n");

    uint64_t last = 0;
    for (;;) {
        uint64_t now = timer_get_ticks();
        stm32_peripherals_service(now);
        if (now - last >= 1000) {
            last = now;
            printf("[TICK] %lu ms\n", (unsigned long)now);
        }
        arch_wfi();
    }
}
