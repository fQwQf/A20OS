#include "core/arch.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "core/smp.h"
#include "shared_kernel.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_core.h"
#include "mm/slab.h"
#include "peripherals.h"
#include "heap.h"
#include "leaf.h"
#include "board.h"
#include "proc/proc.h"
#include "drivers/stm32f1/stm32_uart.h"

void mcu_heap_init(void);
size_t mcu_heap_available(void);

#if !defined(CONFIG_STM32_QEMU) && !defined(CONFIG_MCU_SHARED_KERNEL)
static void stm32_service_thread(void) {
    for (;;) {
        uint64_t now = timer_get_ticks();
#ifdef CONFIG_STM32_CLUSTER_PORT
        a20_mcu_leaf_poll();
        proc_sleep_until(now + 1U);
#else
        stm32_peripherals_service(now);
        proc_sleep_until(now + 5U);
#endif
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
    a20_mcu_leaf_init();
    a20_mcu_leaf_disable();
#ifdef CONFIG_STM32_CLUSTER_PORT
    uint8_t cluster_id[16];
    uint64_t cluster_epoch = 0;
    int cluster_ready =
        stm32_cluster_uart_init() == 0 &&
        stm32_cluster_boot_identity(cluster_id, &cluster_epoch) == 0 &&
        a20_mcu_leaf_configure(cluster_id, cluster_epoch) == 0;
    if (!cluster_ready)
        a20_mcu_leaf_disable();
#endif

    printf("\n======================================\n");
    printf(" A20OS ARMv7-M STM32F103 bringup\n");
    printf("======================================\n");
    printf("[BOOT] board=%s arch=%s heap=%u bytes\n",
           current_board ? current_board->name : "unknown", ARCH_NAME,
           (unsigned)mcu_heap_available());
    void *probe = kmalloc(64);
    printf("[BOOT] allocator=%s\n", probe ? "ok" : "failed");
    kfree(probe);

#if !defined(CONFIG_STM32_QEMU) && !defined(CONFIG_STM32_CLUSTER_PORT) &&      \
    !defined(CONFIG_MCU_SHARED_KERNEL)
    stm32_peripherals_init();
#endif
    proc_init();
#ifdef CONFIG_MCU_SHARED_KERNEL
    smp_init();
    smp_boot_secondaries();
    if (smp_online_cpu_count() != CONFIG_NR_CPUS)
        panic("shared kernel secondary startup incomplete");
#ifdef CONFIG_MCU_SHARED_TEST
    mcu_shared_selftest_start();
#endif
    printf("[SHARED] one native kernel, %u physical CPU ports\n",
           smp_online_cpu_count());
#elif defined(CONFIG_STM32_QEMU)
    /* stm32vldiscovery exposes only 8 KiB SRAM.  proc_init() already creates
     * the idle scheduler context; two additional 512-byte task stacks leave
     * too little heap for the architecture smoke image. */
    printf("[SCHED] constrained QEMU idle probe online\n");
#else
    int peripheral_pid = proc_alloc(stm32_service_thread);
    if (peripheral_pid < 0)
        panic("cannot create peripheral task");
    task_t *peripheral = proc_find_get(peripheral_pid);
#ifdef CONFIG_STM32_CLUSTER_PORT
    proc_set_name(peripheral, "cluster-leaf");
#else
    proc_set_name(peripheral, "stm32-peripherals");
#endif
    proc_put(peripheral);
#endif
#ifdef CONFIG_STM32_CLUSTER_PORT
    printf("[CLUSTER] persistent identity=%s epoch=%llu\n",
           cluster_ready ? "ready" : "disabled",
           (unsigned long long)cluster_epoch);
#endif
    printf("[BOOT] scheduler heap=%u bytes\n", (unsigned)mcu_heap_available());
    printf("[BOOT] scheduler initialized\n");
    arch_local_irq_enable();
    printf("[BOOT] SysTick=1000Hz source=HCLK=%u, entering scheduler\n",
           (unsigned)stm32_hclk_hz());
    sched();
    idle_loop();
}
