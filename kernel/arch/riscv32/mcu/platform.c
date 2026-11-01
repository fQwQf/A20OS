#include "core/arch.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "core/errno.h"
#include "mcu.h"
#include "core/preempt.h"
#include "proc/proc.h"

void arch_switch_tls_restore(void) {}
void firmware_shutdown(void) { arch_halt(); }
void firmware_reboot(void) { riscv_mcu_reboot(); arch_halt(); }
void firmware_console_putchar(char c) { arch_uart_putc(c); }
int firmware_console_getchar(void) { return arch_uart_poll_getc(); }
void firmware_set_timer(uint64_t ticks) { (void)ticks; }
void timer_init(void) { riscv_mcu_timer_init(); }
uint64_t timer_get_ticks(void) { return riscv_mcu_timer_read(); }
void timer_set_interval(uint64_t ticks) { (void)ticks; }
void timer_enable(void) { riscv_mcu_timer_enable(1); }
void timer_disable(void) { riscv_mcu_timer_enable(0); }
void timer_irq_tick(void) {}

/* M-mode firmware runs trusted kernel tasks; no hosted userspace return. */
void user_trap_return(void) { arch_halt(); }
void __trap_from_user(void) { arch_halt(); }
void __return_to_user(void) { arch_halt(); }
void __trap_from_kernel(void) { arch_halt(); }

#ifdef CONFIG_MCU_NATIVE_PROGRAM
extern int64_t riscv_mcu_syscall(trap_context_t *ctx);
#endif

void riscv_mcu_trap_handler(uint32_t *frame) {
    /* entry.S stores the M-mode register file in trap_context_t order:
     * x[0..31], mstatus (as sstatus), mepc (as sepc). */
    trap_context_t *ctx = (trap_context_t *)frame;
    uint32_t cause = (uint32_t)arch_read_cause();
    if (cause & CAUSE_INTR_MASK) {
        hardirq_enter();
        int handled = riscv_mcu_irq_dispatch(cause & CAUSE_CODE_MASK);
        if (!handled) proc_sched_tick(0);
        hardirq_exit();
        if (!handled) {
            if (ctx->sstatus & (1U << 7)) kernel_preempt_at_irq_return();
            return;
        }
    }
    if ((cause & CAUSE_CODE_MASK) == CAUSE_ECALL_U) {
#ifdef CONFIG_MCU_NATIVE_PROGRAM
        (void)riscv_mcu_syscall(ctx);
#else
        /* Bring-up checkpoint without the flat-program profile. */
        printf("[USER] ecall nr=0x%x\n", (unsigned)TRAP_CTX_SYSCALL_NUM(ctx));
        TRAP_CTX_SET_RET(ctx, 0);
        arch_advance_syscall_epc(ctx);
        proc_yield();
#endif
        return;
    }

    printf("\n[RV32 MCU] TRAP cause=0x%x pc=0x%x addr=0x%x\n",
           cause, (unsigned)TRAP_CTX_EPC(ctx), (unsigned)arch_read_tval());
    arch_halt();
}
