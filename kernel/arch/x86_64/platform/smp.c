#ifdef CONFIG_X86_64

#include "core/smp.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/panic.h"
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
static int trampoline_ready;
static int trampoline_usable = 1;
#endif
static volatile unsigned ap_started[CONFIG_NR_CPUS];

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

    /* The two writes of a message are adjacent with interrupts off on every
     * path that brings an AP up, so no per-message serialization is needed
     * here.  Note that ICR_LOW has no delivery status to wait on: bit 8 is the
     * delivery mode of the write that was just issued and QEMU echoes the raw
     * write back on read, so polling it would report every INIT/SIPI as still
     * in flight.  Counting TSC cycles instead ties the sequence to an assumed
     * clock frequency that says nothing about how far the target has got. */
    if (lapic_send(apic_id, 0x0000c500) != 0)
        goto startup_timeout;
    if (lapic_send(apic_id, 0x00008500) != 0)
        goto startup_timeout;
    if (lapic_send(apic_id, 0x00004600 | (AP_TRAMPOLINE_PA >> 12)) != 0)
        goto startup_timeout;
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

int x86_64_smp_send_ipi(unsigned apic_id, uint32_t vector)
{
#if CONFIG_NR_CPUS > 1
    return lapic_send(apic_id, vector);
#else
    (void)apic_id;
    (void)vector;
    return 0;
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

#if CONFIG_NR_CPUS > 1
/*
 * Remote TLB shootdown via a dedicated IPI: each target CPU invalidates the
 * requested range (reloading CR3 when it is not a single page) and
 * acknowledges its request generation.  The requester spins with interrupts
 * enabled so an ABBA pair of flushing CPUs can service each other's IPIs.
 *
 * The range cannot live in a single per-target slot: mm->tlb_lock only
 * serializes one mm, so two CPUs can publish different ranges into the same
 * target concurrently.  With one slot the second publisher overwrites the
 * first range, the target invalidates only the last one and still
 * acknowledges the generation, and the first range keeps its stale
 * translation -- the next access through it writes a frame the unmap has
 * already released.  Each generation therefore gets its own slot.
 *
 * One IPI per generation is also not at-least-once: the request is only a bit
 * in the target's IRR, and nothing re-arms it if it never gets delivered.  The
 * handler drains to tlb_flush_request, so re-sending the IPI for an
 * unacknowledged generation is idempotent and costs one MMIO pair.
 */
#define TLB_FLUSH_SLOTS 8
#define TLB_FLUSH_SLOT_MASK (TLB_FLUSH_SLOTS - 1)
#define TLB_FLUSH_RESEND_MS 100
#define TLB_FLUSH_TIMEOUT_MS 5000

struct tlb_flush_slot {
    _Atomic uint32_t gen;
    _Atomic uint64_t addr;
    _Atomic uint64_t size;
};

static struct tlb_flush_slot tlb_flush_slots[CONFIG_NR_CPUS][TLB_FLUSH_SLOTS];
static _Atomic uint32_t tlb_flush_request[CONFIG_NR_CPUS];
static _Atomic uint32_t tlb_flush_ack[CONFIG_NR_CPUS];
static spinlock_t tlb_flush_publish[CONFIG_NR_CPUS];

void x86_64_ipi_tlb_flush_handler(void)
{
    unsigned cpu = arch_current_cpu_id();
    if (cpu >= CONFIG_NR_CPUS)
        return;
    for (;;) {
        uint32_t request = __atomic_load_n(&tlb_flush_request[cpu],
                                           __ATOMIC_ACQUIRE);
        uint32_t ack = __atomic_load_n(&tlb_flush_ack[cpu],
                                       __ATOMIC_RELAXED);
        if ((int32_t)(ack - request) >= 0)
            break;
        /* Drain every generation the requester is still waiting for, the
         * newest one included.  request is INCLUSIVE here: it is the range
         * whose frame the unmap is about to release, so the loop has to
         * invalidate it and only then stop.  Treating request as an exclusive
         * bound skipped exactly that generation -- and since the unconfirmed
         * generation is almost always 1, that made every remote single-page
         * flush a no-op that still acknowledged the request, leaving the
         * requester free to release a frame a remote CPU still had mapped
         * writable.
         *
         * oldest is the first generation whose slot may already have been
         * recycled away.  It is compared only when it is NOT request itself:
         * when exactly TLB_FLUSH_SLOTS generations are outstanding, oldest and
         * request are the same generation, and stopping there on the bound
         * would skip the newest flush with no fallback left to catch it. */
        uint32_t gen = ack + 1;
        uint32_t oldest = ack + 1 + TLB_FLUSH_SLOTS;
        for (; gen != oldest || gen == request; gen++) {
            struct tlb_flush_slot *slot =
                &tlb_flush_slots[cpu][gen & TLB_FLUSH_SLOT_MASK];
            uint64_t addr = __atomic_load_n(&slot->addr, __ATOMIC_RELAXED);
            uint64_t size = __atomic_load_n(&slot->size, __ATOMIC_RELAXED);
            /* A single page is the common case -- every COW unmap and every PTE
             * clear arrives here -- and invlpg costs a few cycles where a CR3
             * reload also discards the whole user TLB.  A wider range would
             * need a per-page loop to stay targeted, so it keeps the reload,
             * which is correct at any size.  An unreadable slot means the
             * range it belonged to has been recycled: reload everything. */
            if (__atomic_load_n(&slot->gen, __ATOMIC_ACQUIRE) == gen &&
                size == PAGE_SIZE)
                arch_tlb_flush_page(addr);
            else
                arch_tlb_flush();
            if (gen == request)
                break;
        }
        /* Reached oldest without ever reaching request: the ranges in between
         * have been recycled, so one whole-TLB reload is the only correct
         * answer.  Reaching request above leaves gen == request and skips it. */
        if (gen != request)
            arch_tlb_flush();
        __atomic_store_n(&tlb_flush_ack[cpu], request, __ATOMIC_RELEASE);
    }
}

int x86_64_smp_remote_tlb_flush(uint32_t pending, uint64_t addr,
                                uint64_t size)
{
    uint32_t expected[CONFIG_NR_CPUS] = {0};
    uint64_t hw_id[CONFIG_NR_CPUS] = {0};
    uint32_t self = 1U << arch_current_cpu_id();
    pending &= ~self;
    if (!pending)
        return 0;

    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(pending & (1U << cpu)))
            continue;
        if (smp_logical_to_hw(cpu, &hw_id[cpu]) < 0) {
            pending &= ~(1U << cpu);
            continue;
        }
        spin_lock(&tlb_flush_publish[cpu]);
        uint32_t gen =
            __atomic_load_n(&tlb_flush_request[cpu], __ATOMIC_RELAXED) + 1;
        struct tlb_flush_slot *slot =
            &tlb_flush_slots[cpu][gen & TLB_FLUSH_SLOT_MASK];
        /* Range before generation, generation before request, so a target that
         * observes a new generation is guaranteed to observe the range that
         * came with it.  The lock only serializes the publishers: it is held
         * for four stores and never across the wait below. */
        __atomic_store_n(&slot->addr, addr, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->size, size, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->gen, gen, __ATOMIC_RELAXED);
        __atomic_store_n(&tlb_flush_request[cpu], gen, __ATOMIC_RELEASE);
        spin_unlock(&tlb_flush_publish[cpu]);
        expected[cpu] = gen;
    }

    /* An ICR write that never went out must not be counted as delivered: the
     * generation stays unacknowledged and the wait below keeps re-sending. */
    unsigned send_errors = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(pending & (1U << cpu)))
            continue;
        if (x86_64_smp_send_ipi((unsigned)hw_id[cpu],
                                IRQ_VECTOR_TLB_FLUSH) != 0)
            send_errors++;
    }

    int irqs_were_off = !arch_irqs_enabled();
    if (irqs_were_off)
        arch_local_irq_enable();
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(pending & (1U << cpu)))
            continue;
        uint64_t wait_start = timer_get_ticks();
        uint64_t next_resend = wait_start + MS_TO_TICKS(TLB_FLUSH_RESEND_MS);
        unsigned resends = 0;
        while ((int32_t)(__atomic_load_n(&tlb_flush_ack[cpu],
                                         __ATOMIC_ACQUIRE) -
                         expected[cpu]) < 0) {
            uint64_t now = timer_get_ticks();
            if (now - wait_start > MS_TO_TICKS(TLB_FLUSH_TIMEOUT_MS)) {
                printf("[X86_64 TLB] timeout self=%u target=%u expected=%u "
                       "request=%u ack=%u resends=%u send_errors=%u "
                       "online=0x%x\n",
                       arch_current_cpu_id(), cpu, expected[cpu],
                       __atomic_load_n(&tlb_flush_request[cpu],
                                       __ATOMIC_ACQUIRE),
                       __atomic_load_n(&tlb_flush_ack[cpu],
                                       __ATOMIC_ACQUIRE),
                       resends, send_errors, smp_online_cpu_mask());
                panic("x86_64 remote TLB shootdown timed out");
            }
            if (now >= next_resend) {
                if (x86_64_smp_send_ipi((unsigned)hw_id[cpu],
                                        IRQ_VECTOR_TLB_FLUSH) != 0)
                    send_errors++;
                resends++;
                next_resend = now + MS_TO_TICKS(TLB_FLUSH_RESEND_MS);
            }
            cpu_relax();
        }
    }
if (irqs_were_off)
            arch_local_irq_disable();
    return 0;
}

/*
 * Remote message-signalled vector setup.  A message-signalled vector needs a
 * local APIC entry, and a local APIC entry is per processor: the LVT that
 * covers vector V in the boot processor's page says nothing about the one in
 * an AP's page.  So moving a vector to another CPU means writing that CPU's
 * own LVT, which means running there.  Same shape as the TLB shootdown: a
 * per-CPU mailbox published before the request generation, a dedicated IPI,
 * and a bounded wait that lets interrupts be taken so two CPUs moving vectors
 * at each other cannot deadlock.
 */
static _Atomic uint32_t msix_vector_request[CONFIG_NR_CPUS];
static _Atomic uint32_t msix_vector_ack[CONFIG_NR_CPUS];
/* vector in bits 7:0, mask request in bit 31 -- the LVT write takes both. */
static _Atomic uint32_t msix_vector_arg[CONFIG_NR_CPUS];

void x86_64_ipi_msix_vector_handler(void)
{
    unsigned cpu = arch_current_cpu_id();
    if (cpu >= CONFIG_NR_CPUS)
        return;
    for (;;) {
        uint32_t request = __atomic_load_n(&msix_vector_request[cpu],
                                           __ATOMIC_ACQUIRE);
        uint32_t ack = __atomic_load_n(&msix_vector_ack[cpu],
                                       __ATOMIC_RELAXED);
        if (ack == request)
            break;
        uint32_t arg = __atomic_load_n(&msix_vector_arg[cpu], __ATOMIC_RELAXED);
        x86_64_msix_lvt_program(arg & 0xFFU, (arg >> 31) & 1U);
        __atomic_store_n(&msix_vector_ack[cpu], request, __ATOMIC_RELEASE);
    }
}

int x86_64_smp_msix_vector_setup(unsigned cpu, uint32_t vector, int masked)
{
#if CONFIG_NR_CPUS > 1
    if (cpu == 0 || cpu >= CONFIG_NR_CPUS || vector > 0xFFU)
        return -EINVAL;
    if (!smp_cpu_is_online(cpu))
        return -ENODEV;

    uint64_t hw_id;
    if (smp_logical_to_hw(cpu, &hw_id) < 0)
        return -EINVAL;

    __atomic_store_n(&msix_vector_arg[cpu],
                     (vector & 0xFFU) | (masked ? (1U << 31) : 0U),
                     __ATOMIC_RELAXED);
    uint32_t expected = __atomic_add_fetch(&msix_vector_request[cpu], 1,
                                           __ATOMIC_ACQ_REL);
    x86_64_smp_send_ipi((unsigned)hw_id, IRQ_VECTOR_MSIX_VECTOR);

    int irqs_were_off = !arch_irqs_enabled();
    if (irqs_were_off)
        arch_local_irq_enable();
    uint64_t wait_start = timer_get_ticks();
    while ((int32_t)(__atomic_load_n(&msix_vector_ack[cpu],
                                     __ATOMIC_ACQUIRE) - expected) < 0) {
        if (timer_get_ticks() - wait_start > TICKS_PER_SEC / 4) {
            /* A timeout leaves the request published: the target may still
             * apply it, which is the safe direction -- the caller keeps the
             * vector masked either way. */
            if (irqs_were_off)
                arch_local_irq_disable();
            printf("[X86_64 MSI-X] vector %u arm on cpu %u timed out "
                   "(request=%u ack=%u online=0x%x)\n", vector, cpu,
                   expected,
                   __atomic_load_n(&msix_vector_ack[cpu], __ATOMIC_ACQUIRE),
                   smp_online_cpu_mask());
            return -ETIMEDOUT;
        }
        cpu_relax();
    }
    if (irqs_were_off)
        arch_local_irq_disable();
    return 0;
#else
    (void)cpu;
    (void)vector;
    (void)masked;
    return -EINVAL;
#endif
}
#endif

#endif
