/*
 * A20OS — kernel symbol table support (kallsyms).
 *
 * The strong definitions of kallsyms_lookup()/kallsyms_count() live in the
 * generated table object (build-time second link pass, tools/gen_kallsyms
 * .py).  When generation is unavailable (e.g. a judge environment without
 * python3) these weak defaults keep the kernel linkable and backtraces fall
 * back to raw addresses.
 */

#include "core/kallsyms.h"
#include "core/stdio.h"
#include "core/string.h"

__attribute__((weak)) const char *kallsyms_lookup(uint64_t addr,
                                                  uint64_t *offset_out)
{
    if (offset_out)
        *offset_out = addr;
    return NULL;
}

__attribute__((weak)) uint32_t kallsyms_count(void)
{
    return 0;
}

void kallsyms_print(uint64_t addr)
{
    uint64_t off = 0;
    const char *name = kallsyms_lookup(addr, &off);
    if (name && name[0])
        printf("%s+0x%llx", name, (unsigned long long)off);
    else
        printf("0x%llx", (unsigned long long)addr);
}
