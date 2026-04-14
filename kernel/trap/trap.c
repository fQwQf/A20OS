#include "trap.h"
#include "proc.h"
#include "syscall.h"
#include "timer.h"
#include "uart.h"
#include "plic.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"
#include "defs.h"
#include "consts.h"
#include "klog.h"

void trap_init(void) {
    w_stvec((uint64_t)__trap_from_kernel);
    w_sscratch(0);
}

static void handle_irq(uint64_t irq, uint64_t sepc, int from_user) {
    if (irq == IRQ_S_TIMER) {
        timer_set_interval(TICKS_PER_SEC / 100);
        if (from_user) {
            task_t *cur = proc_current();
            if (cur) cur->total_time++;
        }
        proc_yield();
    } else if (irq == IRQ_S_EXT) {
        uint32_t irq_id = plic_claim();
        if (irq_id == UART0_IRQ)
            uart_handle_irq();
        if (irq_id != 0)
            plic_complete(irq_id);
    } else if (irq == IRQ_S_SOFT) {
        w_sip(r_sip() & ~SIE_SSIE);
        timer_set_interval(TICKS_PER_SEC / 100);
        proc_yield();
    } else {
        kdebug("TRAP IRQ: irq=%d sepc=0x%lx\n", (int)irq, sepc);
    }
}

void trap_handler(trap_context_t *ctx) {
    uint64_t scause = r_scause();
    uint64_t stval = r_stval();
    uint64_t sepc = r_sepc();

    if (scause & CAUSE_INTR_MASK) {
        handle_irq(scause & CAUSE_CODE_MASK, sepc, 1);
    } else {
        uint64_t code = scause & CAUSE_CODE_MASK;
        if (code == CAUSE_ECALL_U) {
            ctx->sepc += 4;
            syscall_dispatch(ctx);
        } else if (code == CAUSE_LOAD_PAGE_FAULT || code == CAUSE_STORE_PAGE_FAULT || code == CAUSE_INSN_PAGE_FAULT) {
            kerr("User Page Fault: scause=0x%lx sepc=0x%lx stval=0x%lx\n",
                    scause, sepc, stval);
            proc_exit(-SIGSEGV);
        } else if (code == CAUSE_ILLEGAL_INSN) {
            kerr("User Illegal Instruction: sepc=0x%lx\n", sepc);
            proc_exit(-SIGILL);
        } else {
            kerr("TRAP EXCEPTION: scause=0x%lx code=%lu sepc=0x%lx stval=0x%lx\n",
                   scause, code, sepc, stval);
            proc_exit(-1);
        }
    }
}

void kernel_trap_handler(trap_context_t *ctx) {
    uint64_t scause = r_scause();
    uint64_t sepc = r_sepc();
    uint64_t stval = r_stval();

    if (scause & CAUSE_INTR_MASK) {
        handle_irq(scause & CAUSE_CODE_MASK, sepc, 0);
    } else {
        uint64_t code = scause & CAUSE_CODE_MASK;
        if (code == CAUSE_ECALL_U) {
            ctx->sepc += 4;
        } else {
            kdebug("KERNEL TRAP: scause=0x%lx sepc=0x%lx stval=0x%lx\n",
                   scause, sepc, stval);
            panic("Unhandled kernel trap");
        }
    }
}
