#include "core/arch.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "core/errno.h"
#include "mcu.h"
#include "core/preempt.h"
#include "core/flat_thread.h"
#include "proc/proc.h"

void arch_switch_tls_restore(void) {}
void firmware_shutdown(void) { arch_halt(); }
void firmware_reboot(void) { riscv_mcu_reboot(); arch_halt(); }
void firmware_console_putchar(char c) { arch_uart_putc(c); }
int firmware_console_getchar(void) { return arch_uart_poll_getc(); }
void firmware_set_timer(uint64_t ticks) { (void)ticks; }
static uint64_t timer_last_tick;

void timer_init(void)
{
    riscv_mcu_timer_init();
    timer_last_tick = riscv_mcu_timer_read();
}
uint64_t timer_get_ticks(void) { return riscv_mcu_timer_read(); }
void timer_set_interval(uint64_t ticks)
{
    riscv_mcu_timer_set_interval(ticks);
    riscv_mcu_timer_rearm();
}
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

#define PMP_MAX 16
#define PMP_TOR 0x08U
#define PMP_NAPOT 0x18U
#define PMP_RWX 0x07U

typedef struct pmp_entry {
    uintptr_t top;   /* exclusive upper bound for a TOR entry */
    uint8_t cfg;
} pmp_entry_t;

static void pmp_write_addr(unsigned index, uint32_t value)
{
    switch (index) {
    case 0: __asm__ volatile("csrw pmpaddr0, %0" :: "r"(value) : "memory"); break;
    case 1: __asm__ volatile("csrw pmpaddr1, %0" :: "r"(value) : "memory"); break;
    case 2: __asm__ volatile("csrw pmpaddr2, %0" :: "r"(value) : "memory"); break;
    case 3: __asm__ volatile("csrw pmpaddr3, %0" :: "r"(value) : "memory"); break;
    case 4: __asm__ volatile("csrw pmpaddr4, %0" :: "r"(value) : "memory"); break;
    case 5: __asm__ volatile("csrw pmpaddr5, %0" :: "r"(value) : "memory"); break;
    case 6: __asm__ volatile("csrw pmpaddr6, %0" :: "r"(value) : "memory"); break;
    case 7: __asm__ volatile("csrw pmpaddr7, %0" :: "r"(value) : "memory"); break;
    case 8: __asm__ volatile("csrw pmpaddr8, %0" :: "r"(value) : "memory"); break;
    case 9: __asm__ volatile("csrw pmpaddr9, %0" :: "r"(value) : "memory"); break;
    case 10: __asm__ volatile("csrw pmpaddr10, %0" :: "r"(value) : "memory"); break;
    case 11: __asm__ volatile("csrw pmpaddr11, %0" :: "r"(value) : "memory"); break;
    case 12: __asm__ volatile("csrw pmpaddr12, %0" :: "r"(value) : "memory"); break;
    case 13: __asm__ volatile("csrw pmpaddr13, %0" :: "r"(value) : "memory"); break;
    case 14: __asm__ volatile("csrw pmpaddr14, %0" :: "r"(value) : "memory"); break;
    default: __asm__ volatile("csrw pmpaddr15, %0" :: "r"(value) : "memory"); break;
    }
}

static void pmp_clear_addr(unsigned first)
{
    /* cfg registers are rebuilt in full by pmp_commit(), so only the addresses
     * of unused entries need clearing. */
    for (unsigned i = first; i < PMP_MAX; i++)
        pmp_write_addr(i, 0);
}

static void pmp_commit(const pmp_entry_t *entries, unsigned count)
{
    /* cfg registers hold four bytes each; rebuild them so an entry that used
     * to be enabled cannot survive a later, smaller allow-list. */
    uint32_t cfg[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < count; i++)
        cfg[i >> 2] |= (uint32_t)entries[i].cfg << ((i & 3U) * 8U);
    for (unsigned i = 0; i < count; i++)
        pmp_write_addr(i, (uint32_t)(entries[i].top >> 2));
    pmp_clear_addr(count);
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(cfg[0]) : "memory");
    __asm__ volatile("csrw pmpcfg1, %0" :: "r"(cfg[1]) : "memory");
    __asm__ volatile("csrw pmpcfg2, %0" :: "r"(cfg[2]) : "memory");
    __asm__ volatile("csrw pmpcfg3, %0" :: "r"(cfg[3]) : "memory");
    __asm__ volatile("fence" ::: "memory");
}

static void pmp_add(pmp_entry_t *entries, unsigned *count, uintptr_t top,
                    uint8_t cfg)
{
    if (*count >= PMP_MAX)
        return;
    entries[*count].top = top;
    entries[*count].cfg = cfg;
    (*count)++;
}

/*
 * Build the U-mode allow-list from the current flat program's registered
 * ranges.  The previous version installed one 8-byte stack guard and left
 * entry 1 open across the whole address space, so a user fault anywhere else
 * could read or write kernel heap.  Now every range is a TOR entry and the
 * gaps are explicit deny entries; an address with no matching entry is denied
 * by the hardware for U-mode.  M-mode is not subject to PMP, so the kernel and
 * this trap handler keep full access.
 *
 * Regions come from a20_flat_space_t unsorted, so they are ordered here.  Each
 * region needs one allow entry plus, when it does not abut the previous one,
 * one deny entry.  Eight regions therefore fit the 16 architectural entries;
 * a program that outgrows that falls back to one bounding span, which still
 * excludes kernel memory outside the program.
 */
void riscv_mcu_pmp_update(void)
{
    arch_flat_region_t regions[8];
    int n = arch_flat_region_snapshot(regions, 8);
    pmp_entry_t entries[PMP_MAX];
    unsigned count = 0;

    if (n <= 0) {
        uintptr_t span_base = 0;
        size_t span = arch_flat_current_span(&span_base);
        if (span) {
            if (span_base)
                pmp_add(entries, &count, span_base, PMP_TOR);
            pmp_add(entries, &count, span_base + span,
                    PMP_TOR | PMP_RWX);
            pmp_commit(entries, count);
            return;
        }
        /* No flat program (kernel thread or idle): keep the old full-access
         * mode so early boot and trustless bring-up code still run. */
        entries[0].top = 0xffffffffU;
        entries[0].cfg = PMP_NAPOT | PMP_RWX;
        pmp_commit(entries, 1);
        return;
    }

    /* Insertion sort by base; the space guarantee makes the ranges disjoint. */
    for (int i = 1; i < n; i++) {
        arch_flat_region_t key = regions[i];
        int j = i - 1;
        while (j >= 0 && regions[j].base > key.base) {
            regions[j + 1] = regions[j];
            j--;
        }
        regions[j + 1] = key;
    }

    uintptr_t cursor = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t base = regions[i].base;
        uintptr_t end = base + regions[i].size;
        if (base > cursor)
            pmp_add(entries, &count, base, PMP_TOR);       /* deny gap */
        pmp_add(entries, &count, end,
                PMP_TOR | (regions[i].perms & PMP_RWX));
        cursor = end;
    }
    pmp_commit(entries, count);
}

void riscv_mcu_trap_handler(uint32_t *frame) {
    /* entry.S stores the M-mode register file in trap_context_t order:
     * x[0..31], mstatus (as sstatus), mepc (as sepc). */
    trap_context_t *ctx = (trap_context_t *)frame;
    uint32_t cause = (uint32_t)arch_read_cause();
    if (cause & CAUSE_INTR_MASK) {
        hardirq_enter();
        int handled = riscv_mcu_irq_dispatch(cause & CAUSE_CODE_MASK);
        if (!handled) {
            uint64_t now = timer_get_ticks();
            uint64_t elapsed = now - timer_last_tick;
            timer_last_tick = now;
            if (!elapsed)
                elapsed = 1;
            if (elapsed > 64)
                elapsed = 64;
            /* A one-shot rearm may span several kernel ticks.  Account each
             * of them so RR slices, wait timers and load-average sampling do
             * not silently undercount while the CPU was idle. */
            for (uint64_t i = 0; i < elapsed; i++)
                proc_sched_tick(0);
            timer_set_interval(
                proc_next_timer_interval(timer_get_ticks()));
        }
        hardirq_exit();
        /*
         * A handled IRQ can wake a higher-priority thread just as the timer
         * can (the CMSIS 4.4 vendor dispatcher is the obvious case).  Run the
         * same IRQ-return preemption point for both; it already refuses when
         * preemption is not allowed.
         */
        if (ctx->sstatus & (1U << 7)) kernel_preempt_at_irq_return();
        riscv_mcu_pmp_update();
        return;
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
        riscv_mcu_pmp_update();
        return;
    }

    /* A U-mode access/illegal fault is a task fault, not a kernel panic.
     * The PMP stack guard reports the first push below a task's stack here;
     * exit that task's group and let the scheduler clean up. */
    if (((ctx->sstatus >> 11) & 3U) == 0U) {
        task_t *t = proc_current();
        if (t && t->pid != 0)
            proc_force_exit(t, 128 + 11); /* SIGSEGV */
    }
    printf("\n[RV32 MCU] TRAP cause=0x%x pc=0x%x addr=0x%x\n",
           cause, (unsigned)TRAP_CTX_EPC(ctx), (unsigned)arch_read_tval());
    arch_halt();
}
