/*
 * A20OS riscv64 — 内核自身 W^X（KXAN）
 *
 * 引导期 boot_pgdir 用 1 GiB megapage 把整个 RAM 窗口以 RWX 同时映射到
 * 恒等区与高半区（entry.S）。本文件在 mm_init() 之后（arch_kernel_wx_finalize，
 * 由 kernel_main 调用、仍在单核阶段）把高半区细化为：
 *
 *   - 与 RAM 相交的每个 gigapage 降级为共享 level-1 表的 2 MiB NX 块；
 *     这些 level-1 表被 pt_map_kernel() 复制根项后仍与所有进程页表共享，
 *     之后对表内条目的修改对所有地址空间同时生效。
 *   - 内核映像占用的 2 MiB 块再降级为 4 KiB 页，按段打权限：
 *     [.text)        V|R|X|A        （只读+可执行）
 *     [.rodata)      V|R|A          （只读+NX）
 *     [.data..bss]   V|R|W|A|D      （读写+NX）
 *   - 其余高半区叶项（MMIO 窗口）清除 X。
 *
 * 恒等映射（根表 [0,256)）保留到 arch_unmap_boot_identity()：SMP 从核在
 * .enable_mmu 写入 satp 后的下一条取指仍走恒等地址，提前 NX/拆除会让从核
 * 在启用分页瞬间取指故障。smp_boot_secondaries 之后恒等区整段清除。
 *
 * drvmod 模块从 pfa 取页后经直映射执行；加载时 arch_kwx_module_protect()
 * 把模块页拆成 4 KiB 并置 text=RX、data=RW+NX，卸载时恢复 RW+NX。
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

/* 直映射普通 RAM 页：可读可写、不可执行 */
#define RV64_KFLAGS_DATA  (PTE_R | PTE_W | PTE_A | PTE_D)
/* 内核 .text：只读可执行 */
#define RV64_KFLAGS_TEXT  (PTE_R | PTE_X | PTE_A)
/* 内核 .rodata：只读不可执行 */
#define RV64_KFLAGS_RO    (PTE_R | PTE_A)

/* 找到覆盖 va 的共享 level-1 表（gigapage 已在 finalize 时降级）。 */
static pte_t *rv64_kwx_l1(vaddr_t va)
{
    pte_t e = boot_pgdir[arch_pt_vpn(va, ARCH_PT_ROOT_LEVEL)];
    if (!(e & PTE_V) || arch_pte_is_leaf(e))
        return NULL;
    return arch_pte_to_ptr(e);
}

/* 确保 va 所在的 2 MiB 块已拆成 4 KiB 页表；返回 level-0 表或 NULL。 */
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
        return NULL;    /* 非 RAM 块（或不存在），不应出现在直映射 RAM 中 */

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

/* 把 [pa, pa+size) 的直映射 4 KiB 页置为给定权限。 */
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

    /* 高半区逐槽位重建：新 level-1 表（连同内核映像块的 level-0 拆分）
     * 先在旁路构建完整，再用一次写入替换根项。整个过程中现有映射始终
     * 有效——若先装 NX 大页再拆块，CPU 只能靠 TLB 残存项续命，任何
     * TLB miss 都会在 NX 的 .text 上取指故障，而 trap vector 同样 NX，
     * 直接三连环卡死（SMP=2 构建实测触发）。 */
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
            /* MMIO 等非 RAM 叶项：单条原子去 X（这些页不取指）。 */
            boot_pgdir[slot] = e & ~(pte_t)PTE_X;
            continue;
        }

        pte_t *l1 = frame_alloc();
        if (!l1)
            panic("kwx: cannot allocate level-1 table");
        for (int i = 0; i < 512; i++)
            l1[i] = arch_pte_leaf(base + (paddr_t)i * PMD_SIZE,
                                  RV64_KFLAGS_DATA);

        /* 本槽位内内核映像占用的 2 MiB 块：旁路建好 4 KiB 页表后链入。 */
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

        /* 表已完整，一次写入切换（旧 gigapage 在此之前一直有效）。 */
        boot_pgdir[slot] = arch_pte_from_pa(va_to_pa(l1)) | PTE_DIR;
    }

    arch_tlb_flush();

    /* 自检：直接查询页表确认关键页权限。 */
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
