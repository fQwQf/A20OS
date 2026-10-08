#include "mm/mm.h"

paddr_t arch_pfa_init_limit(void)
{
#ifdef CONFIG_NOMMU
    extern char __drvmod_arena_end[];
    return va_to_pa(__drvmod_arena_end);
#else
    extern char _bss_end[];
    return va_to_pa(_bss_end);
#endif
}
