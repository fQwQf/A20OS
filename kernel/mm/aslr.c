/*
 * A20OS — user-space address space layout randomisation (ASLR)
 *
 * A per-process random layout offset is generated at exec time, reusing the
 * entropy pool in core/random.c (xoshiro state, seeded at boot from a mix of
 * timer ticks, the hardware entropy source arch_hw_entropy_sample() (RDRAND /
 * RDSEED on x86_64), the kernel address and a frame count; getrandom(2) and
 * AT_RANDOM draw from the same pool).
 *
 * The number of entropy bits is bounded by the existing fixed user layout:
 *   - the stack top is capped at USER_STACK_TOP + PAGE = 0x40000000, and just
 *     below it 0x3F7F9000-0x3F800000 is the fixed vDSO/vvar (see
 *     mm/vdso_layout.h), with the TLS at 0x3E000000 below that.  Stack
 *     randomisation can therefore only pick a page offset inside the 8 MiB
 *     window [USER_STACK_FLOOR + initial stack, 0x40000000).  To keep at least
 *     ~4 MiB of usable stack, 64-bit takes 10 bits of entropy (a 4 MiB range)
 *     and 32-bit takes 8 (a 1 MiB range).
 *   - the mmap fallback base grows upward from MMAP_BASE_ADDR = 0x60000000 and
 *     the 64-bit user space floor is 0x4000000000, so it takes 20 bits (a 4 GiB
 *     window); 32-bit takes 10.
 *   - the brk start offset cap is bounded by USER_TLS_BASE = 0x3E000000 and
 *     capped dynamically against the end of the image; 64-bit takes at most 13
 *     bits (32 MiB), 32-bit takes 8.
 *
 * fork matches Linux: the child inherits the mm fields such as mmap_base,
 * stack_top and start_brk through *child = *parent and is not re-randomised.
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

#else /* CONFIG_NOMMU: the address is the physical address; no randomisation */

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
