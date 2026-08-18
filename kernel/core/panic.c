#include "core/panic.h"
#include "drivers/char/uart.h"
#include "core/stdio.h"
#include "core/defs.h"
#include "core/arch.h"

NORETURN void panic(const char *fmt, ...) {
#if defined(CONFIG_LOONGARCH64) && defined(CONFIG_BOARD_LS2K1000) && \
    defined(CONFIG_COOPERATIVE_BOOT)
    uint64_t sp;
    uint64_t crmd, prmd, era, badv;
    uint64_t tlbrera, tlbrbadv, tlbrehi, tlbrelo0, tlbrelo1;
    void *caller = __builtin_return_address(0);

    __asm__ __volatile__("move %0, $sp" : "=r"(sp));
    __asm__ __volatile__("csrrd %0, 0x0" : "=r"(crmd));
    __asm__ __volatile__("csrrd %0, 0x1" : "=r"(prmd));
    __asm__ __volatile__("csrrd %0, 0x6" : "=r"(era));
    __asm__ __volatile__("csrrd %0, 0x7" : "=r"(badv));
    __asm__ __volatile__("csrrd %0, 0x89" : "=r"(tlbrbadv));
    __asm__ __volatile__("csrrd %0, 0x8a" : "=r"(tlbrera));
    __asm__ __volatile__("csrrd %0, 0x8e" : "=r"(tlbrehi));
    __asm__ __volatile__("csrrd %0, 0x8c" : "=r"(tlbrelo0));
    __asm__ __volatile__("csrrd %0, 0x8d" : "=r"(tlbrelo1));
#endif

    va_list args;
    va_start(args, fmt);

    uart_puts("\n\n========== KERNEL PANIC ==========\n");
    vprintf(fmt, args);
    va_end(args);

#if defined(CONFIG_LOONGARCH64) && defined(CONFIG_BOARD_LS2K1000) && \
    defined(CONFIG_COOPERATIVE_BOOT)
    printf("\n[LS2K-DIAG] caller=%p sp=0x%lx crmd=0x%lx prmd=0x%lx"
           " era=0x%lx badv=0x%lx\n",
           caller, (unsigned long)sp, (unsigned long)crmd,
           (unsigned long)prmd, (unsigned long)era, (unsigned long)badv);
    printf("[LS2K-DIAG] tlbrera=0x%lx tlbrbadv=0x%lx tlbrehi=0x%lx"
           " tlbrelo0=0x%lx tlbrelo1=0x%lx\n",
           (unsigned long)tlbrera, (unsigned long)tlbrbadv,
           (unsigned long)tlbrehi, (unsigned long)tlbrelo0,
           (unsigned long)tlbrelo1);
#endif

    uart_puts("\n[PANIC] attempting firmware poweroff\n");
    firmware_shutdown();

    arch_halt();
    for (;;)
        __asm__ volatile("");
}
