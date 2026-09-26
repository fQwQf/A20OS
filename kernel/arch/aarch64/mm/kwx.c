/*
 * A20OS aarch64 — 内核自身 W^X（KXAN）
 *
 * 引导期 entry.S 用单个 1 GiB L1 block（boot_l1[1]，PA 0x40000000 起的
 * DRAM）把内核映像以 EL1 RW + 可执行映射到恒等区与高半区（boot_pgdir[0]
 * 与 [1] 共享同一张 boot_l1），并在启动注释中说明因此暂时清除
 * SCTLR_EL1.WXN。本文件在 mm_init() 之后（arch_kernel_wx_finalize，由
 * kernel_main 调用、仍在 BSP 单核阶段）把 DRAM block 细化为：
 *
 *   - boot_l1[1] 降级为共享 L2 表中的 2 MiB NX block（普通内存、EL1 RW、
 *     PXN|UXN）。boot_l1 经 boot_pgdir[1] 被所有进程页表共享，之后的修改
 *     对所有地址空间同时生效；恒等别名走同一张表，自动一致。
 *   - 内核映像占用的 2 MiB block 再降级为 L3 4 KiB 页，按段打权限：
 *     .text RO+EL1X、.rodata RO+NX、.data/.bss RW+NX。
 *   - boot_l1[0]（设备 MMIO）本已 PXN|UXN，无需处理。
 *
 * 拆分完成后在 C 中置回 SCTLR_EL1.WXN；entry.S 从核路径同步改为置位
 * （qemu-virt 非 NOMMU 构建）。
 *
 * drvmod 模块经直映射执行：arch_kwx_module_protect() 把模块页拆成 4 KiB
 * 并置 text=RX、data=RW+NX，卸载时恢复 RW+NX。aarch64 的 QEMU 板尚未接
 * 远程 TLB shootdown，模块打标只做本核 tlbi vmalle1（已知边界，见
 * docs/mm/kernel-wx.md）。
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

/* boot_l1：boot_pgdir[0] 与 [1] 共享的 L1 表。 */
static uint64_t *a64_kwx_l1(void)
{
    uint64_t e = boot_pgdir[1];
    if ((e & 0x3) != 0x3)
        return NULL;
    return arch_pte_to_ptr(e);
}

/* 覆盖 va 的共享 L2 表（DRAM block 已在 finalize 时降级）。 */
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

/* 确保 va 所在的 2 MiB block 已拆成 L3 4 KiB 页表；返回 L3 表或 NULL。 */
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

    /* DRAM 1 GiB block 重建：新 L2 表（连同内核映像块的 L3 拆分）先在
     * 旁路构建完整，再用一次写入替换 L1 项。整个过程中现有映射始终
     * 有效——若先装 NX block 再拆页，TLB miss 会在 NX 的 .text 上取指
     * 故障，trap vector 同样 NX，直接卡死。 */
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

    /* 表已完整，一次写入切换。 */
    l1[dram_idx] = arch_pte_from_pa(va_to_pa(l2)) | PTE_DIR;

    arch_tlb_flush();

    /* 3. 映射已无 EL1 可写且可执行的页，置回 WXN。 */
    uint64_t sctlr;
    __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1UL << 19; /* WXN */
    __asm__ __volatile__("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
    __asm__ __volatile__("isb" ::: "memory");

    /* 4. 自检：直接查询页表确认关键页权限。 */
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
