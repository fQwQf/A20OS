#include "core/arch.h"
#include "boot/context.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/sync.h"
#include "core/preempt.h"
#include "drivers/core/driver_core.h"
#include "drivers/char/uart.h"
#include "mcu/heap.h"
#include "mm/slab.h"
#include "proc/proc.h"

extern void mcu_user_demo_start(void);

static volatile unsigned worker_count;
static completion_t worker_ready;
static void worker(void) {
    complete(&worker_ready);
    for (;;) {
        worker_count++;
        proc_sleep_until(timer_get_ticks() + 10U);
    }
}

static void atomic_selftest(void) {
    volatile uint8_t a = 1;
    volatile uint16_t b = 2;
    volatile uint32_t c = 3;
    volatile uint64_t d = 4;
    if (__atomic_fetch_add(&a, 2, __ATOMIC_SEQ_CST) != 1 ||
        __atomic_fetch_xor(&b, 3, __ATOMIC_SEQ_CST) != 2 ||
        __atomic_exchange_n(&c, 7, __ATOMIC_SEQ_CST) != 3 ||
        __atomic_add_fetch(&d, 8, __ATOMIC_SEQ_CST) != 12)
        panic("MCU selftest: software atomics");
    uint64_t expected = 10;
    if (__atomic_compare_exchange_n(&d, &expected, 20, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) || expected != 12)
        panic("MCU selftest: failed CAS writeback");
    if (!__atomic_compare_exchange_n(&d, &expected, 20, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        panic("MCU selftest: successful CAS");
    uint32_t flags = arch_irq_save();
    __atomic_store_n(&d, 0, __ATOMIC_RELEASE);
    if (arch_irqs_enabled()) panic("MCU selftest: atomic enabled masked IRQs");
    arch_irq_restore(flags);
}

static void selftest(void) {
    atomic_selftest();
#ifdef CONFIG_KERNEL_PREEMPT
    spinlock_t lock = SPINLOCK_INIT;
    spin_lock(&lock);
    unsigned locked_before = worker_count;
    uint64_t locked_until = timer_get_ticks() + 20U;
    while (timer_get_ticks() < locked_until) arch_cpu_relax();
    if (worker_count != locked_before) panic("MCU selftest: preempted while locked");
    spin_unlock(&lock);
#endif
    uint64_t start = timer_get_ticks();
    unsigned before = worker_count;
    for (unsigned i = 1; i <= 128; i++) {
        unsigned char *p = kmalloc(i);
        if (!p) panic("MCU selftest: allocation failed");
        if ((uintptr_t)p & 15U) panic("MCU selftest: allocator violated RV32 stack alignment");
        memset(p, (int)i, i);
        for (unsigned j = 0; j < i; j++)
            if (p[j] != (unsigned char)i) panic("MCU selftest: heap corruption");
        kfree(p);
    }
    #ifdef CONFIG_KERNEL_PREEMPT
    /* No voluntary yield: the timer must give the sleeping worker its CPU. */
    while (timer_get_ticks() < start + 50U) arch_cpu_relax();
#else
    proc_sleep_until(start + 50U);
#endif
    if (timer_get_ticks() < start + 50U || worker_count <= before)
        panic("MCU selftest: scheduler/timer made no progress");
    printf("[MCU TEST] preemption=%s\n",
#ifdef CONFIG_KERNEL_PREEMPT
           "irq-return"
#else
           "cooperative"
#endif
    );
    printf("[MCU TEST] PASS heap atomics timer sleep context-switch completion worker=%u\n", worker_count);
}

static void console_task(void) {
    wait_for_completion(&worker_ready);
    selftest();
    printf("A20OS MCU READY\nhelp: help info heap ticks tasks test reboot\na20> ");
    char line[48];
    unsigned len = 0;
    for (;;) {
        int c = uart_try_getc();
        if (c < 0) { proc_sleep_until(timer_get_ticks() + 1U); continue; }
        if (c == '\r' || c == '\n') {
            uart_putc('\n'); line[len] = 0;
            if (!strcmp(line, "help"))
                printf("help info heap ticks tasks test reboot\n");
            else if (!strcmp(line, "info"))
                printf("board=%s arch=%s mode=M RV32IMC NOMMU trusted-kernel-tasks\n", current_board->name, ARCH_NAME);
            else if (!strcmp(line, "heap"))
                printf("heap free=%u bytes\n", (unsigned)mcu_heap_available());
            else if (!strcmp(line, "ticks"))
                printf("ticks=%llu hz=%u\n", (unsigned long long)timer_get_ticks(), (unsigned)ARCH_TIMER_FREQ);
            else if (!strcmp(line, "tasks"))
                printf("console pid=%d worker-count=%u\n", proc_current()->pid, worker_count);
            else if (!strcmp(line, "test")) selftest();
            else if (!strcmp(line, "reboot")) firmware_reboot();
            else if (len) printf("unknown command: %s\n", line);
            len = 0; printf("a20> ");
        } else if (c == 8 || c == 127) {
            if (len) { len--; printf("\b \b"); }
        } else if (c >= 32 && c < 127 && len + 1 < sizeof(line)) {
            line[len++] = (char)c; uart_putc((char)c);
        }
    }
}

static void advance(enum boot_phase from, enum boot_phase to) {
    if (boot_context_advance(from, to) < 0) panic("MCU boot: invalid phase");
}

void kernel_main(void) {
    arch_local_irq_disable();
    arch_boot_context_init();
    if (current_board->early_init) current_board->early_init();
    uart_init();
    printf("\nA20OS RV32 M-mode MCU board=%s\n", current_board->name);
    advance(BOOT_PHASE_HANDOFF, BOOT_PHASE_EARLY);
    mcu_heap_init();
    advance(BOOT_PHASE_EARLY, BOOT_PHASE_MEMORY);
    timer_init();
    advance(BOOT_PHASE_MEMORY, BOOT_PHASE_DEVICES);
    preempt_state_init(0);
    proc_init();
    completion_init(&worker_ready);
    if (proc_alloc(worker) < 0 || proc_alloc(console_task) < 0)
        panic("MCU boot: cannot allocate tasks");
#if defined(CONFIG_MCU_USER_DEMO) && defined(CONFIG_MCU_NATIVE_PROGRAM)
    mcu_user_demo_start();
#endif
    printf("[BOOT] scheduler initialized heap=%u bytes\n", (unsigned)mcu_heap_available());
    advance(BOOT_PHASE_DEVICES, BOOT_PHASE_READY);
    arch_local_irq_enable();
    sched();
    idle_loop();
}
