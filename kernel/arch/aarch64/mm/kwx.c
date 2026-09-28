/*
 * A20OS aarch64 — kernel's own W^X (KXAN)
 *
 * At boot, entry.S uses a single 1 GiB L1 block (boot_l1[1], the DRAM at PA
 * 0x40000000) to map the kernel image as EL1 RW + executable into the identity
 * region and the high half (boot_pgdir[0] and [1] share one boot_l1); the boot
 * comment there notes that SCTLR_EL1.WXN is cleared for now.  This file runs
 * after mm_init() (arch_kernel_wx_finalize, still on the BSP single-core path).
 *
 *   - boot_l1[1] becomes 2 MiB NX blocks in a shared L2 table (ordinary
 *     memory, EL1 RW, PXN|UXN), shared via boot_pgdir[1] with every process
 *     page table, so edits reach all spaces; the identity alias shares it too.
 *   - the image's 2 MiB block splits into L3 4 KiB pages, per segment:
 *     .text RO+EL1X, .rodata RO+NX, .data/.bss RW+NX.
 *   - boot_l1[0] (device MMIO) is already PXN|UXN; nothing to do there.
 *
 * Once the split is done, SCTLR_EL1.WXN is set back from C; the entry.S
 * secondary-core path was changed to set the bit (qemu-virt non-NOMMU builds).
 *
 * drvmod modules execute through the direct map: arch_kwx_module_protect()
 * splits the module pages into 4 KiB, text=RX, data=RW+NX, restored on unload.
 * The aarch64 QEMU board has no remote TLB shootdown yet, so tagging a module
 * only does a local tlbi vmalle1 (known boundary, see docs/mm/kernel-wx.md).
 */

#include "core/arch.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/smp.h"
#include "mm/mm.h"
#include "mm/frame.h"

#if !defined(CONFIG_NOMMU) && !defined(CONFIG_BOARD_VIRTUALBOX_AARCH64)

extern char __text_start[], __rodata_start[], __data_start[], _bss_end[];

#define A64_KFLAGS_TEXT  (PTE_R | PTE_X | PTE_MAT1 | PTE_LEAF)
#define A64_KFLAGS_RO    (PTE_R | PTE_MAT1 | PTE_LEAF)
#define A64_KFLAGS_DATA  (PTE_R | PTE_W | PTE_MAT1 | PTE_LEAF)

/* boot_l1: the L1 table shared by boot_pgdir[0] and [1]. */
static uint64_t *a64_kwx_l1(void)
{
    uint64_t e = boot_pgdir[1];
    if ((e & 0x3) != 0x3)
        return NULL;
    return arch_pte_to_ptr(e);
}

/* the shared L2 table covering va (the DRAM block was downgraded earlier) */
static uint64_t *a64_kwx_l2(vaddr_t va)
{
    uint64_t *l1 = a64_kwx_l1();
    if (!l1)
        return NULL;
    uint64_t e = l1[arch_pt_vpn(va, 2)];
    if ((e & 0x3) != 0x3)
        return NULL;
    return arch_pte_to_ptr(e);
}

/* ensure the 2 MiB block holding va is split into an L3; L3 or NULL */
static uint64_t *a64_kwx_split_block(vaddr_t va)
{
    uint64_t *l2 = a64_kwx_l2(va);
    if (!l2)
        return NULL;
    uint64_t *slot = &l2[arch_pt_vpn(va, 1)];
    uint64_t e = *slot;
    if ((e & 0x3) == 0x3)
        return arch_pte_to_ptr(e);
    if (!(e & PTE_V))
        return NULL;
    uint64_t *l3 = frame_alloc();
    if (!l3)
        return NULL;
    paddr_t base = arch_pte_addr(e);
    for (int i = 0; i < 512; i++)
        l3[i] = arch_pte_leaf(base + (paddr_t)i * PAGE_SIZE,
                              A64_KFLAGS_DATA);
    *slot = arch_pte_from_pa(va_to_pa(l3)) | PTE_DIR;
    return l3;
}

static int a64_kwx_set_pages(paddr_t pa, size_t size, pte_t flags)
{
    for (paddr_t p = pa; p < pa + size; p += PAGE_SIZE) {
        vaddr_t va = p + PAGE_OFFSET;
        uint64_t *l3 = a64_kwx_split_block(va);
        if (!l3)
            return -ENOMEM;
        l3[arch_pt_vpn(va, 0)] = arch_pte_leaf(p, flags);
    }
    return 0;
}

int arch_kwx_module_protect(paddr_t base_pa, size_t exec_bytes,
                            size_t total_bytes)
{
    size_t exec_size = ROUND_UP(exec_bytes, PAGE_SIZE);
    size_t total_size = ROUND_UP(total_bytes, PAGE_SIZE);
    if (exec_size > total_size)
        return -EINVAL;
    if (a64_kwx_set_pages(base_pa, exec_size, A64_KFLAGS_TEXT) < 0)
        return -ENOMEM;
    if (a64_kwx_set_pages(base_pa + exec_size, total_size - exec_size,
                          A64_KFLAGS_DATA) < 0)
        return -ENOMEM;
    arch_tlb_flush();
    if (smp_remote_tlb_flush_supported()) {
        uint32_t self = 1U << cpu_current_id();
        uint32_t targets = smp_online_cpu_mask() & ~self;
        if (targets && smp_remote_tlb_flush(targets, 0, 0) < 0)
            panic("kwx: remote TLB flush failed");
    }
    return 0;
}

void arch_kwx_module_unprotect(paddr_t base_pa, size_t total_bytes)
{
    if (a64_kwx_set_pages(base_pa, ROUND_UP(total_bytes, PAGE_SIZE),
                          A64_KFLAGS_DATA) == 0)
        arch_tlb_flush();
}

void arch_kernel_wx_finalize(void)
{
    vaddr_t ro_start   = (vaddr_t)(uintptr_t)__rodata_start;
    vaddr_t data_start = (vaddr_t)(uintptr_t)__data_start;
    vaddr_t img_start  = (vaddr_t)(uintptr_t)__text_start;
    vaddr_t img_end    = (vaddr_t)ROUND_UP((uintptr_t)_bss_end, PAGE_SIZE);

    uint64_t *l1 = a64_kwx_l1();
    if (!l1)
        panic("kwx: boot L1 table missing");
    int dram_idx = arch_pt_vpn(img_start, 2);
    uint64_t e = l1[dram_idx];
    if (!(e & PTE_V) || (e & 0x2))
        panic("kwx: unexpected boot DRAM L1 descriptor 0x%lx",
              (unsigned long)e);
    paddr_t dram_base = arch_pte_addr(e);

    /* Rebuild the DRAM 1 GiB block: the new L2 table (with the L3 split of
     * the image block) is built off to the side and installed with one write,
     * so existing mappings stay valid throughout: NX blocks installed first
     * hang the CPU on a TLB miss into the NX .text, trap vector NX too. */
    uint64_t *l2 = frame_alloc();
    if (!l2)
        panic("kwx: cannot allocate L2 table");
    for (int i = 0; i < 512; i++)
        l2[i] = arch_pte_block(dram_base + (paddr_t)i * PMD_SIZE,
                               A64_KFLAGS_DATA);

    for (vaddr_t blk = ROUND_DOWN(img_start, PMD_SIZE); blk < img_end;
         blk += PMD_SIZE) {
        if (arch_pt_vpn(blk, 2) != dram_idx)
            panic("kwx: kernel image escapes its DRAM L1 block");
        uint64_t *l3 = frame_alloc();
        if (!l3)
            panic("kwx: cannot allocate L3 table");
        paddr_t blk_pa = va_to_pa((void *)blk);
        for (int i = 0; i < 512; i++) {
            vaddr_t va = blk + (vaddr_t)i * PAGE_SIZE;
            pte_t flags = A64_KFLAGS_DATA;
            if (va >= img_start && va < img_end) {
                if (va < ro_start)
                    flags = A64_KFLAGS_TEXT;
                else if (va < data_start)
                    flags = A64_KFLAGS_RO;
            }
            l3[i] = arch_pte_leaf(blk_pa + (paddr_t)i * PAGE_SIZE, flags);
        }
        l2[arch_pt_vpn(blk, 1)] =
            arch_pte_from_pa(va_to_pa(l3)) | PTE_DIR;
    }

    /* the table is complete; switch over with a single write. */
    l1[dram_idx] = arch_pte_from_pa(va_to_pa(l2)) | PTE_DIR;

    arch_tlb_flush();

    /* 3. no EL1-writable and executable page is left; set WXN back. */
    uint64_t sctlr;
    __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1UL << 19; /* WXN */
    __asm__ __volatile__("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
    __asm__ __volatile__("isb" ::: "memory");

    /* 4. Self-check: query the page tables for critical page permissions. */
    mm_leaf_info_t li;
    int ok = mm_query_leaf(boot_pgdir, img_start, &li) &&
             (li.flags & PTE_X) && !(li.flags & PTE_W);
    ok = ok && mm_query_leaf(boot_pgdir, ro_start, &li) &&
         !(li.flags & (PTE_W | PTE_X));
    ok = ok && mm_query_leaf(boot_pgdir, data_start, &li) &&
         (li.flags & PTE_W) && !(li.flags & PTE_X);
    ok = ok && mm_query_leaf(boot_pgdir, img_end, &li) &&
         (li.flags & PTE_W) && !(li.flags & PTE_X);
    if (!ok)
        panic("kwx: aarch64 kernel image permission self-check failed");
    printf("[KXAN] aarch64: text=ROX rodata=RO data=RW+NX dmap=NX WXN=1 "
           "(image 0x%lx..0x%lx)\n",
           (unsigned long)img_start, (unsigned long)img_end);
}

#endif /* !CONFIG_NOMMU && !CONFIG_BOARD_VIRTUALBOX_AARCH64 */
