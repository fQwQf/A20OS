/*
 * A20OS x86_64 — 内核自身 W^X（KXAN）
 *
 * 引导期 entry.S 用四个 1 GiB 大页把物理 0..4 GiB 映射到高半区
 * （boot_pdpt_hh，RWX；EFER.NXE 由 trap_init 打开）。本文件在 mm_init()
 * 之后（arch_kernel_wx_finalize，由 kernel_main 调用、仍在 BSP 单核阶段）
 * 细化为：
 *
 *   - 与 RAM 相交的 1 GiB 槽位降级为 PD 中的 2 MiB NX 块。boot_pdpt_hh
 *     经 PML4[256] 被所有进程页表共享，之后的修改对所有地址空间生效。
 *   - 内核映像占用的 2 MiB 块再降级为 PT 中的 4 KiB 页，按段打权限：
 *     .text ROX、.rodata RO+NX、.data/.bss RW+NX。
 *   - 其余 1 GiB 槽位（MMIO，PCD）只补 NX 位。
 *
 * 恒等映射（PML4[0]）不在此处理：AP 蹦床在 64 位入口前还要经恒等映射取指，
 * 既有 arch_unmap_boot_identity() 已在 smp_boot_secondaries 之后将其整段
 * 清除。
 *
 * drvmod 模块经直映射执行：arch_kwx_module_protect() 把模块页拆成 4 KiB
 * 并置 text=RX、data=RW+NX，卸载时恢复 RW+NX。
 */

#include "core/arch.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/smp.h"
#include "mm/mm.h"
#include "mm/frame.h"

extern char __text_start[], __rodata_start[], __data_start[], _bss_end[];
extern uint64_t boot_pdpt_hh[512];  /* entry.S, 挂在 PML4[256] 下 */

#define X86_GIGA_SIZE   (1UL << 30)
#define X86_EFER        0xC0000080U
#define X86_EFER_NXE    (1ULL << 11)

/* 找到覆盖 va 的共享 PD（对应 1 GiB 槽位须已降级）。 */
static uint64_t *x86_kwx_pd(vaddr_t va)
{
    uint64_t e = boot_pdpt_hh[arch_pt_vpn(va, 2)];
    if (!(e & PTE_V) || (e & PTE_PS))
        return NULL;
    return arch_pte_to_ptr(e);
}

/* 确保 va 所在的 2 MiB 块已拆成 4 KiB 页表；返回 PT 或 NULL。 */
static uint64_t *x86_kwx_split_pmd(vaddr_t va)
{
    uint64_t *pd = x86_kwx_pd(va);
    if (!pd)
        return NULL;
    uint64_t *slot = &pd[arch_pt_vpn(va, 1)];
    uint64_t e = *slot;
    if ((e & PTE_V) && !(e & PTE_PS))
        return arch_pte_to_ptr(e);
    if (!(e & PTE_V))
        return NULL;
    uint64_t *pt = frame_alloc();
    if (!pt)
        return NULL;
    paddr_t base = arch_pte_addr(e);
    for (int i = 0; i < 512; i++)
        pt[i] = arch_pte_leaf(base + (paddr_t)i * PAGE_SIZE, PTE_R | PTE_W);
    *slot = ((uint64_t)(uintptr_t)pt - PAGE_OFFSET) | PTE_V | PTE_W;
    return pt;
}

static int x86_kwx_set_pages(paddr_t pa, size_t size, pte_t flags)
{
    for (paddr_t p = pa; p < pa + size; p += PAGE_SIZE) {
        vaddr_t va = p + PAGE_OFFSET;
        uint64_t *pt = x86_kwx_split_pmd(va);
        if (!pt)
            return -ENOMEM;
        pt[arch_pt_vpn(va, 0)] = arch_pte_leaf(p, flags);
    }
    return 0;
}

static void x86_kwx_flush_all(void)
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
    if (x86_kwx_set_pages(base_pa, exec_size, PTE_R | PTE_X) < 0)
        return -ENOMEM;
    if (x86_kwx_set_pages(base_pa + exec_size, total_size - exec_size,
                          PTE_R | PTE_W) < 0)
        return -ENOMEM;
    x86_kwx_flush_all();
    return 0;
}

void arch_kwx_module_unprotect(paddr_t base_pa, size_t total_bytes)
{
    if (x86_kwx_set_pages(base_pa, ROUND_UP(total_bytes, PAGE_SIZE),
                          PTE_R | PTE_W) == 0)
        x86_kwx_flush_all();
}

void arch_kernel_wx_finalize(void)
{
    /* trap_init() 已在 BSP 上打开 NXE；这里兜底一次，因为本函数会在引导
     * 大页上首次写入 NX 位。 */
    uint64_t efer = rdmsr(X86_EFER);
    if (!(efer & X86_EFER_NXE))
        wrmsr(X86_EFER, efer | X86_EFER_NXE);

    vaddr_t ro_start   = (vaddr_t)(uintptr_t)__rodata_start;
    vaddr_t data_start = (vaddr_t)(uintptr_t)__data_start;
    vaddr_t img_start  = (vaddr_t)(uintptr_t)__text_start;
    vaddr_t img_end    = (vaddr_t)ROUND_UP((uintptr_t)_bss_end, PAGE_SIZE);

    /* 高半区 1 GiB 大页逐槽位重建：新 PD（连同内核映像块的 PT 拆分）先在
     * 旁路构建完整，再用一次写入替换 PDPT 项。整个过程中现有映射始终
     * 有效——若先装 NX 大页再拆块，TLB miss 会在 NX 的 .text 上取指
     * 故障，trap 处理程序同样 NX，直接三连环复位。 */
    for (int slot = 0; slot < 4; slot++) {
        uint64_t e = boot_pdpt_hh[slot];
        if (!(e & PTE_V) || !(e & PTE_PS))
            continue;
        paddr_t base = (paddr_t)slot * X86_GIGA_SIZE;
        int covers_ram = (slot == 0);   /* 槽位 0 含内核映像，必须降级 */
        size_t nranges = arch_ram_range_count();
        for (size_t r = 0; r < nranges && !covers_ram; r++) {
            paddr_t rb = 0, re = 0;
            if (arch_ram_range(r, &rb, &re) == 0 &&
                base < re && base + X86_GIGA_SIZE > rb)
                covers_ram = 1;
        }
        if (!covers_ram) {
            boot_pdpt_hh[slot] = e | PTE_NX;
            continue;
        }

        uint64_t *pd = frame_alloc();
        if (!pd)
            panic("kwx: cannot allocate PD");
        for (int i = 0; i < 512; i++)
            pd[i] = (base + (paddr_t)i * PMD_SIZE) | (e & 0xFFFUL) | PTE_NX;

        /* 本槽位内内核映像占用的 2 MiB 块：旁路建好 PT 后链入。 */
        for (vaddr_t blk = ROUND_DOWN(img_start, PMD_SIZE); blk < img_end;
             blk += PMD_SIZE) {
            if (arch_pt_vpn(blk, 2) != slot)
                continue;
            uint64_t *pt = frame_alloc();
            if (!pt)
                panic("kwx: cannot allocate PT");
            paddr_t blk_pa = va_to_pa((void *)blk);
            for (int i = 0; i < 512; i++) {
                vaddr_t va = blk + (vaddr_t)i * PAGE_SIZE;
                pte_t flags = PTE_R | PTE_W;
                if (va >= img_start && va < img_end) {
                    if (va < ro_start)
                        flags = PTE_R | PTE_X;
                    else if (va < data_start)
                        flags = PTE_R;
                }
                pt[i] = arch_pte_leaf(blk_pa + (paddr_t)i * PAGE_SIZE,
                                      flags);
            }
            pd[arch_pt_vpn(blk, 1)] =
                ((uint64_t)(uintptr_t)pt - PAGE_OFFSET) | PTE_V | PTE_W;
        }

        /* 表已完整，一次写入切换。 */
        boot_pdpt_hh[slot] = ((uint64_t)(uintptr_t)pd - PAGE_OFFSET) |
                             PTE_V | PTE_W;
    }

    arch_tlb_flush();

    /* 自检：直接查询页表确认关键页权限。 */
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
        panic("kwx: x86_64 kernel image permission self-check failed");
    printf("[KXAN] x86_64: text=ROX rodata=RO data=RW+NX dmap=NX "
           "(image 0x%lx..0x%lx)\n",
           (unsigned long)img_start, (unsigned long)img_end);
}
