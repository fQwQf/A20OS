#ifdef CONFIG_AARCH64

#include "core/smp.h"
#include "core/cpu.h"
#include "core/defs.h"
#include "proc/proc.h"
#include "smp_boot.h"

static void clean_boot_table(uint64_t *table, unsigned level) {
    for (unsigned i = 0; i < 512; i++) {
        if (level < 3 && (table[i] & 3) == 3)
            clean_boot_table(
                (void *)(uintptr_t)(arch_pte_addr(table[i]) + PAGE_OFFSET),
                level + 1);
    }
    size_t line = arch_dcache_line_size();
    for (unsigned i = 0; i < 4096; i += line)
        __asm__ __volatile__("dc cvac, %0" ::"r"((const uint8_t *)table + i)
                             : "memory");
}

void aarch64_boot_clean_page_tables(void) {
    clean_boot_table(boot_pgdir, 0);
    __asm__ __volatile__("dsb sy" ::: "memory");
}

uint64_t arch_smp_boot_hw_id(void) {
    uint64_t mpidr;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & 0x000000ff00ffffffUL;
}

uintptr_t arch_smp_secondary_entry_pa(void) {
    extern char aarch64_secondary_start[];
    return (uintptr_t)aarch64_secondary_start - PAGE_OFFSET;
}

void aarch64_secondary_entry(unsigned cpu_id) {
    if (cpu_id == 0 || cpu_id >= CONFIG_NR_CPUS)
        arch_halt();
    smp_secondary_init(cpu_id);
    arch_local_irq_enable();
    idle_loop();
}

#endif /* CONFIG_AARCH64 */
