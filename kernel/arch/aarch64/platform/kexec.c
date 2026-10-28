#ifdef CONFIG_AARCH64

#include "core/kexec_backend.h"
#include "core/defs.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "firmware.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "platform.h"

extern uint64_t aarch64_boot_dtb;
extern char a20_aarch64_kexec_trampoline[];
extern char a20_aarch64_kexec_trampoline_end[];
extern void a20_aarch64_kexec_enter(uintptr_t target, uintptr_t control,
                                    uint64_t boot_ttbr0)
    __attribute__((noreturn));

#define AARCH64_KEXEC_CONTROL_OFFSET 2048UL

/* Dedicated two-level table set: TTBR0 L0[0] covers the physical window,
 * L0[1] covers the PAGE_OFFSET alias, and both point at one shared L1 whose
 * 1-GiB blocks mirror the boot map.  The boot tables cannot be reused for
 * the identity alias: arch_kernel_wx_finalize() splits boot_l1[1] into
 * PXN-marked 2-MiB blocks, so the trampoline could not fetch through it. */
#define AARCH64_KEXEC_L1_DEVICE 0x0060000000000401UL /* PA 0x0, device */
#define AARCH64_KEXEC_L1_RAM    0x0040000040000705UL /* PA 0x40000000, normal */

static void *g_kexec_page;
static uintptr_t g_kexec_page_pa;
static uintptr_t g_kexec_control_pa;
static void *g_kexec_l0;
static void *g_kexec_l1;
static volatile int g_kexec_stop_pending;
static volatile int g_kexec_stop_error;
static uint32_t g_kexec_saved_affinity;
static int g_kexec_affinity_changed;

void aarch64_kexec_ipi(void)
{
#if defined(CONFIG_BOARD_QEMU_VIRT_AARCH64) && !defined(CONFIG_NOMMU)
    if (!__atomic_load_n(&g_kexec_stop_pending, __ATOMIC_ACQUIRE))
        return;
    arch_local_irq_disable();
    int64_t rc = firmware_cpu_off();
    __atomic_store_n(&g_kexec_stop_error, (int)rc, __ATOMIC_RELEASE);
    while (__atomic_load_n(&g_kexec_stop_pending, __ATOMIC_ACQUIRE))
        arch_halt();
    arch_local_irq_enable();
#endif
}

static int stop_other_cpus(void)
{
    uint32_t online = smp_online_cpu_mask();
    unsigned self = arch_current_cpu_id();
    __atomic_store_n(&g_kexec_stop_error, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_kexec_stop_pending, 1, __ATOMIC_RELEASE);
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (cpu == self || !(online & (1U << cpu)))
            continue;
        smp_send_reschedule(cpu);
    }
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (cpu == self || !(online & (1U << cpu)))
            continue;
        const smp_cpu_desc_t *desc = smp_cpu_desc(cpu);
        if (!desc)
            return -EIO;
        uint64_t start = timer_get_ticks();
        for (;;) {
            int error = __atomic_load_n(&g_kexec_stop_error, __ATOMIC_ACQUIRE);
            if (error)
                return error;
            int64_t status = firmware_affinity_info(desc->hw_id, 0);
            if (status < 0)
                return (int)status;
            if (status == 1) /* PSCI AFFINITY_INFO: OFF */
                break;
            if (timer_get_ticks() - start > 5ULL * TICKS_PER_SEC)
                return -ETIMEDOUT;
            arch_cpu_relax();
        }
    }
    return 0;
}

static void free_kexec_pages(void)
{
    if (g_kexec_l1) {
        frame_free(g_kexec_l1);
        g_kexec_l1 = NULL;
    }
    if (g_kexec_l0) {
        frame_free(g_kexec_l0);
        g_kexec_l0 = NULL;
    }
    if (g_kexec_page) {
        frame_free(g_kexec_page);
        g_kexec_page = NULL;
        g_kexec_page_pa = 0;
        g_kexec_control_pa = 0;
    }
}

void machine_kexec_cancel(void)
{
    __atomic_store_n(&g_kexec_stop_pending, 0, __ATOMIC_RELEASE);
    uint32_t online = smp_online_cpu_mask();
    unsigned self = arch_current_cpu_id();
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (cpu == self || !(online & (1U << cpu)))
            continue;
        const smp_cpu_desc_t *desc = smp_cpu_desc(cpu);
        if (desc && firmware_affinity_info(desc->hw_id, 0) == 1)
            (void)firmware_cpu_on(desc->hw_id, arch_smp_secondary_entry_pa(),
                                  cpu);
    }
    free_kexec_pages();
    kexec_restore_affinity(g_kexec_saved_affinity, g_kexec_affinity_changed);
    g_kexec_affinity_changed = 0;
}

uint32_t machine_kexec_backend_id(void)
{
#if defined(CONFIG_BOARD_QEMU_VIRT_AARCH64) && !defined(CONFIG_NOMMU)
    return 3;
#else
    return 0;
#endif
}

static void clean_pages_to_poc(void *first, unsigned count)
{
    size_t line = arch_dcache_line_size();
    for (unsigned p = 0; p < count; p++) {
        const uint8_t *base = (const uint8_t *)first + p * PAGE_SIZE;
        for (size_t i = 0; i < PAGE_SIZE; i += line)
            __asm__ __volatile__("dc cvac, %0" ::"r"(base + i) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
}

int machine_kexec_prepare(const a20_elf_kexec_ledger_t *image)
{
#if !defined(CONFIG_BOARD_QEMU_VIRT_AARCH64) || defined(CONFIG_NOMMU)
    (void)image;
    return -EOPNOTSUPP;
#else
    int rc = kexec_bind_boot_cpu(&g_kexec_saved_affinity,
                                 &g_kexec_affinity_changed);
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    size_t trampoline_size = a20_aarch64_kexec_trampoline_end -
                             a20_aarch64_kexec_trampoline;
    if (trampoline_size > AARCH64_KEXEC_CONTROL_OFFSET ||
        sizeof(kexec_handoff_t) > PAGE_SIZE - AARCH64_KEXEC_CONTROL_OFFSET) {
        machine_kexec_cancel();
        return -E2BIG;
    }
    g_kexec_page = frame_alloc();
    g_kexec_l0 = frame_alloc();
    g_kexec_l1 = frame_alloc();
    if (!g_kexec_page || !g_kexec_l0 || !g_kexec_l1) {
        machine_kexec_cancel();
        return -ENOMEM;
    }
    g_kexec_page_pa = (uintptr_t)va_to_pa(g_kexec_page);
    g_kexec_control_pa = g_kexec_page_pa + AARCH64_KEXEC_CONTROL_OFFSET;
    uint64_t l1_pa = (uint64_t)va_to_pa(g_kexec_l1);
    memset(g_kexec_l0, 0, PAGE_SIZE);
    memset(g_kexec_l1, 0, PAGE_SIZE);
    ((uint64_t *)g_kexec_l0)[0] = l1_pa | 0x3;
    ((uint64_t *)g_kexec_l0)[1] = l1_pa | 0x3;
    ((uint64_t *)g_kexec_l1)[0] = AARCH64_KEXEC_L1_DEVICE;
    ((uint64_t *)g_kexec_l1)[1] = AARCH64_KEXEC_L1_RAM;
    kexec_handoff_t *control =
        (void *)((char *)g_kexec_page + AARCH64_KEXEC_CONTROL_OFFSET);
    rc = kexec_handoff_build(image, control, g_kexec_page_pa,
                             aarch64_boot_dtb, 0);
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    kinfo("[KEXEC] control page pa=0x%lx entry=0x%lx\n",
          (unsigned long)g_kexec_page_pa, (unsigned long)control->entry);
    memcpy(g_kexec_page, a20_aarch64_kexec_trampoline, trampoline_size);
    arch_flush_icache_range(g_kexec_page, PAGE_SIZE);
    clean_pages_to_poc(g_kexec_l0, 1);
    clean_pages_to_poc(g_kexec_l1, 1);
    kinfo("[KEXEC] stopping CPUs from cpu=%u online=0x%x\n",
          arch_current_cpu_id(), smp_online_cpu_mask());
    rc = stop_other_cpus();
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    kinfo("[KEXEC] PSCI reports remote CPUs off\n");
    return 0;
#endif
}

void machine_kexec(const a20_elf_kexec_ledger_t *image)
{
    (void)image;
    arch_local_irq_disable();
    a20_aarch64_kexec_enter(g_kexec_page_pa, g_kexec_control_pa,
                            (uint64_t)va_to_pa(g_kexec_l0));
    __builtin_unreachable();
}

#endif
