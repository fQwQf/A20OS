#ifdef CONFIG_LOONGARCH64

#include "core/smp.h"
#include "core/cpu.h"
#include "core/defs.h"
#include "proc/proc.h"

#define IOCSR_IPI_STATUS       0x1000
#define IOCSR_IPI_ENABLE       0x1004
#define IOCSR_IPI_CLEAR        0x100c
#define IOCSR_IPI_SEND         0x1040
#define IOCSR_SEND_BLOCKING    (1UL << 31)
#define IOCSR_SEND_CPU_SHIFT   16

#define IPI_BOOT_VECTOR        0
#define IPI_RESCHEDULE_VECTOR  1
#define IPI_RESCHEDULE         (1U << IPI_RESCHEDULE_VECTOR)
static inline uint32_t iocsr_read32(uint32_t reg)
{
    uint32_t value;
    __asm__ __volatile__("iocsrrd.w %0, %1" : "=r"(value) : "r"(reg));
    return value;
}

static inline void iocsr_write32(uint32_t value, uint32_t reg)
{
    __asm__ __volatile__("iocsrwr.w %0, %1" :: "r"(value), "r"(reg) : "memory");
}

void loongarch64_iocsr_write64(uint64_t value, uint32_t reg)
{
    __asm__ __volatile__("iocsrwr.d %0, %1" :: "r"(value), "r"(reg) : "memory");
}

void loongarch64_smp_send_ipi(unsigned cpu, unsigned vector)
{
    uint32_t value = (uint32_t)IOCSR_SEND_BLOCKING | vector |
                     (cpu << IOCSR_SEND_CPU_SHIFT);
    arch_wmb();
    iocsr_write32(value, IOCSR_IPI_SEND);
}

void loongarch64_smp_local_init(void)
{
    iocsr_write32(0xffffffffU, IOCSR_IPI_CLEAR);
    iocsr_write32(0xffffffffU, IOCSR_IPI_ENABLE);
}

uint64_t arch_smp_boot_hw_id(void)
{
    return arch_current_cpu_id();
}

uintptr_t arch_smp_secondary_entry_pa(void)
{
    extern char _start[];
    return (uintptr_t)_start;
}

int arch_smp_secondary_prepare(const smp_cpu_desc_t *cpu)
{
    (void)cpu;
    return 0;
}

void loongarch64_smp_handle_ipi(int from_user)
{
    uint32_t action = iocsr_read32(IOCSR_IPI_STATUS);
    iocsr_write32(action, IOCSR_IPI_CLEAR);
    arch_mb();

    if (action & IPI_RESCHEDULE)
        proc_sched_handle_reschedule_ipi();
    (void)from_user;
}

void loongarch64_secondary_entry(unsigned cpu_id)
{
    unsigned logical_id;
    if (smp_hw_to_logical(cpu_id, &logical_id) != 0 || logical_id == 0)
        arch_halt();

    arch_local_irq_disable();
    smp_secondary_init(logical_id);
    arch_local_irq_enable();
    idle_loop();
}

#endif /* CONFIG_LOONGARCH64 */
