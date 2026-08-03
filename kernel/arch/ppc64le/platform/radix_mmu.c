#ifdef CONFIG_PPC64LE

#include "core/defs.h"
#include "page_table.h"
#include "asm/ppc64-regs.h"

/* QEMU's PRTBE_R_GET_RTS = ((pte & 0x1F) << 2) | ((pte >> 61) & 3).  For the
 * 52-bit radix tree used here the 13-bit root page-directory size must be in
 * bits 5..0 and the RTS high bits (63..62) stay 0, so a prtbe is simply
 * (root_pa & RPDB) | 13 with no byte swap: QEMU reads the table with
 * ldq_phys (little-endian), and the kernel stores it little-endian. */
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
    return (root_pa & 0x0FFFFFFFFFFFFF00UL) | PPC64_RADIX_ROOT_BITS;
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
