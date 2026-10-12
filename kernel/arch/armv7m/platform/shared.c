#ifdef CONFIG_MCU_SHARED_KERNEL
#include "shared_kernel.h"
#include "core/arch.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "drivers/core/driver_core.h"
#include "proc/proc.h"
#include "board.h"
#include "console.h"

/* STM32-specific adapter only. Boot/attachment, gates and notifications use
 * the architecture-independent A20 fabric driver in core/shared_fabric.c. */
static uint16_t bus_read16(void *ctx, unsigned offset) {
    (void)ctx;
    return *(volatile uint16_t *)(MCU_FABRIC_BASE + offset);
}
static uint32_t bus_read32(void *ctx, unsigned offset) {
    (void)ctx;
    return *(volatile uint32_t *)(MCU_FABRIC_BASE + offset);
}
static void bus_write16(void *ctx, unsigned offset, uint16_t value) {
    (void)ctx;
    *(volatile uint16_t *)(MCU_FABRIC_BASE + offset) = value;
}
static void bus_barrier(void *ctx) {
    (void)ctx;
    __asm__ __volatile__("dsb" ::: "memory");
}
static void bus_relax(void *ctx) {
    (void)ctx;
    arch_cpu_relax();
}
static uintptr_t bus_irq_save(void *ctx) {
    (void)ctx;
    return arch_irq_save();
}
static void bus_irq_restore(void *ctx, uintptr_t token) {
    (void)ctx;
    arch_irq_restore(token);
}
static const a20_fabric_io_t bus_io = {
    .read16 = bus_read16,
    .read32 = bus_read32,
    .write16 = bus_write16,
    .barrier = bus_barrier,
    .relax = bus_relax,
    .irq_save = bus_irq_save,
    .irq_restore = bus_irq_restore,
};
static const a20_fabric_t fabric = {.io = &bus_io};

const a20_fabric_t *arch_shared_fabric(void) {
    return &fabric;
}
void arch_shared_fabric_stop(void) {
    arch_halt();
}

/* No shared variables, compiler atomics, or kernel locks may be used before
 * fabric CLAIM succeeds. Registers are 16-bit transactions on the FSMC bus. */
static void fabric_fault(uint16_t code) __attribute__((noreturn));
static void fabric_fault(uint16_t code) {
    a20_fabric_latch_fault(&fabric, code);
    arch_halt();
}

uint32_t mcu_fabric_lock(unsigned lock) {
    uintptr_t token;
    if (a20_fabric_gate_lock(&fabric, lock, &token))
        arch_halt();
    return token;
}

void mcu_fabric_unlock(unsigned lock, uint32_t flags) {
    if (a20_fabric_gate_unlock(&fabric, lock, flags))
        arch_halt();
}

void mcu_fabric_poll_ipi(void) {
    int pending = a20_fabric_take_notification(&fabric);
    if (pending < 0)
        arch_halt();
    if (pending)
        proc_sched_handle_reschedule_ipi();
}

static unsigned fabric_discover(smp_cpu_desc_t *cpus, unsigned capacity,
                                uint64_t boot_hw_id) {
    unsigned count = bus_read16(NULL, A20_FABRIC_REG_CPU_COUNT);
    if (boot_hw_id != 0 || count != capacity)
        fabric_fault(2);
    for (unsigned cpu = 0; cpu < count; cpu++) {
        cpus[cpu].logical_id = cpu;
        cpus[cpu].hw_id = cpu;
        cpus[cpu].platform_cookie = 0;
    }
    return count;
}

static int fabric_start(const smp_cpu_desc_t *cpu, uintptr_t entry,
                        uintptr_t context) {
    (void)entry;
    if (!cpu || cpu->hw_id != context || context == 0 ||
        context >= CONFIG_NR_CPUS)
        return -1;
    /* RELEASE is write-one-to-set. Reset_Handler is already waiting on every
     * secondary; it is not another independent kernel_main invocation. */
    return a20_fabric_release(&fabric, context);
}

static void fabric_ipi(const smp_cpu_desc_t *cpu, smp_ipi_reason_t reason) {
    if (reason != SMP_IPI_RESCHEDULE)
        fabric_fault(3);
    if (a20_fabric_notify(&fabric, 1U << cpu->logical_id))
        arch_halt();
}

const smp_platform_ops_t mcu_fabric_smp_ops = {
    .discover = fabric_discover,
    .start = fabric_start,
    .send_ipi = fabric_ipi,
};

uint64_t arch_smp_boot_hw_id(void) {
    return 0;
}

void trap_init(void) {
    extern char armv7m_vector_table[];
    arch_write_tvec((uintptr_t)armv7m_vector_table);
}

/* No canary read from shared BSS before the one-time cold initialization. */
__attribute__((no_stack_protector)) void armv7m_shared_reset(void) {
    extern unsigned armv7m_private_cpu_id;
    extern uint32_t _shared_data_load[], _shared_data_start[];
    extern uint32_t _shared_data_end[], _shared_bss_start[], _shared_bss_end[];
    extern void kernel_main(void);
    arch_local_irq_disable();
    mcu_fabric_bus_init();
    unsigned cpu;
    if (a20_fabric_probe_attach(&fabric, CONFIG_NR_CPUS, &cpu))
        arch_halt();
    armv7m_private_cpu_id = cpu;
    /* ATTACHED is port-specific read-and-set, sticky until whole-domain
     * reset. Never clear BSS or rejoin with a stale saved task after reset. */
    if (cpu == 0) {
        if (a20_fabric_claim(&fabric))
            arch_halt();
        uint32_t *src = _shared_data_load;
        for (uint32_t *dst = _shared_data_start; dst < _shared_data_end;)
            *dst++ = *src++;
        for (uint32_t *dst = _shared_bss_start; dst < _shared_bss_end;)
            *dst++ = 0;
        __asm__ __volatile__("dsb" ::: "memory");
        armv7m_private_shared_ready = 1;
        kernel_main();
    } else {
        if (a20_fabric_wait_release(&fabric, cpu))
            arch_halt();
        armv7m_private_shared_ready = 1;
        if (current_board && current_board->early_init)
            current_board->early_init();
        /* UART driver state is chip-private; do not reset the shared ring. */
        arch_uart_init();
        smp_secondary_init(cpu);
        printf("[SHARED] native secondary cpu=%u online\n", cpu);
        arch_local_irq_enable();
        idle_loop();
    }
    arch_halt();
}
#endif
