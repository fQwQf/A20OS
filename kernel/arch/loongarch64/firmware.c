#ifdef CONFIG_LOONGARCH64

#include "firmware.h"
#include "platform.h"
#include "cpu.h"

void firmware_console_putchar(char c) {
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    while ((uart[5] & 0x20) == 0)
        ;
    uart[0] = (uint8_t)c;
}

int firmware_console_getchar(void) {
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    if (uart[5] & 0x01)
        return uart[0];
    return -1;
}

void firmware_set_timer(uint64_t time) {
    __asm__ __volatile("csrwr %0, 0x41" :: "r"(time | (1UL << 0)));
}

void firmware_shutdown(void) {
    // 1. 关闭本地中断
    arch_local_irq_disable();

    // 2. 根据不同的硬件平台，向电源管理模块（PMU）写入关机信号
#if defined(CONFIG_VIRT)
    // 对应 QEMU virt 机器：写入 8-位 关机信号 0x34
    *(volatile uint8_t *)0x100e001c = 0x34;
#elif defined(CONFIG_LA2000)
    // 对应 LA2000 真实硬件：写入 32-位 关机信号
    *(volatile uint32_t *)(0x1fe27000 + 0x14) = (0b1111 << 10);
#endif

    // 3. 进入死循环并执行 idle 指令，等待硬件完成断电
    while (1) {
        __asm__ __volatile__("idle 0");
    }
}

void firmware_reboot(void) {
    arch_local_irq_disable();

    // 向复位寄存器写入重启命令触发冷/热复位
#if defined(CONFIG_VIRT)
    // QEMU virt: 通过 ACPI GED / FADT 定义的 Reset Register 触发复位
    // 默认通常映射在对应的系统控制 I/O 地址段，写入 0x01 触发 RESET
    // (确切的地址取决于内核读取 ACPI 表的分配，常见 S-state 控制位于 0x10080000 附近)
    *(volatile uint8_t *)0x10080000 = 0x01; 
#elif defined(CONFIG_LA2000)
    // LA2000 硬件: 向专用的复位控制寄存器（如 PMU reset 偏移量 0x30）写入复位信号
    *(volatile uint32_t *)(0x1fe27000 + 0x30) = 0x01; 
#endif

    while (1) {
        __asm__ __volatile__("idle 0");
    }
}

/* SBI compatibility shim — called from generic code via sbi.h */
void sbi_set_timer(uint64_t time) {
    firmware_set_timer(time);
}

void sbi_console_putchar(char c) {
    firmware_console_putchar(c);
}

int sbi_console_getchar(void) {
    return firmware_console_getchar();
}

void sbi_shutdown(void) {
    firmware_shutdown();
}

void sbi_reboot(void) {
    firmware_reboot();
}

#endif /* CONFIG_LOONGARCH64 */
