#include "trap.h"
#include "proc.h"
#include "syscall.h"
#include "timer.h"
#include "uart.h"
#include "plat_irq.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"
#include "consts.h"
#include "klog.h"
#include "arch_ops.h"

void trap_init(void) {
    arch_trap_vector_set((uint64_t)__trap_from_kernel);
    arch_trap_scratch_set(0);
}

static void handle_irq(uint64_t irq, uint64_t sepc, int from_user) {
    if (irq == ARCH_IRQ_CAUSE_TIMER) {
        timer_set_interval(TICKS_PER_SEC / 100);
        if (from_user) {
            task_t *cur = proc_current();
            if (cur) cur->total_time++;
        }
        proc_yield();
    } else if (irq == ARCH_IRQ_CAUSE_EXTERNAL) {
        uint32_t irq_id = plat_irq_claim();
        if (irq_id == UART0_IRQ)
            uart_handle_irq();
        if (irq_id != 0)
            plat_irq_complete(irq_id);
    } else if (irq == ARCH_IRQ_CAUSE_SOFT) {
        arch_softirq_clear();
        timer_set_interval(TICKS_PER_SEC / 100);
        proc_yield();
    } else {
        kdebug("TRAP IRQ: irq=%d sepc=0x%lx\n", (int)irq, sepc);
    }
}

void trap_handler(trap_context_t *ctx) {
    uint64_t scause = arch_trap_cause();
    uint64_t stval = arch_trap_tval();
    uint64_t sepc = arch_trap_epc();

    if (scause & ARCH_TRAP_INTERRUPT_MASK) {
        handle_irq(scause & ARCH_TRAP_CODE_MASK, sepc, 1);
    } else {
        uint64_t code = scause & ARCH_TRAP_CODE_MASK;
        if (code == ARCH_TRAP_ECALL_FROM_U) {
            ctx->sepc += 4;
            syscall_dispatch(ctx);
        } else {
            kdebug("TRAP: scause=0x%lx sepc=0x%lx stval=0x%lx\n",
                   scause, sepc, stval);
            panic("Unhandled user trap");
        }
    }
}

void kernel_trap_handler(trap_context_t *ctx) {
    uint64_t scause = arch_trap_cause();
    uint64_t sepc = arch_trap_epc();
    uint64_t stval = arch_trap_tval();

    if (scause & ARCH_TRAP_INTERRUPT_MASK) {
        handle_irq(scause & ARCH_TRAP_CODE_MASK, sepc, 0);
    } else {
        uint64_t code = scause & ARCH_TRAP_CODE_MASK;
        if (code == ARCH_TRAP_ECALL_FROM_U) {
            ctx->sepc += 4;
        } else {
            kdebug("KERNEL TRAP: scause=0x%lx sepc=0x%lx stval=0x%lx\n",
                   scause, sepc, stval);
            panic("Unhandled kernel trap");
        }
    }
}
