/*
 * A20OS riscv64 — kernel's own W^X (KXAN)
 *
 * At boot, boot_pgdir maps the whole RAM window RWX into the identity region
 * and the high half (entry.S) with 1 GiB megapages.  This file runs after
 * mm_init() (arch_kernel_wx_finalize, from kernel_main, still single-core).
 *
 *   - every gigapage intersecting RAM becomes 2 MiB NX blocks in a shared
 *     level-1 table, still shared with all process page tables after
 *     pt_map_kernel() copies the root entries, so later edits hit all spaces.
 *   - the 2 MiB block holding the kernel image is split into 4 KiB pages:
 *     [.text)        V|R|X|A        (read-only + executable)
 *     [.rodata)      V|R|A          (read-only + NX)
 *     [.data..bss]   V|R|W|A|D      (read-write + NX)
 *   - X is cleared from the remaining high-half leaf entries (MMIO window).
 *
 * The identity map (root table [0,256)) is kept until
 * arch_unmap_boot_identity(): an SMP secondary still fetches at the identity
 * address after satp is written in .enable_mmu, so an early NX or teardown
 * faults it the instant paging is enabled; smp_boot_secondaries then clears
 * the whole identity region.
 *
 * drvmod modules take pages from pfa and execute through the direct map; at
 * load arch_kwx_module_protect() splits them into 4 KiB, text=RX, data=RW+NX.
 */

#include "core/arch.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/smp.h"
#include "mm/mm.h"
#include "mm/frame.h"

void arch_unmap_boot_identity(void)
{
#ifndef CONFIG_NOMMU
    for (int i = 0; i < ARCH_PT_USER_END; i++)
        boot_pgdir[i] = 0;
    arch_tlb_flush();
#endif
}

#ifndef CONFIG_NOMMU

extern char __text_start[], __rodata_start[], __data_start[], _bss_end[];

#define RV64_GIGA_SIZE   (1UL << 30)

/* direct-mapped ordinary RAM page: readable, writable, not executable */
#define RV64_KFLAGS_DATA  (PTE_R | PTE_W | PTE_A | PTE_D)
/* kernel .text: read-only executable */
#define RV64_KFLAGS_TEXT  (PTE_R | PTE_X | PTE_A)
/* kernel .rodata: read-only, not executable */
#define RV64_KFLAGS_RO    (PTE_R | PTE_A)

/* the shared level-1 table covering va (gigapage downgraded at finalize) */
static pte_t *rv64_kwx_l1(vaddr_t va)
{
    pte_t e = boot_pgdir[arch_pt_vpn(va, ARCH_PT_ROOT_LEVEL)];
    if (!(e & PTE_V) || arch_pte_is_leaf(e))
        return NULL;
    return arch_pte_to_ptr(e);
}

/* ensure the 2 MiB block holding va is split into 4 KiB; return l0 or NULL */
static pte_t *rv64_kwx_split_pmd(vaddr_t va)
{
    pte_t *l1 = rv64_kwx_l1(va);
    if (!l1)
        return NULL;
    pte_t *slot = &l1[arch_pt_vpn(va, 1)];
    pte_t e = *slot;
    if ((e & PTE_V) && !arch_pte_is_leaf(e))
        return arch_pte_to_ptr(e);
    if (!(e & PTE_V) || !(e & PTE_W))
        return NULL;    /* not a RAM block (or absent); must not occur in the direct-mapped RAM */

    pte_t *l0 = frame_alloc();
    if (!l0)
        return NULL;
    paddr_t base = arch_pte_addr(e);
    pte_t flags = arch_pte_flags(e);
    for (int i = 0; i < 512; i++)
        l0[i] = arch_pte_leaf(base + (paddr_t)i * PAGE_SIZE, flags);
    *slot = arch_pte_from_pa(va_to_pa(l0)) | PTE_DIR;
    return l0;
}

/* set the direct-mapped 4 KiB pages in [pa, pa+size) to the given perms. */
static int rv64_kwx_set_pages(paddr_t pa, size_t size, pte_t flags)
{
    for (paddr_t p = pa; p < pa + size; p += PAGE_SIZE) {
        vaddr_t va = p + PAGE_OFFSET;
        pte_t *l0 = rv64_kwx_split_pmd(va);
        if (!l0)
            return -ENOMEM;
        l0[arch_pt_vpn(va, 0)] = arch_pte_leaf(p, flags);
    }
    return 0;
}

static void rv64_kwx_flush_all(void)
{
    arch_tlb_flush();
    if (smp_remote_tlb_flush_supported()) {
        uint32_t self = 1U << cpu_current_id();
        uint32_t targets = smp_online_cpu_mask() & ~self;
        if (targets && smp_remote_tlb_flush(targets, 0, 0) < 0)
            panic("kwx: remote TLB flush failed");
    }
}

int arch_kwx_module_protect(paddr_t base_pa, size_t exec_bytes,
                            size_t total_bytes)
{
    size_t exec_size = ROUND_UP(exec_bytes, PAGE_SIZE);
    size_t total_size = ROUND_UP(total_bytes, PAGE_SIZE);
    if (exec_size > total_size)
        return -EINVAL;
    if (rv64_kwx_set_pages(base_pa, exec_size, RV64_KFLAGS_TEXT) < 0)
        return -ENOMEM;
    if (rv64_kwx_set_pages(base_pa + exec_size, total_size - exec_size,
                           RV64_KFLAGS_DATA) < 0)
        return -ENOMEM;
    rv64_kwx_flush_all();
    return 0;
}

void arch_kwx_module_unprotect(paddr_t base_pa, size_t total_bytes)
{
    if (rv64_kwx_set_pages(base_pa, ROUND_UP(total_bytes, PAGE_SIZE),
                           RV64_KFLAGS_DATA) == 0)
        rv64_kwx_flush_all();
}

void arch_kernel_wx_finalize(void)
{
    vaddr_t ro_start   = (vaddr_t)(uintptr_t)__rodata_start;
    vaddr_t data_start = (vaddr_t)(uintptr_t)__data_start;
    vaddr_t img_start  = (vaddr_t)(uintptr_t)__text_start;
    vaddr_t img_end    = (vaddr_t)ROUND_UP((uintptr_t)_bss_end, PAGE_SIZE);

    /* Rebuild the high half slot by slot: the new level-1 table (with the
     * level-0 split of the kernel image block) is built off to the side and
     * installed with one write, so existing mappings stay valid throughout:
     * NX huge pages installed before the split leave the CPU on leftover TLB
     * entries, and a TLB miss then faults fetching from the NX .text while
     * the trap vector is NX too, deadlocking three ways (seen on SMP=2). */
    for (int slot = ARCH_PT_USER_END; slot < ARCH_PT_ENTRIES; slot++) {
        pte_t e = boot_pgdir[slot];
        if (!(e & PTE_V) || !arch_pte_is_leaf(e))
            continue;
        paddr_t base = arch_pte_addr(e);
        int covers_ram = 0;
        size_t nranges = arch_ram_range_count();
        for (size_t r = 0; r < nranges; r++) {
            paddr_t rb = 0, re = 0;
            if (arch_ram_range(r, &rb, &re) == 0 &&
                base < re && base + RV64_GIGA_SIZE > rb) {
                covers_ram = 1;
                break;
            }
        }
        if (!covers_ram) {
            /* non-RAM leaf entries (MMIO): clear X atomically, one entry at a time */
            boot_pgdir[slot] = e & ~(pte_t)PTE_X;
            continue;
        }

        pte_t *l1 = frame_alloc();
        if (!l1)
            panic("kwx: cannot allocate level-1 table");
        for (int i = 0; i < 512; i++)
            l1[i] = arch_pte_leaf(base + (paddr_t)i * PMD_SIZE,
                                  RV64_KFLAGS_DATA);

        /* the kernel image's 2 MiB block in this slot, linked in once l0 is built */
        for (vaddr_t blk = ROUND_DOWN(img_start, PMD_SIZE); blk < img_end;
             blk += PMD_SIZE) {
            if (arch_pt_vpn(blk, ARCH_PT_ROOT_LEVEL) != slot)
                continue;
            pte_t *l0 = frame_alloc();
            if (!l0)
                panic("kwx: cannot allocate level-0 table");
            paddr_t blk_pa = va_to_pa((void *)blk);
            for (int i = 0; i < 512; i++) {
                vaddr_t va = blk + (vaddr_t)i * PAGE_SIZE;
                pte_t flags = RV64_KFLAGS_DATA;
                if (va >= img_start && va < img_end) {
                    if (va < ro_start)
                        flags = RV64_KFLAGS_TEXT;
                    else if (va < data_start)
                        flags = RV64_KFLAGS_RO;
                }
                l0[i] = arch_pte_leaf(blk_pa + (paddr_t)i * PAGE_SIZE,
                                      flags);
            }
            l1[arch_pt_vpn(blk, 1)] =
                arch_pte_from_pa(va_to_pa(l0)) | PTE_DIR;
        }

        /* the table is complete; switch over with one write (old gpage still valid) */
        boot_pgdir[slot] = arch_pte_from_pa(va_to_pa(l1)) | PTE_DIR;
    }

    arch_tlb_flush();

    /* Self-check: query the page tables for the permissions of critical pages. */
    mm_leaf_info_t li;
    int ok = mm_query_leaf(boot_pgdir, img_start, &li) &&
             (li.flags & PTE_R) && (li.flags & PTE_X) && !(li.flags & PTE_W);
    ok = ok && mm_query_leaf(boot_pgdir, ro_start, &li) &&
         (li.flags & PTE_R) && !(li.flags & (PTE_W | PTE_X));
    ok = ok && mm_query_leaf(boot_pgdir, data_start, &li) &&
         (li.flags & PTE_R) && (li.flags & PTE_W) && !(li.flags & PTE_X);
    ok = ok && mm_query_leaf(boot_pgdir, img_end, &li) &&
         (li.flags & PTE_W) && !(li.flags & PTE_X);
    if (!ok)
        panic("kwx: riscv64 kernel image permission self-check failed");
    printf("[KXAN] riscv64: text=ROX rodata=RO data=RW+NX dmap=NX "
           "(image 0x%lx..0x%lx)\n",
           (unsigned long)img_start, (unsigned long)img_end);
}

#endif /* !CONFIG_NOMMU */
