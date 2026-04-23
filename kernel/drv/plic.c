#ifdef CONFIG_RISCV64

#include "plic.h"
#include "consts.h"
#include "defs.h"

// 初始化 PLIC（平台级中断控制器）
void plic_init(void) {
    *(volatile uint32_t *)(PLIC_PRIORITY + UART0_IRQ * 4) = 1;  // 设置 UART0 中断优先级
    plic_init_hart();  // 初始化当前 hart 的中断
    arch_write_sie(arch_read_sie() | SIE_SEIE);  // 启用外部中断
}

// 初始化当前 hart 的 PLIC 设置
void plic_init_hart(void) {
    int hart = 0;
    *(volatile uint32_t *)PLIC_SENABLE(hart) = (1 << UART0_IRQ);  // 启用 UART0 中断
    *(volatile uint32_t *)PLIC_SPRIORITY(hart) = 0;  // 设置 hart 的中断优先级阈值
}

// 声明（claim）一个待处理的中断
uint32_t plic_claim(void) {
    int hart = 0;
    return *(volatile uint32_t *)PLIC_SCLAIM(hart);
}

// 完成一个中断的处理
void plic_complete(uint32_t irq) {
    int hart = 0;
    *(volatile uint32_t *)PLIC_SCLAIM(hart) = irq;
}

#endif /* CONFIG_RISCV64 */
