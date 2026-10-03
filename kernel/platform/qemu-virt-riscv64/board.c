#ifdef CONFIG_RISCV64

#include "drivers/core/driver_core.h"
#include "drivers/irqchip/plic.h"
#include "core/arch.h"
#include "core/cpu.h"
#include "core/panic.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "firmware.h"

/* The PLIC body is shared: see kernel/drivers/irqchip/plic.c.  QEMU virt and
 * every physical RISC-V board program the same controller and differ only in
 * the MMIO base published here. */
static uint64_t rv64_plic_hart_id(void) {
    return arch_cpu_hart_id(cpu_current_id());
}

static uint64_t rv64_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t rv64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t rv64_sbi_timer_ops = {
    .read_ticks    = rv64_timer_read_ticks,
    .ticks_per_sec = rv64_timer_ticks_per_sec,
};

static unsigned rv64_smp_discover(smp_cpu_desc_t *cpus, unsigned capacity,
                                  uint64_t boot_hart) {
    printf("[SMP] qemu boot hart=%lu topology=identity\n",
           (unsigned long)boot_hart);
    for (unsigned cpu = 0; cpu < capacity; cpu++) {
        cpus[cpu].hw_id = (boot_hart + cpu) % capacity;
        cpus[cpu].platform_cookie = 0;
    }
    return capacity;
}

static int rv64_smp_start(const smp_cpu_desc_t *cpu, uintptr_t entry_pa,
                          uintptr_t logical_context) {
    int64_t err = sbi_hart_start(cpu->hw_id, entry_pa, logical_context);
    /* A hart which entered _start before topology publication is already
     * spinning on riscv64_smp_release and is a valid started secondary. */
    if (err == SBI_ERR_ALREADY_AVAILABLE)
        return 0;
    return (int)err;
}

static void rv64_smp_send_ipi(const smp_cpu_desc_t *cpu,
                              smp_ipi_reason_t reason) {
    (void)reason;
    sbi_send_ipi(1UL, cpu->hw_id);
}

/* IPI-based remote TLB flush.  The initiating CPU advances the target's
 * request generation, sends an IPI, and waits for the corresponding completed
 * generation; the soft IRQ handler publishes that acknowledgement only after
 * sfence.vma has retired.
 * This replaces SBI REMOTE SFENCE.VMA, which is not reliably emulated by
 * QEMU TCG for remote harts.  The caller must not hold a spinlock with
 * interrupts disabled (remote-flush calls are deferred until after unlock). */
static _Atomic uint32_t rv64_tlb_request[CONFIG_NR_CPUS];
static _Atomic uint32_t rv64_tlb_ack[CONFIG_NR_CPUS];
static _Atomic uint32_t rv64_tlb_ipi_serviced;
static _Atomic uint32_t rv64_tlb_flush_calls;

uint32_t rv64_tlb_flush_stats(uint32_t *serviced)
{
    if (serviced)
        *serviced = __atomic_load_n(&rv64_tlb_ipi_serviced, __ATOMIC_RELAXED);
    return __atomic_load_n(&rv64_tlb_flush_calls, __ATOMIC_RELAXED);
}

static int rv64_smp_remote_tlb_flush(uint32_t pending, uint64_t addr,
                                     uint64_t size) {
    (void)addr;
    (void)size;
    uint32_t expected[CONFIG_NR_CPUS] = {0};
    uint32_t self = 1U << arch_current_cpu_id();
    pending &= ~self;
    if (!pending)
        return 0;
    __atomic_fetch_add(&rv64_tlb_flush_calls, 1, __ATOMIC_RELAXED);

    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(pending & (1U << cpu)))
            continue;
        uint64_t hw_id;
        if (smp_logical_to_hw(cpu, &hw_id) < 0)
            continue;
        expected[cpu] = __atomic_add_fetch(&rv64_tlb_request[cpu], 1,
                                           __ATOMIC_ACQ_REL);
        sbi_send_ipi(1UL, hw_id);
    }

    /*
     * Wait with interrupts enabled: the targets must service the soft IRQ
     * (sfence + ack).  If we are inside a trap with IRQs off, they may be
     * waiting on us for their own flush; enabling interrupts here lets us
     * service those IPIs and breaks the ABBA cycle.
     */
    int irqs_were_off = !arch_irqs_enabled();
    if (irqs_were_off)
        arch_local_irq_enable();
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(pending & (1U << cpu)))
            continue;
        uint64_t wait_start = timer_get_ticks();
        while ((int32_t)(__atomic_load_n(&rv64_tlb_ack[cpu],
                                         __ATOMIC_ACQUIRE) -
                         expected[cpu]) < 0) {
            if (timer_get_ticks() - wait_start > 5UL * ARCH_TIMER_FREQ) {
                printf("[RV64 TLB] timeout self=%u target=%u expected=%u "
                       "request=%u ack=%u online=0x%x\n",
                       arch_current_cpu_id(), cpu, expected[cpu],
                       __atomic_load_n(&rv64_tlb_request[cpu],
                                       __ATOMIC_ACQUIRE),
                       __atomic_load_n(&rv64_tlb_ack[cpu],
                                       __ATOMIC_ACQUIRE),
                       smp_online_cpu_mask());
                panic("RISC-V remote TLB shootdown timed out");
            }
            arch_cpu_relax();
        }
    }
    if (irqs_were_off)
        arch_local_irq_disable();
    return 0;
}

/* Called from the soft-IRQ handler.  A single interrupt may cover several
 * coalesced requests.  Loop until the completed generation catches the latest
 * request, and never acknowledge a generation before sfence.vma completes. */
void rv64_ipi_tlb_flush_handler(void)
{
    unsigned cpu = arch_current_cpu_id();
    if (cpu >= CONFIG_NR_CPUS)
        return;
    for (;;) {
        uint32_t request = __atomic_load_n(&rv64_tlb_request[cpu],
                                           __ATOMIC_ACQUIRE);
        uint32_t ack = __atomic_load_n(&rv64_tlb_ack[cpu],
                                       __ATOMIC_RELAXED);
        if (ack == request)
            break;
        __asm__ __volatile__("sfence.vma" ::: "memory");
        __atomic_store_n(&rv64_tlb_ack[cpu], request, __ATOMIC_RELEASE);
        __atomic_fetch_add(&rv64_tlb_ipi_serviced, 1, __ATOMIC_RELAXED);
    }
}

static void rv64_smp_secondary_init(const smp_cpu_desc_t *cpu) {
    (void)cpu;
    plic_configure(PLIC_BASE, rv64_plic_hart_id);
}

static const smp_platform_ops_t rv64_smp_ops = {
    .discover       = rv64_smp_discover,
    .start          = rv64_smp_start,
    .send_ipi       = rv64_smp_send_ipi,
    .remote_tlb_flush = rv64_smp_remote_tlb_flush,
    .secondary_init = rv64_smp_secondary_init,
};

static void rv64_early_init(void) {
    plic_configure(PLIC_BASE, rv64_plic_hart_id);
    riscv64_memory_init();
}

static void rv64_poweroff(void) {
    sbi_shutdown();
}

static void rv64_reboot(void) {
    sbi_reboot();
}

extern void virtio_mmio_enumerate(uintptr_t base, int max_slots, int irq_base);
extern void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end);

static void rv64_enumerate_devices(void) {
    virtio_mmio_enumerate(VIRTIO_BASE, 8, 1);
    pci_enumerate(PCIE_ECAM_BASE, 0, 1);
}

static const board_config_t qemu_virt_rv64 = {
    .name              = "qemu-virt-rv64",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_MAX_END,
    .irqchip           = &plic_irqchip_ops,
    .timer             = &rv64_sbi_timer_ops,
    .smp               = &rv64_smp_ops,
    .early_init        = rv64_early_init,
    .poweroff          = rv64_poweroff,
    .reboot            = rv64_reboot,
    .enumerate_devices = rv64_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_rv64;

#endif /* CONFIG_RISCV64 */
