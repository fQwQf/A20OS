/*
 * A20OS — 用户态地址空间布局随机化（ASLR）
 *
 * 在 exec 时为每个进程生成随机布局偏移，复用 core/random.c 的熵池
 * （xoshiro 状态，启动时由 timer 节拍、硬件熵源 arch_hw_entropy_sample()
 * （x86_64 的 RDRAND/RDSEED）、内核地址与帧计数混合播种；getrandom(2)
 * 与 AT_RANDOM 也来自同一熵池）。
 *
 * 熵位数受现有固定用户布局约束：
 *   - 栈顶上限 USER_STACK_TOP+PAGE=0x40000000，下方 0x3F7F9000-0x3F800000
 *     是固定 vDSO/vvar（见 mm/vdso_layout.h），再往下 0x3E000000 是 TLS。
 *     栈随机化只能在 [USER_STACK_FLOOR+初始栈, 0x40000000) 这 8MB 窗口内
 *     取页偏移；为保证可用栈不小于约 4MB，64 位取 10 位熵（4MB 范围），
 *     32 位取 8 位（1MB 范围）。
 *   - mmap 回退基址从 MMAP_BASE_ADDR=0x60000000 向上增长，64 位用户空间
 *     下限 0x4000000000，取 20 位熵（4GB 窗口）；32 位取 10 位。
 *   - brk 起始偏移上限受 USER_TLS_BASE=0x3E000000 约束，按镜像末尾动态
 *     封顶，64 位最多 13 位（32MB），32 位 8 位。
 *
 * fork 语义与 Linux 一致：子进程通过 *child = *parent 继承 mm 的
 * mmap_base/stack_top/start_brk 等字段，不重新随机化。
 */

#include "mm/vm.h"
#include "core/random.h"
#include "core/consts.h"

#ifndef CONFIG_NOMMU

#ifdef CONFIG_64BIT
#define ASLR_STACK_RND_BITS 10
#define ASLR_MMAP_RND_BITS  20
#define ASLR_BRK_RND_BITS   13
#else
#define ASLR_STACK_RND_BITS 8
#define ASLR_MMAP_RND_BITS  10
#define ASLR_BRK_RND_BITS   8
#endif

static vaddr_t aslr_rnd_pages(unsigned bits)
{
    return (vaddr_t)(random_u64() & ((1UL << bits) - 1));
}

vaddr_t mm_aslr_mmap_base(void)
{
    return MMAP_BASE_ADDR + aslr_rnd_pages(ASLR_MMAP_RND_BITS) * PAGE_SIZE;
}

vaddr_t mm_aslr_stack_offset(void)
{
    return aslr_rnd_pages(ASLR_STACK_RND_BITS) * PAGE_SIZE;
}

vaddr_t mm_aslr_brk_offset(vaddr_t brk_base)
{
    vaddr_t max_pages = 0;
    if (brk_base < USER_TLS_BASE)
        max_pages = (USER_TLS_BASE - brk_base) >> PAGE_SIZE_BITS;
    vaddr_t cap = (1UL << ASLR_BRK_RND_BITS) - 1;
    if (max_pages > cap)
        max_pages = cap;
    if (max_pages == 0)
        return 0;
    return (vaddr_t)(random_u64() % max_pages) * PAGE_SIZE;
}

#else /* CONFIG_NOMMU: 地址即物理地址，不做随机化 */

vaddr_t mm_aslr_mmap_base(void)
{
    return MMAP_BASE_ADDR;
}

vaddr_t mm_aslr_stack_offset(void)
{
    return 0;
}

vaddr_t mm_aslr_brk_offset(vaddr_t brk_base)
{
    (void)brk_base;
    return 0;
}

#endif
