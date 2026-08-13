#ifdef CONFIG_BOARD_VISIONFIVE2

/*
 * StarFive VisionFive 2 (星光 2) board support (SoC: JH7110).
 *
 * The VisionFive 2 boots in S-mode under OpenSBI (U-Boot SPL + OpenSBI + U-Boot
 * proper).  This board file therefore mirrors the QEMU virt RISC-V platform:
 * the PLIC is driven through the same arch PLIC macros, the timer timebase is
 * taken from the DTB when the boot loader publishes one, and secondary harts
 * are started through the SBI HSM extension.  Device MMIO bases are the fixed
 * JH7110 addresses; the RAM window starts at 0x40000000 and is narrowed by the
 * DTB memory node in riscv64_memory_init().
 *
 * Reference: RocketOS (MIT) StarFive VisionFive 2 board/driver bring-up.
 * See docs/ACKNOWLEDGMENTS.md and docs/platforms/physical-boards.md.
 */

#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/block/dw_sdio.h"
#include "drivers/net/starfive_gmac.h"
#include "core/arch.h"
#include "core/cpu.h"
#include "core/panic.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "firmware.h"
#include "platform.h"

/* VisionFive2 physical layout (JH7110).  MMIO device bases are kernel-mapped
 * through the direct map, so resources carry PAGE_OFFSET like the QEMU virt
 * RISC-V platform does. */
#define VF2_MEMORY_BASE   0x40000000UL
#define VF2_MEMORY_END    0x240000000UL   /* window covers 2/4/8 GiB variants */
#define VF2_UART0_BASE    0x10000000UL
#define VF2_SDIO_BASE     (0x16020000UL + PAGE_OFFSET)
#define VF2_SDIO_SIZE     0x10000UL
#define VF2_GMAC_BASE     (0x16040000UL + PAGE_OFFSET)  /* EQOS GMAC1 */
#define VF2_GMAC_SIZE     0x10000UL
#define VF2_GMAC_IRQ      78UL            /* PLIC line per RocketOS; verify */
#define VF2_TIMER_FREQ    24000000UL      /* JH7110 STG oscillator */

/* JH7110 SYS_CRG clock/reset controller used to power the GMAC1 (EQOS)
 * interface.  Register map per the StarFive JH7110 documentation; offsets
 * verified against the RocketOS bring-up reference. */
#define VF2_SYS_CRG_BASE  (0x13020000UL + PAGE_OFFSET)
#define VF2_CRG_GMAC1_CLK_AHB   0x184UL
#define VF2_CRG_GMAC1_CLK_AXI   0x188UL
#define VF2_CRG_GMAC1_CLK_PTP   0x198UL
#define VF2_CRG_GMAC1_CLK_TX    0x1A4UL
#define VF2_CRG_GMAC1_CLK_GTXC  0x1ACUL
#define VF2_CRG_RESET2          0x300UL
#define VF2_CRG_GMAC_AXI_RST    (1UL << 2)
#define VF2_CRG_GMAC_AHB_RST    (1UL << 3)

static void vf2_gmac_clock_init(void) {
    volatile uint32_t *crg = (volatile uint32_t *)VF2_SYS_CRG_BASE;
    /* Enable the GMAC1 clock gates (bit 31 = clock enable). */
    crg[VF2_CRG_GMAC1_CLK_AHB  / 4] |= (1U << 31);
    crg[VF2_CRG_GMAC1_CLK_AXI  / 4] |= (1U << 31);
    crg[VF2_CRG_GMAC1_CLK_PTP  / 4] |= (1U << 31);
    crg[VF2_CRG_GMAC1_CLK_TX   / 4] |= (1U << 31);
    crg[VF2_CRG_GMAC1_CLK_GTXC / 4] |= (1U << 31);
    /* Deassert the AXI/AHB resets. */
    crg[VF2_CRG_RESET2 / 4] &= ~(VF2_CRG_GMAC_AXI_RST | VF2_CRG_GMAC_AHB_RST);
}

static void vf2_plic_init(void) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
    *(volatile uint32_t *)PLIC_SENABLE(hart) = 0;
    *(volatile uint32_t *)PLIC_SPRIORITY(hart) = 0;
}

static void vf2_plic_enable(uint32_t irq) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
    *(volatile uint32_t *)PLIC_SENABLE(hart) |= (1U << irq);
    *(volatile uint32_t *)(PLIC_PRIORITY + (uint64_t)irq * 4) = 1;
}

static void vf2_plic_disable(uint32_t irq) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
    *(volatile uint32_t *)PLIC_SENABLE(hart) &= ~(1U << irq);
}

static uint32_t vf2_plic_ack(void) {
    /* PLIC claim/completion is handled by arch_handle_irq(); this callback
     * exists only so driver_irq_dispatch() can optional-call ack. */
    return 0;
}

static void vf2_plic_eoi(uint32_t irq) {
    (void)irq;
}

static const irqchip_ops_t vf2_plic_ops = {
    .init        = vf2_plic_init,
    .enable_irq  = vf2_plic_enable,
    .disable_irq = vf2_plic_disable,
    .ack         = vf2_plic_ack,
    .eoi         = vf2_plic_eoi,
};

static uint64_t vf2_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t vf2_timer_ticks_per_sec(void) {
    /* The DTB timebase-frequency is authoritative when U-Boot publishes it;
     * otherwise fall back to the JH7110 STG oscillator rate. */
    uint64_t freq = riscv64_fdt_timebase_freq();
    return freq ? freq : VF2_TIMER_FREQ;
}

static const timer_ops_t vf2_timer_ops = {
    .read_ticks    = vf2_timer_read_ticks,
    .ticks_per_sec = vf2_timer_ticks_per_sec,
};

/* ---- SMP (SBI HSM + IPI) ------------------------------------------ */

static unsigned vf2_smp_discover(smp_cpu_desc_t *cpus, unsigned capacity,
                                 uint64_t boot_hart) {
    for (unsigned cpu = 0; cpu < capacity; cpu++) {
        cpus[cpu].hw_id = (boot_hart + cpu) % capacity;
        cpus[cpu].platform_cookie = 0;
    }
    return capacity;
}

static int vf2_smp_start(const smp_cpu_desc_t *cpu, uintptr_t entry_pa,
                         uintptr_t logical_context) {
    return (int)sbi_hart_start(cpu->hw_id, entry_pa, logical_context);
}

static void vf2_smp_send_ipi(const smp_cpu_desc_t *cpu,
                             smp_ipi_reason_t reason) {
    (void)reason;
    sbi_send_ipi(1UL, cpu->hw_id);
}

/* IPI-based remote TLB flush, same contract as the QEMU virt RISC-V board:
 * the initiating CPU advances the target's request generation, sends an IPI,
 * and waits for the completed generation published after sfence.vma. */
static _Atomic uint32_t vf2_tlb_request[CONFIG_NR_CPUS];
static _Atomic uint32_t vf2_tlb_ack[CONFIG_NR_CPUS];

static int vf2_smp_remote_tlb_flush(uint32_t pending, uint64_t addr,
                                    uint64_t size) {
    (void)addr;
    (void)size;
    uint32_t expected[CONFIG_NR_CPUS] = {0};
    uint32_t self = 1U << arch_current_cpu_id();
    pending &= ~self;
    if (!pending)
        return 0;

    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(pending & (1U << cpu)))
            continue;
        uint64_t hw_id;
        if (smp_logical_to_hw(cpu, &hw_id) < 0)
            continue;
        expected[cpu] = __atomic_add_fetch(&vf2_tlb_request[cpu], 1,
                                           __ATOMIC_ACQ_REL);
        sbi_send_ipi(1UL, hw_id);
    }

    int irqs_were_off = !arch_irqs_enabled();
    if (irqs_were_off)
        arch_local_irq_enable();
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(pending & (1U << cpu)))
            continue;
        uint64_t wait_start = timer_get_ticks();
        while ((int32_t)(__atomic_load_n(&vf2_tlb_ack[cpu],
                                         __ATOMIC_ACQUIRE) -
                         expected[cpu]) < 0) {
            if (timer_get_ticks() - wait_start > 5UL * VF2_TIMER_FREQ) {
                printf("[VF2 TLB] timeout self=%u target=%u expected=%u "
                       "request=%u ack=%u online=0x%x\n",
                       arch_current_cpu_id(), cpu, expected[cpu],
                       __atomic_load_n(&vf2_tlb_request[cpu],
                                       __ATOMIC_ACQUIRE),
                       __atomic_load_n(&vf2_tlb_ack[cpu],
                                       __ATOMIC_ACQUIRE),
                       smp_online_cpu_mask());
                panic("VisionFive2 remote TLB shootdown timed out");
            }
            arch_cpu_relax();
        }
    }
    if (irqs_were_off)
        arch_local_irq_disable();
    return 0;
}

/* Called from the arch soft-IRQ handler (trap/irqchip.c) under this symbol.
 * Never acknowledge a generation before sfence.vma has retired. */
void rv64_ipi_tlb_flush_handler(void)
{
    unsigned cpu = arch_current_cpu_id();
    if (cpu >= CONFIG_NR_CPUS)
        return;
    for (;;) {
        uint32_t request = __atomic_load_n(&vf2_tlb_request[cpu],
                                           __ATOMIC_ACQUIRE);
        uint32_t ack = __atomic_load_n(&vf2_tlb_ack[cpu],
                                       __ATOMIC_RELAXED);
        if (ack == request)
            break;
        __asm__ __volatile__("sfence.vma" ::: "memory");
        __atomic_store_n(&vf2_tlb_ack[cpu], request, __ATOMIC_RELEASE);
    }
}

static void vf2_smp_secondary_init(const smp_cpu_desc_t *cpu) {
    (void)cpu;
    vf2_plic_init();
}

static const smp_platform_ops_t vf2_smp_ops = {
    .discover        = vf2_smp_discover,
    .start           = vf2_smp_start,
    .send_ipi        = vf2_smp_send_ipi,
    .remote_tlb_flush = vf2_smp_remote_tlb_flush,
    .secondary_init  = vf2_smp_secondary_init,
};

/* ---- board lifecycle ---------------------------------------------- */

static void vf2_early_init(void) {
    riscv64_memory_init();
    vf2_gmac_clock_init();
}

static void vf2_poweroff(void) {
    sbi_shutdown();
}

static void vf2_reboot(void) {
    sbi_reboot();
}

static void vf2_enumerate_devices(void) {
    extern int platform_device_register(platform_device_t *pdev);
    static platform_device_t sdio_dev;
    static resource_t sdio_res[1];

    sdio_res[0].type  = RES_MMIO;
    sdio_res[0].start = VF2_SDIO_BASE;
    sdio_res[0].end   = VF2_SDIO_BASE + VF2_SDIO_SIZE - 1;
    sdio_res[0].flags = IORESOURCE_MMIO_32BIT;

    sdio_dev.dev.name       = "dw-sdio0";
    sdio_dev.dev.res        = sdio_res;
    sdio_dev.dev.res_count  = 1;
    sdio_dev.dev.state      = DEV_STATE_UNINIT;
    sdio_dev.id.vendor      = DW_SDIO_PLATFORM_VENDOR;
    sdio_dev.id.device      = DW_SDIO_PLATFORM_DEVICE;
    platform_device_register(&sdio_dev);

    static platform_device_t gmac_dev;
    static resource_t gmac_res[2];

    gmac_res[0].type  = RES_MMIO;
    gmac_res[0].start = VF2_GMAC_BASE;
    gmac_res[0].end   = VF2_GMAC_BASE + VF2_GMAC_SIZE - 1;
    gmac_res[0].flags = IORESOURCE_MMIO_32BIT;
    gmac_res[1].type  = RES_IRQ;
    gmac_res[1].start = VF2_GMAC_IRQ;
    gmac_res[1].end   = VF2_GMAC_IRQ;
    gmac_res[1].flags = IORESOURCE_IRQ_LEVEL;

    gmac_dev.dev.name       = "starfive-gmac";
    gmac_dev.dev.res        = gmac_res;
    gmac_dev.dev.res_count  = 2;
    gmac_dev.dev.state      = DEV_STATE_UNINIT;
    gmac_dev.id.vendor      = STARFIVE_GMAC_PLATFORM_VENDOR;
    gmac_dev.id.device      = STARFIVE_GMAC_PLATFORM_DEVICE;
    platform_device_register(&gmac_dev);
}

static const board_config_t visionfive2 = {
    .name              = "visionfive2",
    .ram_base          = VF2_MEMORY_BASE,
    .ram_end           = VF2_MEMORY_END,
    .irqchip           = &vf2_plic_ops,
    .timer             = &vf2_timer_ops,
    .smp               = &vf2_smp_ops,
    .early_init        = vf2_early_init,
    .poweroff          = vf2_poweroff,
    .reboot            = vf2_reboot,
    .enumerate_devices = vf2_enumerate_devices,
};

const board_config_t *const current_board = &visionfive2;

#endif /* CONFIG_BOARD_VISIONFIVE2 */
