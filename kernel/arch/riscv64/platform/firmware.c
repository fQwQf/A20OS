#ifdef CONFIG_RISCV64

#include "firmware.h"
#include "core/defs.h"

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

static uint64_t sbi_call4(uint64_t eid, uint64_t fid, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    register uint64_t a0 __asm__("a0") = arg0;
    register uint64_t a1 __asm__("a1") = arg1;
    register uint64_t a2 __asm__("a2") = arg2;
    register uint64_t a3 __asm__("a3") = arg3;
    register uint64_t a6 __asm__("a6") = fid;
    register uint64_t a7 __asm__("a7") = eid;
    __asm__ __volatile__(
        "ecall"
        : "+r"(a0)
        : "r"(a1), "r"(a2), "r"(a3), "r"(a6), "r"(a7)
        : "memory"
    );
    return a0;
}

void firmware_set_timer(uint64_t time) {
    sbi_call(SBI_SET_TIMER_EID, 0, time, 0, 0);
}

void firmware_console_putchar(char c) {
    sbi_call(SBI_CONSOLE_PUTCHAR_EID, 0, (uint64_t)c, 0, 0);
}

int firmware_console_getchar(void) {
    return (int)sbi_call(SBI_CONSOLE_GETCHAR_EID, 0, 0, 0, 0);
}

void firmware_shutdown(void) {
    sbi_call(SBI_SRST_EID, 0, SBI_SRST_SHUTDOWN, 0, 0);
    while (1) {
        __asm__ __volatile__("wfi");
    }
}

void firmware_reboot(void) {
    sbi_call(SBI_SRST_EID, 0, SBI_SRST_COLD_REBOOT, 0, 0);
    while (1) {
        __asm__ __volatile__("wfi");
    }
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

/* SBI v0.2 IPI extension: a0 is a value mask, not the legacy pointer. */
void sbi_send_ipi(uint64_t hart_mask, uint64_t hart_mask_base) {
    sbi_call(SBI_SEND_IPI_EID, 0, hart_mask, hart_mask_base, 0);
}

int64_t sbi_hart_start(uint64_t hart_id, uint64_t start_addr, uint64_t opaque) {
    return (int64_t)sbi_call(SBI_HSM_EID, 0, hart_id, start_addr, opaque);
}

int64_t sbi_remote_sfence_vma(uint64_t hart_mask, uint64_t hart_mask_base,
                              uint64_t start, uint64_t size) {
    return (int64_t)sbi_call4(SBI_RFENCE_EID, SBI_RFENCE_SFENCE_VMA,
                              hart_mask, hart_mask_base, start, size);
}

#endif /* CONFIG_RISCV64 */
