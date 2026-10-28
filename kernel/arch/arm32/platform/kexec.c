#ifdef CONFIG_ARM32

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

extern uint32_t arm32_boot_dtb;
extern char a20_arm32_kexec_trampoline[];
extern char a20_arm32_kexec_trampoline_end[];
extern void a20_arm32_kexec_enter(uintptr_t target, uintptr_t control,
                                  uint32_t ttbr0) __attribute__((noreturn));

#define ARM32_KEXEC_CONTROL_OFFSET 2048UL

/* Same attributes as the boot identity window in entry.S. */
#define ARM32_KEXEC_SECTION 0x60EUL

static void *g_kexec_page;
static uintptr_t g_kexec_page_pa;
static uintptr_t g_kexec_control_pa;
static uint32_t *g_kexec_l1;       /* 16 KiB short-descriptor table */
static uintptr_t g_kexec_l1_pa;
static volatile int g_kexec_stop_pending;
static volatile int g_kexec_stop_error;
static uint32_t g_kexec_saved_affinity;
static int g_kexec_affinity_changed;

void arm32_kexec_ipi(void)
{
#if defined(CONFIG_BOARD_QEMU_VIRT_ARM32) && !defined(CONFIG_NOMMU)
    if (!__atomic_load_n(&g_kexec_stop_pending, __ATOMIC_ACQUIRE))
        return;
    arch_local_irq_disable();
    int32_t rc = firmware_cpu_off();
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
            int32_t status = firmware_affinity_info(desc->hw_id, 0);
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
    if (g_kexec_l1) {
        pfa_free(virt_to_pfn(g_kexec_l1), 2);
        g_kexec_l1 = NULL;
        g_kexec_l1_pa = 0;
    }    if (g_kexec_page) {
        frame_free(g_kexec_page);
        g_kexec_page = NULL;
        g_kexec_page_pa = 0;
        g_kexec_control_pa = 0;
    }
    kexec_restore_affinity(g_kexec_saved_affinity, g_kexec_affinity_changed);
    g_kexec_affinity_changed = 0;
}

uint32_t machine_kexec_backend_id(void)
{
#if defined(CONFIG_BOARD_QEMU_VIRT_ARM32) && !defined(CONFIG_NOMMU)
    return 5;
#else
    return 0;
#endif
}

int machine_kexec_prepare(const a20_elf_kexec_ledger_t *image)
{
#if !defined(CONFIG_BOARD_QEMU_VIRT_ARM32) || defined(CONFIG_NOMMU)
    (void)image;
    return -EOPNOTSUPP;
#else
    int rc = kexec_bind_boot_cpu(&g_kexec_saved_affinity,
                                 &g_kexec_affinity_changed);
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    size_t trampoline_size = a20_arm32_kexec_trampoline_end -
                             a20_arm32_kexec_trampoline;
    if (trampoline_size > ARM32_KEXEC_CONTROL_OFFSET ||
        sizeof(kexec_handoff_t) > PAGE_SIZE - ARM32_KEXEC_CONTROL_OFFSET) {
        machine_kexec_cancel();
        return -E2BIG;
    }
    g_kexec_page = frame_alloc();
    /* The short-descriptor L1 is 16 KiB and must be 16 KiB aligned. */
    pfn_t l1_pfn = pfa_alloc(2);
    if (!g_kexec_page || !l1_pfn || (l1_pfn & 3)) {
        machine_kexec_cancel();
        return -ENOMEM;
    }
    uintptr_t l1_pa = (uintptr_t)pfn_to_phys(l1_pfn);
    g_kexec_l1 = (uint32_t *)pfn_to_virt(l1_pfn);
    if (!g_kexec_l1 || l1_pa == (uintptr_t)-1) {
        machine_kexec_cancel();
        return -ENOMEM;
    }
    g_kexec_l1_pa = l1_pa;
    memset(g_kexec_l1, 0, 4 * PAGE_SIZE);
    /* Identity-map the RAM window and keep the high-half kernel alias; the
     * enter stub fetches a few instructions at its linked VA after TTBR0
     * switches, and the trampoline then lives on the identity side. */
    for (uint64_t pa = PHYS_MEMORY_BASE; pa < PHYS_MEMORY_END; pa += 0x100000) {
        g_kexec_l1[pa >> 20] = (uint32_t)pa | ARM32_KEXEC_SECTION;
        uint64_t va = pa + PAGE_OFFSET;
        g_kexec_l1[va >> 20] = (uint32_t)pa | ARM32_KEXEC_SECTION;
    }
    g_kexec_page_pa = (uintptr_t)va_to_pa(g_kexec_page);
    g_kexec_control_pa = g_kexec_page_pa + ARM32_KEXEC_CONTROL_OFFSET;
    kexec_handoff_t *control =
        (void *)((char *)g_kexec_page + ARM32_KEXEC_CONTROL_OFFSET);
    uint32_t dtb = *(const uint32_t *)((uintptr_t)&arm32_boot_dtb + PAGE_OFFSET);
    rc = kexec_handoff_build(image, control, g_kexec_page_pa, 0, dtb);
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    kinfo("[KEXEC] control page pa=0x%lx entry=0x%lx\n",
          (unsigned long)g_kexec_page_pa, (unsigned long)control->entry);
    memcpy(g_kexec_page, a20_arm32_kexec_trampoline, trampoline_size);
    arch_flush_icache_range(g_kexec_page, PAGE_SIZE);
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
    a20_arm32_kexec_enter(g_kexec_page_pa, g_kexec_control_pa,
                          (uint32_t)g_kexec_l1_pa);
    __builtin_unreachable();
}

#endif
