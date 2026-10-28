#if defined(CONFIG_RISCV64) || defined(CONFIG_RISCV32)

#include "core/kexec_backend.h"
#include "core/defs.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "kexec.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "platform.h"

#ifdef CONFIG_RISCV32
extern uint32_t __boot_hart_id;
extern uint32_t __boot_dtb_ptr;
#else
extern uint64_t __boot_hart_id;
extern uint64_t __boot_dtb_ptr;
#endif
extern char a20_rv64_kexec_trampoline[];
extern char a20_rv64_kexec_trampoline_end[];
extern void a20_rv64_kexec_enter(uintptr_t target, uintptr_t argument,
                                 uint64_t boot_satp)
    __attribute__((noreturn));

#define RISCV_KEXEC_CONTROL_OFFSET 1024UL

static void *g_kexec_page;
static uintptr_t g_kexec_page_pa;
static uintptr_t g_kexec_control_pa;
static volatile int g_kexec_stop_pending;
static volatile int g_kexec_stop_error;
static uint32_t g_kexec_saved_affinity;
static int g_kexec_affinity_changed;

void riscv64_kexec_ipi(void)
{
#if (defined(CONFIG_BOARD_QEMU_VIRT_RISCV64) || defined(CONFIG_BOARD_QEMU_VIRT_RISCV32)) && !defined(CONFIG_NOMMU)
    if (!__atomic_load_n(&g_kexec_stop_pending, __ATOMIC_ACQUIRE))
        return;
    arch_local_irq_disable();
    int64_t rc = sbi_hart_stop();
    __atomic_store_n(&g_kexec_stop_error, (int)rc, __ATOMIC_RELEASE);
    while (__atomic_load_n(&g_kexec_stop_pending, __ATOMIC_ACQUIRE))
        arch_halt();
    arch_local_irq_enable();
#endif
}

static int stop_other_harts(void)
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
            int64_t status = sbi_hart_get_status(desc->hw_id);
            if (status < 0) {
                kinfo("[KEXEC] hart_get_status(%lu) -> %d\n",
                      (unsigned long)desc->hw_id, (int)status);
                return (int)status;
            }
            if (status == 1)
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
        if (desc && sbi_hart_get_status(desc->hw_id) == 1)
            (void)sbi_hart_start(desc->hw_id, arch_smp_secondary_entry_pa(), cpu);
    }
    if (g_kexec_page) {
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
#if (defined(CONFIG_BOARD_QEMU_VIRT_RISCV64) || defined(CONFIG_BOARD_QEMU_VIRT_RISCV32)) && !defined(CONFIG_NOMMU)
    return 1;
#else
    return 0;
#endif
}

int machine_kexec_prepare(const a20_elf_kexec_ledger_t *image)
{
#if (!defined(CONFIG_BOARD_QEMU_VIRT_RISCV64) && !defined(CONFIG_BOARD_QEMU_VIRT_RISCV32)) || defined(CONFIG_NOMMU)
    (void)image;
    return -EOPNOTSUPP;
#else
    int rc = kexec_bind_boot_cpu(&g_kexec_saved_affinity,
                                 &g_kexec_affinity_changed);
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    size_t trampoline_size = a20_rv64_kexec_trampoline_end -
                             a20_rv64_kexec_trampoline;
    if (trampoline_size > RISCV_KEXEC_CONTROL_OFFSET ||
        sizeof(kexec_handoff_t) > PAGE_SIZE - RISCV_KEXEC_CONTROL_OFFSET) {
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
    g_kexec_control_pa = g_kexec_page_pa + RISCV_KEXEC_CONTROL_OFFSET;
    kexec_handoff_t *control = (void *)((char *)page + RISCV_KEXEC_CONTROL_OFFSET);
    rc = kexec_handoff_build(image, control, g_kexec_page_pa,
                             __boot_hart_id, __boot_dtb_ptr);
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    kinfo("[KEXEC] control page pa=0x%lx entry=0x%lx\n",
          (unsigned long)g_kexec_page_pa, (unsigned long)control->entry);
    memcpy(page, a20_rv64_kexec_trampoline, trampoline_size);
    arch_fence_i();
    kinfo("[KEXEC] stopping harts from cpu=%u online=0x%x\n",
          arch_current_cpu_id(), smp_online_cpu_mask());
    rc = stop_other_harts();
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    kinfo("[KEXEC] HSM reports remote harts stopped\n");
    return 0;
#endif
}

void machine_kexec(const a20_elf_kexec_ledger_t *image)
{
    (void)image;
    arch_local_irq_disable();
    arch_write_sie(0);
    arch_fence_i();
#ifdef CONFIG_RISCV64
    boot_pgdir[0] = 0xcf;
    uint64_t pte = (PHYS_MEMORY_BASE >> 2) | 0xcf;
    unsigned slot = PHYS_MEMORY_BASE >> 30;
    for (unsigned i = 0; i < 16; i++) {
        boot_pgdir[slot + i] = pte;
        pte += 1UL << 28;
    }
    uint64_t boot_satp = (8ULL << 60) |
                         ((uint64_t)va_to_pa(boot_pgdir) >> PAGE_SIZE_BITS);
#else
    uint64_t boot_satp = (1UL << 31) |
                         ((uint64_t)va_to_pa(boot_pgdir) >> PAGE_SIZE_BITS);
#endif
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    a20_rv64_kexec_enter(g_kexec_page_pa, g_kexec_control_pa, boot_satp);
    __builtin_unreachable();
}

#endif
