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
