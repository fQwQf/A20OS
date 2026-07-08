#ifdef CONFIG_RISCV32

#include "firmware.h"

uint32_t sbi_call(uint32_t eid, uint32_t fid, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    register uint32_t a0 __asm__("a0") = arg0;
    register uint32_t a1 __asm__("a1") = arg1;
    register uint32_t a2 __asm__("a2") = arg2;
    register uint32_t a6 __asm__("a6") = fid;
    register uint32_t a7 __asm__("a7") = eid;
    __asm__ __volatile__("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a6), "r"(a7) : "memory");
    return a0;
}

void firmware_set_timer(uint64_t time) {
    sbi_call(SBI_SET_TIMER_EID, 0, (uint32_t)time, (uint32_t)(time >> 32), 0);
}

void firmware_console_putchar(char c) {
    sbi_call(SBI_CONSOLE_PUTCHAR_EID, 0, (uint32_t)(uint8_t)c, 0, 0);
}

int firmware_console_getchar(void) {
    return (int)(int32_t)sbi_call(SBI_CONSOLE_GETCHAR_EID, 0, 0, 0, 0);
}

void firmware_shutdown(void) {
    sbi_call(SBI_SRST_EID, 0, SBI_SRST_SHUTDOWN, 0, 0);
    while (1)
        __asm__ __volatile__("wfi");
}

void firmware_reboot(void) {
    sbi_call(SBI_SRST_EID, 0, SBI_SRST_COLD_REBOOT, 0, 0);
    while (1)
        __asm__ __volatile__("wfi");
}

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

void sbi_send_ipi(uint32_t hart_mask) {
    sbi_call(SBI_SEND_IPI_EID, 0, hart_mask, 0, 0);
}

#endif /* CONFIG_RISCV32 */
