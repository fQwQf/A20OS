#ifdef CONFIG_PPC64LE

#include "core/defs.h"
#include "page_table.h"
#include "asm/ppc64-regs.h"

#define PPC64_RADIX_RTS_52BIT    ((0x5UL << 5) | (0x2UL << 61))
#define PPC64_RADIX_ROOT_BITS    13UL
#define PPC64_PROCESS_TABLE_PA   0x20000UL
#define PPC64_PROCESS_TABLE_SIZE 5UL

typedef struct {
    uint64_t root;
    uint64_t reserved;
} ppc64_process_table_entry_t;

volatile uint64_t ppc64_current_addr_space;

static inline ppc64_process_table_entry_t *ppc64_process_table(void)
{
    return (ppc64_process_table_entry_t *)(PAGE_OFFSET +
                                           PPC64_PROCESS_TABLE_PA);
}

static inline uint64_t ppc64_radix_root_entry(uint64_t root_pa)
{
    uint64_t entry = PPC64_RADIX_RTS_52BIT |
                     (root_pa & 0x01FFFFFFFFFFF000UL) |
                     PPC64_RADIX_ROOT_BITS;
    return __builtin_bswap64(entry);
}

void ppc64_radix_set_process_root(uint64_t token)
{
    ppc64_process_table_entry_t *table = ppc64_process_table();
    table[1].root = ppc64_radix_root_entry(token);
    table[1].reserved = 0;
    __asm__ __volatile__("sync" ::: "memory");
    ppc64_current_addr_space = token;
}

#endif
