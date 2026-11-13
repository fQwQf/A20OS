/*
 * x86_64 device MMIO mapping.
 *
 * Two windows exist:
 *
 *  1. The boot direct map covers physical 0..4 GiB at PAGE_OFFSET.  Entry.S
 *     marks leaves at or above 2 GiB uncached (PWT|PCD), so most PCI MMIO
 *     already has a usable alias.  A leaf that resolves to a cached memory
 *     type is not returned as a device mapping.
 *
 *  2. Physical addresses above 4 GiB (64-bit PCI BARs, which is where modern
 *     firmware puts NVMe, xHCI and most PCIe endpoints) have no boot alias.
 *     They are mapped on demand into spare 1 GiB slots of the same PML4 entry
 *     as the direct map, using 2 MiB uncached, non-executable leaves.  The
 *     mappings are permanent, matching arch_device_unmap_mmio()'s contract.
 *
 * Dynamic mappings start after the RAM direct-map slots.
 */
#include "core/types.h"
#include "platform.h"
#include "page_table.h"
#include "core/lock.h"
#include "mm/frame.h"
#include "mm/mm.h"

#include "cpu.h"
#include "boot/framebuffer.h"

extern uint64_t boot_pdpt_hh[512]; /* entry.S, hung under PML4[256] */

#define MMIO_DYNAMIC_FIRST_SLOT (X86_HIGH_RAM_MAP_END >> 30)
#define MMIO_HUGE_SIZE          (2ULL * 1024 * 1024)
#define MMIO_SLOT_SIZE          (1ULL << 30)
#define MMIO_CACHE_ENTRIES      48U

typedef struct {
    uint64_t physical; /* first mapped physical address (2 MiB aligned) */
    uint64_t length;   /* mapped length (2 MiB rounded) */
    void *address;
    uint32_t flags;    /* ARCH_MMIO_WC changes the effective memory type */
} mmio_mapping_t;

static mmio_mapping_t g_mmio_cache[MMIO_CACHE_ENTRIES];
static unsigned g_mmio_cache_next;
static spinlock_t g_mmio_lock = SPINLOCK_INIT;

static inline uintptr_t phys_to_direct(uint64_t pa) {
    return (uintptr_t)(pa + PAGE_OFFSET);
}

static inline uint64_t direct_to_phys(const void *p) {
    return (uint64_t)(uintptr_t)p - PAGE_OFFSET;
}

static void mmio_tlb_flush(void) {
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/* Effective memory type of a direct-map leaf.  Returns 1 for uncached. */
static int direct_leaf_uncached(uint64_t pa, uint64_t *span) {
    uint64_t va = pa + PAGE_OFFSET;
    uint64_t *table = boot_pgdir;
    uint64_t entry = 0;
    unsigned shift = 39;
    unsigned level;
    for (level = 0; level < 4; level++, shift -= 9) {
        entry = table[(va >> shift) & 511U];
        if (!(entry & PTE_V))
            return 0;
        if (level == 3 || (level && (entry & PTE_PS)))
            break;
        table = (uint64_t *)phys_to_direct(entry & 0x000ffffffffff000ULL);
    }
    *span = 1ULL << shift;
    uint64_t mask = 0x000ffffffffff000ULL & ~(*span - 1);
    if ((entry & mask) != (pa & ~(*span - 1)))
        return 0;
    unsigned pat_bit = (entry >> (level == 3 ? 7 : 12)) & 1U;
    unsigned pcd = (entry >> 4) & 1U;
    unsigned pwt = (entry >> 3) & 1U;
    unsigned index = (pat_bit << 2) | (pcd << 1) | pwt;
    uint64_t pat = rdmsr(0x277);
    uint64_t type = (pat >> (index * 8)) & 0xffU;
    return type == 0x00U /* UC */ || type == 0x07U /* UC- */;
}

static int direct_range_uncached(uint64_t pa, size_t length) {
    uint64_t end = pa + length;
    while (pa < end) {
        uint64_t span;
        if (!direct_leaf_uncached(pa, &span))
            return 0;
        uint64_t next = span - (pa & (span - 1));
        pa += next < end - pa ? next : end - pa;
    }
    return 1;
}

static uint64_t *dynamic_pd(unsigned slot) {
    uint64_t entry = boot_pdpt_hh[slot];
    if (entry & PTE_V) {
        if (entry & PTE_PS)
            return 0;
        return (uint64_t *)phys_to_direct(entry & 0x000ffffffffff000ULL);
    }
    void *page = frame_alloc_nr();
    if (!page)
        return 0;
    boot_pdpt_hh[slot] = direct_to_phys(page) | PTE_V | PTE_W;
    return page;
}

/* Map [start, end) of physical device space into a free 1 GiB VA slot. */
static void *map_explicit(uint64_t pa, size_t length, uint32_t flags) {
    /* The frame allocator backs new page tables.  Before pfa_init() there is
     * nothing to allocate from; the one early caller (PCI ECAM, always below
     * 4 GiB and uncached in the boot map) takes the direct-map path. */
    if (!pfa.meta)
        return 0;
    uint64_t start = pa & ~(MMIO_HUGE_SIZE - 1U);
    uint64_t end = (pa + length + MMIO_HUGE_SIZE - 1U) & ~(MMIO_HUGE_SIZE - 1U);
    if (end <= start || end - start > MMIO_SLOT_SIZE)
        return 0;
    uint64_t offset = pa - start;
    unsigned pages = (unsigned)((end - start) / MMIO_HUGE_SIZE);

    uint64_t pte_flags = PTE_V | PTE_W | PTE_PS | PTE_NX |
                         ((flags & ARCH_MMIO_WC) ? PTE_PAT : PTE_DEVICE);
    for (unsigned slot = MMIO_DYNAMIC_FIRST_SLOT; slot < 512U; slot++) {
        uint64_t *pd = dynamic_pd(slot);
        if (!pd)
            continue;
        int usable = 1;
        for (unsigned i = 0; i < pages; i++) {
            if (pd[i] & PTE_V) {
                usable = 0;
                break;
            }
        }
        if (!usable)
            continue;
        for (unsigned i = 0; i < pages; i++)
            pd[i] = (start + i * MMIO_HUGE_SIZE) | pte_flags;
        mmio_tlb_flush();
        uintptr_t base = (uintptr_t)(PAGE_OFFSET + (uint64_t)slot * MMIO_SLOT_SIZE);
        return (void *)(base + offset);
    }
    return 0;
}

/* PAT reset entry 4 is WB, not WC.  Program it to WC once before the first
 * write-combining framebuffer mapping; PTE_PAT selects this entry. */
static void ensure_wc_pat(void) {
    static int ready;
    if (ready)
        return;
    uint64_t pat = rdmsr(0x277);
    if (((pat >> 32) & 0xffU) != 0x01U) {
        pat = (pat & ~(0xffULL << 32)) | (0x01ULL << 32);
        wrmsr(0x277, pat);
    }
    ready = 1;
}

void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags) {
    if (!length || pa + length < pa)
        return 0;

    if (flags & ARCH_MMIO_WC)
        ensure_wc_pat();

    uint64_t lock_flags = spin_lock_irqsave(&g_mmio_lock);
    for (unsigned i = 0; i < MMIO_CACHE_ENTRIES; i++) {
        if (g_mmio_cache[i].address && g_mmio_cache[i].physical == pa &&
            g_mmio_cache[i].length == length &&
            (g_mmio_cache[i].flags & ARCH_MMIO_WC) ==
                (flags & ARCH_MMIO_WC)) {
            void *cached = g_mmio_cache[i].address;
            spin_unlock_irqrestore(&g_mmio_lock, lock_flags);
            return cached;
        }
    }

    void *mapped = 0;
    int uncached = pa < 0x100000000ULL &&
                   length <= 0x100000000ULL - pa && direct_range_uncached(pa, length);
    /* A non-WC caller keeps an existing uncached direct alias.  A WC caller
     * (the framebuffer) needs an explicit PAT alias; before the page
     * allocator exists arch_fbcon_map() falls back to the direct alias. */
    if (uncached && !(flags & ARCH_MMIO_WC)) {
        mapped = (void *)phys_to_direct(pa);
    } else {
        mapped = map_explicit(pa, length, flags);
    }

    if (mapped) {
        unsigned slot = g_mmio_cache_next++ % MMIO_CACHE_ENTRIES;
        g_mmio_cache[slot] = (mmio_mapping_t){pa, length, mapped, flags};
    }
    spin_unlock_irqrestore(&g_mmio_lock, lock_flags);
    return mapped;
}

void arch_device_unmap_mmio(void *mapped, size_t length) {
    (void)mapped;
    (void)length; /* Permanent, shared mappings; see the file comment. */
}
