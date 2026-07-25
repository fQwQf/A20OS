#ifdef CONFIG_X86_64

#include "core/smp.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "cpu.h"
#include "platform.h"

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
static volatile unsigned ap_started[CONFIG_NR_CPUS];
static int trampoline_ready;
static int trampoline_usable = 1;

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
    unsigned cpu;
    if (smp_hw_to_logical(apic_id, &cpu) == 0)
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

uint64_t arch_smp_boot_hw_id(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1), "c"(0));
    return ebx >> 24;
}

uintptr_t arch_smp_secondary_entry_pa(void)
{
    return (uintptr_t)x86_64_secondary_entry;
}

int arch_smp_secondary_prepare(const smp_cpu_desc_t *cpu)
{
    (void)cpu;
    return 0;
}

int x86_64_smp_start_ap(unsigned apic_id, uintptr_t entry,
                        unsigned logical_id)
{
#if CONFIG_NR_CPUS > 1
    if (!trampoline_usable || logical_id == 0 || logical_id >= CONFIG_NR_CPUS)
        return -1;

    if (!trampoline_ready) {
        size_t trampoline_size =
            x86_64_ap_trampoline_end - x86_64_ap_trampoline_start;
        if (trampoline_size > 4096) {
            printf("[SMP] AP trampoline too large: %lu\n",
                   (unsigned long)trampoline_size);
            trampoline_usable = 0;
            return -1;
        }
        memcpy((void *)(PAGE_OFFSET + AP_TRAMPOLINE_PA),
               x86_64_ap_trampoline_start, trampoline_size);
        *(uint32_t *)trampoline_field(x86_64_ap_trampoline_gdt_base) =
            AP_TRAMPOLINE_PA + (uint32_t)(x86_64_ap_trampoline_gdt -
                                          x86_64_ap_trampoline_start);
        *(uint32_t *)trampoline_field(x86_64_ap_trampoline_cr3) =
            (uint32_t)((uintptr_t)boot_pgdir - PAGE_OFFSET);
        trampoline_ready = 1;
    }

    __atomic_store_n(&ap_started[logical_id], 0, __ATOMIC_RELAXED);
    *(uint64_t *)trampoline_field(x86_64_ap_trampoline_stack) =
        (uint64_t)(uintptr_t)&ap_stacks[logical_id][AP_STACK_SIZE];
    *(uint32_t *)trampoline_field(x86_64_ap_trampoline_cpu) = logical_id;
    *(uint64_t *)trampoline_field(x86_64_ap_trampoline_entry) =
        (uint64_t)entry;
    __atomic_thread_fence(__ATOMIC_RELEASE);

    if (lapic_send(apic_id, 0x0000c500) != 0)
        goto startup_timeout;
    tsc_delay(10000000);
    if (lapic_send(apic_id, 0x00008500) != 0)
        goto startup_timeout;
    tsc_delay(200000);
    if (lapic_send(apic_id, 0x00004600 | (AP_TRAMPOLINE_PA >> 12)) != 0)
        goto startup_timeout;
    tsc_delay(200000);
    if (lapic_send(apic_id, 0x00004600 | (AP_TRAMPOLINE_PA >> 12)) != 0)
        goto startup_timeout;

    uint64_t deadline = timer_get_ticks() + AP_START_WAIT_TICKS;
    while (!__atomic_load_n(&ap_started[logical_id], __ATOMIC_ACQUIRE) &&
           timer_get_ticks() < deadline)
        cpu_relax();
    if (__atomic_load_n(&ap_started[logical_id], __ATOMIC_ACQUIRE))
        return 0;

startup_timeout:
    trampoline_usable = 0;
    printf("[SMP] AP cpu=%u apic=%u startup timed out\n", logical_id, apic_id);
    return -1;
#else
    (void)apic_id;
    (void)entry;
    (void)logical_id;
    return -1;
#endif
}

void x86_64_smp_send_ipi(unsigned apic_id, uint32_t vector)
{
#if CONFIG_NR_CPUS > 1
    (void)lapic_send(apic_id, vector);
#else
    (void)apic_id;
    (void)vector;
#endif
}

void x86_64_smp_secondary_init(void)
{
    x86_64_enable_fpu_sse();
}

void x86_64_secondary_entry(unsigned cpu_id)
{
    if (cpu_id == 0 || cpu_id >= CONFIG_NR_CPUS)
        arch_halt();
    __atomic_store_n(&ap_started[cpu_id], 1, __ATOMIC_RELEASE);
    smp_secondary_init(cpu_id);
    arch_local_irq_enable();
    idle_loop();
}

#endif
