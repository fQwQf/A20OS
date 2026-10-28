#ifdef CONFIG_LOONGARCH64

#include "core/kexec_backend.h"
#include "core/defs.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "platform.h"

extern char a20_la64_kexec_trampoline[];
extern char a20_la64_kexec_trampoline_end[];
extern void a20_la64_kexec_enter(uintptr_t target, uintptr_t control)
    __attribute__((noreturn));

#define LA64_KEXEC_CONTROL_OFFSET 2048UL

/* QEMU virt LoongArch runs on an identity cached DMW (PAGE_OFFSET == 0), so
 * the trampoline needs no page-table swap at all: physical destinations,
 * source chain nodes and the control page are all reached at their physical
 * address, and the final CRMD write that enters DA mode keeps the PC valid
 * because the mapping is already one-to-one.
 *
 * Secondary parking reuses the firmware contract: QEMU's aux boot stub at
 * the reset vector 0x1c000000 waits on IOCSR mailbox 0 and jumps to whatever
 * entry the mailbox names.  A CPU that jumps back there is indistinguishable
 * from a freshly reset one, so the next kernel boots it through the normal
 * mailbox path, and the cancel path does the same with the kernel's own
 * secondary entry. */
#define LA64_AUX_BOOT_PA 0x1c000000UL

#define IOCSR_MBUF_SEND        0x1048
#define IOCSR_MBUF_DATA_SHIFT  32
#define IOCSR_SEND_BLOCKING    (1UL << 31)
#define IOCSR_SEND_CPU_SHIFT   16
#define LA64_IPI_BOOT_VECTOR   0

static void *g_kexec_page;
static uintptr_t g_kexec_page_pa;
static uintptr_t g_kexec_control_pa;
static volatile uint32_t g_stop_pending;
static volatile uint32_t g_stopped_mask;
static uint32_t g_saved_affinity;
static int g_affinity_changed;

void loongarch64_kexec_ipi(void)
{
#if defined(CONFIG_BOARD_QEMU_VIRT_LOONGARCH64) && !defined(CONFIG_NOMMU)
    if (!__atomic_load_n(&g_stop_pending, __ATOMIC_ACQUIRE))
        return;
    unsigned cpu = arch_current_cpu_id();
    if (cpu < 32)
        __atomic_fetch_or(&g_stopped_mask, 1U << cpu, __ATOMIC_RELEASE);
    arch_local_irq_disable();
    /* Does not return: the aux stub owns this CPU until somebody boots it
     * through mailbox 0 again. */
    ((void (*)(void))(uintptr_t)LA64_AUX_BOOT_PA)();
    __builtin_unreachable();
#endif
}

static int stop_other_cpus(void)
{
    uint32_t online = smp_online_cpu_mask();
    unsigned self = arch_current_cpu_id();
    __atomic_store_n(&g_stopped_mask, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stop_pending, 1, __ATOMIC_RELEASE);
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (cpu == self || !(online & (1U << cpu)))
            continue;
        loongarch64_smp_send_ipi(cpu, 1); /* IPI_RESCHEDULE_VECTOR */
    }
    uint32_t wanted = online & ~(1U << self);
    uint64_t start = timer_get_ticks();
    while ((__atomic_load_n(&g_stopped_mask, __ATOMIC_ACQUIRE) & wanted) != wanted) {
        if (timer_get_ticks() - start > 5ULL * TICKS_PER_SEC)
            return -ETIMEDOUT;
        arch_cpu_relax();
    }
    return 0;
}

static void boot_cpu_through_mailbox(uint64_t hw_id, uintptr_t entry_pa)
{
    uint64_t common = IOCSR_SEND_BLOCKING |
                      (hw_id << IOCSR_SEND_CPU_SHIFT);
    /* QEMU consumes mailbox 0 as two masked 32-bit writes. */
    loongarch64_iocsr_write64(
        common | (1UL << 2) | (entry_pa & 0xffffffff00000000UL),
        IOCSR_MBUF_SEND);
    loongarch64_iocsr_write64(
        common | ((uint64_t)(uint32_t)entry_pa << IOCSR_MBUF_DATA_SHIFT),
        IOCSR_MBUF_SEND);
    loongarch64_smp_send_ipi((unsigned)hw_id, LA64_IPI_BOOT_VECTOR);
}

void machine_kexec_cancel(void)
{
    __atomic_store_n(&g_stop_pending, 0, __ATOMIC_RELEASE);
    uint32_t stopped = __atomic_load_n(&g_stopped_mask, __ATOMIC_ACQUIRE);
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(stopped & (1U << cpu)))
            continue;
        const smp_cpu_desc_t *desc = smp_cpu_desc(cpu);
        if (desc)
            boot_cpu_through_mailbox(desc->hw_id,
                                     arch_smp_secondary_entry_pa());
    }
    __atomic_store_n(&g_stopped_mask, 0, __ATOMIC_RELEASE);
    if (g_kexec_page) {
        frame_free(g_kexec_page);
        g_kexec_page = NULL;
        g_kexec_page_pa = 0;
        g_kexec_control_pa = 0;
    }
    kexec_restore_affinity(g_saved_affinity, g_affinity_changed);
    g_affinity_changed = 0;
}

uint32_t machine_kexec_backend_id(void)
{
#if defined(CONFIG_BOARD_QEMU_VIRT_LOONGARCH64) && !defined(CONFIG_NOMMU)
    return 4;
#else
    return 0;
#endif
}

int machine_kexec_prepare(const a20_elf_kexec_ledger_t *image)
{
#if !defined(CONFIG_BOARD_QEMU_VIRT_LOONGARCH64) || defined(CONFIG_NOMMU)
    (void)image;
    return -EOPNOTSUPP;
#else
    int rc = kexec_bind_boot_cpu(&g_saved_affinity, &g_affinity_changed);
    if (rc < 0)
        return rc;
    size_t size = a20_la64_kexec_trampoline_end - a20_la64_kexec_trampoline;
    if (size > LA64_KEXEC_CONTROL_OFFSET ||
        sizeof(kexec_handoff_t) > PAGE_SIZE - LA64_KEXEC_CONTROL_OFFSET) {
        machine_kexec_cancel();
        return -E2BIG;
    }
    void *page = frame_alloc();
    if (!page) {
        machine_kexec_cancel();
        return -ENOMEM;
    }
    g_kexec_page = page;
    g_kexec_page_pa = (uintptr_t)va_to_pa(page);
    g_kexec_control_pa = g_kexec_page_pa + LA64_KEXEC_CONTROL_OFFSET;
    kexec_handoff_t *control =
        (void *)((char *)page + LA64_KEXEC_CONTROL_OFFSET);
    rc = kexec_handoff_build(image, control, g_kexec_page_pa, 0, 0);
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    kinfo("[KEXEC] control page pa=0x%lx entry=0x%lx\n",
          (unsigned long)g_kexec_page_pa, (unsigned long)control->entry);
    memcpy(page, a20_la64_kexec_trampoline, size);
    arch_flush_icache_range(page, PAGE_SIZE);
    kinfo("[KEXEC] stopping CPUs from cpu=%u online=0x%x\n",
          arch_current_cpu_id(), smp_online_cpu_mask());
    rc = stop_other_cpus();
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    kinfo("[KEXEC] remote CPUs parked at firmware reset stub\n");
    return 0;
#endif
}

void machine_kexec(const a20_elf_kexec_ledger_t *image)
{
    (void)image;
    arch_local_irq_disable();
    a20_la64_kexec_enter(g_kexec_page_pa, g_kexec_control_pa);
    __builtin_unreachable();
}

#endif
