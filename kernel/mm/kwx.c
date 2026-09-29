/*
 * A20OS — weak default hooks for the kernel's own W^X (KXAN)
 *
 * Each architecture may override these hooks with a strong symbol in
 * kernel/arch/<arch>/mm/kwx.c:
 *   - arch_kernel_wx_finalize()  after mm_init(), splits the single boot-time
 *     RWX huge page into per-segment permission mappings
 *     (text=ROX / rodata=RO / data=RW+NX, direct map NX);
 *   - arch_kwx_module_protect()  re-tags the direct-mapped pages a drvmod
 *     module occupies as text=RX / data=RW+NX;
 *   - arch_kwx_module_unprotect() restores RW+NX on module unload.
 *
 * An architecture that has not finished the split keeps the empty default:
 * the direct map stays executable and module loading needs no cooperation.
 *
 * loongarch64 is NOT such an unfinished case and must not be ported by
 * copying an existing implementation.  Its kernel space is translated by the
 * LoongArch DMW (CSR_DMW0/1), a direct map window that bypasses the TLB and
 * the multi-level page-table walk entirely, and it stays active in paging mode
 * (kernel/arch/loongarch64/boot/entry.S, and the note above pt_map_kernel()).
 * boot_pgdir is therefore empty by design, so arch_kernel_wx_finalize() has no
 * mapping to re-tag: text/data permissions for the kernel image are not
 * expressible through page tables on this architecture.  Giving loongarch64 a
 * real split would mean dropping DMW for kernel space and routing it through
 * an explicit page-table window, which is a board/architecture decision rather
 * than a kernel code change.
 */

#include "core/types.h"
#include "mm/mm.h"

__attribute__((weak)) void arch_kernel_wx_finalize(void)
{
}

__attribute__((weak)) int arch_kwx_module_protect(paddr_t base_pa,
                                                  size_t exec_bytes,
                                                  size_t total_bytes)
{
    (void)base_pa;
    (void)exec_bytes;
    (void)total_bytes;
    return 0;
}

__attribute__((weak)) void arch_kwx_module_unprotect(paddr_t base_pa,
                                                     size_t total_bytes)
{
    (void)base_pa;
    (void)total_bytes;
}
