#ifdef CONFIG_X86_64

#include "core/smp.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/trap.h"
#include "proc/proc.h"
#include "cpu.h"
#include "platform.h"
#include "firmware.h"

#define AP_TRAMPOLINE_PA 0x7000UL
#define AP_STACK_SIZE    (16 * 1024)
#define AP_ICR_WAIT_TICKS (TICKS_PER_SEC / 10)
#define AP_START_WAIT_TICKS (TICKS_PER_SEC * 2)

#if CONFIG_NR_CPUS > 8
#error x86_64 syscall entry stubs currently support at most 8 CPUs
#endif

#if CONFIG_NR_CPUS > 1
static uint8_t ap_stacks[CONFIG_NR_CPUS][AP_STACK_SIZE] __attribute__((aligned(16)));
#endif
static unsigned apic_ids[CONFIG_NR_CPUS];
static volatile unsigned ap_started[CONFIG_NR_CPUS];
static unsigned discovered_cpus = 1;

extern char x86_64_ap_trampoline_start[];
extern char x86_64_ap_trampoline_end[];
extern char x86_64_ap_trampoline_gdt_base[];
extern char x86_64_ap_trampoline_gdt[];
extern char x86_64_ap_trampoline_cr3[];
extern char x86_64_ap_trampoline_stack[];
extern char x86_64_ap_trampoline_cpu[];
extern char x86_64_ap_trampoline_entry[];
extern void x86_64_secondary_entry(unsigned cpu_id);

#if CONFIG_NR_CPUS > 1
static void *trampoline_field(char *symbol)
{
    return (void *)(PAGE_OFFSET + AP_TRAMPOLINE_PA +
                    (uintptr_t)(symbol - x86_64_ap_trampoline_start));
}
#endif

unsigned x86_64_apic_to_cpu(unsigned apic_id)
{
    for (unsigned cpu = 0; cpu < discovered_cpus; cpu++)
        if (apic_ids[cpu] == apic_id)
            return cpu;
    return 0;
}

#if CONFIG_NR_CPUS > 1
static int lapic_wait_icr(void)
{
    uint64_t deadline = timer_get_ticks() + AP_ICR_WAIT_TICKS;
    while (lapic_read(LAPIC_ICR_LOW) & (1U << 12)) {
        if (timer_get_ticks() >= deadline)
            return -1;
        cpu_relax();
    }
    return 0;
}

static int lapic_send(unsigned apic_id, uint32_t command)
{
    if (lapic_wait_icr() != 0)
        return -1;
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, command);
    return lapic_wait_icr();
}

static void tsc_delay(uint64_t cycles)
{
    uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < cycles)
        cpu_relax();
}
#endif

void smp_send_reschedule(unsigned cpu)
{
#if CONFIG_NR_CPUS > 1
    if (cpu >= discovered_cpus || !smp_cpu_is_online(cpu))
        return;
    (void)lapic_send(apic_ids[cpu], IRQ_VECTOR_RESCHEDULE);
#else
    (void)cpu;
#endif
}

void smp_init(void)
{
    smp_core_init();
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1), "c"(0));
    unsigned bsp_apic = ebx >> 24;
    discovered_cpus = firmware_acpi_apic_ids(apic_ids, CONFIG_NR_CPUS,
                                             bsp_apic);
    if (!discovered_cpus) {
        discovered_cpus = 1;
        apic_ids[0] = bsp_apic;
        printf("[SMP] No valid ACPI MADT; using BSP only\n");
    }
    ap_started[0] = 1;
}

void smp_boot_secondaries(void)
{
#if CONFIG_NR_CPUS > 1
    if (discovered_cpus <= 1)
        return;

    size_t trampoline_size = x86_64_ap_trampoline_end - x86_64_ap_trampoline_start;
    if (trampoline_size > 4096) {
        printf("[SMP] AP trampoline too large: %lu\n", (unsigned long)trampoline_size);
        return;
    }
    memcpy((void *)(PAGE_OFFSET + AP_TRAMPOLINE_PA),
           x86_64_ap_trampoline_start, trampoline_size);
    *(uint32_t *)trampoline_field(x86_64_ap_trampoline_gdt_base) =
        AP_TRAMPOLINE_PA + (uint32_t)(x86_64_ap_trampoline_gdt -
                                      x86_64_ap_trampoline_start);
    *(uint32_t *)trampoline_field(x86_64_ap_trampoline_cr3) =
        (uint32_t)((uintptr_t)boot_pgdir - PAGE_OFFSET);
    *(uint64_t *)trampoline_field(x86_64_ap_trampoline_entry) =
        (uint64_t)(uintptr_t)x86_64_secondary_entry;

    for (unsigned cpu = 1; cpu < discovered_cpus; cpu++) {
        *(uint64_t *)trampoline_field(x86_64_ap_trampoline_stack) =
            (uint64_t)(uintptr_t)&ap_stacks[cpu][AP_STACK_SIZE];
        *(uint32_t *)trampoline_field(x86_64_ap_trampoline_cpu) = cpu;
        __atomic_thread_fence(__ATOMIC_RELEASE);

        if (lapic_send(apic_ids[cpu], 0x0000c500) != 0)
            goto startup_timeout;
        tsc_delay(10000000);
        if (lapic_send(apic_ids[cpu], 0x00008500) != 0)
            goto startup_timeout;
        tsc_delay(200000);
        if (lapic_send(apic_ids[cpu], 0x00004600 | (AP_TRAMPOLINE_PA >> 12)) != 0)
            goto startup_timeout;
        tsc_delay(200000);
        if (lapic_send(apic_ids[cpu], 0x00004600 | (AP_TRAMPOLINE_PA >> 12)) != 0)
            goto startup_timeout;

        uint64_t deadline = timer_get_ticks() + AP_START_WAIT_TICKS;
        while (!__atomic_load_n(&ap_started[cpu], __ATOMIC_ACQUIRE) &&
               timer_get_ticks() < deadline)
            cpu_relax();
        if (!__atomic_load_n(&ap_started[cpu], __ATOMIC_ACQUIRE)) {
startup_timeout:
            printf("[SMP] AP cpu=%u apic=%u startup timed out\n",
                   cpu, apic_ids[cpu]);
            /* The timed-out AP may still consume the shared parameter slot.
             * Do not reuse it for another AP. */
            break;
        }
    }
    printf("[SMP] %u/%u CPUs online\n", smp_online_cpu_count(), discovered_cpus);
#endif
}

void smp_secondary_init(unsigned cpu_id)
{
    proc_init_secondary(cpu_id);
    x86_64_enable_fpu_sse();
    trap_init();
    timer_init();
    smp_cpu_mark_online(cpu_id);
    __atomic_store_n(&ap_started[cpu_id], 1, __ATOMIC_RELEASE);
}

void x86_64_secondary_entry(unsigned cpu_id)
{
    smp_secondary_init(cpu_id);
    arch_local_irq_enable();
    idle_loop();
}

#endif
