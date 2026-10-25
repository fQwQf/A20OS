
#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/bootargs.h"
#include "core/string.h"
#include "boot/context.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "firmware.h"
#include "gicv2_device.h"

#if CONFIG_NR_CPUS > 8
#error "QEMU virt AArch64 SMP supports at most 8 CPUs with GICv2 SGI targets"
#endif

#define GICD_SGIR 0xF00U

static inline volatile uint32_t *aa64_gicd_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}

static inline volatile uint32_t *aa64_gicc_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICC_BASE + off);
}

static uint64_t aa64_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t aa64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t aa64_generic_timer_ops = {
    .read_ticks = aa64_timer_read_ticks,
    .ticks_per_sec = aa64_timer_ticks_per_sec,
};

static unsigned aa64_smp_discover(smp_cpu_desc_t *cpus, unsigned capacity,
                                  uint64_t boot_mpidr) {
    if (boot_mpidr != 0)
        return 0;
    for (unsigned cpu = 0; cpu < capacity; cpu++) {
        cpus[cpu].hw_id = cpu;
        cpus[cpu].platform_cookie = 0;
    }
    return capacity;
}

static int aa64_smp_start(const smp_cpu_desc_t *cpu, uintptr_t entry_pa,
                          uintptr_t logical_context) {
    return (int)firmware_cpu_on(cpu->hw_id, entry_pa, logical_context);
}

static void aa64_smp_send_ipi(const smp_cpu_desc_t *cpu,
                              smp_ipi_reason_t reason) {
    (void)reason;
    arch_wmb();
    *aa64_gicd_reg32(GICD_SGIR) = (1U << (16 + cpu->hw_id)) | IRQ_S_SOFT;
}

static void aa64_smp_secondary_init(const smp_cpu_desc_t *cpu) {
    (void)cpu;
    *aa64_gicc_reg32(0x0000) = 0;
    *aa64_gicc_reg32(0x0004) = 0xFF;
    *aa64_gicc_reg32(0x0000) = 1;
}

static const smp_platform_ops_t aa64_smp_ops = {
    .discover = aa64_smp_discover,
    .start = aa64_smp_start,
    .send_ipi = aa64_smp_send_ipi,
    .secondary_init = aa64_smp_secondary_init,
};

static void aa64_early_init(void) {
}

/* QEMU -kernel hands the DTB in x0 (Linux boot protocol); entry.S stashes it
 * in aarch64_boot_dtb.  Prefer /chosen/bootargs from the DTB; fall back to
 * the deterministic slirp defaults when the FDT carries no command line. */
extern uint64_t aarch64_boot_dtb;

const char *arch_bootargs_get(void) {
    static char buf[1024];
    static int ready;

    if (!ready) {
        buf[0] = '\0';
        const boot_context_t *b = boot_context_get();
        if (b && b->command_line && strlen(b->command_line) < sizeof(buf))
            memcpy(buf, b->command_line, strlen(b->command_line) + 1);
        ready = 1;
    }
    if (buf[0])
        return buf;
    return "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 "
           "a20.gateway=10.0.2.2 a20.dns=10.0.2.3 "
           "a20.hostname=a20os-qemu";
}

static void aa64_poweroff(void) {
    sbi_shutdown();
}

static void aa64_reboot(void) {
    sbi_reboot();
}

extern void virtio_mmio_enumerate(uintptr_t base, int max_slots, int irq_base);

static void aa64_enumerate_devices(void) {
    /* The DT encodes SPI numbers relative to INTID 32. request_irq() and
     * GICD registers use the full INTID, so SPI 16 is interrupt 48. */
    virtio_mmio_enumerate((VIRTIO_BASE - PAGE_OFFSET), 8, 32 + 16);
}

void board_configure_devices(void);

static const board_config_t qemu_virt_aa64 = {
    .configure_devices = board_configure_devices,
    .name = "qemu-virt-aa64",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &aarch64_gicv2_ops,
    .timer = &aa64_generic_timer_ops,
    .smp = &aa64_smp_ops,
    .early_init = aa64_early_init,
    .poweroff = aa64_poweroff,
    .reboot = aa64_reboot,
    .enumerate_devices = aa64_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_aa64;
