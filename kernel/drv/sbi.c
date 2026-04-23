#ifdef CONFIG_RISCV64

#include "sbi.h"
#include "defs.h"

// SBI 调用封装函数（使用 ecall 指令）
uint64_t sbi_call(uint64_t eid, uint64_t fid, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    register uint64_t a0 __asm__("a0") = arg0;
    register uint64_t a1 __asm__("a1") = arg1;
    register uint64_t a2 __asm__("a2") = arg2;
    register uint64_t a6 __asm__("a6") = fid;
    register uint64_t a7 __asm__("a7") = eid;
    __asm__ __volatile__(
        "ecall"
        : "+r"(a0)
        : "r"(a1), "r"(a2), "r"(a6), "r"(a7)
        : "memory"
    );
    return a0;
}

// 设置定时器
void sbi_set_timer(uint64_t time) {
    sbi_call(SBI_SET_TIMER_EID, 0, time, 0, 0);
}

// 输出一个字符到控制台
void sbi_console_putchar(char c) {
    sbi_call(SBI_CONSOLE_PUTCHAR_EID, 0, (uint64_t)c, 0, 0);
}

// 从控制台读取一个字符
int sbi_console_getchar(void) {
    return (int)sbi_call(SBI_CONSOLE_GETCHAR_EID, 0, 0, 0, 0);
}

// 关机
void sbi_shutdown(void) {
    sbi_call(SBI_SRST_EID, 0, SBI_SRST_SHUTDOWN, 0, 0);
    while (1) __asm__ volatile("wfi");
}

// 重启系统
void sbi_reboot(void) {
    sbi_call(SBI_SRST_EID, 0, SBI_SRST_COLD_REBOOT, 0, 0);
    while (1) __asm__ volatile("wfi");
}

#endif /* CONFIG_RISCV64 */
