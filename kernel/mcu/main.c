#include "core/arch.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_core.h"
#include "mm/slab.h"
#include "peripherals.h"
#include "heap.h"
#include "proc/proc.h"
#include "drivers/stm32f1/stm32_uart.h"

void mcu_heap_init(void);
size_t mcu_heap_available(void);

static void stm32_peripheral_thread(void) {
    for (;;) {
        uint64_t now = timer_get_ticks();
        stm32_peripherals_service(now);
        proc_sleep_until(now + 5U);
    }
}

#ifdef CONFIG_STM32_QEMU
extern volatile uint32_t armv7m_preemptions;
static volatile uint32_t preempt_probe_progress;

static void scheduler_probe_thread(void) {
    for (;;)
        preempt_probe_progress++;
}

static void scheduler_observer_thread(void) {
    printf("[SCHED] two-task preemption probe online (no voluntary yields)\n");
    int reported = 0;
    for (;;) {
        if (!reported && armv7m_preemptions >= 3U &&
            preempt_probe_progress != 0U) {
            reported = 1;
            printf("[SCHED] PREEMPT PASS tasks=2 count=%u slice=10ms\n",
                   (unsigned)armv7m_preemptions);
        }
    }
}
#endif

void kernel_main(void) {
    arch_local_irq_disable();
    if (current_board && current_board->early_init)
        current_board->early_init();
    uart_init();
    mcu_heap_init();
    timer_init();

    printf("\n======================================\n");
    printf(" A20OS ARMv7-M STM32F103 bringup\n");
    printf("======================================\n");
    printf("[BOOT] board=%s arch=%s heap=%u bytes\n",
           current_board ? current_board->name : "unknown", ARCH_NAME,
           (unsigned)mcu_heap_available());
    void *probe = kmalloc(64);
    printf("[BOOT] allocator=%s\n", probe ? "ok" : "failed");
    kfree(probe);

#ifndef CONFIG_STM32_QEMU
    stm32_peripherals_init();
#endif
    proc_init();
#ifdef CONFIG_STM32_QEMU
    /* The small QEMU image is a scheduler/architecture smoke test. */
    int probe_pid = proc_alloc(scheduler_probe_thread);
    if (probe_pid < 0)
        panic("cannot create scheduler probe task");
    int observer_pid = proc_alloc(scheduler_observer_thread);
    if (observer_pid < 0)
        panic("cannot create scheduler observer task");
#else
    int peripheral_pid = proc_alloc(stm32_peripheral_thread);
    if (peripheral_pid < 0)
        panic("cannot create peripheral task");
    proc_set_name(proc_find(peripheral_pid), "stm32-peripherals");
#endif
    printf("[BOOT] scheduler initialized\n");
    arch_local_irq_enable();
    printf("[BOOT] SysTick=1000Hz source=HCLK=%u, entering scheduler\n",
           (unsigned)stm32_hclk_hz());
    sched();
    idle_loop();
}
