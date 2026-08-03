#ifdef CONFIG_PPC64LE

#include "core/defs.h"
#include "page_table.h"
#include "asm/ppc64-regs.h"

/* QEMU's PRTBE_R_GET_RTS = ((pte >> 58) & 0x18 | (pte >> 5) & 0x7) + 31.
 * For the 52-bit radix tree used here (13-bit root, 9-bit lower levels)
 * the RTS must decode to 52: bits 61-62 = 0b10 and bits 5-7 = 0b101, i.e.
 * 0x40000000000000a0, exactly as the bootstrap PID0 entry in entry.S.
 * The 13-bit root page-directory size goes in the RPDS field (bits 0-4).
 *
 * qemu-system-ppc64 reads the process table big-endian (ldq_be_p).  The
 * bootstrap writes the entries with stdbrx (BE image in memory), so the C
 * runtime store must byte-swap the architectural value to produce the same
 * memory bytes. */
#define PPC64_RADIX_ROOT_BITS    13UL
#define PPC64_RADIX_PRTS_MASK    0x40000000000000a0UL
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
    uint64_t arch = (root_pa & 0x0FFFFFFFFFFFFF00UL) |
                    PPC64_RADIX_ROOT_BITS | PPC64_RADIX_PRTS_MASK;
    return __builtin_bswap64(arch);
}

void ppc64_radix_set_process_root(uint64_t token)
{
    ppc64_process_table_entry_t *table = ppc64_process_table();
    table[1].root = ppc64_radix_root_entry(token);
    table[1].reserved = 0;
    __asm__ __volatile__("sync" ::: "memory");
    /* PID 0 is the bootstrap context.  User and kernel task contexts use
     * the process-table entry at PID 1, which is also the PID invalidated by
     * the local TLB helpers. */
    register uint64_t pid __asm__("r3") = 1;
    __asm__ __volatile__("mtspr %1,%0\n\tisync"
                         :: "r"(pid), "i"(PPC64_SPR_PID)
                         : "memory");
    ppc64_current_addr_space = token;
}

#endif
