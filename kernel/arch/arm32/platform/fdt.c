#ifdef CONFIG_ARM32
#include "core/types.h"
#include "core/bootargs.h"
#include "platform.h"
extern uint32_t arm32_boot_dtb;
const char *arm32_fdt_bootargs_get(void)
{
    static char buf[1024];
    static int ready;
    if (!ready) {
        uint32_t pa = *(uint32_t *)((uintptr_t)&arm32_boot_dtb + PAGE_OFFSET);
        if (pa >= PHYS_MEMORY_BASE && pa < PHYS_MEMORY_END)
            fdt_extract_bootargs((void *)(uintptr_t)(pa + PAGE_OFFSET), buf, sizeof(buf));
        ready = 1;
    }
    return buf;
}
#endif
