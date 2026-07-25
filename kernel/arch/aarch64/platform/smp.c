#ifdef CONFIG_AARCH64

#include "core/smp.h"
#include "core/cpu.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "core/trap.h"
#include "proc/proc.h"
#include "firmware.h"
#include "platform.h"

static volatile unsigned aarch64_cpu_online[CONFIG_NR_CPUS];

#ifdef CONFIG_BOARD_QEMU_VIRT_AARCH64
#if CONFIG_NR_CPUS > 8
#error "QEMU virt AArch64 SMP supports at most 8 CPUs with GICv2 SGI targets"
#endif

extern char aarch64_secondary_start[];
static uint32_t aarch64_reschedule_ipi_seen;

#define GICD_SGIR 0xF00U
#define GIC_RESCHEDULE_SGI IRQ_S_SOFT
#define SMP_WAIT_LOOPS 10000000U

void aarch64_reschedule_ipi_received(void)
{
    unsigned cpu = cpu_current_id();
    if (cpu < 32)
        __atomic_fetch_or(&aarch64_reschedule_ipi_seen, 1U << cpu,
                          __ATOMIC_RELEASE);
}
#endif

void smp_send_reschedule(unsigned cpu)
{
#ifdef CONFIG_BOARD_QEMU_VIRT_AARCH64
    if (cpu >= CONFIG_NR_CPUS || cpu >= 8 ||
        !smp_cpu_is_online(cpu))
        return;
    arch_wmb();
    *(volatile uint32_t *)(uintptr_t)(GICD_BASE + GICD_SGIR) =
        (1U << (16 + cpu)) | GIC_RESCHEDULE_SGI;
#else
    (void)cpu;
#endif
}

void smp_init(void)
{
    smp_core_init();
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++)
        aarch64_cpu_online[cpu] = 0;
    aarch64_cpu_online[0] = 1;
#ifdef CONFIG_BOARD_QEMU_VIRT_AARCH64
    aarch64_reschedule_ipi_seen = 0;
#endif
}

void smp_boot_secondaries(void)
{
#ifdef CONFIG_BOARD_QEMU_VIRT_AARCH64
    if (CONFIG_NR_CPUS <= 1)
        return;

    uint64_t entry_pa = (uint64_t)(uintptr_t)aarch64_secondary_start - PAGE_OFFSET;
    uint32_t started_mask = 0;
    for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++) {
        /* QEMU virt exposes -smp CPUs as one cluster with dense Aff0 MPIDRs. */
        int64_t err = firmware_cpu_on(cpu, entry_pa, cpu);
        if (err == 0)
            started_mask |= 1U << cpu;
        else
            printf("[SMP] PSCI CPU_ON failed: cpu=%u err=%ld\n", cpu, (long)err);
    }

    uint32_t online_mask = 0;
    for (unsigned wait = 0; wait < SMP_WAIT_LOOPS; wait++) {
        for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++) {
            if (__atomic_load_n(&aarch64_cpu_online[cpu], __ATOMIC_ACQUIRE))
                online_mask |= 1U << cpu;
        }
        if ((online_mask & started_mask) == started_mask)
            break;
        cpu_relax();
    }
    for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++)
        if ((started_mask & (1U << cpu)) && !(online_mask & (1U << cpu)))
            printf("[SMP] secondary start timed out: cpu=%u\n", cpu);

    uint32_t handshake_mask = started_mask & online_mask;
    for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++)
        if (handshake_mask & (1U << cpu))
            smp_send_reschedule(cpu);
    for (unsigned wait = 0; wait < SMP_WAIT_LOOPS; wait++) {
        if ((__atomic_load_n(&aarch64_reschedule_ipi_seen, __ATOMIC_ACQUIRE) &
             handshake_mask) == handshake_mask)
            break;
        cpu_relax();
    }
    uint32_t seen = __atomic_load_n(&aarch64_reschedule_ipi_seen,
                                    __ATOMIC_ACQUIRE);
    for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++)
        if ((handshake_mask & (1U << cpu)) && !(seen & (1U << cpu)))
            printf("[SMP] reschedule IPI timed out: cpu=%u\n", cpu);
    printf("[SMP] %u CPUs online\n", smp_online_cpu_count());
#endif
}

void smp_secondary_init(unsigned cpu_id)
{
    proc_init_secondary(cpu_id);
    trap_init();
    timer_init();
    smp_cpu_mark_online(cpu_id);
    __atomic_store_n(&aarch64_cpu_online[cpu_id], 1, __ATOMIC_RELEASE);
}

void aarch64_secondary_entry(unsigned cpu_id)
{
    if (cpu_id == 0 || cpu_id >= CONFIG_NR_CPUS)
        arch_halt();
    smp_secondary_init(cpu_id);
    arch_local_irq_enable();
    idle_loop();
}

#endif /* CONFIG_AARCH64 */
