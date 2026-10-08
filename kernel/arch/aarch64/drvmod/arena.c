/* AArch64 NOMMU-only privileged module arena. */
#include "core/arch.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/mm.h"
#include "proc/proc_internal.h"
#include "drvmod/drvmod.h"

#ifdef CONFIG_NOMMU
/*
 * drvmod image arena (aarch64 NOMMU only).
 *
 * AArch64 makes a stage-1 block that grants EL0 write execute-never for every
 * higher exception level, and QEMU enforces that unconditionally for AA64
 * (target/arm/ptw.c get_S1prot(): the regime_has_2_ranges() gate never
 * consults TCR, so SCTLR_EL1.WXN cannot relax it).  The boot map therefore
 * has to split DRAM -- kernel image plus this arena AP=00, everything the
 * allocator hands out AP=01 -- and drvmod code, the only privileged code the
 * frame pool ever holds, has to come from the privileged side because EL1
 * has to fetch from it.  pfa_init() starts past __drvmod_arena_end, so no
 * kmalloc can land here.
 *
 * This is a first-fit map rather than a bump pointer because drvmod_unload()
 * releases the space again.  Every span is 2^order-aligned *and* a whole
 * number of 2 MiB units, so a module image never straddles a block boundary
 * and cannot run past the end of the AP=00 prefix.
 */
extern char __drvmod_arena_start[];
extern char __drvmod_arena_end[];

/* DRV_MOD_MAX_SIZE is 512 KiB, i.e. 2^7 pages, so order 7 is the ceiling. */
#define DRVMOD_ARENA_MAX_ORDER 7
#define DRVMOD_ARENA_SPAN_PAGES (1u << DRVMOD_ARENA_MAX_ORDER)
/* One bit per 2^DRVMOD_ARENA_MAX_ORDER-page span over a 16 MiB arena. */
#define DRVMOD_ARENA_UNITS 32

static uint8_t drvmod_arena_used[DRVMOD_ARENA_UNITS];
static spinlock_t drvmod_arena_lock;
static bool drvmod_arena_ready;

static void drvmod_arena_init(void)
{
    uintptr_t start = (uintptr_t)__drvmod_arena_start;
    uintptr_t end = (uintptr_t)__drvmod_arena_end;
    uintptr_t span = (uintptr_t)DRVMOD_ARENA_SPAN_PAGES * PAGE_SIZE;

    if (end < start + (uintptr_t)DRVMOD_ARENA_UNITS * span) {
        kerr("[DRVMOD] arena too small: %lx..%lx need %lu\n",
             (unsigned long)start, (unsigned long)end,
             (unsigned long)((uintptr_t)DRVMOD_ARENA_UNITS * span));
        return;
    }
    memset(drvmod_arena_used, 0, sizeof(drvmod_arena_used));
    spin_init(&drvmod_arena_lock);
    drvmod_arena_ready = true;
    printf("[DRVMOD] arena %lx..%lx (%u MiB)\n", (unsigned long)start,
           (unsigned long)end,
           (unsigned int)(((uintptr_t)DRVMOD_ARENA_UNITS * span) >> 20));
}

/*
 * How many arena units an order-@order request occupies.  One unit is a whole
 * 2^DRVMOD_ARENA_MAX_ORDER-page span, so the count is
 * ceil(2^order / 2^MAX_ORDER) -- which is 1 for every order up to the cap,
 * since nothing is larger than a span.
 *
 * Derived as a shift on MAX_ORDER - order this came out inverted: an order-1
 * (two-page) module asked for 1 << 6 = 64 units out of a 32-unit arena, the
 * allocation loop's `i + units <= DRVMOD_ARENA_UNITS` bound was never
 * satisfied, and drvmod_arena_alloc() returned NULL for every module whose
 * image was not exactly 512 KiB.  Nothing logged that as an error -- the load
 * just failed with ENOMEM and the guest carried on without the driver -- so a
 * NOMMU instance lost every .a20drv except the order-7 ones and still booted
 * to a shell.
 */
static uint32_t drvmod_arena_units(uint32_t order)
{
    if (order >= DRVMOD_ARENA_MAX_ORDER)
        return 1u << (order - DRVMOD_ARENA_MAX_ORDER);
    return 1;   /* smaller than a span; a span is the indivisible unit */
}

/* Reserve @order pages' worth of arena, or NULL if it is full.  NOMMU links
 * at VIRT_BASE == PHYS_BASE, so the returned VA is also the PA that the boot
 * identity map describes. */
static void *drvmod_arena_alloc(uint32_t order)
{
    if (!drvmod_arena_ready) {
        /* Initialize on first use: drvmod_load() is the only consumer, so
         * there is no ordering requirement against the boot map. */
        drvmod_arena_init();
        if (!drvmod_arena_ready)
            return NULL;
    }
    if (order > DRVMOD_ARENA_MAX_ORDER)
        return NULL;

    uint32_t units = drvmod_arena_units(order);
    uintptr_t span = (uintptr_t)DRVMOD_ARENA_SPAN_PAGES * PAGE_SIZE;
    void *ret = NULL;

    spin_lock(&drvmod_arena_lock);
    for (uint32_t i = 0; i + units <= DRVMOD_ARENA_UNITS; i++) {
        bool clear = true;
        for (uint32_t j = 0; j < units; j++) {
            if (drvmod_arena_used[i + j]) {
                clear = false;
                break;
            }
        }
        if (!clear)
            continue;
        for (uint32_t j = 0; j < units; j++)
            drvmod_arena_used[i + j] = 1;
        ret = (void *)((uintptr_t)__drvmod_arena_start + (uintptr_t)i * span);
        break;
    }
    spin_unlock(&drvmod_arena_lock);
    return ret;
}

static void drvmod_arena_free(void *addr, uint32_t order)
{
    if (!drvmod_arena_ready || !addr || order > DRVMOD_ARENA_MAX_ORDER)
        return;

    uintptr_t start = (uintptr_t)__drvmod_arena_start;
    uintptr_t span = (uintptr_t)DRVMOD_ARENA_SPAN_PAGES * PAGE_SIZE;
    if ((uintptr_t)addr < start)
        return;

    uint32_t i = (uint32_t)(((uintptr_t)addr - start) / span);
    uint32_t units = drvmod_arena_units(order);
    if (i >= DRVMOD_ARENA_UNITS)
        return;

    spin_lock(&drvmod_arena_lock);
    for (uint32_t j = 0; j < units && i + j < DRVMOD_ARENA_UNITS; j++)
        drvmod_arena_used[i + j] = 0;
    spin_unlock(&drvmod_arena_lock);
}

int arch_drvmod_alloc_reserved(uint32_t order, uintptr_t *addr_out)
{
    void *addr = drvmod_arena_alloc(order);
    if (!addr)
        return -ENOMEM;
    *addr_out = (uintptr_t)addr;
    return 1;
}

void arch_drvmod_free_reserved(uintptr_t addr, uint32_t order)
{
    drvmod_arena_free((void *)addr, order);
}
#else
int arch_drvmod_alloc_reserved(uint32_t order, uintptr_t *addr_out)
{
    (void)order;
    (void)addr_out;
    return 0;
}

void arch_drvmod_free_reserved(uintptr_t addr, uint32_t order)
{
    (void)addr;
    (void)order;
}
#endif
