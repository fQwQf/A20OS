#ifdef CONFIG_MCU_SHARED_KERNEL
#include "shared_kernel.h"
#include "core/arch.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "drivers/core/driver_core.h"
#include "proc/proc.h"
#include "board.h"
#include "console.h"

static volatile uint16_t *reg16(unsigned offset) {
    return (volatile uint16_t *)(MCU_FABRIC_BASE + offset);
}

/* No shared variables, compiler atomics, or kernel locks may be used before
 * fabric CLAIM succeeds. Registers are 16-bit transactions on the FSMC bus. */
static void fabric_fault(uint16_t code) __attribute__((noreturn));
static void fabric_fault(uint16_t code) {
    *reg16(MCU_FABRIC_REG_FAULT) = code;
    arch_halt();
}

uint32_t mcu_fabric_lock(unsigned lock) {
    uint32_t flags = arch_irq_save();
    volatile uint16_t *gate = reg16(MCU_FABRIC_REG_LOCKS + lock * 4U);
    for (;;) {
        if (*reg16(MCU_FABRIC_REG_FAULT))
            arch_halt();
        if (*gate == 1U)
            break;
        arch_cpu_relax();
    }
    /* The controller completes all previous shared writes before granting. */
    __asm__ __volatile__("dsb" ::: "memory");
    return flags;
}

void mcu_fabric_unlock(unsigned lock, uint32_t flags) {
    if (*reg16(MCU_FABRIC_REG_FAULT))
        arch_halt();
    __asm__ __volatile__("dsb" ::: "memory");
    *reg16(MCU_FABRIC_REG_LOCKS + lock * 4U) = 1U;
    __asm__ __volatile__("dsb" ::: "memory");
    arch_irq_restore(flags);
}

void mcu_fabric_poll_ipi(void) {
    if (*reg16(MCU_FABRIC_REG_FAULT))
        arch_halt();
    /* ACK atomically consumes this port's pending bit. An arriving request
     * after the read remains pending for the next local SysTick. */
    if (*reg16(MCU_FABRIC_REG_IPI_ACK))
        proc_sched_handle_reschedule_ipi();
}

static unsigned fabric_discover(smp_cpu_desc_t *cpus, unsigned capacity,
                                uint64_t boot_hw_id) {
    unsigned count = *reg16(MCU_FABRIC_REG_CPU_COUNT);
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
    __asm__ __volatile__("dsb" ::: "memory");
    *reg16(MCU_FABRIC_REG_RELEASE) = (uint16_t)(1U << context);
    return 0;
}

static void fabric_ipi(const smp_cpu_desc_t *cpu, smp_ipi_reason_t reason) {
    if (reason != SMP_IPI_RESCHEDULE)
        fabric_fault(3);
    __asm__ __volatile__("dsb" ::: "memory");
    *reg16(MCU_FABRIC_REG_IPI_SET) = (uint16_t)(1U << cpu->logical_id);
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

void armv7m_shared_reset(void) {
    extern unsigned armv7m_private_cpu_id;
    extern uint32_t _shared_data_load[], _shared_data_start[];
    extern uint32_t _shared_data_end[], _shared_bss_start[], _shared_bss_end[];
    extern void kernel_main(void);
    arch_local_irq_disable();
    mcu_fabric_bus_init();
    uint32_t magic = *(volatile uint32_t *)MCU_FABRIC_BASE;
    if (magic != MCU_FABRIC_MAGIC)
        arch_halt();
    unsigned cpu = *reg16(MCU_FABRIC_REG_CPU_ID);
    if (cpu >= CONFIG_NR_CPUS ||
        *reg16(MCU_FABRIC_REG_CPU_COUNT) != CONFIG_NR_CPUS)
        fabric_fault(4);
    armv7m_private_cpu_id = cpu;
    /* ATTACHED is port-specific read-and-set, sticky until whole-domain
     * reset. Never clear BSS or rejoin with a stale saved task after reset. */
    if (*reg16(MCU_FABRIC_REG_ATTACHED) != 1U)
        fabric_fault(5);
    if (cpu == 0) {
        if (*reg16(MCU_FABRIC_REG_CLAIM) != 1U)
            fabric_fault(6);
        uint32_t *src = _shared_data_load;
        for (uint32_t *dst = _shared_data_start; dst < _shared_data_end;)
            *dst++ = *src++;
        for (uint32_t *dst = _shared_bss_start; dst < _shared_bss_end;)
            *dst++ = 0;
        __asm__ __volatile__("dsb" ::: "memory");
        armv7m_private_shared_ready = 1;
        kernel_main();
    } else {
        while (!(*reg16(MCU_FABRIC_REG_RELEASE) & (1U << cpu))) {
            if (*reg16(MCU_FABRIC_REG_FAULT))
                arch_halt();
            arch_cpu_relax();
        }
        __asm__ __volatile__("dsb" ::: "memory");
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
