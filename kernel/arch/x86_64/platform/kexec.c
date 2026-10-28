#ifdef CONFIG_X86_64

#include "core/kexec_backend.h"
#include "core/defs.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "boot_memory.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "platform.h"

extern char x86_64_kexec_trampoline[];
extern char x86_64_kexec_trampoline_end[];
extern void x86_64_kexec_enter(uintptr_t target, uintptr_t control,
                               uintptr_t boot_cr3) __attribute__((noreturn));
extern uint64_t x86_boot_acpi_rsdp;
extern uint64_t x86_boot_framebuffer_pa;
extern volatile uint32_t g_mb_magic;
extern volatile uint32_t g_mb_info;

#define X86_KEXEC_CONTROL_OFFSET 2048UL
#define X86_KEXEC_IDENTITY_GIB 4UL
#define X86_KEXEC_IDENTITY_LIMIT (X86_KEXEC_IDENTITY_GIB << 30)
/* PML4 + PDPT + four PDs (2 MiB pages over 4 GiB) in pool frames.  The boot
 * tables in the old kernel's .bss cannot serve as the identity map: the
 * trampoline zero-fills the next kernel's .bss over exactly those pages while
 * the walker is still using them. */
#define X86_KEXEC_TABLE_PAGES (1 + 1 + X86_KEXEC_IDENTITY_GIB)
/* The Multiboot info and memory map live right after the loaded image, inside
 * what becomes the next kernel's .bss: the handoff zero-fill destroys them.
 * They are snapshotted into one pool page (header first, map at +64) and the
 * copy's address replaces boot_arg0. */
#define X86_KEXEC_MBINFO_MAP_OFFSET 64UL

static void *g_kexec_mbinfo;
static uint32_t g_kexec_mbinfo_pa;

static void *g_kexec_page;
static uintptr_t g_kexec_page_pa;
static uintptr_t g_kexec_control_pa;
static void *g_kexec_tables[X86_KEXEC_TABLE_PAGES];
static uintptr_t g_kexec_pml4_pa;
static volatile uint32_t g_stop_pending;
static volatile uint32_t g_stopped_mask;
static uint32_t g_saved_affinity;
static int g_affinity_changed;

void x86_64_kexec_ipi(void)
{
    if (!__atomic_load_n(&g_stop_pending, __ATOMIC_ACQUIRE))
        return;
    unsigned cpu = arch_current_cpu_id();
    if (cpu < 32)
        __atomic_fetch_or(&g_stopped_mask, 1U << cpu, __ATOMIC_RELEASE);
    for (;;)
        __asm__ __volatile__("cli; hlt" ::: "memory");
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
        smp_send_reschedule(cpu);
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

static void resume_other_cpus(void)
{
    uint32_t stopped = __atomic_load_n(&g_stopped_mask, __ATOMIC_ACQUIRE);
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (!(stopped & (1U << cpu)))
            continue;
        const smp_cpu_desc_t *desc = smp_cpu_desc(cpu);
        if (desc)
            (void)x86_64_smp_start_ap(desc->hw_id,
                                      arch_smp_secondary_entry_pa(), cpu);
    }
}

static int destination_identity_ok(const kexec_handoff_t *control)
{
    for (uint32_t i = 0; i < control->nr; i++) {
        uint64_t end;
        if (__builtin_add_overflow(control->seg[i].mem, control->seg[i].memsz,
                                   &end) || end > X86_KEXEC_IDENTITY_LIMIT)
            return 0;
        /* Every source chain node must also sit inside the identity window:
         * the trampoline walks the chain by physical address. */
        uint64_t pa = control->seg[i].pages;
        uint64_t remaining = control->seg[i].memsz;
        while (pa && remaining) {
            if (pa >= X86_KEXEC_IDENTITY_LIMIT)
                return 0;
            const void *node = (const void *)(uintptr_t)(pa + PAGE_OFFSET);
            pa = *(const uint64_t *)node
                 ? *(const uint64_t *)node - PAGE_OFFSET : 0;
            remaining = remaining > PAGE_SIZE - sizeof(void *)
                        ? remaining - (PAGE_SIZE - sizeof(void *)) : 0;
        }
        if (remaining)
            return 0;
    }
    return g_kexec_control_pa < X86_KEXEC_IDENTITY_LIMIT;
}

static int snapshot_boot_memory(void)
{
    g_kexec_mbinfo = NULL;
    g_kexec_mbinfo_pa = g_mb_info;
    if (g_mb_magic != X86_MB_BOOTLOADER_MAGIC || !g_mb_info)
        return 0;
    const struct x86_mb_info *src =
        (const void *)(uintptr_t)(PAGE_OFFSET + g_mb_info);
    uint32_t map_len = (src->flags & X86_MB_INFO_MEM_MAP) ? src->mmap_length : 0;
    if (map_len > PAGE_SIZE - X86_KEXEC_MBINFO_MAP_OFFSET)
        return -E2BIG;
    void *copy = frame_alloc();
    if (!copy)
        return -ENOMEM;
    uint64_t copy_pa = (uint64_t)va_to_pa(copy);
    if (copy_pa >= X86_KEXEC_IDENTITY_LIMIT) {
        frame_free(copy);
        return -EADDRNOTAVAIL;
    }
    memset(copy, 0, PAGE_SIZE);
    memcpy(copy, src, sizeof(*src));
    kinfo("[KEXEC] mb snapshot: magic=0x%x info_pa=0x%x flags=0x%x map_len=%u -> pa=0x%lx\n",
          g_mb_magic, g_mb_info, src->flags, map_len, (unsigned long)copy_pa);
    if (map_len) {
        memcpy((char *)copy + X86_KEXEC_MBINFO_MAP_OFFSET,
               (const void *)(uintptr_t)(PAGE_OFFSET + (uint64_t)src->mmap_addr),
               map_len);
        ((struct x86_mb_info *)copy)->mmap_addr =
            (uint32_t)(copy_pa + X86_KEXEC_MBINFO_MAP_OFFSET);
    }
    g_kexec_mbinfo = copy;
    g_kexec_mbinfo_pa = (uint32_t)copy_pa;
    return 0;
}

void machine_kexec_cancel(void)
{
    __atomic_store_n(&g_stop_pending, 0, __ATOMIC_RELEASE);
    resume_other_cpus();
    if (g_kexec_mbinfo) {
        frame_free(g_kexec_mbinfo);
        g_kexec_mbinfo = NULL;
    }
    g_kexec_mbinfo_pa = 0;
    for (unsigned i = 0; i < X86_KEXEC_TABLE_PAGES; i++) {
        if (g_kexec_tables[i]) {
            frame_free(g_kexec_tables[i]);
            g_kexec_tables[i] = NULL;
        }
    }
    g_kexec_pml4_pa = 0;
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
#if defined(CONFIG_BOARD_QEMU_VIRT_X86_64) && !defined(CONFIG_NOMMU)
    return 2;
#else
    return 0;
#endif
}

int machine_kexec_prepare(const a20_elf_kexec_ledger_t *image)
{
#if !defined(CONFIG_BOARD_QEMU_VIRT_X86_64) || defined(CONFIG_NOMMU)
    (void)image;
    return -EOPNOTSUPP;
#else
    int rc = kexec_bind_boot_cpu(&g_saved_affinity, &g_affinity_changed);
    if (rc < 0)
        return rc;
    size_t size = x86_64_kexec_trampoline_end - x86_64_kexec_trampoline;
    if (size > X86_KEXEC_CONTROL_OFFSET ||
        sizeof(kexec_handoff_t) > PAGE_SIZE - X86_KEXEC_CONTROL_OFFSET) {
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
    g_kexec_control_pa = g_kexec_page_pa + X86_KEXEC_CONTROL_OFFSET;
    kexec_handoff_t *control = (void *)((char *)page + X86_KEXEC_CONTROL_OFFSET);
    rc = snapshot_boot_memory();
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    rc = kexec_handoff_build(image, control, g_kexec_page_pa,
                             g_kexec_mbinfo_pa, x86_boot_acpi_rsdp);
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    control->boot_arg2 = x86_boot_framebuffer_pa;
    if (!destination_identity_ok(control)) {
        machine_kexec_cancel();
        return -EADDRNOTAVAIL;
    }
    for (unsigned i = 0; i < X86_KEXEC_TABLE_PAGES; i++) {
        g_kexec_tables[i] = frame_alloc();
        if (!g_kexec_tables[i]) {
            machine_kexec_cancel();
            return -ENOMEM;
        }
        memset(g_kexec_tables[i], 0, PAGE_SIZE);
    }
    uint64_t *pml4 = g_kexec_tables[0];
    uint64_t *pdpt = g_kexec_tables[1];
    g_kexec_pml4_pa = (uintptr_t)va_to_pa(pml4);
    /* The enter stub still fetches a handful of high-half instructions after
     * CR3 loads, so the kernel half mirrors the live table; the identity half
     * points at the dedicated PD set. */
    memcpy(pml4, boot_pgdir, PAGE_SIZE);
    pml4[0] = ((uint64_t)va_to_pa(pdpt)) | 0x003;
    for (uint64_t i = 0; i < X86_KEXEC_IDENTITY_GIB; i++)
        pdpt[i] = ((uint64_t)va_to_pa(g_kexec_tables[2 + i])) | 0x003;
    for (uint64_t g = 0; g < X86_KEXEC_IDENTITY_GIB; g++) {
        uint64_t *pd = g_kexec_tables[2 + g];
        for (uint64_t i = 0; i < 512; i++)
            pd[i] = ((g << 30) + (i << 21)) | 0x83;
    }
    memcpy(page, x86_64_kexec_trampoline, size);
    arch_flush_icache_range(page, PAGE_SIZE);
    kinfo("[KEXEC] control page pa=0x%lx entry=0x%lx\n",
          (unsigned long)g_kexec_page_pa, (unsigned long)control->entry);
    kinfo("[KEXEC] stopping CPUs from cpu=%u online=0x%x\n",
          arch_current_cpu_id(), smp_online_cpu_mask());
    rc = stop_other_cpus();
    if (rc < 0) {
        machine_kexec_cancel();
        return rc;
    }
    return 0;
#endif
}

void machine_kexec(const a20_elf_kexec_ledger_t *image)
{
    (void)image;
    arch_local_irq_disable();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    x86_64_kexec_enter(g_kexec_page_pa, g_kexec_control_pa, g_kexec_pml4_pa);
    __builtin_unreachable();
}

#endif
