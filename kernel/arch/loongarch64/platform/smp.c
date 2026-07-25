#ifdef CONFIG_LOONGARCH64

#include "core/smp.h"
#include "core/cpu.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "proc/proc.h"

#define IOCSR_IPI_STATUS       0x1000
#define IOCSR_IPI_ENABLE       0x1004
#define IOCSR_IPI_CLEAR        0x100c
#define IOCSR_IPI_SEND         0x1040
#define IOCSR_MBUF_SEND        0x1048
#define IOCSR_SEND_BLOCKING    (1UL << 31)
#define IOCSR_SEND_CPU_SHIFT   16
#define IOCSR_MBUF_DATA_SHIFT  32

#define IPI_BOOT_VECTOR        0
#define IPI_RESCHEDULE_VECTOR  1
#define IPI_RESCHEDULE         (1U << IPI_RESCHEDULE_VECTOR)
#define SMP_WAIT_LOOPS         10000000U

static volatile unsigned loongarch64_cpu_online[CONFIG_NR_CPUS];

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

static inline void iocsr_write64(uint64_t value, uint32_t reg)
{
    __asm__ __volatile__("iocsrwr.d %0, %1" :: "r"(value), "r"(reg) : "memory");
}

static void send_ipi(unsigned cpu, unsigned vector)
{
    uint32_t value = (uint32_t)IOCSR_SEND_BLOCKING | vector |
                     (cpu << IOCSR_SEND_CPU_SHIFT);
    arch_wmb();
    iocsr_write32(value, IOCSR_IPI_SEND);
}

static void send_mailbox0(unsigned cpu, uintptr_t entry)
{
    uint64_t common = IOCSR_SEND_BLOCKING |
                      ((uint64_t)cpu << IOCSR_SEND_CPU_SHIFT);

    /* QEMU implements the architected pair of masked 32-bit mailbox writes. */
    iocsr_write64(common | (1UL << 2) | (entry & 0xffffffff00000000UL),
                  IOCSR_MBUF_SEND);
    iocsr_write64(common | ((uint64_t)(uint32_t)entry << IOCSR_MBUF_DATA_SHIFT),
                  IOCSR_MBUF_SEND);
}

void smp_send_reschedule(unsigned cpu)
{
    if (CONFIG_NR_CPUS > 1 && cpu < CONFIG_NR_CPUS &&
        cpu != cpu_current_id() && smp_cpu_is_online(cpu))
        send_ipi(cpu, IPI_RESCHEDULE_VECTOR);
}

void smp_init(void)
{
    smp_core_init();
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++)
        loongarch64_cpu_online[cpu] = 0;
    loongarch64_cpu_online[0] = 1;

    iocsr_write32(0xffffffffU, IOCSR_IPI_CLEAR);
    iocsr_write32(0xffffffffU, IOCSR_IPI_ENABLE);
}

void smp_boot_secondaries(void)
{
    if (CONFIG_NR_CPUS <= 1)
        return;

    extern char _start[];
    for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++) {
        send_mailbox0(cpu, (uintptr_t)_start);
        send_ipi(cpu, IPI_BOOT_VECTOR);
    }

    for (unsigned wait = 0; wait < SMP_WAIT_LOOPS; wait++) {
        unsigned pending = 0;
        for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++)
            pending |= !__atomic_load_n(&loongarch64_cpu_online[cpu],
                                        __ATOMIC_ACQUIRE);
        if (!pending)
            break;
        cpu_relax();
    }
    for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++)
        if (!__atomic_load_n(&loongarch64_cpu_online[cpu], __ATOMIC_ACQUIRE))
            printf("[SMP] secondary start timed out: cpu=%u\n", cpu);

    printf("[SMP] %u CPUs online\n", smp_online_cpu_count());
}

void smp_secondary_init(unsigned cpu_id)
{
    arch_local_irq_disable();
    proc_init_secondary(cpu_id);
    trap_init();
    iocsr_write32(0xffffffffU, IOCSR_IPI_CLEAR);
    iocsr_write32(0xffffffffU, IOCSR_IPI_ENABLE);
    timer_init();
    smp_cpu_mark_online(cpu_id);
    __atomic_store_n(&loongarch64_cpu_online[cpu_id], 1, __ATOMIC_RELEASE);
}

void loongarch64_smp_handle_ipi(int from_user)
{
    uint32_t action = iocsr_read32(IOCSR_IPI_STATUS);
    iocsr_write32(action, IOCSR_IPI_CLEAR);
    arch_mb();

    if ((action & IPI_RESCHEDULE) && from_user)
        proc_yield();
}

void loongarch64_secondary_entry(unsigned cpu_id)
{
    if (cpu_id == 0 || cpu_id >= CONFIG_NR_CPUS)
        arch_halt();

    smp_secondary_init(cpu_id);
    arch_local_irq_enable();
    idle_loop();
}

#endif /* CONFIG_LOONGARCH64 */
