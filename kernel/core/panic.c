#include "core/panic.h"
#include "drivers/char/uart.h"
#include "core/stdio.h"
#include "core/defs.h"
#include "core/arch.h"
#include "core/cpu.h"
#include "core/kallsyms.h"
#include "proc/proc.h"

#define PANIC_MAX_BACKTRACE 16

/*
 * Set by the first CPU to enter panic().  Any later entry — a nested panic
 * raised by the dump path itself, or a secondary CPU while the first is
 * still dumping — takes the quiet path: one UART line and an immediate
 * halt.  This keeps the primary dump from being interleaved with, or lost
 * to, a recursive failure.
 *
 * A20OS has no cross-CPU "stop" IPI; other CPUs keep running until they
 * panic, take a fatal trap, or the firmware poweroff lands.  This guard is
 * what keeps their output out of the primary dump.
 */
static volatile int g_in_panic;

static void panic_halt_quiet(const char *why) {
    uart_puts("\n[PANIC] ");
    uart_puts(why);
    uart_puts(" — halting immediately\n");
    arch_halt();
    for (;;)
        __asm__ volatile("");
}

NORETURN void panic(const char *fmt, ...) {
    /* Freeze this core before touching any shared state. */
    arch_local_irq_disable();

    if (__atomic_exchange_n(&g_in_panic, 1, __ATOMIC_ACQ_REL))
        panic_halt_quiet("recursive/secondary panic");

    /* Capture the cheap, arch-independent context first: these builtins and
     * the CSR reads below must happen before printf runs arbitrary code. */
    void *caller = __builtin_return_address(0);
    uint64_t fp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    /* No portable sp builtin exists; the address of a local approximates it
     * closely enough for stack-range diagnostics. */
    uint64_t sp_approx = (uint64_t)(uintptr_t)&fmt;

#if defined(CONFIG_LOONGARCH64) && defined(CONFIG_BOARD_LS2K1000) && \
    defined(CONFIG_COOPERATIVE_BOOT)
    uint64_t sp;
    uint64_t crmd, prmd, era, badv;
    uint64_t tlbrera, tlbrbadv, tlbrehi, tlbrelo0, tlbrelo1;

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
    printf("\n");

    /* Generic context — identical output on every arch/board. */
    printf("[PANIC] arch=%s cpu=%u\n", ARCH_NAME, cpu_current_id());
    task_t *cur = proc_current();
    if (cur && arch_is_kernel_address(cur))
        printf("[PANIC] task: pid=%d name=%s state=%d\n",
               cur->pid, cur->name, cur->state);
    else
        printf("[PANIC] task: <none/early boot>\n");

    printf("[PANIC] caller=");
    kallsyms_print((uint64_t)(uintptr_t)caller);
    printf(" sp=~0x%lx fp=0x%lx\n",
           (unsigned long)sp_approx, (unsigned long)fp);

    /* Frame-pointer backtrace.  Degrades to zero frames when frame pointers
     * are omitted or the chain is corrupt; the caller line above still
     * identifies the panic site in that case. */
    struct backtrace_frame frames[PANIC_MAX_BACKTRACE];
    int nframes = arch_unwind_frames(fp, frames, PANIC_MAX_BACKTRACE);
    if (nframes > 0) {
        printf("[PANIC] backtrace:\n");
        for (int i = 0; i < nframes; i++) {
            printf("  [%d] ", i);
            kallsyms_print(frames[i].pc);
            printf("\n");
        }
    }

#if defined(CONFIG_LOONGARCH64) && defined(CONFIG_BOARD_LS2K1000) && \
    defined(CONFIG_COOPERATIVE_BOOT)
    printf("[LS2K-DIAG] sp=0x%lx crmd=0x%lx prmd=0x%lx"
           " era=0x%lx badv=0x%lx\n",
           (unsigned long)sp, (unsigned long)crmd,
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
