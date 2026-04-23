#ifdef CONFIG_RISCV64

#include "defs.h"
#include "timer.h"
#include "sbi.h"

static uint64_t timer_freq;  // 定时器频率

// 初始化定时器
void timer_init(void) {
    timer_freq = CLINT_TIMER_FREQ;
    timer_set_interval(TICKS_PER_SEC / 100);  // 设置定时器间隔为 10ms
    timer_enable();  // 启用定时器中断
}

// 设置定时器间隔
void timer_set_interval(uint64_t ticks) {
    uint64_t now = timer_get_ticks();
    sbi_set_timer(now + ticks);
}

// 获取当前时钟周期数
uint64_t timer_get_ticks(void) {
    uint64_t val;
    __asm__ volatile("csrr %0, time" : "=r"(val));
    return val;
}

// 启用定时器中断
void timer_enable(void) {
    uint64_t sie = arch_read_sie();
    arch_write_sie(sie | SIE_STIE);
}

// 禁用定时器中断
void timer_disable(void) {
    uint64_t sie = arch_read_sie();
    arch_write_sie(sie & ~SIE_STIE);
}

#endif /* CONFIG_RISCV64 */
