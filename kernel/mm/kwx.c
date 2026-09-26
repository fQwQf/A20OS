/*
 * A20OS — 内核自身 W^X（KXAN）弱默认钩子
 *
 * 各架构可在 kernel/arch/<arch>/mm/kwx.c 中用强符号覆盖这些钩子：
 *   - arch_kernel_wx_finalize()  在 mm_init() 之后把引导期整块 RWX 大页
 *     拆成按段权限的映射（text=ROX / rodata=RO / data=RW+NX，直映射 NX）；
 *   - arch_kwx_module_protect()  把 drvmod 模块占用的直映射页按
 *     text=RX / data=RW+NX 重新打标；
 *   - arch_kwx_module_unprotect() 模块卸载时恢复 RW+NX。
 *
 * 未完成拆分的架构保持默认空实现：直映射仍可执行，模块加载无需配合。
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
