#include "core/smp.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "core/trap.h"
#include "drivers/core/driver_core.h"
#include "proc/proc.h"

#define SMP_START_WAIT_LOOPS 10000000U

static smp_cpu_desc_t cpu_descs[CONFIG_NR_CPUS];
static unsigned present_count = 1;
static unsigned configured_count = 1;
static uint32_t online_mask = 1U;

static const smp_platform_ops_t *platform_ops(void)
{
    return current_board ? current_board->smp : 0;
}

static void smp_set_boot_only(uint64_t boot_hw_id)
{
    cpu_descs[0].logical_id = 0;
    cpu_descs[0].hw_id = boot_hw_id;
    cpu_descs[0].platform_cookie = 0;
    present_count = 1;
    configured_count = 1;
}

void smp_cpu_mark_online(unsigned cpu)
{
    if (cpu < configured_count)
        __atomic_fetch_or(&online_mask, 1U << cpu, __ATOMIC_RELEASE);
}

unsigned smp_present_cpu_count(void)
{
    return present_count;
}

unsigned smp_configured_cpu_count(void)
{
    return configured_count;
}

uint32_t smp_online_cpu_mask(void)
{
    return __atomic_load_n(&online_mask, __ATOMIC_ACQUIRE);
}

unsigned smp_online_cpu_count(void)
{
    return (unsigned)__builtin_popcount(smp_online_cpu_mask());
}

int smp_cpu_is_online(unsigned cpu)
{
    return cpu < configured_count &&
           (smp_online_cpu_mask() & (1U << cpu)) != 0;
}

const smp_cpu_desc_t *smp_cpu_desc(unsigned logical_id)
{
    if (logical_id >= configured_count)
        return 0;
    return &cpu_descs[logical_id];
}

int smp_hw_to_logical(uint64_t hw_id, unsigned *logical_id)
{
    if (!logical_id)
        return -1;
    for (unsigned cpu = 0; cpu < configured_count; cpu++) {
        if (cpu_descs[cpu].hw_id == hw_id) {
            *logical_id = cpu;
            return 0;
        }
    }
    return -1;
}

int smp_logical_to_hw(unsigned logical_id, uint64_t *hw_id)
{
    if (!hw_id || logical_id >= configured_count)
        return -1;
    *hw_id = cpu_descs[logical_id].hw_id;
    return 0;
}

__attribute__((weak)) uint64_t arch_smp_boot_hw_id(void)
{
    return 0;
}

__attribute__((weak)) uintptr_t arch_smp_secondary_entry_pa(void)
{
    return 0;
}

__attribute__((weak)) int arch_smp_secondary_prepare(const smp_cpu_desc_t *cpu)
{
    (void)cpu;
    return 0;
}

void smp_init(void)
{
    const smp_platform_ops_t *ops = platform_ops();
    uint64_t boot_hw_id = arch_smp_boot_hw_id();

    smp_set_boot_only(boot_hw_id);
    __atomic_store_n(&online_mask, 1U, __ATOMIC_RELEASE);
    if (!ops || !ops->discover)
        return;

    unsigned discovered = ops->discover(cpu_descs, CONFIG_NR_CPUS, boot_hw_id);
    if (!discovered) {
        smp_set_boot_only(boot_hw_id);
        return;
    }

    if (discovered > CONFIG_NR_CPUS) {
        printf("[SMP] platform returned %u CPUs for capacity %u; truncating\n",
               discovered, CONFIG_NR_CPUS);
        discovered = CONFIG_NR_CPUS;
    }
    present_count = discovered;
    configured_count = discovered;

    for (unsigned cpu = 0; cpu < configured_count; cpu++) {
        for (unsigned other = cpu + 1; other < configured_count; other++) {
            if (cpu_descs[cpu].hw_id == cpu_descs[other].hw_id) {
                printf("[SMP] duplicate hardware CPU ID %lu; using boot CPU only\n",
                       (unsigned long)cpu_descs[cpu].hw_id);
                smp_set_boot_only(boot_hw_id);
                return;
            }
        }
    }

    unsigned boot = configured_count;
    for (unsigned cpu = 0; cpu < configured_count; cpu++) {
        if (cpu_descs[cpu].hw_id == boot_hw_id) {
            boot = cpu;
            break;
        }
    }
    if (boot == configured_count) {
        printf("[SMP] boot CPU hw=%lu missing from topology; using boot CPU only\n",
               (unsigned long)boot_hw_id);
        smp_set_boot_only(boot_hw_id);
        return;
    }
    if (boot != 0) {
        smp_cpu_desc_t tmp = cpu_descs[0];
        cpu_descs[0] = cpu_descs[boot];
        cpu_descs[boot] = tmp;
    }
    for (unsigned cpu = 0; cpu < configured_count; cpu++)
        cpu_descs[cpu].logical_id = cpu;
}

void smp_boot_secondaries(void)
{
#if CONFIG_NR_CPUS > 1
    const smp_platform_ops_t *ops = platform_ops();
    uint32_t started_mask = 0;

    if (configured_count <= 1 || !ops || !ops->start)
        return;

    uintptr_t entry_pa = arch_smp_secondary_entry_pa();
    for (unsigned cpu = 1; cpu < configured_count; cpu++) {
        int err = arch_smp_secondary_prepare(&cpu_descs[cpu]);
        if (!err)
            err = ops->start(&cpu_descs[cpu], entry_pa, cpu);
        if (!err)
            started_mask |= 1U << cpu;
        else
            printf("[SMP] secondary start failed: cpu=%u hw=%lu err=%d\n",
                   cpu, (unsigned long)cpu_descs[cpu].hw_id, err);
    }

    for (unsigned wait = 0; wait < SMP_START_WAIT_LOOPS; wait++) {
        if ((smp_online_cpu_mask() & started_mask) == started_mask)
            break;
        cpu_relax();
    }

    uint32_t timed_out = started_mask & ~smp_online_cpu_mask();
    for (unsigned cpu = 1; cpu < configured_count; cpu++) {
        if (timed_out & (1U << cpu))
            printf("[SMP] secondary start timed out: cpu=%u hw=%lu\n",
                   cpu, (unsigned long)cpu_descs[cpu].hw_id);
    }
    printf("[SMP] %u/%u configured CPUs online\n",
           smp_online_cpu_count(), configured_count);
#endif
}

void smp_send_reschedule(unsigned cpu)
{
    const smp_platform_ops_t *ops = platform_ops();

    if (cpu >= CONFIG_NR_CPUS || !ops || !ops->send_ipi ||
        !smp_cpu_is_online(cpu))
        return;
    ops->send_ipi(&cpu_descs[cpu], SMP_IPI_RESCHEDULE);
}

int smp_remote_tlb_flush(uint32_t logical_mask, uint64_t addr, uint64_t size)
{
    const smp_platform_ops_t *ops = platform_ops();
    if (!logical_mask)
        return 0;
    if (!ops || !ops->remote_tlb_flush)
        return -1;
    return ops->remote_tlb_flush(logical_mask & smp_online_cpu_mask(),
                                 addr, size);
}

int smp_remote_tlb_flush_supported(void)
{
    const smp_platform_ops_t *ops = platform_ops();
    return ops && ops->remote_tlb_flush;
}

/*
 * membarrier(2) expedited barrier support.
 *
 * A membarrier GLOBAL/PRIVATE_EXPEDITED barrier must reach every online CPU
 * so that threads on remote CPUs observe all prior stores.  A20OS reuses the
 * scheduler reschedule IPI as the barrier transport: the remote handler
 * (proc_sched_handle_reschedule_ipi) executes an acquire fence, so a CPU
 * which acknowledged the request has crossed a barrier after the requesting
 * CPU published its stores.
 *
 * The caller must not hold a spinlock with interrupts disabled, because the
 * wait loop re-enables interrupts to service inbound IPIs (same constraint as
 * the remote TLB flush).
 */
static _Atomic uint32_t mb_request[CONFIG_NR_CPUS];
static _Atomic uint32_t mb_ack[CONFIG_NR_CPUS];

/* Called by the reschedule-IPI handler after it has executed the acquire
 * fence.  arch_fence_i() gives the advertised
 * PRIVATE_EXPEDITED_SYNC_CORE command its Linux semantics: generated code
 * published before the membarrier is visible to instruction fetch on every
 * acknowledged CPU.  Applying the stronger fence to the other commands is
 * safe and keeps one request/ack transport for all membarrier variants.
 * Advances the acknowledgment only up to the request the caller published,
 * so the initiator's wait loop cannot be satisfied by a stale ack from an
 * unrelated reschedule IPI. */
void smp_membarrier_ipi_ack(unsigned cpu)
{
    if (cpu >= CONFIG_NR_CPUS)
        return;
    uint32_t req = __atomic_load_n(&mb_request[cpu], __ATOMIC_ACQUIRE);
    uint32_t ack = __atomic_load_n(&mb_ack[cpu], __ATOMIC_RELAXED);
    if (ack != req) {
        arch_fence_i();
        __atomic_store_n(&mb_ack[cpu], req, __ATOMIC_RELEASE);
    }
}

/* Issue a full memory barrier on every online CPU.  On uniprocessor
 * builds this is a single compiler/CPU fence. */
int smp_membarrier_sync_all(void)
{
    unsigned self = arch_current_cpu_id();
    uint32_t target = smp_online_cpu_mask() & ~(1U << self);
    uint32_t expected[CONFIG_NR_CPUS] = {0};

    if (!target) {
        arch_fence_i();
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return 0;
    }

    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(target & (1U << cpu)))
            continue;
        uint64_t hw_id;
        if (smp_logical_to_hw(cpu, &hw_id) < 0)
            continue;
        expected[cpu] = __atomic_add_fetch(&mb_request[cpu], 1,
                                           __ATOMIC_ACQ_REL);
        const smp_platform_ops_t *ops = platform_ops();
        if (ops && ops->send_ipi && smp_cpu_is_online(cpu))
            ops->send_ipi(&cpu_descs[cpu], SMP_IPI_RESCHEDULE);
    }

    int irqs_were_off = !arch_irqs_enabled();
    if (irqs_were_off)
        arch_local_irq_enable();
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(target & (1U << cpu)))
            continue;
        uint64_t wait_start = timer_get_ticks();
        while ((int32_t)(__atomic_load_n(&mb_ack[cpu], __ATOMIC_ACQUIRE) -
                         (int32_t)expected[cpu]) < 0) {
            if (timer_get_ticks() - wait_start > 5UL * ARCH_TIMER_FREQ) {
                printf("[SMP membarrier] timeout self=%u target=%u\n", self,
                       cpu);
                return -EIO;
            }
            arch_cpu_relax();
        }
    }
    if (irqs_were_off)
        arch_local_irq_disable();

    arch_fence_i();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

void smp_secondary_init(unsigned cpu_id)
{
    const smp_platform_ops_t *ops = platform_ops();

    if (cpu_id == 0 || cpu_id >= CONFIG_NR_CPUS ||
        cpu_id >= configured_count)
        arch_halt();
    proc_init_secondary(cpu_id);
    if (ops && ops->secondary_init)
        ops->secondary_init(&cpu_descs[cpu_id]);
    trap_init();
    timer_init();
    smp_cpu_mark_online(cpu_id);
}
